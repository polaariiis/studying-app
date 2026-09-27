#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Records.hpp>
#include <studyapp/document/Workspace.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

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
// test. Errors: NotFound for unknown ids, InvalidArgument for invalid input. Renaming to
// the current title gives an empty (no-op) command.

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
    PageBackground background{};
    std::string firstLayerName = "Layer 1";
};

/// Creates the page together with its first layer (a page always has at least one layer).
[[nodiscard]] core::Result<Created<core::PageId>>
createPage(const Workspace& workspace, core::SectionId section, std::string title,
           const PageOptions& options, const core::Clock& clock, core::IdGenerator& ids);
[[nodiscard]] core::Result<Command> renamePage(const Workspace& workspace, core::PageId page,
                                               std::string title, const core::Clock& clock);
[[nodiscard]] core::Result<Command> deletePage(const Workspace& workspace, core::PageId page);

// ---- ordering -------------------------------------------------------------------------
// Moves change only the record's parent, order key and modified time; the subtree moves
// with it. `index` is the position among the destination's children once the record is
// there (0 = first; an index past the end appends). The new order key lies strictly
// between the neighbours' keys (core::FractionalIndex), so no sibling changes. Moving to
// the current position gives an empty (no-op) command. Errors: NotFound for unknown ids.

[[nodiscard]] core::Result<Command> moveNotebook(const Workspace& workspace,
                                                 core::NotebookId notebook, std::size_t index,
                                                 const core::Clock& clock);
/// Also moves a section into another notebook.
[[nodiscard]] core::Result<Command> moveSection(const Workspace& workspace, core::SectionId section,
                                                core::NotebookId destination, std::size_t index,
                                                const core::Clock& clock);
/// Also moves a page into another section.
[[nodiscard]] core::Result<Command> movePage(const Workspace& workspace, core::PageId page,
                                             core::SectionId destination, std::size_t index,
                                             const core::Clock& clock);

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
    Transform transform{};
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

/// Removes several elements (possibly on different pages) as one undoable edit.
/// Connectors outside the set that are attached to removed elements are detached.
/// Errors: InvalidArgument for an empty set, NotFound for unknown ids.
[[nodiscard]] core::Result<Command> deleteElements(const Workspace& workspace,
                                                   std::span<const core::ElementId> elements);

/// Translates several elements by `worldDelta` as one undoable edit. Connectors are
/// defined by world end positions, so for them the ends move instead of the transform:
/// a moved connector's free ends and the ends attached to moved elements follow, and the
/// cached end position of any other connector attached to a moved element follows too.
/// Errors: InvalidArgument for an empty set or a non-finite delta, NotFound for unknown
/// ids. A zero delta gives an empty (no-op) command.
[[nodiscard]] core::Result<Command> moveElements(const Workspace& workspace,
                                                 std::span<const core::ElementId> elements,
                                                 core::DVec2 worldDelta);

/// Pastes copies of `elements` (in painter order, e.g. a copied selection) on top of
/// `layer` as one undoable edit ("Paste"). Every copy gets a new id and keeps the order it
/// is given in; `offset` translates the copies (connector ends included). A connector end
/// attached to an element that is copied too is attached to that element's copy; an end
/// attached to anything else is detached where it is. Images keep referencing their asset
/// (the stored file is shared, not copied). Returns the new ids in the given order. The
/// workspace validates the copies when the patch is applied.
/// Errors: InvalidArgument for an empty list, a repeated id or a non-finite offset;
/// NotFound for an unknown layer.
[[nodiscard]] core::Result<Created<std::vector<core::ElementId>>>
pasteElements(const Workspace& workspace, core::LayerId layer, std::span<const Element> elements,
              core::DVec2 offset, core::IdGenerator& ids);

/// Resizes a box element (shape, image, text box) or a line/arrow shape as one undoable
/// edit (selection handles): the element gets `transform` and payload size `size` (for a
/// text box the caller gives the laid-out height). The ends of connectors attached to it
/// are mapped from its old world bounds to the new ones (same relative place on the box).
/// Unchanged geometry gives an empty command. Errors: NotFound for an unknown id,
/// InvalidArgument for other kinds or non-finite / negative sizes.
[[nodiscard]] core::Result<Command> resizeElement(const Workspace& workspace,
                                                  core::ElementId element, Transform transform,
                                                  core::Vec2 size);

/// Sets a connector's ends (position and attachment) and keeps its style, as one undoable
/// edit (the connector tool's endpoint drag). The workspace checks the attachments (a
/// non-connector element on the same page). Unchanged ends give an empty command.
/// Errors: NotFound for an unknown id, InvalidArgument for an element that is not a
/// connector or a non-finite position.
[[nodiscard]] core::Result<Command> setConnectorEnds(const Workspace& workspace,
                                                     core::ElementId connector, ConnectorEnd start,
                                                     ConnectorEnd end);

/// Replaces a text box's text and size as one undoable edit (the text tool, docs/CANVAS.md
/// §9). Unchanged text and size give an empty (no-op) command. Errors: NotFound for an
/// unknown id, InvalidArgument for an element that is not a text box.
[[nodiscard]] core::Result<Command> editText(const Workspace& workspace, core::ElementId element,
                                             std::string text, core::Vec2 size);

/// What is left of a stroke after partial erasing: runs of its points (element-local, as
/// stored). No pieces: the stroke is erased completely.
struct StrokePieces {
    core::ElementId stroke;
    std::vector<StrokePoints> pieces;
};

/// Replaces strokes by pieces of themselves as one undoable edit (the partial eraser,
/// docs/CANVAS.md §5.2). Every piece keeps the stroke's brush, colour, width, transform,
/// layer and lock state and takes the stroke's place in the draw order: the first piece
/// keeps the stroke's id (an update, so attached connectors stay attached), further pieces
/// are new elements ordered right after it, before the stroke's next sibling. A stroke
/// without pieces is removed (connectors attached to it are detached). The workspace
/// validates the pieces when the patch is applied.
/// Errors: InvalidArgument for an empty list, a repeated stroke, an element that is not a
/// stroke or an empty piece; NotFound for unknown ids.
[[nodiscard]] core::Result<Command> splitStrokes(const Workspace& workspace,
                                                 std::span<const StrokePieces> strokes,
                                                 core::IdGenerator& ids);

// ---- page format -----------------------------------------------------------------------
struct PageFormat {
    PageExtent extent = PageExtent::Infinite;
    core::DVec2 size{}; ///< required (> 0) for bounded pages
    PageBackground background{};
};

/// Changes a page's extent, size and background (validated by the workspace). An
/// unchanged format gives an empty (no-op) command.
[[nodiscard]] core::Result<Command> setPageFormat(const Workspace& workspace, core::PageId page,
                                                  const PageFormat& format,
                                                  const core::Clock& clock);

} // namespace studyapp::document::commands
