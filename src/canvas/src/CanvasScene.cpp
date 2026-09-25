#include <studyapp/canvas/CanvasScene.hpp>

#include <algorithm>
#include <type_traits>
#include <variant>

namespace studyapp::canvas {

void CanvasScene::rebuild(const document::Workspace& workspace, std::optional<core::PageId> page) {
    for (const auto& [id, entry] : entries_) {
        removed_.push_back(id);
    }
    entries_.clear();
    grid_.clear();
    order_.clear();
    ++generation_;
    page_ = page && workspace.findPage(*page) != nullptr ? page : std::nullopt;
    if (!page_) {
        return;
    }
    for (const core::LayerId layer : workspace.layersOf(*page_)) {
        for (const core::ElementId id : workspace.elementsOf(layer)) {
            upsert(workspace, id, true);
        }
    }
    rebuildOrder(workspace);
}

void CanvasScene::upsert(const document::Workspace& workspace, core::ElementId id,
                         bool contentChanged) {
    const document::Element* element = workspace.findElement(id);
    if (element == nullptr) {
        removeEntry(id);
        return;
    }
    const core::DRect bounds = document::worldBounds(*element);
    auto [it, inserted] = entries_.try_emplace(id);
    SceneEntry& entry = it->second;
    entry.id = id;
    entry.layer = element->layer;
    entry.bounds = bounds;
    if (inserted || contentChanged) {
        entry.contentVersion = nextVersion_++;
    }
    grid_.upsert(id, bounds);
}

void CanvasScene::removeEntry(core::ElementId id) {
    if (entries_.erase(id) > 0) {
        grid_.remove(id);
        removed_.push_back(id);
    }
}

void CanvasScene::rebuildOrder(const document::Workspace& workspace) {
    order_.clear();
    order_.reserve(entries_.size());
    if (!page_) {
        return;
    }
    for (const core::LayerId layerId : workspace.layersOf(*page_)) {
        const document::Layer* layer = workspace.findLayer(layerId);
        for (const core::ElementId id : workspace.elementsOf(layerId)) {
            const auto it = entries_.find(id);
            if (it == entries_.end()) {
                continue;
            }
            it->second.drawIndex = order_.size();
            it->second.layerVisible = layer->visible;
            it->second.layerLocked = layer->locked;
            it->second.layerOpacity = layer->opacity;
            order_.push_back(id);
        }
    }
}

void CanvasScene::onPatch(const document::Workspace& workspace, const document::Patch& patch) {
    if (!page_) {
        return;
    }
    if (workspace.findPage(*page_) == nullptr) {
        rebuild(workspace, std::nullopt); // the page itself was removed
        return;
    }
    bool orderDirty = false;
    for (const document::AnyChange& change : patch.changes()) {
        std::visit(
            [&](const auto& c) {
                using Change = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<Change, document::ElementChange>) {
                    const core::ElementId id = c.after ? c.after->id : c.before->id;
                    const bool onPage = workspace.pageOf(id) == page_;
                    if (onPage) {
                        const bool contentChanged =
                            !c.before || !c.after || !(c.before->payload == c.after->payload);
                        upsert(workspace, id, contentChanged);
                        orderDirty = true;
                    } else if (entries_.contains(id)) {
                        removeEntry(id);
                        orderDirty = true;
                    }
                } else if constexpr (std::is_same_v<Change, document::LayerChange>) {
                    const bool touchesPage = (c.before && c.before->page == *page_) ||
                                             (c.after && c.after->page == *page_);
                    if (touchesPage) {
                        orderDirty = true;
                        // A layer moved to this page brings its elements along.
                        if (c.after && c.after->page == *page_ &&
                            (!c.before || c.before->page != *page_)) {
                            for (const core::ElementId id : workspace.elementsOf(c.after->id)) {
                                upsert(workspace, id, true);
                            }
                        }
                        if (c.before && c.before->page == *page_ &&
                            (!c.after || c.after->page != *page_)) {
                            std::vector<core::ElementId> leaving;
                            for (const auto& [id, entry] : entries_) {
                                if (entry.layer == c.before->id) {
                                    leaving.push_back(id);
                                }
                            }
                            for (const core::ElementId id : leaving) {
                                removeEntry(id);
                            }
                        }
                    }
                }
            },
            change);
    }
    if (orderDirty) {
        rebuildOrder(workspace);
        ++generation_;
    }
}

const SceneEntry* CanvasScene::find(core::ElementId id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

void CanvasScene::queryOrdered(const core::DRect& rect, std::vector<OrderedId>& out) const {
    std::vector<core::ElementId> candidates;
    grid_.query(rect, candidates);
    const std::size_t start = out.size();
    for (const core::ElementId id : candidates) {
        const SceneEntry& entry = entries_.at(id); // one lookup per element
        if (entry.layerVisible) {
            out.push_back({entry.drawIndex, id});
        }
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(start), out.end(),
              [](const OrderedId& a, const OrderedId& b) { return a.drawIndex < b.drawIndex; });
}

void CanvasScene::query(const core::DRect& rect, std::vector<core::ElementId>& out) const {
    std::vector<OrderedId> ordered;
    queryOrdered(rect, ordered);
    out.reserve(out.size() + ordered.size());
    for (const OrderedId& item : ordered) {
        out.push_back(item.id);
    }
}

std::vector<core::ElementId> CanvasScene::takeRemoved() {
    std::vector<core::ElementId> removed;
    removed.swap(removed_);
    return removed;
}

} // namespace studyapp::canvas
