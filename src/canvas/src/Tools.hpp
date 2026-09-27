#pragma once

#include <studyapp/canvas/Camera.hpp>
#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/DocumentPort.hpp>
#include <studyapp/canvas/Input.hpp>
#include <studyapp/canvas/Selection.hpp>
#include <studyapp/canvas/SelectionHandles.hpp>
#include <studyapp/canvas/StrokeBuilder.hpp>
#include <studyapp/canvas/StrokeEraser.hpp>
#include <studyapp/canvas/TextLayout.hpp>
#include <studyapp/canvas/ToolSettings.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/document/Patch.hpp>

#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace studyapp::canvas::detail {

/// Transient, non-document render state produced by tools (docs/CANVAS.md §5): the live
/// stroke, the marquee, the move offset and elements the eraser is about to remove. Only
/// the controller draws it; nothing here is persisted.
struct Preview {
    std::optional<StrokeBuilder> stroke;
    std::optional<core::DRect> marquee; ///< world
    bool moving = false;
    core::DVec2 moveOffset{}; ///< world, applied to selected elements while moving
    std::unordered_set<core::ElementId> erased; ///< hidden until the eraser gesture commits
    /// Partial eraser: what is left of each stroke it cut (hidden in `erased`, drawn from
    /// these pieces instead). Keyed by id: every eraser step looks candidates up here.
    struct ErasedPieces {
        std::vector<StrokePiece> pieces;
        bool dirty = true; ///< changed since the controller last tessellated them
    };
    std::unordered_map<core::ElementId, ErasedPieces> partial;
    /// The shape or connector being dragged (not in the document; for a new one the id is
    /// null, for an edited connector it is that connector's, which is hidden meanwhile).
    std::optional<document::Element> shape;
    /// Connectors attached to elements being moved (not moved themselves): drawn with
    /// their attached ends following the move offset. Found once per move gesture from
    /// the workspace's attachment index.
    std::unordered_set<core::ElementId> followers;
    /// The element being resized with a handle, at its new geometry (same id; the original
    /// is drawn like this until the gesture commits or is cancelled).
    std::optional<document::Element> resized;

    void clear() {
        shape.reset();
        followers.clear();
        resized.reset();
        stroke.reset();
        marquee.reset();
        moving = false;
        moveOffset = {};
        erased.clear();
        partial.clear();
    }
};

/// Everything a tool may touch — nothing else (docs/CANVAS.md §5).
struct ToolContext {
    Camera& camera;
    const CanvasScene& scene;
    const DocumentPort& document;
    Selection& selection;
    Preview& preview;
    const PenStyle& pen;
    EraserMode eraserMode;
    const ShapeStyle& shape;
    const StrokeOptions& strokeOptions;
    core::IdGenerator& ids;
    /// Executes a command built by the tool through the DocumentPort; failures are
    /// recorded by the controller. The only way a tool changes the document.
    std::function<void(core::Result<document::Command>)> commit;
    /// Starts editing text in the UI's editor (the text tool); nothing is written until the
    /// edit is finished.
    std::function<void(TextEdit)> beginTextEdit;
    /// Height of text at a width (the canvas's text layout), for resizing text boxes.
    std::function<float(std::string_view, float)> textHeight;
};

class Tool {
public:
    Tool() = default;
    virtual ~Tool() = default;
    Tool(const Tool&) = delete;
    Tool& operator=(const Tool&) = delete;
    Tool(Tool&&) = delete;
    Tool& operator=(Tool&&) = delete;

    virtual void onPointer(const PointerEvent& event, ToolContext& context) = 0;
    /// Abandons the gesture in progress without changing the document.
    virtual void cancel(ToolContext& context) = 0;
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
    [[nodiscard]] virtual CursorShape cursor() const noexcept = 0;
};

/// Freehand ink: the pen and the highlighter. Both draw a stroke in ToolContext::pen (the
/// style fixed at the gesture's start); they differ only in their settings and cursor.
class PenTool final : public Tool {
public:
    explicit PenTool(CursorShape cursor = CursorShape::Crosshair) noexcept : cursor_(cursor) {}

    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return cursor_; }

private:
    CursorShape cursor_;
    bool active_ = false;
};

class SelectTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return mode_ != Mode::Idle; }
    [[nodiscard]] CursorShape cursor() const noexcept override {
        return mode_ == Mode::Moving || mode_ == Mode::Resizing ? CursorShape::SizeAll
                                                                : CursorShape::Arrow;
    }

    /// How close (view px) a press must be to a selection handle to grab it.
    static constexpr double kHandleGrabViewPx = 6.0;
    /// Handles are drawn as squares this big (view px).
    static constexpr double kHandleViewPx = 7.0;

private:
    enum class Mode : std::uint8_t {
        Idle,
        PendingMove, ///< pressed on a selected element; becomes Moving past the drag threshold
        Moving,
        Marquee,
        Resizing, ///< dragging a selection handle of the one selected element
    };
    [[nodiscard]] bool beginResize(const core::DVec2& world, ToolContext& context);
    void commitResize(const core::DVec2& world, bool constrain, ToolContext& context);

    Mode mode_ = Mode::Idle;
    core::ElementId resizing_{};
    HandleKind handle_ = HandleKind::BottomRight;
    core::DVec2 startView_{};
    core::DVec2 startWorld_{};
    bool additive_ = false;
};

class EraserTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::EraserRing; }

    static constexpr double kRadiusViewPx = kEraserRadiusViewPx;
    /// Partial erasing drops pieces shorter than this (view px at the erasing zoom) or than
    /// the stroke's width, whichever is longer: no stray dots are left behind.
    static constexpr double kMinFragmentViewPx = 1.5;

private:
    void eraseAlong(const core::DVec2& a, const core::DVec2& b, ToolContext& context) const;
    void commit(ToolContext& context);

    bool active_ = false;
    bool partial_ = false; ///< the gesture's mode, fixed at its start
    core::DVec2 lastWorld_{};
};

/// Drags out a shape (docs/CANVAS.md §13): press at one corner or end, release at the
/// other; Shift makes boxes square and snaps lines to 45°. One gesture, one command.
class ShapeTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::Crosshair; }

    /// Shorter drags (view px) create nothing: a click is not a shape.
    static constexpr double kMinDragViewPx = 3.0;

private:
    void updatePreview(const core::DVec2& world, bool constrain, ToolContext& context) const;

    bool active_ = false;
    core::DVec2 startWorld_{};
    core::DVec2 startView_{};
};

/// Starts text editing (docs/CANVAS.md §9): a click on a text box edits it, a click
/// elsewhere starts a new box kDefaultTextWidth wide, a drag starts one as wide as the drag.
class TextTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& /*context*/) override { active_ = false; }
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::IBeam; }

private:
    bool active_ = false;
    core::DVec2 startWorld_{};
    core::DVec2 startView_{};
};

/// Draws connectors (docs/CANVAS.md §13): press on an element (or empty space) and release
/// on another; attached ends sit where the line leaves each element's bounds and follow
/// the element when it moves. Pressing on an end of an existing connector drags that end
/// (re-attaching it to the element it is released on). One gesture, one command.
class ConnectorTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return mode_ != Mode::Idle; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::Crosshair; }

    /// How close (view px) a press must be to a connector end to grab it.
    static constexpr double kEndGrabViewPx = 6.0;

private:
    enum class Mode : std::uint8_t {
        Idle,
        Creating,
        MovingEnd,
    };
    /// The connector the gesture would commit, for the pointer at `world`.
    [[nodiscard]] document::Connector draft(const core::DVec2& world,
                                            const ToolContext& context) const;

    Mode mode_ = Mode::Idle;
    core::DVec2 startWorld_{};
    core::DVec2 startView_{};
    std::optional<core::ElementId> startAttached_;
    core::ElementId editing_{}; ///< MovingEnd: the connector
    bool editingStart_ = false; ///< MovingEnd: which end
};

/// The topmost element (not a connector, not locked) whose geometry is near `world`: what
/// a connector end attaches to. `except`: never this one.
[[nodiscard]] std::optional<core::ElementId> attachTarget(const ToolContext& context,
                                                          const core::DVec2& world,
                                                          std::optional<core::ElementId> except);

class PanTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& context) override;
    [[nodiscard]] bool isActive() const noexcept override { return active_; }
    [[nodiscard]] CursorShape cursor() const noexcept override {
        return active_ ? CursorShape::ClosedHand : CursorShape::OpenHand;
    }

private:
    bool active_ = false;
    core::DVec2 lastView_{};
};

class ZoomTool final : public Tool {
public:
    void onPointer(const PointerEvent& event, ToolContext& context) override;
    void cancel(ToolContext& /*context*/) override {}
    [[nodiscard]] bool isActive() const noexcept override { return false; }
    [[nodiscard]] CursorShape cursor() const noexcept override { return CursorShape::ZoomIn; }

    static constexpr double kStep = 2.0;
};

/// The layer new strokes go to: the topmost visible, unlocked layer of the page.
[[nodiscard]] std::optional<core::LayerId> targetLayer(const document::Workspace& workspace,
                                                       core::PageId page);

/// Topmost element on a visible, unlocked layer whose geometry is within `toleranceWorld`
/// of `world`.
[[nodiscard]] std::optional<core::ElementId> topmostAt(const CanvasScene& scene,
                                                       const document::Workspace& workspace,
                                                       const core::DVec2& world,
                                                       double toleranceWorld);

} // namespace studyapp::canvas::detail
