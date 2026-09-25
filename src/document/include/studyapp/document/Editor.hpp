#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/UndoStack.hpp>
#include <studyapp/document/Workspace.hpp>

#include <cstddef>

namespace studyapp::document {

/// Executes commands against a Workspace and keeps the undo/redo history.
///
///   execute(command): apply command.patch        -> record on the undo side, clear redo
///   undo():           apply inverse of last entry -> move entry to the redo side
///   redo():           re-apply last undone entry  -> move entry back to the undo side
///
/// Because Workspace::apply is atomic, a failed execute/undo/redo leaves both the
/// workspace and the history unchanged. Failed commands are never recorded; commands with
/// an empty patch (no-ops) succeed without being recorded.
///
/// The Editor does not own the Workspace; the Workspace must outlive it. It is the
/// intended mutation path for user edits (later phases forward the same patches to
/// persistence and canvas invalidation).
class Editor {
public:
    explicit Editor(Workspace& workspace,
                    std::size_t historyCapacity = UndoStack::kDefaultCapacity);

    [[nodiscard]] const Workspace& workspace() const noexcept { return *workspace_; }
    [[nodiscard]] const UndoStack& history() const noexcept { return history_; }

    [[nodiscard]] core::Result<void> execute(Command command);

    /// Nothing to undo/redo is reported as NotFound and changes nothing.
    [[nodiscard]] core::Result<void> undo();
    [[nodiscard]] core::Result<void> redo();

    [[nodiscard]] bool canUndo() const noexcept { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return history_.canRedo(); }

private:
    Workspace* workspace_;
    UndoStack history_;
};

} // namespace studyapp::document
