#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Workspace.hpp>

#include <cstddef>
#include <optional>

namespace studyapp::application {

/// The active page of an open workspace: application state of the shell, not document
/// data (docs/ARCHITECTURE.md §3.5). It is never persisted in the workspace and never
/// part of the undo history.
///
/// The navigator only reads the workspace. The owner reports every applied patch
/// (onPatch), so the active page stays valid when pages are deleted — by a command or by
/// undo/redo: the page now at the removed page's position in the same section becomes
/// active, else the one before it, else the first page of the same notebook, else the
/// first page of the workspace, else none. With no active page, the first page that
/// appears (e.g. by undoing a delete) becomes active.
///
/// Not thread-safe; used on the GUI thread.
class PageNavigator {
public:
    explicit PageNavigator(const document::Workspace& workspace) noexcept;

    [[nodiscard]] std::optional<core::PageId> activePage() const noexcept { return active_; }
    [[nodiscard]] std::optional<core::SectionId> activeSection() const;
    [[nodiscard]] std::optional<core::NotebookId> activeNotebook() const;

    /// Makes `page` active. NotFound if it does not exist. true if the active page changed.
    [[nodiscard]] core::Result<bool> open(core::PageId page);
    /// Clears the active page (e.g. before closing the workspace).
    void clear() noexcept;

    /// Keeps the active page valid after `patch` was applied. true if it changed.
    bool onPatch(const document::Patch& patch);

    /// The pages before and after the active one in workspace order (notebook, section,
    /// page), across section and notebook boundaries.
    [[nodiscard]] std::optional<core::PageId> previousPage() const;
    [[nodiscard]] std::optional<core::PageId> nextPage() const;

private:
    void remember();
    [[nodiscard]] std::optional<core::PageId> fallback() const;

    const document::Workspace* workspace_;
    std::optional<core::PageId> active_;
    // Where the active page was, to pick its neighbour when it disappears.
    core::SectionId section_;
    core::NotebookId notebook_;
    std::size_t index_ = 0;
};

/// The one existing page whose content (elements, layers or the page record itself)
/// `patch` changes, or nullopt if it changes none or several. The shell shows this page
/// after undo/redo, so an undone edit is never invisible.
[[nodiscard]] std::optional<core::PageId> pageChangedBy(const document::Patch& patch,
                                                        const document::Workspace& workspace);

} // namespace studyapp::application
