#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Workspace.hpp>

#include <optional>

namespace studyapp::application {

/// First page of the workspace in hierarchy order (first notebook, first section, first
/// page), if there is one.
[[nodiscard]] std::optional<core::PageId> firstPage(const document::Workspace& workspace);

/// The first page of a new workspace: firstPage(), or — if it is empty — a new
/// "Notebook › Notes › Page 1" (infinite, dotted background) created as one undoable
/// command. The shell calls it when it creates a workspace; opening an existing
/// workspace never writes to it.
/// Fails with Unsupported if the workspace is read-only and has no page.
[[nodiscard]] core::Result<core::PageId>
ensureStartPage(WorkspaceSession& session, const core::Clock& clock, core::IdGenerator& ids);

} // namespace studyapp::application
