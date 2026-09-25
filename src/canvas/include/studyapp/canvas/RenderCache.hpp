#pragma once

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/render/Renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace studyapp::canvas {

/// Tessellated meshes per element and their GPU uploads (docs/CANVAS.md §3.3).
///
/// Never a source of truth: an entry is keyed by the scene's content version of the
/// element, so any payload change (a new version) rebuilds it on next use, while a move
/// (transform only, same version) reuses it. Level of detail is a power-of-two zoom
/// bucket; an entry is rebuilt only when drawn at a finer bucket than it was built for.
/// CPU meshes are kept so GPU resources can be recreated after a context loss.
///
/// Refinement is spread over frames: at most refinementBudget() existing entries are
/// re-tessellated per frame for a finer bucket; beyond that the coarser mesh is used (the
/// same geometry in world units, only rounder joins and caps are missing) and
/// refinementPending() tells the caller to draw another frame. Zooming in with a whole
/// 10 000-stroke page in view otherwise re-tessellated every stroke in one frame (130 ms).
/// New or changed content is always built at once.
///
/// GPU handles of evicted entries are queued and destroyed in flush(), which runs while
/// the renderer's context is current.
class RenderCache {
public:
    struct Entry {
        std::vector<MeshPart> parts;
        std::vector<render::MeshHandle> gpu; ///< parallel to parts (empty until uploaded)
        std::uint64_t version = 0;
        int lodBucket = 0;
        std::size_t bytes = 0;
    };

    struct Stats {
        std::size_t entries = 0;
        std::size_t cpuBytes = 0;
        std::uint32_t builtLastFrame = 0;   ///< including refinements
        std::uint32_t refinedLastFrame = 0; ///< rebuilt only for a finer level of detail
        std::uint32_t uploadedLastFrame = 0;
        bool refinementPending = false; ///< entries were drawn coarser than asked for
    };

    /// Unlimited by default. The controller uses kDefaultRefinementBudget.
    void setRefinementBudget(std::size_t perFrame) noexcept { refinementBudget_ = perFrame; }
    [[nodiscard]] std::size_t refinementBudget() const noexcept { return refinementBudget_; }
    /// Since beginFrame(): an entry was used coarser than asked for (budget exhausted).
    [[nodiscard]] bool refinementPending() const noexcept { return refinementPending_; }

    /// Measured on the reference laptop, zooming with a whole 10 000-stroke page in view
    /// (bench: BM_BuildFrameZoomingWholePage): refining frames take ≈ 6 ms median, 10.5 ms
    /// worst, including the batch rebuilds they cause (512: ≈ 9 ms; unlimited: one 130 ms
    /// frame). The page is refined within ≈ 40 frames (≈ 0.3 s at 144 Hz).
    static constexpr std::size_t kDefaultRefinementBudget = 256;

    /// The entry for `element`, (re)built as needed and uploaded to `uploadTo` (nullptr:
    /// CPU mesh only, e.g. for elements drawn through a batch).
    const Entry& ensure(const document::Element& element, std::uint64_t version, int lodBucket,
                        render::Renderer* uploadTo);

    void evict(core::ElementId id);
    void evictAll();
    /// Destroys queued GPU meshes. Call with the renderer's context current.
    void flush(render::Renderer& renderer);
    /// The context was lost: GPU handles are gone (the renderer released them). CPU meshes
    /// stay and are uploaded again on next use.
    void forgetGpuResources() noexcept;

    void beginFrame() noexcept;
    [[nodiscard]] Stats stats() const noexcept;

    /// Power-of-two bucket for `zoom` and the pixels-per-unit it tessellates for.
    [[nodiscard]] static int lodBucketFor(double zoom) noexcept;
    [[nodiscard]] static float pixelsPerUnit(int lodBucket) noexcept;

private:
    std::unordered_map<core::ElementId, Entry> entries_;
    std::vector<render::MeshHandle> pendingDestroy_;
    std::size_t cpuBytes_ = 0;
    std::uint32_t built_ = 0;
    std::uint32_t refined_ = 0;
    std::uint32_t uploaded_ = 0;
    std::size_t refinementBudget_ = static_cast<std::size_t>(-1);
    bool refinementPending_ = false;
};

} // namespace studyapp::canvas
