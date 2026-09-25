#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Workspace.hpp>

namespace studyapp::canvas {

/// The canvas's only way to read and change the document (docs/CANVAS.md §5: "the Editor
/// is the only way to change the document"). The canvas builds a Command from the current
/// workspace and hands it to execute(); it never mutates records itself and has no idea
/// how or when the change is saved.
///
/// In the application this is backed by application::WorkspaceSession (which applies the
/// command through its Editor, records it for undo and persists the patch); in tests by a
/// plain document::Editor. Whoever owns the port also reports every applied patch —
/// including undo/redo issued elsewhere — to CanvasController::onDocumentChanged().
class DocumentPort {
public:
    DocumentPort() = default;
    virtual ~DocumentPort() = default;
    DocumentPort(const DocumentPort&) = delete;
    DocumentPort& operator=(const DocumentPort&) = delete;
    DocumentPort(DocumentPort&&) = delete;
    DocumentPort& operator=(DocumentPort&&) = delete;

    [[nodiscard]] virtual const document::Workspace& workspace() const = 0;
    [[nodiscard]] virtual core::Result<void> execute(document::Command command) = 0;
    [[nodiscard]] virtual bool isReadOnly() const = 0;
};

} // namespace studyapp::canvas
