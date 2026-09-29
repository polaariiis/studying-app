#pragma once

#include <studyapp/canvas/Camera.hpp>
#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/DocumentPort.hpp>
#include <studyapp/canvas/DocumentRasterizer.hpp>
#include <studyapp/canvas/ImageSource.hpp>
#include <studyapp/canvas/Input.hpp>
#include <studyapp/canvas/RenderBatches.hpp>
#include <studyapp/canvas/RenderCache.hpp>
#include <studyapp/canvas/Selection.hpp>
#include <studyapp/canvas/StrokeBuilder.hpp>
#include <studyapp/canvas/TextLayout.hpp>
#include <studyapp/canvas/ToolSettings.hpp>
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
#include <unordered_map>
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
    Highlighter, ///< broad translucent ink (ToolSettings::highlighter)
    Shape,       ///< lines, arrows, rectangles, ellipses (ToolSettings::shape)
    Text,        ///< text boxes (edited in the UI's editor overlay)
    Connector,   ///< connectors between elements (ToolSettings::shape colour and width)
};

inline constexpr std::size_t kToolKindCount = 9;
static_assert(static_cast<std::size_t>(ToolKind::Connector) + 1 == kToolKindCount,
              "kToolKindCount must follow ToolKind");

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
    std::size_t textTextures = 0; ///< rasterised text boxes held as textures
    std::uint32_t textRasterizedLastFrame = 0;
    std::size_t imageTextures = 0; ///< decoded assets held as textures (one per asset)
    std::size_t imageTextureBytes = 0;
    std::uint32_t imagesDecodedLastFrame = 0;
    std::size_t documentTiles = 0; ///< document (PDF) tiles held as textures
    std::size_t documentTileBytes = 0;
    std::uint32_t documentTilesUploadedLastFrame = 0;
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

    /// What new strokes look like (sanitized; see ToolSettings): the pen's style for the
    /// pen, the highlighter's for the highlighter. Existing content and the stroke being
    /// drawn keep their style; nothing is written to the document and nothing is redrawn.
    void setToolSettings(const ToolSettings& settings);
    [[nodiscard]] const ToolSettings& toolSettings() const noexcept { return toolSettings_; }
    [[nodiscard]] PenStyle& penStyle() noexcept { return toolSettings_.pen; }
    [[nodiscard]] StrokeOptions& strokeOptions() noexcept { return strokeOptions_; }

    // ---- text (docs/CANVAS.md §9) ----------------------------------------------------------
    /// Lays out and rasterises text boxes (the UI's Qt implementation; not owned). Without
    /// one, text boxes are drawn as a neutral frame and heights use fallbackTextHeight().
    void setTextLayout(TextLayout* layout);
    /// Called when text editing starts or ends; the UI shows or hides its editor.
    void setTextEditHandler(std::function<void()> handler) {
        textEditHandler_ = std::move(handler);
    }
    /// The text box being edited (hidden on the canvas meanwhile), if any.
    [[nodiscard]] const std::optional<TextEdit>& textEdit() const noexcept { return textEdit_; }
    /// Starts editing (as the text tool does); ends any edit in progress without writing it.
    void beginTextEdit(TextEdit edit);
    /// Ends the edit and writes `text` (UTF-8) as one command: a new box (nothing if the
    /// text is blank), the edited box's new text and height, or its removal if cleared.
    core::Result<void> finishTextEdit(std::string text);
    /// Ends the edit without writing anything.
    void cancelTextEdit();
    /// Height the box being edited needs for `text` (the editor overlay follows it).
    [[nodiscard]] float textHeightFor(std::string_view text, float width) const;

    // ---- images (docs/CANVAS.md §10) -------------------------------------------------------
    /// Decodes image assets (the UI's implementation over the workspace; not owned).
    /// Without one, image elements are drawn as a neutral frame.
    void setImageSource(ImageSource* source);
    /// Inserts an image element for `asset` (already imported) whose pixels are
    /// `pixelSize`: at one world unit per pixel, scaled down to fit 60 % of the view,
    /// centred in the view, on the page's target layer, selected. One command.
    core::Result<void> insertImage(core::AssetId asset, const core::Vec2& pixelSize);

    // ---- document pages (PDF, Phase 8; docs/CANVAS.md §10) -----------------------------
    /// Renders the pages of imported documents shown behind page content (not owned).
    /// Without one, a document page shows only its paper.
    void setDocumentRasterizer(DocumentRasterizer* rasterizer);

    // ---- actions -------------------------------------------------------------------------
    /// Deletes the selected elements as one command. No-op for an empty selection.
    [[nodiscard]] core::Result<void> deleteSelection();
    void selectAll();
    /// Selects `element` (on the shown page) and centres the view on it unless it is already
    /// entirely in view (e.g. a search result). Returns false if it is not on this page.
    bool revealElement(core::ElementId element);

    // ---- clipboard (docs/CANVAS.md §7) ---------------------------------------------------
    // The canvas clipboard holds copies of elements of this controller's workspace (it is
    // not the system clipboard), so pasted images keep referencing assets the workspace has.
    /// Copies the selected elements (in painter order) to the clipboard. Returns whether
    /// anything was copied; an empty selection leaves the clipboard as it is.
    bool copySelection();
    /// Copies, then deletes the selection as one command.
    [[nodiscard]] core::Result<void> cutSelection();
    [[nodiscard]] bool canPaste() const noexcept { return !clipboard_.empty(); }
    /// Pastes the clipboard onto the current page's target layer as one command (new ids,
    /// connectors among the copies stay attached to each other) and selects the copies.
    /// On the page it was copied from, each paste is offset 16 view px further (a paste
    /// after a cut starts in place); on another page the copies keep their position. They
    /// are centred in the view instead when that would be out of view (on another page:
    /// not entirely in view).
    [[nodiscard]] core::Result<void> paste();
    void clearSelection();
    void zoomBy(double factor);               ///< around the viewport centre
    void panBy(const core::DVec2& viewDelta); ///< content moves by viewDelta view pixels
    /// Infinite pages: zoom 1, origin at the top-left. Bounded pages: fit the page.
    void resetView();
    /// Fits everything on the page (bounded pages: the page) into the viewport.
    void zoomToFit();
    /// Shows world point `center` at the viewport centre at `zoom` (clamped as any zoom;
    /// bounded pages stay in reach), instead of the initial view of the page — e.g. to
    /// return to where the user left a page. Call after setPage().
    void setView(const core::DVec2& center, double zoom);

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
    /// Deletes the selection as one command, labelled `label` (empty: the command's own).
    core::Result<void> removeSelection(std::string_view label);
    void cancelGesture();
    void clampCamera() noexcept;
    void frameInitialView() noexcept;
    void requestRedraw() const;
    void updateErasePreview(render::Renderer& renderer, int lodBucket);
    void endTextEdit();
    /// The texture of a text box at `pixelsPerUnit`, (re)rasterised when its content
    /// changed or a finer resolution is due (refinements within kTextRefinePixelBudget).
    render::TextureHandle textTexture(const document::Element& element, std::uint64_t version,
                                      float pixelsPerUnit, render::Renderer& renderer);

    DocumentPort* document_;
    core::IdGenerator* ids_;
    Camera camera_;
    CanvasScene scene_;
    RenderCache cache_;
    RenderBatches batches_;
    bool lastFrameBatched_ = false;
    Selection selection_;
    std::unique_ptr<detail::Preview> preview_;
    std::array<std::unique_ptr<detail::Tool>, kToolKindCount> tools_;
    ToolKind toolKind_ = ToolKind::Pen;
    detail::Tool* gestureTool_ = nullptr; ///< receives Move/Up until the gesture ends
    bool spaceHeld_ = false;
    bool viewportKnown_ = false;
    bool needsInitialView_ = true;
    ToolSettings toolSettings_;
    /// The style of the stroke being drawn (pen or highlighter, fixed at its start).
    PenStyle strokeStyle_;
    StrokeOptions strokeOptions_;
    CanvasColors colors_;
    std::optional<core::Error> lastError_;
    /// Clipboard: copied elements in painter order, their visual bounds, where they come
    /// from and how many times they were pasted on each page since the copy.
    std::vector<document::Element> clipboard_;
    core::DRect clipboardBounds_{};
    std::optional<core::PageId> clipboardPage_;
    bool clipboardCut_ = false;
    std::unordered_map<core::PageId, int> pastesOn_;
    std::function<void()> redraw_;
    core::Profiler profiler_;

    // Frame storage (spans handed out by buildFrame point here).
    std::vector<CanvasScene::OrderedId> visible_;
    std::vector<render::DrawItem> content_;
    std::vector<render::DrawItem> overlay_;
    std::size_t lastDrawItems_ = 0;
    render::MeshHandle liveMesh_;
    /// Meshes of what the partial eraser leaves of the strokes it is cutting.
    std::unordered_map<core::ElementId, render::MeshHandle> erasePreviewMeshes_;
    /// The shape being dragged (a fill and an outline at most).
    std::array<render::MeshHandle, 2> shapePreviewMeshes_{};
    /// Connectors following a move preview (rebuilt per frame: their ends move).
    std::unordered_map<core::ElementId, render::MeshHandle> followerMeshes_;
    /// The element being resized (up to its part count), re-used every frame.
    std::vector<render::MeshHandle> draftMeshes_;
    /// Selection handles: squares and their light centres.
    std::array<render::MeshHandle, 2> handleMeshes_{};
    /// The shape style of the gesture (fixed at its start).
    ShapeStyle shapeStyle_;

    // Images: one texture per asset, at the finest resolution a view has needed so far.
    struct ImageTexture {
        render::TextureHandle texture{};
        int side = 0;        ///< longer side loaded, pixels (0: not loaded yet)
        bool full = false;   ///< the asset's full resolution is loaded
        bool failed = false; ///< missing or unreadable: drawn as a frame, not retried
        std::uint64_t lastFrame = 0;
        std::size_t bytes = 0;
    };
    /// Pixels decoded per frame for finer image resolutions (new images always load).
    static constexpr double kImageRefinePixelBudget = 2'000'000.0;
    render::TextureHandle imageTexture(const document::Image& image, double devicePixelsPerUnit,
                                       render::Renderer& renderer);
    void releaseImageTextures();
    void trimImageTextures(render::Renderer& renderer);
    ImageSource* imageSource_ = nullptr;
    std::unordered_map<core::AssetId, ImageTexture> imageTextures_;
    std::size_t imageTextureBytes_ = 0;
    std::uint64_t frameNumber_ = 0;
    double imageRefinedPixels_ = 0.0;
    std::uint32_t imagesDecoded_ = 0;
    bool imageRefinementPending_ = false;

    // Document pages: tiles as textures, least recently drawn released beyond the budget.
    struct DocumentTile {
        render::TextureHandle texture{};
        bool failed = false; ///< the rasterizer could not render it: not retried
        std::uint64_t lastFrame = 0;
        std::size_t bytes = 0;
    };
    /// New tiles turned into textures per frame; the rest arrive in the next frames.
    static constexpr std::uint32_t kDocumentTileUploadsPerFrame = 8;
    /// Tile entries kept before those without a texture that are out of view are dropped.
    static constexpr std::size_t kDocumentTileEntries = 512;
    void drawDocumentPage(const document::PageInfo& page, render::Renderer& renderer);
    /// The tile's texture (invalid while it is being rendered, or if it failed).
    render::TextureHandle documentTile(const DocumentTileKey& key, render::Renderer& renderer);
    void releaseDocumentTiles();
    void trimDocumentTiles(render::Renderer& renderer);
    DocumentRasterizer* documents_ = nullptr;
    std::unordered_map<DocumentTileKey, DocumentTile, DocumentTileKeyHash> documentTiles_;
    std::size_t documentTileBytes_ = 0;
    std::uint32_t documentTilesUploaded_ = 0;
    bool documentTilesPending_ = false;
    std::vector<DocumentTileKey> wantedTiles_; ///< tiles asked for this frame (reused)

    // Text.
    struct TextTexture {
        std::uint64_t version = 0;
        float pixelsPerUnit = 0.0F;
        render::TextureHandle texture{};
    };
    /// Raster pixels spent per frame on resolution refinements of text (changed text is
    /// always rasterised at once; at least one refinement per frame). Measured: one
    /// 1.2 Mpx text raster took ≈ 36 ms in the release app, so refinements are spread
    /// over frames instead of stalling one.
    static constexpr double kTextRefinePixelBudget = 1'000'000.0;
    TextLayout* textLayout_ = nullptr;
    std::optional<TextEdit> textEdit_;
    std::function<void()> textEditHandler_;
    std::unordered_map<core::ElementId, TextTexture> textTextures_;
    std::vector<render::TextureHandle> pendingTextureDestroy_;
    std::uint32_t textRasterized_ = 0;
    double textRefinedPixels_ = 0.0;
    bool textRefinementPending_ = false;
    render::MeshHandle selectionMesh_;
    render::MeshHandle marqueeMesh_;
};

} // namespace studyapp::canvas
