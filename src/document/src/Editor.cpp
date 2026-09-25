#include <studyapp/document/Editor.hpp>

#include <utility>

namespace studyapp::document {

Editor::Editor(Workspace& workspace, std::size_t historyCapacity)
    : workspace_(&workspace), history_(historyCapacity) {}

core::Result<void> Editor::execute(Command command) {
    if (command.patch.empty()) {
        return {};
    }
    if (auto applied = workspace_->apply(command.patch); !applied) {
        return applied;
    }
    history_.record(std::move(command));
    return {};
}

core::Result<void> Editor::undo() {
    const Command* entry = history_.nextUndo();
    if (entry == nullptr) {
        return core::makeError(core::ErrorCode::NotFound, "nothing to undo");
    }
    if (auto applied = workspace_->apply(entry->patch.inverted()); !applied) {
        return applied;
    }
    history_.markUndone();
    return {};
}

core::Result<void> Editor::redo() {
    const Command* entry = history_.nextRedo();
    if (entry == nullptr) {
        return core::makeError(core::ErrorCode::NotFound, "nothing to redo");
    }
    if (auto applied = workspace_->apply(entry->patch); !applied) {
        return applied;
    }
    history_.markRedone();
    return {};
}

} // namespace studyapp::document
