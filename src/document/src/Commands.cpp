#include <studyapp/document/Commands.hpp>

#include "CommandSupport.hpp"

#include <studyapp/document/StudyCommands.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace studyapp::document::commands {

using core::ErrorCode;
using core::FractionalIndex;
using core::makeError;
using core::Result;

using detail::appendKey;
using detail::keyAt;
using detail::makeCommand;
using detail::notFound;

namespace {

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

/// Pages of a section, for unlinking tasks before the pages go.
void collectPages(const Workspace& ws, core::SectionId section, std::vector<core::PageId>& out) {
    const auto pages = ws.pagesOf(section);
    out.insert(out.end(), pages.begin(), pages.end());
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
    if (current->title == title) {
        return Command{"Rename notebook", Patch{}}; // unchanged: nothing to record
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
    std::vector<core::PageId> pages;
    for (const core::SectionId section : workspace.sectionsOf(notebook)) {
        collectPages(workspace, section, pages);
    }
    std::vector<AnyChange> changes;
    appendTaskUnlinks(workspace, pages, changes);
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
    if (current->title == title) {
        return Command{"Rename section", Patch{}};
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
    std::vector<core::PageId> pages;
    collectPages(workspace, section, pages);
    std::vector<AnyChange> changes;
    appendTaskUnlinks(workspace, pages, changes);
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
    if (current->title == title) {
        return Command{"Rename page", Patch{}};
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
    appendTaskUnlinks(workspace, std::span(&page, 1), changes);
    appendPageRemoval(workspace, *current, changes);
    return makeCommand("Delete page", std::move(changes));
}

Result<Created<core::SectionId>>
createDocumentSection(const Workspace& workspace, core::NotebookId notebook, std::string title,
                      core::AssetId asset, std::span<const core::DVec2> pageSizes,
                      const core::Clock& clock, core::IdGenerator& ids) {
    if (workspace.findNotebook(notebook) == nullptr) {
        return tl::unexpected(notFound("notebook", notebook));
    }
    if (auto check = validateName(title, "section title"); !check) {
        return tl::unexpected(check.error());
    }
    if (pageSizes.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the document has no pages");
    }
    const auto now = clock.now();
    SectionInfo section{
        .id = core::SectionId::generate(ids),
        .notebook = notebook,
        .title = title,
        .order = appendKey(workspace.sectionsOf(notebook),
                           [&](core::SectionId id) { return workspace.findSection(id)->order; }),
        .created = now,
        .modified = now,
    };
    std::vector<AnyChange> changes;
    changes.reserve(1 + 2 * pageSizes.size());
    changes.push_back(created(section));
    std::optional<FractionalIndex> order;
    for (std::size_t i = 0; i < pageSizes.size(); ++i) {
        const core::DVec2 size = pageSizes[i];
        if (!(std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0 && size.y > 0.0)) {
            return makeError(ErrorCode::InvalidArgument, "a document page has no valid size");
        }
        order = order ? FractionalIndex::after(*order) : FractionalIndex::first();
        PageInfo page{
            .id = core::PageId::generate(ids),
            .section = section.id,
            .title = title + " · p. " + std::to_string(i + 1),
            .order = *order,
            .extent = PageExtent::Bounded,
            .size = size,
            .document = PageDocument{.asset = asset, .index = static_cast<std::int32_t>(i)},
            .created = now,
            .modified = now,
        };
        Layer layer{.id = core::LayerId::generate(ids),
                    .page = page.id,
                    .name = "Layer 1",
                    .order = FractionalIndex::first()};
        changes.push_back(created(std::move(page)));
        changes.push_back(created(std::move(layer)));
    }
    const auto id = section.id;
    return Created<core::SectionId>{id, makeCommand("Import PDF", std::move(changes))};
}

// ---------------------------------------------------------------------------- import

Result<Created<std::vector<core::NotebookId>>>
importNotebooks(const Workspace& workspace, const Workspace& source,
                std::span<const core::NotebookId> notebooks,
                const std::unordered_map<core::AssetId, core::AssetId>& assets,
                core::IdGenerator& ids) {
    if (notebooks.empty()) {
        return makeError(ErrorCode::InvalidArgument, "there is no notebook to import");
    }
    std::unordered_set<core::NotebookId> listed;
    for (const core::NotebookId notebook : notebooks) {
        if (source.findNotebook(notebook) == nullptr) {
            return tl::unexpected(notFound("notebook", notebook));
        }
        if (!listed.insert(notebook).second) {
            return makeError(ErrorCode::InvalidArgument, "a notebook is listed twice");
        }
    }
    // Conflict detection: one id already in use means every copied record gets a new one.
    bool remap = false;
    for (const core::NotebookId notebook : notebooks) {
        remap = remap || workspace.findNotebook(notebook) != nullptr;
        for (const core::SectionId section : source.sectionsOf(notebook)) {
            remap = remap || workspace.findSection(section) != nullptr;
            for (const core::PageId page : source.pagesOf(section)) {
                remap = remap || workspace.findPage(page) != nullptr;
                for (const core::LayerId layer : source.layersOf(page)) {
                    remap = remap || workspace.findLayer(layer) != nullptr;
                    for (const core::ElementId element : source.elementsOf(layer)) {
                        remap = remap || workspace.findElement(element) != nullptr;
                    }
                }
            }
        }
    }
    const auto idFor = [&](auto id) {
        using Id = decltype(id);
        return remap ? Id::generate(ids) : id;
    };
    const auto assetFor = [&](core::AssetId asset) -> Result<core::AssetId> {
        const auto it = assets.find(asset);
        if (it == assets.end()) {
            return makeError(ErrorCode::NotFound, "asset " + asset.toString() + " is missing");
        }
        return it->second;
    };

    std::vector<AnyChange> tagChanges; // before the pages that use them
    std::vector<AnyChange> changes;
    std::unordered_map<core::TagId, core::TagId> tagIds;
    std::unordered_map<core::ElementId, core::ElementId> elementIds;
    std::vector<core::NotebookId> created;
    std::optional<FractionalIndex> order;
    if (const auto existing = workspace.notebooks(); !existing.empty()) {
        order = workspace.findNotebook(existing.back())->order;
    }
    for (const core::NotebookId notebookId : notebooks) {
        NotebookInfo notebook = *source.findNotebook(notebookId);
        notebook.id = idFor(notebook.id);
        order = order ? FractionalIndex::after(*order) : FractionalIndex::first();
        notebook.order = *order;
        created.push_back(notebook.id);
        const core::NotebookId newNotebook = notebook.id;
        changes.push_back(document::created(std::move(notebook)));
        for (const core::SectionId sectionId : source.sectionsOf(notebookId)) {
            SectionInfo section = *source.findSection(sectionId);
            section.id = idFor(section.id);
            section.notebook = newNotebook;
            const core::SectionId newSection = section.id;
            changes.push_back(document::created(std::move(section)));
            for (const core::PageId pageId : source.pagesOf(sectionId)) {
                PageInfo page = *source.findPage(pageId);
                page.id = idFor(page.id);
                page.section = newSection;
                if (page.document) {
                    auto asset = assetFor(page.document->asset);
                    if (!asset) {
                        return tl::unexpected(asset.error());
                    }
                    page.document->asset = *asset;
                }
                for (core::TagId& tag : page.tags) {
                    auto [it, inserted] = tagIds.try_emplace(tag);
                    if (inserted) {
                        const study::Tag& from = *source.findTag(tag);
                        if (const study::Tag* existing = workspace.findTagByName(from.name)) {
                            it->second = existing->id;
                        } else {
                            study::Tag copy = from;
                            if (workspace.findTag(copy.id) != nullptr) {
                                copy.id = core::TagId::generate(ids);
                            }
                            it->second = copy.id;
                            tagChanges.push_back(document::created(std::move(copy)));
                        }
                    }
                    tag = it->second;
                }
                std::sort(page.tags.begin(), page.tags.end());
                const core::PageId newPage = page.id;
                changes.push_back(document::created(std::move(page)));
                for (const core::LayerId layerId : source.layersOf(pageId)) {
                    Layer layer = *source.findLayer(layerId);
                    layer.id = idFor(layer.id);
                    layer.page = newPage;
                    const core::LayerId newLayer = layer.id;
                    changes.push_back(document::created(std::move(layer)));
                    for (const core::ElementId elementId : source.elementsOf(layerId)) {
                        Element element = *source.findElement(elementId);
                        element.id = idFor(element.id);
                        element.layer = newLayer;
                        if (remap) {
                            elementIds.emplace(elementId, element.id);
                        }
                        if (auto* image = std::get_if<Image>(&element.payload)) {
                            auto asset = assetFor(image->asset);
                            if (!asset) {
                                return tl::unexpected(asset.error());
                            }
                            image->asset = *asset;
                        }
                        changes.push_back(document::created(std::move(element)));
                    }
                }
            }
        }
    }
    // Connector ends attach to elements of the same page, all copied above.
    if (remap) {
        for (AnyChange& change : changes) {
            auto* elementChange = std::get_if<ElementChange>(&change);
            if (elementChange == nullptr) {
                continue;
            }
            if (auto* connector = std::get_if<Connector>(&elementChange->after->payload)) {
                for (ConnectorEnd* end : {&connector->start, &connector->end}) {
                    if (end->attachedTo) {
                        end->attachedTo = elementIds.at(*end->attachedTo);
                    }
                }
            }
        }
    }
    tagChanges.insert(tagChanges.end(), std::make_move_iterator(changes.begin()),
                      std::make_move_iterator(changes.end()));
    return Created<std::vector<core::NotebookId>>{
        std::move(created),
        makeCommand(notebooks.size() == 1 ? "Import notebook" : "Import notebooks",
                    std::move(tagChanges))};
}

// ---------------------------------------------------------------------------- ordering

Result<Command> moveNotebook(const Workspace& workspace, core::NotebookId notebook,
                             std::size_t index, const core::Clock& clock) {
    const NotebookInfo* current = workspace.findNotebook(notebook);
    if (current == nullptr) {
        return tl::unexpected(notFound("notebook", notebook));
    }
    auto key = keyAt(workspace.notebooks(), notebook, index,
                     [&](core::NotebookId id) { return workspace.findNotebook(id)->order; });
    if (!key) {
        return tl::unexpected(key.error());
    }
    if (!*key) {
        return Command{"Move notebook", Patch{}};
    }
    NotebookInfo after = *current;
    after.order = std::move(**key);
    after.modified = clock.now();
    return makeCommand("Move notebook", {updated(*current, std::move(after))});
}

Result<Command> moveSection(const Workspace& workspace, core::SectionId section,
                            core::NotebookId destination, std::size_t index,
                            const core::Clock& clock) {
    const SectionInfo* current = workspace.findSection(section);
    if (current == nullptr) {
        return tl::unexpected(notFound("section", section));
    }
    if (workspace.findNotebook(destination) == nullptr) {
        return tl::unexpected(notFound("notebook", destination));
    }
    auto key = keyAt(workspace.sectionsOf(destination), section, index,
                     [&](core::SectionId id) { return workspace.findSection(id)->order; });
    if (!key) {
        return tl::unexpected(key.error());
    }
    if (!*key) {
        return Command{"Move section", Patch{}};
    }
    SectionInfo after = *current;
    after.notebook = destination;
    after.order = std::move(**key);
    after.modified = clock.now();
    return makeCommand("Move section", {updated(*current, std::move(after))});
}

Result<Command> movePage(const Workspace& workspace, core::PageId page, core::SectionId destination,
                         std::size_t index, const core::Clock& clock) {
    const PageInfo* current = workspace.findPage(page);
    if (current == nullptr) {
        return tl::unexpected(notFound("page", page));
    }
    if (workspace.findSection(destination) == nullptr) {
        return tl::unexpected(notFound("section", destination));
    }
    auto key = keyAt(workspace.pagesOf(destination), page, index,
                     [&](core::PageId id) { return workspace.findPage(id)->order; });
    if (!key) {
        return tl::unexpected(key.error());
    }
    if (!*key) {
        return Command{"Move page", Patch{}};
    }
    PageInfo after = *current;
    after.section = destination;
    after.order = std::move(**key);
    after.modified = clock.now();
    return makeCommand("Move page", {updated(*current, std::move(after))});
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
    // Connectors attached to moved elements, from the workspace's attachment index: only
    // those are touched (not a scan of the page). Sorted: the patch is deterministic.
    std::vector<core::ElementId> attached;
    for (const core::ElementId id : *ids) {
        for (const core::ElementId connector : workspace.connectorsAttachedTo(id)) {
            if (!moved.contains(connector)) {
                attached.push_back(connector);
            }
        }
    }
    std::sort(attached.begin(), attached.end());
    attached.erase(std::unique(attached.begin(), attached.end()), attached.end());
    for (const core::ElementId id : attached) {
        const Element& element = *workspace.findElement(id);
        Element after = element;
        auto& connector = std::get<Connector>(after.payload);
        const bool startFollows = follow(connector.start, false);
        const bool endFollows = follow(connector.end, false);
        if (startFollows || endFollows) {
            changes.push_back(updated(element, std::move(after)));
        }
    }
    return makeCommand(label, std::move(changes));
}

Result<Command> splitStrokes(const Workspace& workspace, std::span<const StrokePieces> strokes,
                             core::IdGenerator& ids) {
    if (strokes.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no strokes given");
    }
    // The key of each stroke's next sibling bounds the keys of its extra pieces. It comes
    // from one pass over each affected layer: O(layer elements + strokes), not a search per
    // stroke.
    std::unordered_map<core::ElementId, std::optional<FractionalIndex>> nextKey;
    nextKey.reserve(strokes.size());
    std::vector<core::LayerId> layers;
    for (const StrokePieces& split : strokes) {
        const Element* element = workspace.findElement(split.stroke);
        if (element == nullptr) {
            return tl::unexpected(notFound("element", split.stroke));
        }
        if (!std::holds_alternative<Stroke>(element->payload)) {
            return makeError(ErrorCode::InvalidArgument,
                             "element " + split.stroke.toString() + " is not a stroke");
        }
        if (!nextKey.try_emplace(split.stroke).second) {
            return makeError(ErrorCode::InvalidArgument,
                             "stroke " + split.stroke.toString() + " is given twice");
        }
        for (const StrokePoints& piece : split.pieces) {
            if (!piece || piece->empty()) {
                return makeError(ErrorCode::InvalidArgument, "a stroke piece has no points");
            }
        }
        layers.push_back(element->layer);
    }
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
    for (const core::LayerId layer : layers) {
        const auto order = workspace.elementsOf(layer);
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (const auto it = nextKey.find(order[i]); it != nextKey.end()) {
                it->second = workspace.findElement(order[i + 1])->z;
            }
        }
    }

    std::vector<AnyChange> changes;
    std::vector<core::ElementId> erased;
    for (const StrokePieces& split : strokes) {
        const Element& element = *workspace.findElement(split.stroke);
        if (split.pieces.empty()) {
            erased.push_back(split.stroke);
            continue;
        }
        const auto& stroke = std::get<Stroke>(element.payload);
        const auto pieceOf = [&](const StrokePoints& points) {
            return Stroke{.brush = stroke.brush,
                          .color = stroke.color,
                          .baseWidth = stroke.baseWidth,
                          .points = points};
        };
        Element first = element;
        first.payload = pieceOf(split.pieces.front());
        changes.push_back(updated(element, std::move(first)));
        FractionalIndex lower = element.z;
        const std::optional<FractionalIndex>& upper = nextKey.at(split.stroke);
        for (std::size_t k = 1; k < split.pieces.size(); ++k) {
            auto key = FractionalIndex::between(lower, upper);
            if (!key) {
                return tl::unexpected(key.error());
            }
            lower = *key;
            changes.push_back(created(Element{.id = core::ElementId::generate(ids),
                                              .layer = element.layer,
                                              .z = std::move(*key),
                                              .transform = element.transform,
                                              .locked = element.locked,
                                              .payload = pieceOf(split.pieces[k])}));
        }
    }
    if (!erased.empty()) {
        for (const core::PageId page : pagesOf(workspace, erased)) {
            ElementSet doomed;
            for (const core::ElementId id : erased) {
                if (*workspace.pageOf(id) == page) {
                    doomed.insert(id);
                }
            }
            appendElementRemovals(workspace, page, doomed, changes);
        }
    }
    return makeCommand("Erase", std::move(changes));
}

Result<Command> editText(const Workspace& workspace, core::ElementId element, std::string text,
                         core::Vec2 size) {
    const Element* current = workspace.findElement(element);
    if (current == nullptr) {
        return tl::unexpected(notFound("element", element));
    }
    const auto* box = std::get_if<TextBox>(&current->payload);
    if (box == nullptr) {
        return makeError(ErrorCode::InvalidArgument,
                         "element " + element.toString() + " is not a text box");
    }
    if (box->text == text && box->size == size) {
        return Command{"Edit text", Patch{}};
    }
    Element after = *current;
    after.payload = TextBox{.size = size, .text = std::move(text)};
    return makeCommand("Edit text", {updated(*current, std::move(after))});
}

Result<Command> setConnectorEnds(const Workspace& workspace, core::ElementId connector,
                                 ConnectorEnd start, ConnectorEnd end) {
    const Element* current = workspace.findElement(connector);
    if (current == nullptr) {
        return tl::unexpected(notFound("element", connector));
    }
    const auto* before = std::get_if<Connector>(&current->payload);
    if (before == nullptr) {
        return makeError(ErrorCode::InvalidArgument,
                         "element " + connector.toString() + " is not a connector");
    }
    for (const ConnectorEnd* e : {&start, &end}) {
        if (!std::isfinite(e->position.x) || !std::isfinite(e->position.y)) {
            return makeError(ErrorCode::InvalidArgument, "connector ends must be finite");
        }
    }
    if (before->start == start && before->end == end) {
        return Command{"Edit connector", Patch{}};
    }
    Element after = *current;
    auto& payload = std::get<Connector>(after.payload);
    payload.start = start;
    payload.end = end;
    return makeCommand("Edit connector", {updated(*current, std::move(after))});
}

Result<Created<std::vector<core::ElementId>>>
pasteElements(const Workspace& workspace, core::LayerId layer, std::span<const Element> elements,
              core::DVec2 offset, core::IdGenerator& ids) {
    if (workspace.findLayer(layer) == nullptr) {
        return tl::unexpected(notFound("layer", layer));
    }
    if (elements.empty()) {
        return makeError(ErrorCode::InvalidArgument, "nothing to paste");
    }
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
        return makeError(ErrorCode::InvalidArgument, "a paste needs a finite offset");
    }
    // Old id -> copy id, for re-attaching connectors among the copies (temporary, O(n)).
    std::unordered_map<core::ElementId, core::ElementId> copyOf;
    copyOf.reserve(elements.size());
    std::vector<core::ElementId> pasted;
    pasted.reserve(elements.size());
    for (const Element& element : elements) {
        const core::ElementId id = core::ElementId::generate(ids);
        if (!copyOf.try_emplace(element.id, id).second) {
            return makeError(ErrorCode::InvalidArgument,
                             "element " + element.id.toString() + " is pasted twice");
        }
        pasted.push_back(id);
    }
    const auto siblings = workspace.elementsOf(layer);
    std::optional<FractionalIndex> lower;
    if (!siblings.empty()) {
        lower = workspace.findElement(siblings.back())->z;
    }
    // Connectors are created after everything else: a connector may lie below the element it
    // is attached to, and the workspace checks each change against those before it.
    std::vector<AnyChange> changes;
    changes.reserve(elements.size());
    std::vector<AnyChange> connectors;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        Element copy = elements[i];
        copy.id = pasted[i];
        copy.layer = layer;
        copy.z = lower ? FractionalIndex::after(*lower) : FractionalIndex::first();
        lower = copy.z;
        if (auto* connector = std::get_if<Connector>(&copy.payload)) {
            for (ConnectorEnd* end : {&connector->start, &connector->end}) {
                end->position += offset;
                if (end->attachedTo) {
                    const auto it = copyOf.find(*end->attachedTo);
                    end->attachedTo = it != copyOf.end() ? std::optional{it->second} : std::nullopt;
                }
            }
            connectors.push_back(created(std::move(copy)));
        } else {
            copy.transform.position += offset;
            changes.push_back(created(std::move(copy)));
        }
    }
    std::move(connectors.begin(), connectors.end(), std::back_inserter(changes));
    return Created<std::vector<core::ElementId>>{std::move(pasted),
                                                 makeCommand("Paste", std::move(changes))};
}

Result<Command> resizeElement(const Workspace& workspace, core::ElementId element,
                              Transform transform, core::Vec2 size) {
    const Element* current = workspace.findElement(element);
    if (current == nullptr) {
        return tl::unexpected(notFound("element", element));
    }
    if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 0.0F || size.y < 0.0F ||
        !std::isfinite(transform.position.x) || !std::isfinite(transform.position.y)) {
        return makeError(ErrorCode::InvalidArgument, "a resized element needs a finite size");
    }
    Element after = *current;
    after.transform = transform;
    const bool resizable = std::visit(
        [&](auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, Shape> || std::is_same_v<T, Image> ||
                          std::is_same_v<T, TextBox>) {
                payload.size = size;
                return true;
            } else {
                return false;
            }
        },
        after.payload);
    if (!resizable) {
        return makeError(ErrorCode::InvalidArgument,
                         "element " + element.toString() + " cannot be resized");
    }
    if (after == *current) {
        return Command{"Resize element", Patch{}};
    }
    std::vector<AnyChange> changes;
    // Attached connector ends keep their relative place on the element's bounds.
    const core::DRect from = worldBounds(*current);
    const core::DRect to = worldBounds(after);
    const auto map = [&](core::DVec2 p) {
        const auto axis = [](double v, double min0, double size0, double min1, double size1) {
            return size0 > 0.0 ? min1 + (v - min0) * (size1 / size0) : min1 + size1 * 0.5;
        };
        return core::DVec2{axis(p.x, from.min.x, from.width(), to.min.x, to.width()),
                           axis(p.y, from.min.y, from.height(), to.min.y, to.height())};
    };
    std::vector<core::ElementId> attached(workspace.connectorsAttachedTo(element).begin(),
                                          workspace.connectorsAttachedTo(element).end());
    std::sort(attached.begin(), attached.end());
    attached.erase(std::unique(attached.begin(), attached.end()), attached.end());
    changes.push_back(updated(*current, std::move(after)));
    for (const core::ElementId id : attached) {
        const Element& link = *workspace.findElement(id);
        Element moved = link;
        auto& connector = std::get<Connector>(moved.payload);
        for (ConnectorEnd* end : {&connector.start, &connector.end}) {
            if (end->attachedTo == element) {
                end->position = map(end->position);
            }
        }
        changes.push_back(updated(link, std::move(moved)));
    }
    return makeCommand("Resize element", std::move(changes));
}

} // namespace studyapp::document::commands
