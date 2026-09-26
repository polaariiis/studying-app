#include <studyapp/application/WorkspaceStructure.hpp>

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

namespace studyapp::application {

namespace commands = document::commands;
using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

/// Several creating commands combined into one: each is built against a scratch copy (a
/// command needs its parent to exist) and their patches are concatenated.
class Compound {
public:
    explicit Compound(const document::Workspace& workspace) : scratch_(workspace) {}

    [[nodiscard]] const document::Workspace& workspace() const noexcept { return scratch_; }

    template <class Id>
    Result<Id> add(Result<commands::Created<Id>> created) {
        if (!created) {
            return tl::unexpected(created.error());
        }
        if (auto applied = scratch_.apply(created->command.patch); !applied) {
            return tl::unexpected(applied.error());
        }
        for (const auto& change : created->command.patch.changes()) {
            combined_.add(change);
        }
        return created->id;
    }

    [[nodiscard]] document::Command command(std::string label) && {
        return {std::move(label), std::move(combined_)};
    }

private:
    document::Workspace scratch_;
    document::Patch combined_;
};

/// "<base> <n>" with the smallest n ≥ count + 1 not used by a sibling.
template <class Id, class TitleOf>
std::string nextDefaultTitle(std::string_view base, std::span<const Id> siblings, TitleOf titleOf) {
    for (std::size_t n = siblings.size() + 1;; ++n) {
        std::string candidate = std::string(base) + " " + std::to_string(n);
        const bool used = std::any_of(siblings.begin(), siblings.end(),
                                      [&](const Id& id) { return titleOf(id) == candidate; });
        if (!used) {
            return candidate;
        }
    }
}

bool isBlank(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

commands::PageOptions optionsFollowing(const document::Workspace& workspace,
                                       core::SectionId section) {
    commands::PageOptions options = defaultPageOptions();
    const auto pages = workspace.pagesOf(section);
    if (!pages.empty()) {
        const document::PageInfo& last = *workspace.findPage(pages.back());
        options.extent = last.extent;
        options.size = last.size;
        options.background = last.background;
    }
    return options;
}

std::string defaultPageTitle(const document::Workspace& workspace, core::SectionId section) {
    return nextDefaultTitle("Page", workspace.pagesOf(section),
                            [&](core::PageId id) { return workspace.findPage(id)->title; });
}

std::string defaultSectionTitle(const document::Workspace& workspace, core::NotebookId notebook) {
    return nextDefaultTitle("Section", workspace.sectionsOf(notebook),
                            [&](core::SectionId id) { return workspace.findSection(id)->title; });
}

} // namespace

commands::PageOptions defaultPageOptions() {
    return commands::PageOptions{.extent = document::PageExtent::Infinite,
                                 .size = {},
                                 .background = {.color = core::Color::white(),
                                                .pattern = document::BackgroundPattern::Dots,
                                                .spacing = 24.0F},
                                 .firstLayerName = "Layer 1"};
}

WorkspaceStructure::WorkspaceStructure(WorkspaceSession& session, const core::Clock& clock,
                                       core::IdGenerator& ids) noexcept
    : session_(&session), clock_(&clock), ids_(&ids) {}

Result<void> WorkspaceStructure::run(Result<document::Command> command) {
    if (!command) {
        return tl::unexpected(command.error());
    }
    if (command->patch.empty()) {
        return {}; // e.g. a move to where the item already is: nothing to record
    }
    return session_->execute(std::move(*command));
}

// ---------------------------------------------------------------------------- create

Result<WorkspaceStructure::NewNotebook> WorkspaceStructure::createNotebook(std::string title) {
    const document::Workspace& ws = session_->workspace();
    if (isBlank(title)) {
        title = nextDefaultTitle("Notebook", ws.notebooks(),
                                 [&](core::NotebookId id) { return ws.findNotebook(id)->title; });
    }
    Compound compound(ws);
    auto notebook = compound.add(
        commands::createNotebook(compound.workspace(), std::move(title), *clock_, *ids_));
    if (!notebook) {
        return tl::unexpected(notebook.error());
    }
    auto section = compound.add(
        commands::createSection(compound.workspace(), *notebook, "Section 1", *clock_, *ids_));
    if (!section) {
        return tl::unexpected(section.error());
    }
    auto page = compound.add(commands::createPage(compound.workspace(), *section, "Page 1",
                                                  defaultPageOptions(), *clock_, *ids_));
    if (!page) {
        return tl::unexpected(page.error());
    }
    if (auto executed = session_->execute(std::move(compound).command("Create notebook"));
        !executed) {
        return tl::unexpected(executed.error());
    }
    return NewNotebook{.notebook = *notebook, .section = *section, .page = *page};
}

Result<WorkspaceStructure::NewSection> WorkspaceStructure::createSection(core::NotebookId notebook,
                                                                         std::string title) {
    const document::Workspace& ws = session_->workspace();
    if (ws.findNotebook(notebook) == nullptr) {
        return makeError(ErrorCode::NotFound, "the notebook does not exist");
    }
    if (isBlank(title)) {
        title = defaultSectionTitle(ws, notebook);
    }
    Compound compound(ws);
    auto section = compound.add(
        commands::createSection(compound.workspace(), notebook, std::move(title), *clock_, *ids_));
    if (!section) {
        return tl::unexpected(section.error());
    }
    auto page = compound.add(commands::createPage(compound.workspace(), *section, "Page 1",
                                                  defaultPageOptions(), *clock_, *ids_));
    if (!page) {
        return tl::unexpected(page.error());
    }
    if (auto executed = session_->execute(std::move(compound).command("Create section"));
        !executed) {
        return tl::unexpected(executed.error());
    }
    return NewSection{.section = *section, .page = *page};
}

Result<core::PageId> WorkspaceStructure::createPage(core::SectionId section, std::string title) {
    const document::Workspace& ws = session_->workspace();
    if (ws.findSection(section) == nullptr) {
        return makeError(ErrorCode::NotFound, "the section does not exist");
    }
    if (isBlank(title)) {
        title = defaultPageTitle(ws, section);
    }
    auto created = commands::createPage(ws, section, std::move(title),
                                        optionsFollowing(ws, section), *clock_, *ids_);
    if (!created) {
        return tl::unexpected(created.error());
    }
    const core::PageId page = created->id;
    if (auto executed = session_->execute(std::move(created->command)); !executed) {
        return tl::unexpected(executed.error());
    }
    return page;
}

// ---------------------------------------------------------------------------- edit

Result<void> WorkspaceStructure::rename(const HierarchyItem& item, std::string title) {
    const document::Workspace& ws = session_->workspace();
    return std::visit(
        [&](const auto& id) -> Result<void> {
            using Id = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, core::NotebookId>) {
                return run(commands::renameNotebook(ws, id, std::move(title), *clock_));
            } else if constexpr (std::is_same_v<Id, core::SectionId>) {
                return run(commands::renameSection(ws, id, std::move(title), *clock_));
            } else {
                return run(commands::renamePage(ws, id, std::move(title), *clock_));
            }
        },
        item);
}

Result<void> WorkspaceStructure::remove(const HierarchyItem& item) {
    const document::Workspace& ws = session_->workspace();
    return std::visit(
        [&](const auto& id) -> Result<void> {
            using Id = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, core::NotebookId>) {
                return run(commands::deleteNotebook(ws, id));
            } else if constexpr (std::is_same_v<Id, core::SectionId>) {
                return run(commands::deleteSection(ws, id));
            } else {
                return run(commands::deletePage(ws, id));
            }
        },
        item);
}

Result<void> WorkspaceStructure::moveWithinParent(const HierarchyItem& item, std::size_t index) {
    const document::Workspace& ws = session_->workspace();
    return std::visit(
        [&](const auto& id) -> Result<void> {
            using Id = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, core::NotebookId>) {
                return run(commands::moveNotebook(ws, id, index, *clock_));
            } else if constexpr (std::is_same_v<Id, core::SectionId>) {
                const document::SectionInfo* section = ws.findSection(id);
                if (section == nullptr) {
                    return makeError(ErrorCode::NotFound, "the section does not exist");
                }
                return run(commands::moveSection(ws, id, section->notebook, index, *clock_));
            } else {
                const document::PageInfo* page = ws.findPage(id);
                if (page == nullptr) {
                    return makeError(ErrorCode::NotFound, "the page does not exist");
                }
                return run(commands::movePage(ws, id, page->section, index, *clock_));
            }
        },
        item);
}

Result<void> WorkspaceStructure::moveBy(const HierarchyItem& item, int delta) {
    const auto index = indexOf(session_->workspace(), item);
    if (!index) {
        return makeError(ErrorCode::NotFound, "the item does not exist");
    }
    if (delta < 0 && *index == 0) {
        return {};
    }
    const auto target = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*index) + delta);
    return moveWithinParent(item, target); // past the end is a no-op there too
}

Result<void> WorkspaceStructure::moveSection(core::SectionId section, core::NotebookId destination,
                                             std::size_t index) {
    return run(commands::moveSection(session_->workspace(), section, destination, index, *clock_));
}

Result<void> WorkspaceStructure::movePage(core::PageId page, core::SectionId destination,
                                          std::size_t index) {
    return run(commands::movePage(session_->workspace(), page, destination, index, *clock_));
}

// ---------------------------------------------------------------------------- queries

std::string WorkspaceStructure::displayTitle(const document::Workspace& workspace,
                                             const HierarchyItem& item) {
    return std::visit(
        [&](const auto& id) -> std::string {
            using Id = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, core::NotebookId>) {
                const auto* notebook = workspace.findNotebook(id);
                return notebook != nullptr ? notebook->title : std::string();
            } else if constexpr (std::is_same_v<Id, core::SectionId>) {
                const auto* section = workspace.findSection(id);
                return section != nullptr ? section->title : std::string();
            } else {
                const auto* page = workspace.findPage(id);
                if (page == nullptr) {
                    return {};
                }
                return isBlank(page->title) ? std::string("Untitled page") : page->title;
            }
        },
        item);
}

std::optional<std::size_t> WorkspaceStructure::indexOf(const document::Workspace& workspace,
                                                       const HierarchyItem& item) {
    const auto find = [](auto siblings, auto id) -> std::optional<std::size_t> {
        const auto it = std::find(siblings.begin(), siblings.end(), id);
        if (it == siblings.end()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(it - siblings.begin());
    };
    return std::visit(
        [&](const auto& id) -> std::optional<std::size_t> {
            using Id = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, core::NotebookId>) {
                return find(workspace.notebooks(), id);
            } else if constexpr (std::is_same_v<Id, core::SectionId>) {
                const auto* section = workspace.findSection(id);
                return section != nullptr ? find(workspace.sectionsOf(section->notebook), id)
                                          : std::nullopt;
            } else {
                const auto* page = workspace.findPage(id);
                return page != nullptr ? find(workspace.pagesOf(page->section), id) : std::nullopt;
            }
        },
        item);
}

bool WorkspaceStructure::exists(const document::Workspace& workspace, const HierarchyItem& item) {
    return indexOf(workspace, item).has_value();
}

} // namespace studyapp::application
