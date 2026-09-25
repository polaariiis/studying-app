#pragma once

#include <studyapp/canvas/Camera.hpp>
#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/DocumentPort.hpp>
#include <studyapp/canvas/Input.hpp>
#include <studyapp/canvas/RenderBatches.hpp>
#include <studyapp/canvas/RenderCache.hpp>
#include <studyapp/canvas/Selection.hpp>
#include <studyapp/canvas/StrokeBuilder.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Profiler.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/render/Renderer.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace studyapp::canvas {

namespace detail {
struct Preview;
class Tool;
} // namespace detail

enum class ToolKind : std::uint8_t {
    Pen,
    Select,
    Eraser, ///< stroke eraser: removes whole strokes the eraser touches
    Pan,
    Zoom,
};

[[nodiscard]] std::string_view toString(ToolKind tool) noexcept;

/// Display colours (from the UI's design tokens); none of them is stored in the document.
struct CanvasColors {
    core::Color desk = core::Color::fromRgba(0xE5, 0xE5, 0xE5);    ///< outside bounded pages
    core::Color pattern = core::Color::fromRgba(0xD4, 0xD4, 0xD4); ///< lines and dots
    core::Color selection = core::Color::fromRgba(0x52, 0x52, 0x52);
    core::Color marquee = core::Color::fromRgba(0x73, 0x73, 0x73);
    core::Color eraser = core::Color::fromRgba(0x73, 0x73, 0x73); ///< CursorShape::EraserRing
    render::ColorTransform contentTransform = render::ColorTransform::None;
};

struct CanvasStats {
    ToolKind tool = ToolKind::Pen;
    std::size_t sceneElements = 0;
    std::size_t visibleElements = 0; ///< broad-phase result for the viewport
    std::size_t drawItems = 0;       ///< content items submitted last frame
    std::size_t selected = 0;
    std::size_t livePoints = 0; ///< smoothed points of the stroke being drawn
    bool batched = false;       ///< last frame drew through RenderBatches
    std::size_t batches = 0;
    std::uint32_t batchesRebuilt = 0;
    RenderCache::Stats cache{};
};

/// The canvas engine (docs/CANVAS.md): routes plain input events to tools, owns the
/// camera, scene, render cache, selection and previews, commits every document change as
/// one Command through the DocumentPort, and turns the page into a render::RenderFrame.
///
/// Gesture routing: the middle button, or Space held, pans with any tool; a pen's eraser
/// end erases; the wheel zooms around the pointer (touchpad scrolling pans); every other
/// press goes to the active tool. One gesture produces at most one command (one undo step).
///
/// Rendering: with few visible elements every element is one draw item (its cached mesh
/// moved by its transform); with many (zoomed out) consecutive elements are drawn through
/// RenderBatches, one draw item per run. Both give the same painter's order.
///
/// The controller never mutates the document and has no undo stack of its own; it learns
/// about every applied patch (its own and undo/redo) through onDocumentChanged().
/// Not thread-safe; used on the GUI thread.
class CanvasController {
public:
    CanvasController(DocumentPort& document, core::IdGenerator& ids);
    ~CanvasController();
    CanvasController(const CanvasController&) = delete;
    CanvasController& operator=(const CanvasController&) = delete;
    CanvasController(CanvasController&&) = delete;
    CanvasController& operator=(CanvasController&&) = delete;

    // ---- document ------------------------------------------------------------------------
    /// Shows `page` (nullopt: nothing). Resets selection and previews and frames the page.
    void setPage(std::optional<core::PageId> page);
    [[nodiscard]] std::optional<core::PageId> page() const noexcept { return scene_.page(); }
    /// Must be called after every patch applied to the document.
    void onDocumentChanged(const document::Patch& patch);

    // ---- input ---------------------------------------------------------------------------
    void onPointer(const PointerEvent& event);
    void onWheel(const WheelEvent& event);
    void onZoomGesture(const ZoomGestureEvent& event);
    void onKey(const KeyEvent& event);
    /// Logical size and device pixel ratio of the canvas surface.
    void setViewport(const core::DVec2& logicalSize, double devicePixelRatio);

    void setTool(ToolKind tool);
    [[nodiscard]] ToolKind tool() const noexcept { return toolKind_; }
    [[nodiscard]] CursorShape cursor() const noexcept;
    [[nodiscard]] bool isGestureActive() const noexcept;

    [[nodiscard]] PenStyle& penStyle() noexcept { return pen_; }
    [[nodiscard]] StrokeOptions& strokeOptions() noexcept { return strokeOptions_; }

    // ---- actions -------------------------------------------------------------------------
    /// Deletes the selected elements as one command. No-op for an empty selection.
    [[nodiscard]] core::Result<void> deleteSelection();
    void selectAll();
    void clearSelection();
    void zoomBy(double factor);               ///< around the viewport centre
    void panBy(const core::DVec2& viewDelta); ///< content moves by viewDelta view pixels
    /// Infinite pages: zoom 1, origin at the top-left. Bounded pages: fit the page.
    void resetView();
    /// Fits everything on the page (bounded pages: the page) into the viewport.
    void zoomToFit();

    [[nodiscard]] const Camera& camera() const noexcept { return camera_; }
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] const CanvasScene& scene() const noexcept { return scene_; }
    /// The most recent failed commit (e.g. read-only workspace), if any.
    [[nodiscard]] const std::optional<core::Error>& lastError() const noexcept {
        return lastError_;
    }

    void setColors(const CanvasColors& colors) { colors_ = colors; }
    [[nodiscard]] const CanvasColors& colors() const noexcept { return colors_; }
    /// Called whenever the canvas needs repainting (input changed something). Hover moves
    /// without a gesture change nothing on the canvas and do not request a repaint.
    void setRedrawCallback(std::function<void()> callback) { redraw_ = std::move(callback); }

    // ---- rendering -----------------------------------------------------------------------
    /// Builds the frame for the current state. Creates, updates and destroys meshes on
    /// `renderer` (its context must be current). The returned spans stay valid until the
    /// next call.
    [[nodiscard]] render::RenderFrame buildFrame(render::Renderer& renderer);
    /// The graphics context was lost; every GPU handle is gone. CPU caches are kept.
    void onGraphicsReset() noexcept;

    [[nodiscard]] CanvasStats stats() const noexcept;
    /// Level-of-detail refinements per frame (RenderCache::setRefinementBudget).
    void setRefinementBudget(std::size_t perFrame) noexcept {
        cache_.setRefinementBudget(perFrame);
    }
    [[nodiscard]] core::Profiler& profiler() noexcept { return profiler_; }

private:
    [[nodiscard]] detail::Tool& toolFor(ToolKind kind) noexcept;
    void dispatch(detail::Tool& tool, const PointerEvent& event);
    void commit(core::Result<document::Command> command);
    void cancelGesture();
    void clampCamera() noexcept;
    void frameInitialView() noexcept;
    void requestRedraw() const;

    DocumentPort* document_;
    core::IdGenerator* ids_;
    Camera camera_;
    CanvasScene scene_;
    RenderCache cache_;
    RenderBatches batches_;
    bool lastFrameBatched_ = false;
    Selection selection_;
    std::unique_ptr<detail::Preview> preview_;
    std::array<std::unique_ptr<detail::Tool>, 5> tools_;
    ToolKind toolKind_ = ToolKind::Pen;
    detail::Tool* gestureTool_ = nullptr; ///< receives Move/Up until the gesture ends
    bool spaceHeld_ = false;
    bool viewportKnown_ = false;
    bool needsInitialView_ = true;
    PenStyle pen_;
    StrokeOptions strokeOptions_;
    CanvasColors colors_;
    std::optional<core::Error> lastError_;
    std::function<void()> redraw_;
    core::Profiler profiler_;

    // Frame storage (spans handed out by buildFrame point here).
    std::vector<CanvasScene::OrderedId> visible_;
    std::vector<render::DrawItem> content_;
    std::vector<render::DrawItem> overlay_;
    std::size_t lastDrawItems_ = 0;
    render::MeshHandle liveMesh_;
    render::MeshHandle selectionMesh_;
    render::MeshHandle marqueeMesh_;
};

} // namespace studyapp::canvas
