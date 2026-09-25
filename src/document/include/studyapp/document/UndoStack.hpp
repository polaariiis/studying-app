#pragma once

#include <studyapp/document/Patch.hpp>

#include <cstddef>
#include <deque>
#include <string_view>
#include <vector>

namespace studyapp::document {

/// Linear undo/redo history of executed Commands (docs/DATA_MODEL.md §6.3).
///
/// Entries are the commands as executed: undoing applies `patch.inverted()`, redoing
/// applies `patch` again. The stack itself never touches a document; Editor moves entries
/// between the two sides only after the corresponding patch was applied successfully.
///
/// Recording a new command clears the redo side (the redo branch is discarded). When
/// more than `capacity` entries are recorded, the oldest ones are dropped.
class UndoStack {
public:
    static constexpr std::size_t kDefaultCapacity = 500;

    explicit UndoStack(std::size_t capacity = kDefaultCapacity);

    /// Records an executed command and clears the redo side.
    void record(Command command);

    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }

    /// The command the next undo reverts / the next redo re-applies (nullptr if none).
    [[nodiscard]] const Command* nextUndo() const noexcept;
    [[nodiscard]] const Command* nextRedo() const noexcept;

    /// Label for "Undo <label>" / "Redo <label>"; empty if nothing to undo/redo.
    [[nodiscard]] std::string_view undoLabel() const noexcept;
    [[nodiscard]] std::string_view redoLabel() const noexcept;

    /// Moves the next undo entry to the redo side. Precondition: canUndo().
    void markUndone();
    /// Moves the next redo entry to the undo side. Precondition: canRedo().
    void markRedone();

    void clear() noexcept;

    [[nodiscard]] std::size_t undoCount() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redoCount() const noexcept { return redo_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    std::deque<Command> undo_;  ///< back = most recent
    std::vector<Command> redo_; ///< back = next to redo
};

} // namespace studyapp::document
