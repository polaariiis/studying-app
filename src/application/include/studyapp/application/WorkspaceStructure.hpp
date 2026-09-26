#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Commands.hpp>

#include <cstddef>
#include <string>
#include <variant>

namespace studyapp::application {

/// A record of the navigation hierarchy (Workspace → Notebook → Section → Page).
using HierarchyItem = std::variant<core::NotebookId, core::SectionId, core::PageId>;

/// Format of pages created without another page to follow: infinite, white, dotted
/// (24 world units), as the start page.
[[nodiscard]] document::commands::PageOptions defaultPageOptions();

/// Structure edits of the workspace shell (docs/ARCHITECTURE.md §3.5).
///
/// Every operation builds document commands (Phase 2 commands; moves since Phase 5) and
/// executes them through the WorkspaceSession, so each is exactly one undoable, persisted
/// edit — there is no other mutation path. Operations that create several records
/// (a notebook with its first section and page) are combined into one command.
///
/// Naming: an empty title gets the next free default ("Notebook 2", "Section 3",
/// "Page 4") among the new record's siblings. New pages take the format (extent, size,
/// background) of the last page of their section, so a ruled section stays ruled.
///
/// Deleting follows the document model: hard deletes of the whole subtree, undoable.
class WorkspaceStructure {
public:
    WorkspaceStructure(WorkspaceSession& session, const core::Clock& clock,
                       core::IdGenerator& ids) noexcept;

    struct NewNotebook {
        core::NotebookId notebook;
        core::SectionId section;
        core::PageId page;
    };
    struct NewSection {
        core::SectionId section;
        core::PageId page;
    };

    /// A notebook with a first section and page (one undo step).
    [[nodiscard]] core::Result<NewNotebook> createNotebook(std::string title = {});
    /// A section (appended to `notebook`) with a first page (one undo step).
    [[nodiscard]] core::Result<NewSection> createSection(core::NotebookId notebook,
                                                         std::string title = {});
    /// A page appended to `section`.
    [[nodiscard]] core::Result<core::PageId> createPage(core::SectionId section,
                                                        std::string title = {});

    /// Page titles may be empty ("Untitled page"); notebook and section titles may not.
    [[nodiscard]] core::Result<void> rename(const HierarchyItem& item, std::string title);
    /// Deletes the item and everything inside it (one undo step).
    [[nodiscard]] core::Result<void> remove(const HierarchyItem& item);

    /// Moves the item to `index` among its siblings (see document::commands::moveNotebook).
    [[nodiscard]] core::Result<void> moveWithinParent(const HierarchyItem& item, std::size_t index);
    /// One position up (-1) or down (+1) among its siblings; no-op at either end.
    [[nodiscard]] core::Result<void> moveBy(const HierarchyItem& item, int delta);
    [[nodiscard]] core::Result<void> moveSection(core::SectionId section,
                                                 core::NotebookId destination, std::size_t index);
    [[nodiscard]] core::Result<void> movePage(core::PageId page, core::SectionId destination,
                                              std::size_t index);

    /// Title as shown to the user ("Untitled page" for an empty page title).
    [[nodiscard]] static std::string displayTitle(const document::Workspace& workspace,
                                                  const HierarchyItem& item);
    /// Position of the item among its siblings, or nullopt if it does not exist.
    [[nodiscard]] static std::optional<std::size_t> indexOf(const document::Workspace& workspace,
                                                            const HierarchyItem& item);
    [[nodiscard]] static bool exists(const document::Workspace& workspace,
                                     const HierarchyItem& item);

private:
    [[nodiscard]] core::Result<void> run(core::Result<document::Command> command);

    WorkspaceSession* session_;
    const core::Clock* clock_;
    core::IdGenerator* ids_;
};

} // namespace studyapp::application
