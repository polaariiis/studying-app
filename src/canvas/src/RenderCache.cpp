#include <studyapp/canvas/RenderCache.hpp>

#include "ParallelFor.hpp"

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
    const bool changed = inserted || entry.version != version;
    bool refine = !changed && entry.lodBucket < lodBucket;
    if (refine && refined_ >= refinementBudget_) {
        refine = false; // keep the coarser mesh this frame
        refinementPending_ = true;
    }
    if (changed || refine) {
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
        if (refine) {
            ++refined_;
        }
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

void RenderCache::prebuild(std::span<const Request> requests, int lodBucket) {
    // Serial: find what needs building (the map is not modified while threads run; values
    // of an unordered_map keep their address when it grows).
    std::vector<std::pair<Entry*, const document::Element*>> pending;
    for (const Request& request : requests) {
        auto [it, inserted] = entries_.try_emplace(request.element->id);
        Entry& entry = it->second;
        if (!inserted && entry.version == request.version) {
            continue; // up to date (a coarser level is ensure()'s budgeted refinement)
        }
        for (const render::MeshHandle handle : entry.gpu) {
            pendingDestroy_.push_back(handle);
        }
        entry.gpu.clear();
        cpuBytes_ -= entry.bytes;
        entry.bytes = 0;
        entry.version = request.version;
        entry.lodBucket = lodBucket;
        pending.emplace_back(&entry, request.element);
    }
    // Parallel: each entry is written by one thread only.
    const float pixels = pixelsPerUnit(lodBucket);
    detail::parallelFor(
        pending.size(), kPrebuildPerThread, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                pending[i].first->parts = buildElementMeshes(*pending[i].second, pixels);
            }
        });
    for (const auto& [entry, element] : pending) {
        entry->bytes = bytesOf(entry->parts);
        cpuBytes_ += entry->bytes;
    }
    built_ += static_cast<std::uint32_t>(pending.size());
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
    refined_ = 0;
    uploaded_ = 0;
    refinementPending_ = false;
}

RenderCache::Stats RenderCache::stats() const noexcept {
    return {.entries = entries_.size(),
            .cpuBytes = cpuBytes_,
            .builtLastFrame = built_,
            .refinedLastFrame = refined_,
            .uploadedLastFrame = uploaded_,
            .refinementPending = refinementPending_};
}

} // namespace studyapp::canvas
