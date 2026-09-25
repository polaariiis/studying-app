#include <studyapp/canvas/RenderBatches.hpp>

#include <studyapp/canvas/ElementGeometry.hpp>

#include <bit>
#include <utility>

namespace studyapp::canvas {

namespace {

/// 64-bit FNV-1a style mixing of the values a batch's geometry depends on.
class Signature {
public:
    void add(std::uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            hash_ ^= (value >> (8 * i)) & 0xFFU;
            hash_ *= 0x100000001B3ULL;
        }
    }
    void add(double value) noexcept { add(std::bit_cast<std::uint64_t>(value)); }
    void add(float value) noexcept {
        add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
    }
    void add(const core::Uuid& uuid) noexcept {
        for (const std::uint8_t byte : uuid.bytes()) {
            hash_ ^= byte;
            hash_ *= 0x100000001B3ULL;
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 0xCBF29CE484222325ULL;
};

} // namespace

void RenderBatches::update(const CanvasScene& scene, const document::Workspace& workspace,
                           RenderCache& cache, int lodBucket, render::Renderer& renderer) {
    rebuilt_ = 0;
    if (valid_ && refined_ && sceneGeneration_ == scene.generation() && lodBucket_ == lodBucket) {
        return;
    }
    bool refined = true;

    // 1. Partition the draw order into runs and compute each run's signature.
    std::vector<Batch> next;
    std::vector<core::LayerId> layers; // parallel to next
    for (const core::ElementId id : scene.drawOrder()) {
        const SceneEntry* entry = scene.find(id);
        const document::Element* element = workspace.findElement(id);
        if (entry == nullptr || element == nullptr || !entry->layerVisible) {
            continue;
        }
        if (next.empty() || next.back().members.size() >= kBatchSize ||
            layers.back() != entry->layer) {
            next.push_back(Batch{.firstDrawIndex = entry->drawIndex,
                                 .origin = meshToWorld(*element).apply({0.0, 0.0}),
                                 .opacity = entry->layerOpacity});
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

    // 2. Reuse unchanged runs; rebuild the others (reusing their GPU meshes).
    for (std::size_t k = 0; k < next.size(); ++k) {
        Batch& batch = next[k];
        render::MeshHandle reusable;
        if (k < batches_.size()) {
            reusable = std::exchange(batches_[k].gpu, render::MeshHandle{});
            if (batches_[k].signature == batch.signature && reusable.isValid()) {
                batch.gpu = reusable;
                batch.triangles = batches_[k].triangles;
                continue;
            }
        }
        render::MeshData mesh;
        const core::Affine2 toRun = core::Affine2::translation(-batch.origin);
        for (const core::ElementId id : batch.members) {
            const document::Element& element = *workspace.findElement(id);
            const SceneEntry& entry = *scene.find(id);
            const RenderCache::Entry& cached =
                cache.ensure(element, entry.contentVersion, lodBucket, nullptr);
            const core::Affine2f transform = (toRun * meshToWorld(element)).cast<float>();
            for (const MeshPart& part : cached.parts) {
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                for (const core::Vec2& v : part.mesh.vertices) {
                    const core::Vec2 p = transform.apply(v);
                    mesh.vertices.push_back(p);
                    mesh.bounds = mesh.bounds.including(p);
                }
                mesh.colors.insert(mesh.colors.end(), part.mesh.vertices.size(), part.color);
                for (const std::uint32_t index : part.mesh.indices) {
                    mesh.indices.push_back(base + index);
                }
            }
        }
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
