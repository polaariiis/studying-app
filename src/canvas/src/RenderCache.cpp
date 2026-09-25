#include <studyapp/canvas/RenderCache.hpp>

#include <algorithm>
#include <cmath>

namespace studyapp::canvas {

namespace {

std::size_t bytesOf(const std::vector<MeshPart>& parts) noexcept {
    std::size_t bytes = 0;
    for (const MeshPart& part : parts) {
        bytes += part.mesh.byteSize();
    }
    return bytes;
}

} // namespace

int RenderCache::lodBucketFor(double zoom) noexcept {
    if (!std::isfinite(zoom) || zoom <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::clamp(std::ceil(std::log2(zoom)), -8.0, 8.0));
}

float RenderCache::pixelsPerUnit(int lodBucket) noexcept {
    return std::ldexp(1.0F, lodBucket);
}

const RenderCache::Entry& RenderCache::ensure(const document::Element& element,
                                              std::uint64_t version, int lodBucket,
                                              render::Renderer* uploadTo) {
    auto [it, inserted] = entries_.try_emplace(element.id);
    Entry& entry = it->second;
    const bool stale = inserted || entry.version != version || entry.lodBucket < lodBucket;
    if (stale) {
        for (const render::MeshHandle handle : entry.gpu) {
            pendingDestroy_.push_back(handle);
        }
        entry.gpu.clear();
        cpuBytes_ -= entry.bytes;
        entry.parts = buildElementMeshes(element, pixelsPerUnit(lodBucket));
        entry.bytes = bytesOf(entry.parts);
        cpuBytes_ += entry.bytes;
        entry.version = version;
        entry.lodBucket = lodBucket;
        ++built_;
    }
    if (uploadTo != nullptr && entry.gpu.size() != entry.parts.size()) {
        entry.gpu.clear();
        for (const MeshPart& part : entry.parts) {
            entry.gpu.push_back(part.mesh.empty() ? render::MeshHandle{}
                                                  : uploadTo->createMesh(part.mesh));
        }
        ++uploaded_;
    }
    return entry;
}

void RenderCache::evict(core::ElementId id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }
    for (const render::MeshHandle handle : it->second.gpu) {
        pendingDestroy_.push_back(handle);
    }
    cpuBytes_ -= it->second.bytes;
    entries_.erase(it);
}

void RenderCache::evictAll() {
    for (auto& [id, entry] : entries_) {
        for (const render::MeshHandle handle : entry.gpu) {
            pendingDestroy_.push_back(handle);
        }
    }
    entries_.clear();
    cpuBytes_ = 0;
}

void RenderCache::flush(render::Renderer& renderer) {
    for (const render::MeshHandle handle : pendingDestroy_) {
        if (handle.isValid()) {
            renderer.destroyMesh(handle);
        }
    }
    pendingDestroy_.clear();
}

void RenderCache::forgetGpuResources() noexcept {
    for (auto& [id, entry] : entries_) {
        entry.gpu.clear();
    }
    pendingDestroy_.clear();
}

void RenderCache::beginFrame() noexcept {
    built_ = 0;
    uploaded_ = 0;
}

RenderCache::Stats RenderCache::stats() const noexcept {
    return {.entries = entries_.size(),
            .cpuBytes = cpuBytes_,
            .builtLastFrame = built_,
            .uploadedLastFrame = uploaded_};
}

} // namespace studyapp::canvas
