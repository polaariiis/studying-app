#pragma once

#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/RenderCache.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Workspace.hpp>
#include <studyapp/render/Renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace studyapp::canvas {

/// Draw-call batching for views with many visible elements (docs/RENDERING.md §6.3,
/// "consecutive-item batching" / "static chunks"; introduced after measurement, see
/// docs/ROADMAP.md Phase 4 results).
///
/// The page's draw order (visible layers only) is cut into runs of at most `kBatchSize`
/// consecutive elements of one layer. Each run becomes one mesh: the members' cached
/// meshes transformed into the run's space (world minus the first member's position, so
/// floats only span the run) with per-vertex colours. Because runs are consecutive in
/// draw order and drawn in order, painter's order is exactly the same as drawing every
/// element on its own.
///
/// A run is rebuilt only when its signature changes: member ids, their content versions,
/// render-cache detail levels and transforms. Moves, edits, inserts and deletes therefore
/// never show stale geometry. Batches are never a source of truth; they are derived from
/// the scene and the render cache every time the scene changes.
class RenderBatches {
public:
    static constexpr std::size_t kBatchSize = 256;

    struct Batch {
        std::vector<core::ElementId> members{}; ///< consecutive in draw order
        std::size_t firstDrawIndex = 0;
        std::size_t lastDrawIndex = 0;
        core::DVec2 origin{}; ///< world position the mesh is relative to
        float opacity = 1.0F; ///< layer opacity
        std::uint64_t signature = 0;
        render::MeshHandle gpu{};
        std::size_t triangles = 0;
    };

    /// Brings the batches up to date with the scene (cheap when nothing changed: the
    /// partition is recomputed only when the scene generation or detail level changes).
    void update(const CanvasScene& scene, const document::Workspace& workspace, RenderCache& cache,
                int lodBucket, render::Renderer& renderer);

    [[nodiscard]] const std::vector<Batch>& batches() const noexcept { return batches_; }
    [[nodiscard]] std::uint32_t rebuiltLastUpdate() const noexcept { return rebuilt_; }

    /// Destroys every GPU mesh (on page switch). Context must be current.
    void clear(render::Renderer& renderer);
    /// The context was lost: forget GPU handles; meshes are rebuilt on next update.
    void forgetGpuResources() noexcept;

private:
    std::vector<Batch> batches_;
    std::uint64_t sceneGeneration_ = 0;
    int lodBucket_ = 0;
    bool valid_ = false;
    std::uint32_t rebuilt_ = 0;
};

} // namespace studyapp::canvas
