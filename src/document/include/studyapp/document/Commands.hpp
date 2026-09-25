#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Records.hpp>
#include <studyapp/document/Workspace.hpp>

#include <string>

namespace studyapp::document::commands {

// Edit operations of the document model (docs/DATA_MODEL.md §6.2).
//
// A command is a pure function: it reads the current workspace and returns a Command
// (label + Patch) describing the edit, without changing anything. The Editor applies the
// patch and records it for undo. Commands never mutate the workspace directly, so every
// edit is expressible as data and goes through the same validation.
//
// New records are appended after their last sibling. Ids come from the injected
// IdGenerator and timestamps from the injected Clock, so results are deterministic under
// test. Errors: NotFound for unknown ids, InvalidArgument for invalid input.

/// Result of a creating command: the id of the new record plus the command to execute.
template <class Id>
struct Created {
    Id id;
    Command command;
};

// ---- notebooks ------------------------------------------------------------------------
[[nodiscard]] core::Result<Created<core::NotebookId>> createNotebook(const Workspace& workspace,
                                                                     std::string title,
                                                                     const core::Clock& clock,
                                                                     core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> renameNotebook(const Workspace& workspace,
                                                   core::NotebookId notebook, std::string title,
                                                   const core::Clock& clock);
/// Removes the notebook and everything inside it (one undoable patch).
[[nodiscard]] core::Result<Command> deleteNotebook(const Workspace& workspace,
                                                   core::NotebookId notebook);

// ---- sections -------------------------------------------------------------------------
[[nodiscard]] core::Result<Created<core::SectionId>>
createSection(const Workspace& workspace, core::NotebookId notebook, std::string title,
              const core::Clock& clock, core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> renameSection(const Workspace& workspace,
                                                  core::SectionId section, std::string title,
                                                  const core::Clock& clock);
[[nodiscard]] core::Result<Command> deleteSection(const Workspace& workspace,
                                                  core::SectionId section);

// ---- pages ----------------------------------------------------------------------------
struct PageOptions {
    PageExtent extent = PageExtent::Infinite;
    core::DVec2 size{}; ///< required (> 0) for bounded pages
    PageBackground background;
    std::string firstLayerName = "Layer 1";
};

/// Creates the page together with its first layer (a page always has at least one layer).
[[nodiscard]] core::Result<Created<core::PageId>>
createPage(const Workspace& workspace, core::SectionId section, std::string title,
           const PageOptions& options, const core::Clock& clock, core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> renamePage(const Workspace& workspace, core::PageId page,
                                               std::string title, const core::Clock& clock);
[[nodiscard]] core::Result<Command> deletePage(const Workspace& workspace, core::PageId page);

// ---- layers ---------------------------------------------------------------------------
[[nodiscard]] core::Result<Created<core::LayerId>> createLayer(const Workspace& workspace,
                                                               core::PageId page, std::string name,
                                                               core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> renameLayer(const Workspace& workspace, core::LayerId layer,
                                                std::string name);
/// Removes the layer and its elements. Fails if it is the page's last layer.
[[nodiscard]] core::Result<Command> deleteLayer(const Workspace& workspace, core::LayerId layer);

// ---- elements -------------------------------------------------------------------------
struct NewElement {
    Transform transform;
    ElementPayload payload;
    bool locked = false;
};

[[nodiscard]] core::Result<Created<core::ElementId>> createElement(const Workspace& workspace,
                                                                   core::LayerId layer,
                                                                   NewElement element,
                                                                   core::IdGenerator& ids);
/// Removes the element. Connectors attached to it are detached in the same patch.
[[nodiscard]] core::Result<Command> deleteElement(const Workspace& workspace,
                                                  core::ElementId element);

} // namespace studyapp::document::commands
