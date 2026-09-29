#include <studyapp/canvas/RenderBatches.hpp>

#include "ParallelFor.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>

#include <bit>
#include <cstddef>
#include <utility>
#include <vector>

namespace studyapp::canvas {

namespace {

/// 64-bit hash of the values a batch's geometry depends on.
class Signature {
public:
    void add(std::uint64_t value) noexcept {
        // One multiply-xorshift round per 64-bit word (not per byte): this runs for every
        // element of the page whenever the scene changes.
        hash_ = (hash_ ^ value) * 0x9E3779B97F4A7C15ULL;
        hash_ ^= hash_ >> 32U;
    }
    void add(double value) noexcept { add(std::bit_cast<std::uint64_t>(value)); }
    void add(float value) noexcept {
        add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
    }
    void add(const core::Uuid& uuid) noexcept {
        const auto& bytes = uuid.bytes();
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            high = (high << 8U) | bytes[i];
            low = (low << 8U) | bytes[i + 8];
        }
        add(high);
        add(low);
    }
    /// With a final avalanche (murmur3's fmix64), so every bit depends on all input bits
    /// (run boundaries test the low bits).
    [[nodiscard]] std::uint64_t value() const noexcept {
        std::uint64_t h = hash_;
        h ^= h >> 33U;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33U;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33U;
        return h;
    }

private:
    std::uint64_t hash_ = 0xCBF29CE484222325ULL;
};

/// Whether a run starts at this element (content-defined: the same for the element
/// wherever it is in the draw order). FNV-1a over the id's bytes: the run layout D40 was
/// measured with (only elements of runs past kMinBatchSize are tested).
bool startsRun(const core::ElementId& id) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const std::uint8_t byte : id.value().bytes()) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash % RenderBatches::kBoundaryModulus == 0;
}

} // namespace

void RenderBatches::update(const CanvasScene& scene, const document::Workspace& workspace,
                           RenderCache& cache, int lodBucket, render::Renderer& renderer) {
    rebuilt_ = 0;
    if (valid_ && refined_ && sceneGeneration_ == scene.generation() && lodBucket_ == lodBucket) {
        return;
    }
    bool refined = true;

    // 0. Build missing meshes in parallel (the first frame of a page, a large paste).
    {
        std::vector<RenderCache::Request> requests;
        requests.reserve(scene.drawOrder().size());
        for (const core::ElementId id : scene.drawOrder()) {
            const SceneEntry* entry = scene.find(id);
            const document::Element* element = workspace.findElement(id);
            if (entry != nullptr && element != nullptr && entry->layerVisible) {
                requests.push_back({.element = element, .version = entry->contentVersion});
            }
        }
        cache.prebuild(requests, lodBucket);
    }

    // 1. Partition the draw order into runs and compute each run's signature.
    std::vector<Batch> next;
    std::vector<core::LayerId> layers; // parallel to next
    for (const core::ElementId id : scene.drawOrder()) {
        const SceneEntry* entry = scene.find(id);
        const document::Element* element = workspace.findElement(id);
        if (entry == nullptr || element == nullptr || !entry->layerVisible) {
            continue;
        }
        const bool textured = std::holds_alternative<document::TextBox>(element->payload) ||
                              std::holds_alternative<document::Image>(element->payload);
        if (next.empty() || next.back().members.size() >= kBatchSize ||
            layers.back() != entry->layer || textured || next.back().textured ||
            (next.back().members.size() >= kMinBatchSize && startsRun(id))) {
            next.push_back(Batch{.firstDrawIndex = entry->drawIndex,
                                 .origin = meshToWorld(*element).apply({0.0, 0.0}),
                                 .opacity = entry->layerOpacity,
                                 .textured = textured});
            layers.push_back(entry->layer);
        }
        Batch& batch = next.back();
        batch.members.push_back(id);
        batch.lastDrawIndex = entry->drawIndex;
        const RenderCache::Entry& cached =
            cache.ensure(*element, entry->contentVersion, lodBucket, nullptr);
        refined = refined && cached.lodBucket >= lodBucket;
        const document::Transform& t = element->transform;
        Signature signature;
        signature.add(batch.signature);
        signature.add(id.value());
        signature.add(entry->contentVersion);
        signature.add(static_cast<std::uint64_t>(static_cast<std::int64_t>(cached.lodBucket)));
        signature.add(t.position.x);
        signature.add(t.position.y);
        signature.add(t.rotation);
        signature.add(t.scale.x);
        signature.add(t.scale.y);
        signature.add(entry->layerOpacity);
        batch.signature = signature.value();
    }

    // 2. Reuse unchanged runs; rebuild the others (reusing their GPU meshes). The merged
    //    meshes are built in parallel (each from its own members, into its own slot);
    //    uploads stay on this thread (the renderer is not thread-safe).
    std::vector<std::size_t> rebuild;
    std::vector<render::MeshHandle> reusables(next.size());
    for (std::size_t k = 0; k < next.size(); ++k) {
        Batch& batch = next[k];
        if (k < batches_.size()) {
            reusables[k] = std::exchange(batches_[k].gpu, render::MeshHandle{});
            if (batches_[k].signature == batch.signature && reusables[k].isValid()) {
                batch.gpu = std::exchange(reusables[k], render::MeshHandle{});
                batch.triangles = batches_[k].triangles;
                continue;
            }
        }
        if (batch.textured) {
            if (reusables[k].isValid()) {
                renderer.destroyMesh(reusables[k]);
                reusables[k] = {};
            }
            continue; // drawn by the controller, element by element
        }
        rebuild.push_back(k);
    }
    // Every member is in the cache now (step 1); the cache is only read from here on.
    std::vector<const RenderCache::Entry*> members;
    std::vector<std::size_t> firstMember(rebuild.size() + 1, 0);
    for (std::size_t r = 0; r < rebuild.size(); ++r) {
        for (const core::ElementId id : next[rebuild[r]].members) {
            const document::Element& element = *workspace.findElement(id);
            members.push_back(
                &cache.ensure(element, scene.find(id)->contentVersion, lodBucket, nullptr));
        }
        firstMember[r + 1] = members.size();
    }
    std::vector<render::MeshData> merged(rebuild.size());
    detail::parallelFor(rebuild.size(), 4, [&](std::size_t begin, std::size_t end) {
        for (std::size_t r = begin; r < end; ++r) {
            const Batch& batch = next[rebuild[r]];
            render::MeshData& mesh = merged[r];
            std::size_t vertices = 0;
            std::size_t indices = 0;
            for (std::size_t m = firstMember[r]; m < firstMember[r + 1]; ++m) {
                for (const MeshPart& part : members[m]->parts) {
                    vertices += part.mesh.vertices.size();
                    indices += part.mesh.indices.size();
                }
            }
            mesh.vertices.reserve(vertices);
            mesh.colors.reserve(vertices);
            mesh.parts.reserve(vertices);
            mesh.indices.reserve(indices);
            const core::Affine2 toRun = core::Affine2::translation(-batch.origin);
            // Every member mesh part is its own part of the batch: it covers a pixel at
            // most once, exactly as when drawn on its own (render::RenderFrame::content).
            std::uint32_t partNumber = 0;
            for (std::size_t m = firstMember[r]; m < firstMember[r + 1]; ++m) {
                const document::Element& element =
                    *workspace.findElement(batch.members[m - firstMember[r]]);
                const core::Affine2f transform = (toRun * meshToWorld(element)).cast<float>();
                for (const MeshPart& part : members[m]->parts) {
                    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                    for (const core::Vec2& v : part.mesh.vertices) {
                        const core::Vec2 p = transform.apply(v);
                        mesh.vertices.push_back(p);
                        mesh.bounds = mesh.bounds.including(p);
                    }
                    mesh.colors.insert(mesh.colors.end(), part.mesh.vertices.size(), part.color);
                    mesh.parts.insert(mesh.parts.end(), part.mesh.vertices.size(), partNumber++);
                    for (const std::uint32_t index : part.mesh.indices) {
                        mesh.indices.push_back(base + index);
                    }
                }
            }
        }
    });
    for (std::size_t r = 0; r < rebuild.size(); ++r) {
        const std::size_t k = rebuild[r];
        Batch& batch = next[k];
        render::MeshData& mesh = merged[r];
        const render::MeshHandle reusable = reusables[k];
        batch.triangles = mesh.triangleCount();
        if (mesh.empty()) {
            if (reusable.isValid()) {
                renderer.destroyMesh(reusable);
            }
        } else if (reusable.isValid()) {
            renderer.updateMesh(reusable, mesh);
            batch.gpu = reusable;
        } else {
            batch.gpu = renderer.createMesh(mesh);
        }
        mesh = {}; // release the CPU copy right away
        ++rebuilt_;
    }
    for (std::size_t k = next.size(); k < batches_.size(); ++k) {
        if (batches_[k].gpu.isValid()) {
            renderer.destroyMesh(batches_[k].gpu);
        }
    }
    batches_ = std::move(next);
    sceneGeneration_ = scene.generation();
    lodBucket_ = lodBucket;
    valid_ = true;
    refined_ = refined;
}

void RenderBatches::clear(render::Renderer& renderer) {
    for (const Batch& batch : batches_) {
        if (batch.gpu.isValid()) {
            renderer.destroyMesh(batch.gpu);
        }
    }
    batches_.clear();
    valid_ = false;
}

void RenderBatches::forgetGpuResources() noexcept {
    batches_.clear();
    valid_ = false;
}

} // namespace studyapp::canvas
