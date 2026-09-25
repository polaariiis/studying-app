#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/canvas/DocumentPort.hpp>

#include <utility>

namespace studyapp::ui {

/// The canvas's DocumentPort in the application: every canvas edit goes through
/// WorkspaceSession::execute — Command → Patch → Editor → Workspace → persistence — so it
/// is recorded for undo and written to the workspace database immediately (continuous
/// autosave, docs/DATABASE_SCHEMA.md §7.1). No second mutation or save path exists.
class SessionDocumentPort final : public canvas::DocumentPort {
public:
    explicit SessionDocumentPort(application::WorkspaceSession& session) : session_(&session) {}

    [[nodiscard]] const document::Workspace& workspace() const override {
        return session_->workspace();
    }
    [[nodiscard]] core::Result<void> execute(document::Command command) override {
        return session_->execute(std::move(command));
    }
    [[nodiscard]] bool isReadOnly() const override { return session_->isReadOnly(); }

private:
    application::WorkspaceSession* session_;
};

} // namespace studyapp::ui
