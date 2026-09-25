#include <studyapp/document/Commands.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace studyapp::document::commands {

using core::ErrorCode;
using core::FractionalIndex;
using core::makeError;
using core::Result;

namespace {

template <class Id, class Lookup>
FractionalIndex appendKey(std::span<const Id> siblings, Lookup orderOf) {
    return siblings.empty() ? FractionalIndex::first()
                            : FractionalIndex::after(orderOf(siblings.back()));
}

template <class Id>
core::Error notFound(std::string_view what, const Id& id) {
    return core::Error{ErrorCode::NotFound,
                       std::string(what) + " " + id.toString() + " does not exist"};
}

using ElementSet = std::unordered_set<core::ElementId>;

/// Appends the removal of `doomed` elements (all on `page`). Connectors outside the set
/// that are attached to a doomed element are detached first; doomed connectors are removed
/// before other doomed elements so that no removal violates the attachment invariant.
void appendElementRemovals(const Workspace& ws, core::PageId page, const ElementSet& doomed,
                           std::vector<AnyChange>& out) {
    std::vector<const Element*> doomedConnectors;
    std::vector<const Element*> doomedOthers;
    for (const core::LayerId layer : ws.layersOf(page)) {
        for (const core::ElementId id : ws.elementsOf(layer)) {
            const Element& element = *ws.findElement(id);
            const bool isDoomed = doomed.contains(id);
            const auto* connector = std::get_if<Connector>(&element.payload);
            if (isDoomed) {
                (connector != nullptr ? doomedConnectors : doomedOthers).push_back(&element);
                continue;
            }
            if (connector == nullptr) {
                continue;
            }
            Connector detached = *connector;
            bool changed = false;
            for (ConnectorEnd* end : {&detached.start, &detached.end}) {
                if (end->attachedTo && doomed.contains(*end->attachedTo)) {
                    end->attachedTo.reset(); // keep the cached position
                    changed = true;
                }
            }
            if (changed) {
                Element after = element;
                after.payload = detached;
                out.push_back(updated(element, std::move(after)));
            }
        }
    }
    for (const Element* e : doomedConnectors) {
        out.push_back(removed(*e));
    }
    for (const Element* e : doomedOthers) {
        out.push_back(removed(*e));
    }
}

void appendPageRemoval(const Workspace& ws, const PageInfo& page, std::vector<AnyChange>& out) {
    ElementSet doomed;
    for (const core::LayerId layer : ws.layersOf(page.id)) {
        for (const core::ElementId id : ws.elementsOf(layer)) {
            doomed.insert(id);
        }
    }
    appendElementRemovals(ws, page.id, doomed, out);
    for (const core::LayerId layer : ws.layersOf(page.id)) {
        out.push_back(removed(*ws.findLayer(layer)));
    }
    out.push_back(removed(page));
}

void appendSectionRemoval(const Workspace& ws, const SectionInfo& section,
                          std::vector<AnyChange>& out) {
    for (const core::PageId page : ws.pagesOf(section.id)) {
        appendPageRemoval(ws, *ws.findPage(page), out);
    }
    out.push_back(removed(section));
}

Command makeCommand(std::string label, std::vector<AnyChange> changes) {
    return Command{std::move(label), Patch(std::move(changes))};
}

} // namespace

// ---------------------------------------------------------------------------- notebooks

Result<Created<core::NotebookId>> createNotebook(const Workspace& workspace, std::string title,
                                                 const core::Clock& clock, core::IdGenerator& ids) {
    if (auto check = validateName(title, "notebook title"); !check) {
        return tl::unexpected(check.error());
    }
    const auto now = clock.now();
    NotebookInfo notebook{
        .id = core::NotebookId::generate(ids),
        .title = std::move(title),
        .order = appendKey(workspace.notebooks(),
                           [&](core::NotebookId id) { return workspace.findNotebook(id)->order; }),
        .created = now,
        .modified = now,
    };
    const auto id = notebook.id;
    return Created<core::NotebookId>{
        id, makeCommand("Create notebook", {created(std::move(notebook))})};
}

Result<Command> renameNotebook(const Workspace& workspace, core::NotebookId notebook,
                               std::string title, const core::Clock& clock) {
    const NotebookInfo* current = workspace.findNotebook(notebook);
    if (current == nullptr) {
        return tl::unexpected(notFound("notebook", notebook));
    }
    if (auto check = validateName(title, "notebook title"); !check) {
        return tl::unexpected(check.error());
    }
    NotebookInfo after = *current;
    after.title = std::move(title);
    after.modified = clock.now();
    return makeCommand("Rename notebook", {updated(*current, std::move(after))});
}

Result<Command> deleteNotebook(const Workspace& workspace, core::NotebookId notebook) {
    const NotebookInfo* current = workspace.findNotebook(notebook);
    if (current == nullptr) {
        return tl::unexpected(notFound("notebook", notebook));
    }
    std::vector<AnyChange> changes;
    for (const core::SectionId section : workspace.sectionsOf(notebook)) {
        appendSectionRemoval(workspace, *workspace.findSection(section), changes);
    }
    changes.push_back(removed(*current));
    return makeCommand("Delete notebook", std::move(changes));
}

// ---------------------------------------------------------------------------- sections

Result<Created<core::SectionId>> createSection(const Workspace& workspace,
                                               core::NotebookId notebook, std::string title,
                                               const core::Clock& clock, core::IdGenerator& ids) {
    if (workspace.findNotebook(notebook) == nullptr) {
        return tl::unexpected(notFound("notebook", notebook));
    }
    if (auto check = validateName(title, "section title"); !check) {
        return tl::unexpected(check.error());
    }
    const auto now = clock.now();
    SectionInfo section{
        .id = core::SectionId::generate(ids),
        .notebook = notebook,
        .title = std::move(title),
        .order = appendKey(workspace.sectionsOf(notebook),
                           [&](core::SectionId id) { return workspace.findSection(id)->order; }),
        .created = now,
        .modified = now,
    };
    const auto id = section.id;
    return Created<core::SectionId>{id,
                                    makeCommand("Create section", {created(std::move(section))})};
}

Result<Command> renameSection(const Workspace& workspace, core::SectionId section,
                              std::string title, const core::Clock& clock) {
    const SectionInfo* current = workspace.findSection(section);
    if (current == nullptr) {
        return tl::unexpected(notFound("section", section));
    }
    if (auto check = validateName(title, "section title"); !check) {
        return tl::unexpected(check.error());
    }
    SectionInfo after = *current;
    after.title = std::move(title);
    after.modified = clock.now();
    return makeCommand("Rename section", {updated(*current, std::move(after))});
}

Result<Command> deleteSection(const Workspace& workspace, core::SectionId section) {
    const SectionInfo* current = workspace.findSection(section);
    if (current == nullptr) {
        return tl::unexpected(notFound("section", section));
    }
    std::vector<AnyChange> changes;
    appendSectionRemoval(workspace, *current, changes);
    return makeCommand("Delete section", std::move(changes));
}

// ---------------------------------------------------------------------------- pages

Result<Created<core::PageId>> createPage(const Workspace& workspace, core::SectionId section,
                                         std::string title, const PageOptions& options,
                                         const core::Clock& clock, core::IdGenerator& ids) {
    if (workspace.findSection(section) == nullptr) {
        return tl::unexpected(notFound("section", section));
    }
    if (auto check = validateName(options.firstLayerName, "layer name"); !check) {
        return tl::unexpected(check.error());
    }
    const auto now = clock.now();
    PageInfo page{
        .id = core::PageId::generate(ids),
        .section = section,
        .title = std::move(title),
        .order = appendKey(workspace.pagesOf(section),
                           [&](core::PageId id) { return workspace.findPage(id)->order; }),
        .extent = options.extent,
        .size = options.size,
        .background = options.background,
        .created = now,
        .modified = now,
    };
    Layer layer{
        .id = core::LayerId::generate(ids),
        .page = page.id,
        .name = options.firstLayerName,
        .order = FractionalIndex::first(),
    };
    const auto id = page.id;
    return Created<core::PageId>{
        id, makeCommand("Create page", {created(std::move(page)), created(std::move(layer))})};
}

Result<Command> renamePage(const Workspace& workspace, core::PageId page, std::string title,
                           const core::Clock& clock) {
    const PageInfo* current = workspace.findPage(page);
    if (current == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    PageInfo after = *current;
    after.title = std::move(title); // an empty page title is allowed ("Untitled")
    after.modified = clock.now();
    return makeCommand("Rename page", {updated(*current, std::move(after))});
}

Result<Command> deletePage(const Workspace& workspace, core::PageId page) {
    const PageInfo* current = workspace.findPage(page);
    if (current == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    std::vector<AnyChange> changes;
    appendPageRemoval(workspace, *current, changes);
    return makeCommand("Delete page", std::move(changes));
}

// ---------------------------------------------------------------------------- layers

Result<Created<core::LayerId>> createLayer(const Workspace& workspace, core::PageId page,
                                           std::string name, core::IdGenerator& ids) {
    if (workspace.findPage(page) == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    if (auto check = validateName(name, "layer name"); !check) {
        return tl::unexpected(check.error());
    }
    Layer layer{
        .id = core::LayerId::generate(ids),
        .page = page,
        .name = std::move(name),
        .order = appendKey(workspace.layersOf(page),
                           [&](core::LayerId id) { return workspace.findLayer(id)->order; }),
    };
    const auto id = layer.id;
    return Created<core::LayerId>{id, makeCommand("Create layer", {created(std::move(layer))})};
}

Result<Command> renameLayer(const Workspace& workspace, core::LayerId layer, std::string name) {
    const Layer* current = workspace.findLayer(layer);
    if (current == nullptr) {
        return tl::unexpected(notFound("layer", layer));
    }
    if (auto check = validateName(name, "layer name"); !check) {
        return tl::unexpected(check.error());
    }
    Layer after = *current;
    after.name = std::move(name);
    return makeCommand("Rename layer", {updated(*current, std::move(after))});
}

Result<Command> deleteLayer(const Workspace& workspace, core::LayerId layer) {
    const Layer* current = workspace.findLayer(layer);
    if (current == nullptr) {
        return tl::unexpected(notFound("layer", layer));
    }
    if (workspace.layersOf(current->page).size() <= 1) {
        return makeError(ErrorCode::InvalidArgument, "cannot delete the last layer of a page");
    }
    ElementSet doomed;
    for (const core::ElementId id : workspace.elementsOf(layer)) {
        doomed.insert(id);
    }
    std::vector<AnyChange> changes;
    appendElementRemovals(workspace, current->page, doomed, changes);
    changes.push_back(removed(*current));
    return makeCommand("Delete layer", std::move(changes));
}

// ---------------------------------------------------------------------------- elements

Result<Created<core::ElementId>> createElement(const Workspace& workspace, core::LayerId layer,
                                               NewElement element, core::IdGenerator& ids) {
    if (workspace.findLayer(layer) == nullptr) {
        return tl::unexpected(notFound("layer", layer));
    }
    Element record{
        .id = core::ElementId::generate(ids),
        .layer = layer,
        .z = appendKey(workspace.elementsOf(layer),
                       [&](core::ElementId id) { return workspace.findElement(id)->z; }),
        .transform = element.transform,
        .locked = element.locked,
        .payload = std::move(element.payload),
    };
    const auto id = record.id;
    const std::string label = "Create " + std::string(toString(record.kind()));
    return Created<core::ElementId>{id, makeCommand(label, {created(std::move(record))})};
}

Result<Command> setPageFormat(const Workspace& workspace, core::PageId page,
                              const PageFormat& format, const core::Clock& clock) {
    const PageInfo* current = workspace.findPage(page);
    if (current == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    PageInfo after = *current;
    after.extent = format.extent;
    after.size = format.size;
    after.background = format.background;
    if (after == *current) {
        return Command{"Change page format", Patch{}};
    }
    after.modified = clock.now();
    return makeCommand("Change page format", {updated(*current, std::move(after))});
}

Result<Command> deleteElement(const Workspace& workspace, core::ElementId element) {
    const Element* current = workspace.findElement(element);
    if (current == nullptr) {
        return tl::unexpected(notFound("element", element));
    }
    std::vector<AnyChange> changes;
    appendElementRemovals(workspace, *workspace.pageOf(element), ElementSet{element}, changes);
    return makeCommand("Delete " + std::string(toString(current->kind())), std::move(changes));
}

namespace {

/// Validates `ids` and returns them deduplicated and sorted (deterministic patches).
Result<std::vector<core::ElementId>> existingElements(const Workspace& workspace,
                                                      std::span<const core::ElementId> ids) {
    if (ids.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no elements given");
    }
    std::vector<core::ElementId> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (const core::ElementId id : sorted) {
        if (workspace.findElement(id) == nullptr) {
            return tl::unexpected(notFound("element", id));
        }
    }
    return sorted;
}

std::string elementsLabel(std::string_view verb, std::size_t count) {
    return std::string(verb) +
           (count == 1 ? std::string(" element") : " " + std::to_string(count) + " elements");
}

/// Pages containing `ids`, in id order.
std::vector<core::PageId> pagesOf(const Workspace& workspace,
                                  std::span<const core::ElementId> ids) {
    std::vector<core::PageId> pages;
    for (const core::ElementId id : ids) {
        pages.push_back(*workspace.pageOf(id));
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

} // namespace

Result<Command> deleteElements(const Workspace& workspace,
                               std::span<const core::ElementId> elements) {
    auto ids = existingElements(workspace, elements);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    std::vector<AnyChange> changes;
    for (const core::PageId page : pagesOf(workspace, *ids)) {
        ElementSet doomed;
        for (const core::ElementId id : *ids) {
            if (*workspace.pageOf(id) == page) {
                doomed.insert(id);
            }
        }
        appendElementRemovals(workspace, page, doomed, changes);
    }
    return makeCommand(elementsLabel("Delete", ids->size()), std::move(changes));
}

Result<Command> moveElements(const Workspace& workspace, std::span<const core::ElementId> elements,
                             core::DVec2 worldDelta) {
    auto ids = existingElements(workspace, elements);
    if (!ids) {
        return tl::unexpected(ids.error());
    }
    if (!std::isfinite(worldDelta.x) || !std::isfinite(worldDelta.y)) {
        return makeError(ErrorCode::InvalidArgument, "move delta must be finite");
    }
    const std::string label = elementsLabel("Move", ids->size());
    if (worldDelta.x == 0.0 && worldDelta.y == 0.0) {
        return Command{label, Patch{}};
    }
    const ElementSet moved(ids->begin(), ids->end());
    // An end follows if it is attached to a moved element, or if it is a free end of a
    // connector that is itself being moved.
    const auto follow = [&](ConnectorEnd& end, bool connectorMoves) {
        const bool attachedToMoved = end.attachedTo && moved.contains(*end.attachedTo);
        if (attachedToMoved || (connectorMoves && !end.attachedTo)) {
            end.position += worldDelta;
            return true;
        }
        return false;
    };

    std::vector<AnyChange> changes;
    for (const core::ElementId id : *ids) {
        const Element& element = *workspace.findElement(id);
        Element after = element;
        if (auto* connector = std::get_if<Connector>(&after.payload)) {
            follow(connector->start, true);
            follow(connector->end, true);
        } else {
            after.transform.position += worldDelta;
        }
        changes.push_back(updated(element, std::move(after)));
    }
    for (const core::PageId page : pagesOf(workspace, *ids)) {
        for (const core::LayerId layer : workspace.layersOf(page)) {
            for (const core::ElementId id : workspace.elementsOf(layer)) {
                const Element& element = *workspace.findElement(id);
                if (moved.contains(id) || !std::holds_alternative<Connector>(element.payload)) {
                    continue;
                }
                Element after = element;
                auto& connector = std::get<Connector>(after.payload);
                const bool startFollows = follow(connector.start, false);
                const bool endFollows = follow(connector.end, false);
                if (startFollows || endFollows) {
                    changes.push_back(updated(element, std::move(after)));
                }
            }
        }
    }
    return makeCommand(label, std::move(changes));
}

} // namespace studyapp::document::commands
