#pragma once

#include <studyapp/core/Ids.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace studyapp::canvas {

/// Selected elements, by stable document id (docs/CANVAS.md §7). Interaction state only:
/// never persisted, never a copy of the records. Kept sorted so iteration (and the
/// commands built from it) is deterministic.
class Selection {
public:
    [[nodiscard]] std::span<const core::ElementId> ids() const noexcept { return ids_; }
    [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
    [[nodiscard]] bool contains(core::ElementId id) const noexcept {
        return std::binary_search(ids_.begin(), ids_.end(), id);
    }

    void clear() noexcept { ids_.clear(); }
    void set(std::vector<core::ElementId> ids) {
        ids_ = std::move(ids);
        normalise();
    }
    void add(core::ElementId id) {
        if (!contains(id)) {
            ids_.insert(std::lower_bound(ids_.begin(), ids_.end(), id), id);
        }
    }
    void toggle(core::ElementId id) {
        const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
        if (it != ids_.end() && *it == id) {
            ids_.erase(it);
        } else {
            ids_.insert(it, id);
        }
    }
    /// Drops ids for which `exists` returns false (after a patch removed elements).
    template <class Predicate>
    void retainIf(Predicate exists) {
        ids_.erase(std::remove_if(ids_.begin(), ids_.end(),
                                  [&](core::ElementId id) { return !exists(id); }),
                   ids_.end());
    }

private:
    void normalise() {
        std::sort(ids_.begin(), ids_.end());
        ids_.erase(std::unique(ids_.begin(), ids_.end()), ids_.end());
    }

    std::vector<core::ElementId> ids_;
};

} // namespace studyapp::canvas
