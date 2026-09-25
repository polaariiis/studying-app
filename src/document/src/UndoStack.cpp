#include <studyapp/document/UndoStack.hpp>

#include <algorithm>
#include <cassert>
#include <utility>

namespace studyapp::document {

UndoStack::UndoStack(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

void UndoStack::record(Command command) {
    redo_.clear();
    undo_.push_back(std::move(command));
    while (undo_.size() > capacity_) {
        undo_.pop_front();
    }
}

const Command* UndoStack::nextUndo() const noexcept {
    return undo_.empty() ? nullptr : &undo_.back();
}

const Command* UndoStack::nextRedo() const noexcept {
    return redo_.empty() ? nullptr : &redo_.back();
}

std::string_view UndoStack::undoLabel() const noexcept {
    return undo_.empty() ? std::string_view{} : std::string_view(undo_.back().label);
}

std::string_view UndoStack::redoLabel() const noexcept {
    return redo_.empty() ? std::string_view{} : std::string_view(redo_.back().label);
}

void UndoStack::markUndone() {
    assert(canUndo());
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
}

void UndoStack::markRedone() {
    assert(canRedo());
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
}

void UndoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

} // namespace studyapp::document
