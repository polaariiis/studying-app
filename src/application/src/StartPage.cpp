#include <studyapp/application/StartPage.hpp>

#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/document/Commands.hpp>

#include <utility>

namespace studyapp::application {

namespace commands = document::commands;

std::optional<core::PageId> firstPage(const document::Workspace& workspace) {
    for (const core::NotebookId notebook : workspace.notebooks()) {
        for (const core::SectionId section : workspace.sectionsOf(notebook)) {
            const auto pages = workspace.pagesOf(section);
            if (!pages.empty()) {
                return pages.front();
            }
        }
    }
    return std::nullopt;
}

core::Result<core::PageId> ensureStartPage(WorkspaceSession& session, const core::Clock& clock,
                                           core::IdGenerator& ids) {
    if (const auto page = firstPage(session.workspace())) {
        return *page;
    }
    if (session.isReadOnly()) {
        return core::makeError(core::ErrorCode::Unsupported,
                               "the read-only workspace contains no page");
    }
    // Commands need their parents to exist, so they are built against a scratch copy and
    // their patches combined into one command (one undo step for the whole start page).
    document::Workspace scratch = session.workspace();
    document::Patch combined;
    const auto step = [&](auto created) -> core::Result<decltype(created->id)> {
        if (!created) {
            return tl::unexpected(created.error());
        }
        if (auto applied = scratch.apply(created->command.patch); !applied) {
            return tl::unexpected(applied.error());
        }
        for (const auto& change : created->command.patch.changes()) {
            combined.add(change);
        }
        return created->id;
    };
    auto notebook = step(commands::createNotebook(scratch, "Notebook", clock, ids));
    if (!notebook) {
        return tl::unexpected(notebook.error());
    }
    auto section = step(commands::createSection(scratch, *notebook, "Notes", clock, ids));
    if (!section) {
        return tl::unexpected(section.error());
    }
    auto page =
        step(commands::createPage(scratch, *section, "Page 1", defaultPageOptions(), clock, ids));
    if (!page) {
        return tl::unexpected(page.error());
    }
    if (auto executed = session.execute({"Create start page", std::move(combined)}); !executed) {
        return tl::unexpected(executed.error());
    }
    return *page;
}

} // namespace studyapp::application
