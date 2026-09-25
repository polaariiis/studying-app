#include <studyapp/canvas/SpatialGrid.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace studyapp::canvas {

namespace {

bool isUsable(const core::DRect& r) noexcept {
    return !r.isEmpty() && std::isfinite(r.min.x) && std::isfinite(r.min.y) &&
           std::isfinite(r.max.x) && std::isfinite(r.max.y);
}

void eraseId(std::vector<core::ElementId>& ids, core::ElementId id) {
    const auto it = std::find(ids.begin(), ids.end(), id);
    if (it != ids.end()) {
        *it = ids.back();
        ids.pop_back();
    }
}

} // namespace

std::uint64_t SpatialGrid::CellRange::count() const noexcept {
    if (maxX < minX || maxY < minY) {
        return 0;
    }
    const auto w = static_cast<std::uint64_t>(maxX - minX) + 1;
    const auto h = static_cast<std::uint64_t>(maxY - minY) + 1;
    return w > std::numeric_limits<std::uint64_t>::max() / h
               ? std::numeric_limits<std::uint64_t>::max()
               : w * h;
}

SpatialGrid::SpatialGrid(double cellSize, std::size_t largeCellLimit)
    : cellSize_(cellSize > 0.0 && std::isfinite(cellSize) ? cellSize : 512.0),
      largeCellLimit_(std::max<std::size_t>(largeCellLimit, 1)) {}

std::int64_t SpatialGrid::cellCoordinate(double world) const noexcept {
    // Clamp far away coordinates instead of overflowing the integer cell index.
    constexpr double kLimit = 4.0e15;
    return static_cast<std::int64_t>(std::floor(std::clamp(world / cellSize_, -kLimit, kLimit)));
}

SpatialGrid::CellRange SpatialGrid::cellsOf(const core::DRect& bounds) const noexcept {
    return {cellCoordinate(bounds.min.x), cellCoordinate(bounds.min.y),
            cellCoordinate(bounds.max.x), cellCoordinate(bounds.max.y)};
}

void SpatialGrid::unlink(core::ElementId id, const Entry& entry) {
    if (entry.large) {
        eraseId(large_, id);
        return;
    }
    for (std::int64_t y = entry.cells.minY; y <= entry.cells.maxY; ++y) {
        for (std::int64_t x = entry.cells.minX; x <= entry.cells.maxX; ++x) {
            const auto it = cells_.find({x, y});
            if (it == cells_.end()) {
                continue;
            }
            eraseId(it->second, id);
            if (it->second.empty()) {
                cells_.erase(it);
            }
        }
    }
}

void SpatialGrid::upsert(core::ElementId id, const core::DRect& bounds) {
    if (!isUsable(bounds)) {
        remove(id);
        return;
    }
    const CellRange cells = cellsOf(bounds);
    const bool large = cells.count() > largeCellLimit_;
    if (const auto it = entries_.find(id); it != entries_.end()) {
        Entry& entry = it->second;
        const bool sameCells =
            entry.large == large &&
            (large || (entry.cells.minX == cells.minX && entry.cells.minY == cells.minY &&
                       entry.cells.maxX == cells.maxX && entry.cells.maxY == cells.maxY));
        if (sameCells) {
            entry.bounds = bounds; // e.g. a small move within the same cells
            return;
        }
        unlink(id, entry);
        entries_.erase(it);
    }
    entries_.emplace(id, Entry{.bounds = bounds, .cells = cells, .large = large});
    if (large) {
        large_.push_back(id);
        return;
    }
    for (std::int64_t y = cells.minY; y <= cells.maxY; ++y) {
        for (std::int64_t x = cells.minX; x <= cells.maxX; ++x) {
            cells_[{x, y}].push_back(id);
        }
    }
}

void SpatialGrid::remove(core::ElementId id) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return;
    }
    unlink(id, it->second);
    entries_.erase(it);
}

void SpatialGrid::clear() noexcept {
    cells_.clear();
    entries_.clear();
    large_.clear();
}

void SpatialGrid::query(const core::DRect& rect, std::vector<core::ElementId>& out) const {
    if (!isUsable(rect)) { // a zero-area rectangle (a point) is usable; an empty one is not
        return;
    }
    const std::size_t start = out.size();
    const auto accept = [&](core::ElementId id) {
        if (entries_.at(id).bounds.intersects(rect)) {
            out.push_back(id);
        }
    };
    const CellRange cells = cellsOf(rect);
    // A huge query (zoomed far out) touches more cells than exist: scan entries instead.
    if (cells.count() > cells_.size()) {
        for (const auto& [id, entry] : entries_) {
            if (!entry.large && entry.bounds.intersects(rect)) {
                out.push_back(id);
            }
        }
    } else {
        for (std::int64_t y = cells.minY; y <= cells.maxY; ++y) {
            for (std::int64_t x = cells.minX; x <= cells.maxX; ++x) {
                if (const auto it = cells_.find({x, y}); it != cells_.end()) {
                    for (const core::ElementId id : it->second) {
                        accept(id);
                    }
                }
            }
        }
    }
    for (const core::ElementId id : large_) {
        accept(id);
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
    out.erase(std::unique(out.begin() + static_cast<std::ptrdiff_t>(start), out.end()), out.end());
}

} // namespace studyapp::canvas
