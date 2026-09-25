#pragma once

#include <studyapp/canvas/SpatialGrid.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/core/Rect.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Workspace.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace studyapp::canvas {

/// Per-element scene data: derived from the document, never authoritative.
struct SceneEntry {
    core::ElementId id;
    core::LayerId layer;
    core::DRect bounds;               ///< world bounds (document::worldBounds)
    std::size_t drawIndex = 0;        ///< position in drawOrder()
    std::uint64_t contentVersion = 0; ///< bumped when the payload changes (cache key)
    bool layerVisible = true;
    bool layerLocked = false;
    float layerOpacity = 1.0F;
};

/// The displayed page as the canvas sees it (docs/CANVAS.md §3): a spatial index for
/// culling and hit testing, the painter's order, and per-element versions that key the
/// render cache. It mirrors the Workspace (the source of truth) and is updated
/// incrementally from applied patches; it holds ids and derived values only, never copies
/// of the records.
///
/// Draw order is layer order, then each layer's element order — exactly the Workspace's
/// ordered child indexes (FractionalIndex keys, ties by id), so there is no second sort.
class CanvasScene {
public:
    CanvasScene() = default;

    /// Rebuilds everything for `page` (nullopt or an unknown page: empty scene).
    void rebuild(const document::Workspace& workspace, std::optional<core::PageId> page);

    /// Updates the scene after `patch` was applied to `workspace`.
    void onPatch(const document::Workspace& workspace, const document::Patch& patch);

    [[nodiscard]] std::optional<core::PageId> page() const noexcept { return page_; }
    [[nodiscard]] std::size_t elementCount() const noexcept { return entries_.size(); }
    [[nodiscard]] const SceneEntry* find(core::ElementId id) const;
    [[nodiscard]] std::span<const core::ElementId> drawOrder() const noexcept { return order_; }

    /// Elements on visible layers whose bounds intersect `rect`, in draw order (back to
    /// front). The broad phase for rendering and hit testing.
    void query(const core::DRect& rect, std::vector<core::ElementId>& out) const;

    struct OrderedId {
        std::size_t drawIndex = 0;
        core::ElementId id;
    };
    /// As query(), with each element's draw index (appended, sorted by draw index).
    void queryOrdered(const core::DRect& rect, std::vector<OrderedId>& out) const;

    /// Changes whenever the scene's content, order or layer state changes.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Ids removed from the scene since the last call (for render cache cleanup).
    [[nodiscard]] std::vector<core::ElementId> takeRemoved();

    [[nodiscard]] const SpatialGrid& grid() const noexcept { return grid_; }

private:
    void rebuildOrder(const document::Workspace& workspace);
    void upsert(const document::Workspace& workspace, core::ElementId id, bool contentChanged);
    void removeEntry(core::ElementId id);

    std::optional<core::PageId> page_;
    SpatialGrid grid_;
    std::unordered_map<core::ElementId, SceneEntry> entries_;
    std::vector<core::ElementId> order_;
    std::vector<core::ElementId> removed_;
    std::uint64_t nextVersion_ = 1;
    std::uint64_t generation_ = 1;
};

} // namespace studyapp::canvas
