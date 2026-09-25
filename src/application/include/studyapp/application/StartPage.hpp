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

/// The page the canvas opens: firstPage(), or — for an empty workspace — a new
/// "Notebook › Notes › Page 1" (infinite, dotted background) created as one undoable
/// command. Page navigation is Phase 5; until then the canvas shows this page.
/// Fails with Unsupported if the workspace is read-only and has no page.
[[nodiscard]] core::Result<core::PageId>
ensureStartPage(WorkspaceSession& session, const core::Clock& clock, core::IdGenerator& ids);

} // namespace studyapp::application
