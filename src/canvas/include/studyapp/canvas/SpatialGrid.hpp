#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/core/Rect.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace studyapp::canvas {

/// Sparse spatial hash grid over world-space bounding boxes (docs/CANVAS.md §3.1): the
/// broad phase for culling and hit testing.
///
/// Cells are created only where elements are, so an infinite canvas costs nothing where
/// it is empty. Elements that would cover more than `largeCellLimit` cells are kept in a
/// separate list checked by every query (there are few of them). Queries return every
/// element whose bounds intersect the query rectangle, each once, sorted by id so results
/// are deterministic; callers do the exact geometric tests.
class SpatialGrid {
public:
    explicit SpatialGrid(double cellSize = 512.0, std::size_t largeCellLimit = 16);

    /// Inserts or replaces the element's bounds. Empty or non-finite bounds remove it.
    void upsert(core::ElementId id, const core::DRect& bounds);
    void remove(core::ElementId id);
    void clear() noexcept;

    /// Appends the ids of all elements whose bounds intersect `rect` (sorted, unique).
    void query(const core::DRect& rect, std::vector<core::ElementId>& out) const;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t cellCount() const noexcept { return cells_.size(); }
    [[nodiscard]] std::size_t largeCount() const noexcept { return large_.size(); }
    [[nodiscard]] double cellSize() const noexcept { return cellSize_; }

private:
    struct CellKey {
        std::int64_t x = 0;
        std::int64_t y = 0;
        [[nodiscard]] friend bool operator==(const CellKey&, const CellKey&) = default;
    };
    struct CellKeyHash {
        [[nodiscard]] std::size_t operator()(const CellKey& key) const noexcept {
            const auto ux = static_cast<std::uint64_t>(key.x);
            const auto uy = static_cast<std::uint64_t>(key.y);
            return static_cast<std::size_t>((ux * 0x9E3779B97F4A7C15ULL) ^
                                            (uy + 0x632BE59BD9B4E019ULL));
        }
    };
    struct CellRange {
        std::int64_t minX = 0, minY = 0, maxX = -1, maxY = -1;
        [[nodiscard]] std::uint64_t count() const noexcept;
    };
    struct Entry {
        core::DRect bounds;
        CellRange cells;
        bool large = false;
    };

    [[nodiscard]] CellRange cellsOf(const core::DRect& bounds) const noexcept;
    [[nodiscard]] std::int64_t cellCoordinate(double world) const noexcept;
    void unlink(core::ElementId id, const Entry& entry);

    double cellSize_;
    std::size_t largeCellLimit_;
    std::unordered_map<CellKey, std::vector<core::ElementId>, CellKeyHash> cells_;
    std::unordered_map<core::ElementId, Entry> entries_;
    std::vector<core::ElementId> large_;
};

} // namespace studyapp::canvas
