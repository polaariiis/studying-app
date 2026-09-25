#include <studyapp/canvas/CanvasController.hpp>

#include "Tools.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/render/Tessellation.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace studyapp::canvas {

namespace {

constexpr double kFitMarginPx = 32.0;
constexpr double kCullMarginPx = 8.0;
/// Pixels of a bounded page that always stay in view while panning.
constexpr double kKeepVisiblePx = 48.0;
/// Wheel zoom per 1/8 degree of rotation: one 15° notch (120 units) zooms by ~1.2.
constexpr double kWheelZoomBase = 1.0015;
constexpr float kOverlayHalfWidthPx = 0.75F;
constexpr double kSelectionPaddingPx = 3.0;
/// Above this many visible elements, frames are drawn through RenderBatches: per-element
/// draw calls dominated the frame time at 10 000 visible strokes (measured, ROADMAP.md).
constexpr std::size_t kBatchingThreshold = 1024;

std::size_t indexOf(ToolKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

render::BackgroundPattern toRender(document::BackgroundPattern pattern) noexcept {
    switch (pattern) {
    case document::BackgroundPattern::None:
        return render::BackgroundPattern::None;
    case document::BackgroundPattern::Ruled:
        return render::BackgroundPattern::Ruled;
    case document::BackgroundPattern::Grid:
        return render::BackgroundPattern::Grid;
    case document::BackgroundPattern::Dots:
        return render::BackgroundPattern::Dots;
    }
    return render::BackgroundPattern::None;
}

double positiveModulo(double value, double modulus) noexcept {
    const double result = std::fmod(value, modulus);
    return result < 0.0 ? result + modulus : result;
}

core::Vec2 toFloat(const core::DVec2& v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y)};
}

/// Creates or refreshes a per-frame scratch mesh; returns false if there is nothing to draw.
bool uploadScratch(render::Renderer& renderer, render::MeshHandle& handle,
                   const render::MeshData& mesh) {
    if (mesh.empty()) {
        return false;
    }
    if (handle.isValid()) {
        renderer.updateMesh(handle, mesh);
    } else {
        handle = renderer.createMesh(mesh);
    }
    return handle.isValid();
}

core::Rect viewRectOf(const Camera& camera, const core::DRect& world) noexcept {
    return core::Rect::fromPoints(toFloat(camera.worldToView(world.min)),
                                  toFloat(camera.worldToView(world.max)));
}

} // namespace

std::string_view toString(ToolKind tool) noexcept {
    switch (tool) {
    case ToolKind::Pen:
        return "Pen";
    case ToolKind::Select:
        return "Select";
    case ToolKind::Eraser:
        return "Eraser";
    case ToolKind::Pan:
        return "Pan";
    case ToolKind::Zoom:
        return "Zoom";
    }
    return "Tool";
}

CanvasController::CanvasController(DocumentPort& document, core::IdGenerator& ids)
    : document_(&document), ids_(&ids), preview_(std::make_unique<detail::Preview>()) {
    tools_[indexOf(ToolKind::Pen)] = std::make_unique<detail::PenTool>();
    tools_[indexOf(ToolKind::Select)] = std::make_unique<detail::SelectTool>();
    tools_[indexOf(ToolKind::Eraser)] = std::make_unique<detail::EraserTool>();
    tools_[indexOf(ToolKind::Pan)] = std::make_unique<detail::PanTool>();
    tools_[indexOf(ToolKind::Zoom)] = std::make_unique<detail::ZoomTool>();
}

CanvasController::~CanvasController() = default;

detail::Tool& CanvasController::toolFor(ToolKind kind) noexcept {
    return *tools_[indexOf(kind)];
}

void CanvasController::requestRedraw() const {
    if (redraw_) {
        redraw_();
    }
}

// ---------------------------------------------------------------------------- document

void CanvasController::setPage(std::optional<core::PageId> page) {
    cancelGesture();
    selection_.clear();
    preview_->clear();
    scene_.rebuild(document_->workspace(), page);
    needsInitialView_ = true;
    frameInitialView();
    requestRedraw();
}

void CanvasController::onDocumentChanged(const document::Patch& patch) {
    {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "scene update");
        scene_.onPatch(document_->workspace(), patch);
    }
    selection_.retainIf([this](core::ElementId id) { return scene_.find(id) != nullptr; });
    std::erase_if(preview_->erased,
                  [this](core::ElementId id) { return scene_.find(id) == nullptr; });
    clampCamera();
    requestRedraw();
}

void CanvasController::commit(core::Result<document::Command> command) {
    if (!command) {
        lastError_ = command.error();
        core::logWarning("canvas", "edit rejected: " + command.error().message);
        return;
    }
    if (command->patch.empty()) {
        return;
    }
    if (auto executed = document_->execute(std::move(*command)); !executed) {
        lastError_ = executed.error();
        core::logWarning("canvas", "edit failed: " + executed.error().message);
        return;
    }
    lastError_.reset();
}

// ---------------------------------------------------------------------------- input

void CanvasController::dispatch(detail::Tool& tool, const PointerEvent& event) {
    detail::ToolContext context{
        .camera = camera_,
        .scene = scene_,
        .document = *document_,
        .selection = selection_,
        .preview = *preview_,
        .pen = pen_,
        .strokeOptions = strokeOptions_,
        .ids = *ids_,
        .commit = [this](core::Result<document::Command> command) { commit(std::move(command)); },
    };
    tool.onPointer(event, context);
}

void CanvasController::onPointer(const PointerEvent& event) {
    hoverView_ = event.viewPos;
    if (event.phase == PointerPhase::Down && gestureTool_ == nullptr) {
        if (event.button == PointerButton::Middle || spaceHeld_) {
            gestureTool_ = &toolFor(ToolKind::Pan);
        } else if (event.device == PointerDevice::Eraser) {
            gestureTool_ = &toolFor(ToolKind::Eraser);
        } else {
            gestureTool_ = &toolFor(toolKind_);
        }
        dispatch(*gestureTool_, event);
        if (!gestureTool_->isActive()) {
            gestureTool_ = nullptr; // e.g. a click that already completed, or was refused
        }
    } else if (gestureTool_ != nullptr) {
        dispatch(*gestureTool_, event);
        if (event.phase == PointerPhase::Up || event.phase == PointerPhase::Cancel ||
            !gestureTool_->isActive()) {
            gestureTool_ = nullptr;
        }
    } else if (toolKind_ == ToolKind::Eraser && event.phase == PointerPhase::Move) {
        dispatch(toolFor(ToolKind::Eraser), event); // hover: moves the eraser cursor only
    }
    clampCamera();
    requestRedraw();
}

void CanvasController::onWheel(const WheelEvent& event) {
    const bool pixelScroll = event.pixelDelta.x != 0.0 || event.pixelDelta.y != 0.0;
    if (pixelScroll && !event.modifiers.control) {
        camera_.panBy(event.pixelDelta); // touchpad two-finger scroll
    } else {
        const double notches = event.angleDelta.y != 0.0 ? event.angleDelta.y : event.pixelDelta.y;
        if (notches == 0.0) {
            return;
        }
        camera_.zoomAt(event.viewPos, std::pow(kWheelZoomBase, notches));
    }
    clampCamera();
    requestRedraw();
}

void CanvasController::onZoomGesture(const ZoomGestureEvent& event) {
    camera_.zoomAt(event.viewPos, event.scaleFactor);
    clampCamera();
    requestRedraw();
}

void CanvasController::onKey(const KeyEvent& event) {
    switch (event.key) {
    case Key::Space:
        if (!event.autoRepeat) {
            spaceHeld_ = event.pressed;
        }
        break;
    case Key::Escape:
        if (event.pressed) {
            if (gestureTool_ != nullptr) {
                cancelGesture();
            } else {
                selection_.clear();
            }
        }
        break;
    case Key::Delete:
        if (event.pressed && !event.autoRepeat) {
            (void)deleteSelection();
        }
        break;
    case Key::Other:
        break;
    }
    requestRedraw();
}

void CanvasController::setViewport(const core::DVec2& logicalSize, double devicePixelRatio) {
    camera_.setViewport(logicalSize, devicePixelRatio);
    viewportKnown_ = true;
    frameInitialView();
    clampCamera();
    requestRedraw();
}

void CanvasController::setTool(ToolKind tool) {
    if (tool == toolKind_) {
        return;
    }
    cancelGesture();
    preview_->eraserCenter.reset();
    toolKind_ = tool;
    requestRedraw();
}

CursorShape CanvasController::cursor() const noexcept {
    if (gestureTool_ != nullptr) {
        return gestureTool_->cursor();
    }
    if (spaceHeld_) {
        return CursorShape::OpenHand;
    }
    return tools_[indexOf(toolKind_)]->cursor();
}

bool CanvasController::isGestureActive() const noexcept {
    return gestureTool_ != nullptr;
}

void CanvasController::cancelGesture() {
    if (gestureTool_ != nullptr) {
        detail::ToolContext context{
            .camera = camera_,
            .scene = scene_,
            .document = *document_,
            .selection = selection_,
            .preview = *preview_,
            .pen = pen_,
            .strokeOptions = strokeOptions_,
            .ids = *ids_,
            .commit = [](core::Result<document::Command>) {},
        };
        gestureTool_->cancel(context);
        gestureTool_ = nullptr;
    }
}

// ---------------------------------------------------------------------------- actions

core::Result<void> CanvasController::deleteSelection() {
    if (selection_.empty()) {
        return {};
    }
    const std::vector<core::ElementId> ids(selection_.ids().begin(), selection_.ids().end());
    auto command = document::commands::deleteElements(document_->workspace(), ids);
    if (!command) {
        return tl::unexpected(command.error());
    }
    if (auto executed = document_->execute(std::move(*command)); !executed) {
        lastError_ = executed.error();
        return executed;
    }
    selection_.clear();
    requestRedraw();
    return {};
}

void CanvasController::selectAll() {
    std::vector<core::ElementId> ids;
    for (const core::ElementId id : scene_.drawOrder()) {
        const SceneEntry* entry = scene_.find(id);
        if (entry != nullptr && entry->layerVisible && !entry->layerLocked) {
            ids.push_back(id);
        }
    }
    selection_.set(std::move(ids));
    requestRedraw();
}

void CanvasController::clearSelection() {
    selection_.clear();
    requestRedraw();
}

void CanvasController::zoomBy(double factor) {
    camera_.zoomAt(camera_.viewportSize() * 0.5, factor);
    clampCamera();
    requestRedraw();
}

void CanvasController::panBy(const core::DVec2& viewDelta) {
    camera_.panBy(viewDelta);
    clampCamera();
    requestRedraw();
}

void CanvasController::resetView() {
    needsInitialView_ = true;
    frameInitialView();
    requestRedraw();
}

void CanvasController::zoomToFit() {
    core::DRect content = core::DRect::emptyBounds();
    for (const core::ElementId id : scene_.drawOrder()) {
        if (const SceneEntry* entry = scene_.find(id); entry != nullptr && entry->layerVisible) {
            content = content.united(entry->bounds);
        }
    }
    const auto page = scene_.page();
    const document::PageInfo* info = page ? document_->workspace().findPage(*page) : nullptr;
    if (info != nullptr && info->extent == document::PageExtent::Bounded) {
        content = content.united(core::DRect::fromOriginSize({0.0, 0.0}, info->size));
    }
    if (!content.isEmpty()) {
        camera_.fitRect(content, kFitMarginPx);
        clampCamera();
        requestRedraw();
    }
}

void CanvasController::frameInitialView() noexcept {
    if (!needsInitialView_ || !viewportKnown_) {
        return;
    }
    needsInitialView_ = false;
    const auto page = scene_.page();
    const document::PageInfo* info = page ? document_->workspace().findPage(*page) : nullptr;
    if (info != nullptr && info->extent == document::PageExtent::Bounded) {
        camera_.fitRect(core::DRect::fromOriginSize({0.0, 0.0}, info->size), kFitMarginPx);
    } else {
        camera_.reset();
    }
}

void CanvasController::clampCamera() noexcept {
    const auto page = scene_.page();
    const document::PageInfo* info = page ? document_->workspace().findPage(*page) : nullptr;
    if (info == nullptr || info->extent != document::PageExtent::Bounded) {
        return;
    }
    // Keep at least kKeepVisiblePx of the page on screen along each axis.
    const core::DVec2 half = camera_.viewportSize() * (0.5 / camera_.zoom());
    const double keep = camera_.viewToWorldLength(kKeepVisiblePx);
    const auto clampAxis = [&](double centre, double size, double halfView) {
        const double low = -halfView + keep;
        const double high = size + halfView - keep;
        return low > high ? size * 0.5 : std::clamp(centre, low, high);
    };
    const core::DVec2 centre = camera_.center();
    camera_.setCenter(
        {clampAxis(centre.x, info->size.x, half.x), clampAxis(centre.y, info->size.y, half.y)});
}

// ---------------------------------------------------------------------------- rendering

void CanvasController::onGraphicsReset() noexcept {
    cache_.forgetGpuResources();
    batches_.forgetGpuResources();
    liveMesh_ = {};
    selectionMesh_ = {};
    marqueeMesh_ = {};
    eraserMesh_ = {};
}

render::RenderFrame CanvasController::buildFrame(render::Renderer& renderer) {
    STUDYAPP_PROFILE_SCOPE(&profiler_, "build frame");
    cache_.beginFrame();
    for (const core::ElementId id : scene_.takeRemoved()) {
        cache_.evict(id);
    }
    cache_.flush(renderer);
    content_.clear();
    overlay_.clear();

    render::RenderFrame frame;
    frame.viewportSize = toFloat(camera_.viewportSize());
    frame.devicePixelRatio = static_cast<float>(camera_.devicePixelRatio());
    frame.zoom = static_cast<float>(camera_.zoom());
    frame.contentColorTransform = colors_.contentTransform;

    const document::Workspace& workspace = document_->workspace();
    const auto page = scene_.page();
    const document::PageInfo* info = page ? workspace.findPage(*page) : nullptr;
    const core::DVec2 centre = camera_.center();

    // Background: page colour and pattern are document data; desk/pattern colours are UI.
    frame.background.deskColor = colors_.desk;
    frame.background.patternColor = colors_.pattern;
    if (info != nullptr) {
        const double spacing = static_cast<double>(info->background.spacing);
        frame.background.paperColor = info->background.color;
        frame.background.pattern = toRender(info->background.pattern);
        frame.background.spacing = info->background.spacing;
        frame.background.patternPhase =
            toFloat({positiveModulo(centre.x, spacing), positiveModulo(centre.y, spacing)});
        frame.background.bounded = info->extent == document::PageExtent::Bounded;
        frame.background.pageRect = core::Rect::fromPoints(toFloat(core::DVec2{0.0, 0.0} - centre),
                                                           toFloat(info->size - centre));
    } else {
        frame.background.paperColor = colors_.desk;
    }

    // Content: broad phase on the viewport, painter's order, cached meshes.
    visible_.clear();
    {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "scene query");
        scene_.queryOrdered(
            camera_.visibleWorldRect().expanded(camera_.viewToWorldLength(kCullMarginPx)),
            visible_);
    }
    const int lod = RenderCache::lodBucketFor(camera_.zoom());
    const core::Affine2 toCameraRelative = core::Affine2::translation(-centre);
    const core::Affine2 moveOffset = core::Affine2::translation(preview_->moveOffset);
    const auto drawElement = [&](core::ElementId id) {
        if (preview_->erased.contains(id)) {
            return;
        }
        const document::Element* element = workspace.findElement(id);
        const SceneEntry* entry = scene_.find(id);
        if (element == nullptr || entry == nullptr) {
            return;
        }
        const RenderCache::Entry& cached =
            cache_.ensure(*element, entry->contentVersion, lod, &renderer);
        core::Affine2 toWorld = meshToWorld(*element);
        if (preview_->moving && selection_.contains(id)) {
            toWorld = moveOffset * toWorld;
        }
        const core::Affine2f transform = (toCameraRelative * toWorld).cast<float>();
        for (std::size_t i = 0; i < cached.parts.size() && i < cached.gpu.size(); ++i) {
            if (cached.gpu[i].isValid()) {
                content_.push_back({.mesh = cached.gpu[i],
                                    .transform = transform,
                                    .color = cached.parts[i].color,
                                    .opacity = entry->layerOpacity});
            }
        }
    };
    lastFrameBatched_ = visible_.size() > kBatchingThreshold;
    {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "prepare content");
        if (!lastFrameBatched_) {
            for (const CanvasScene::OrderedId& item : visible_) {
                drawElement(item.id);
            }
        } else {
            batches_.update(scene_, workspace, cache_, lod, renderer);
            // visible_ is in draw order; walk it alongside the (draw-ordered) batches.
            std::size_t v = 0;
            for (const RenderBatches::Batch& batch : batches_.batches()) {
                while (v < visible_.size() && visible_[v].drawIndex < batch.firstDrawIndex) {
                    ++v;
                }
                const std::size_t first = v;
                while (v < visible_.size() && visible_[v].drawIndex <= batch.lastDrawIndex) {
                    ++v;
                }
                if (first == v) {
                    continue; // no member of this run is in view
                }
                // Runs touched by a preview (move, erase) are drawn element by element, so
                // previews never force a rebuild.
                const bool anyPreview = !preview_->erased.empty() || preview_->moving;
                const bool previewed =
                    anyPreview &&
                    std::any_of(batch.members.begin(), batch.members.end(),
                                [&](core::ElementId id) {
                                    return preview_->erased.contains(id) ||
                                           (preview_->moving && selection_.contains(id));
                                });
                if (previewed || !batch.gpu.isValid()) {
                    for (std::size_t i = first; i < v; ++i) {
                        drawElement(visible_[i].id);
                    }
                    continue;
                }
                content_.push_back(
                    {.mesh = batch.gpu,
                     .transform = (toCameraRelative * core::Affine2::translation(batch.origin))
                                      .cast<float>(),
                     .color = core::Color::white(),
                     .opacity = batch.opacity});
            }
        }
    }

    // Live stroke: tessellated every frame from the smoothed samples, camera-relative.
    if (preview_->stroke) {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "live stroke");
        const document::Stroke style{
            .brush = pen_.brush, .color = pen_.color, .baseWidth = pen_.width, .points = {}};
        std::vector<render::WidthPoint> points;
        points.reserve(preview_->stroke->points().size());
        for (const StrokeSample& sample : preview_->stroke->points()) {
            points.push_back(
                {toFloat(sample.world - centre), strokeRadius(style, sample.pressure)});
        }
        const render::MeshData mesh = render::tessellatePolyline(
            points, {.pixelsPerUnit = static_cast<float>(camera_.zoom())});
        if (uploadScratch(renderer, liveMesh_, mesh)) {
            content_.push_back({.mesh = liveMesh_, .transform = {}, .color = pen_.color});
        }
    }

    // Overlays in view space.
    if (!selection_.empty()) {
        render::MeshData mesh;
        const core::DVec2 offset = preview_->moving ? preview_->moveOffset : core::DVec2{};
        for (const core::ElementId id : selection_.ids()) {
            if (const SceneEntry* entry = scene_.find(id)) {
                const core::Rect view = viewRectOf(camera_, entry->bounds.translated(offset));
                render::appendMesh(mesh, render::tessellateRectOutline(
                                             view.expanded(static_cast<float>(kSelectionPaddingPx)),
                                             kOverlayHalfWidthPx));
            }
        }
        if (uploadScratch(renderer, selectionMesh_, mesh)) {
            overlay_.push_back(
                {.mesh = selectionMesh_, .transform = {}, .color = colors_.selection});
        }
    }
    if (preview_->marquee) {
        const render::MeshData mesh = render::tessellateRectOutline(
            viewRectOf(camera_, *preview_->marquee), kOverlayHalfWidthPx);
        if (uploadScratch(renderer, marqueeMesh_, mesh)) {
            overlay_.push_back({.mesh = marqueeMesh_, .transform = {}, .color = colors_.marquee});
        }
    }
    if (toolKind_ == ToolKind::Eraser && hoverView_) {
        const auto radius = static_cast<float>(detail::EraserTool::kRadiusViewPx);
        const core::Vec2 c = toFloat(*hoverView_);
        const auto circle = render::ellipsePolygon(
            core::Rect{c - core::Vec2{radius, radius}, c + core::Vec2{radius, radius}});
        std::vector<render::WidthPoint> ring;
        for (const core::Vec2& p : circle) {
            ring.push_back({p, kOverlayHalfWidthPx});
        }
        if (!circle.empty()) {
            ring.push_back({circle.front(), kOverlayHalfWidthPx});
        }
        if (uploadScratch(renderer, eraserMesh_, render::tessellatePolyline(ring))) {
            overlay_.push_back({.mesh = eraserMesh_, .transform = {}, .color = colors_.eraser});
        }
    }

    lastDrawItems_ = content_.size();
    frame.content = content_;
    frame.overlay = overlay_;
    return frame;
}

CanvasStats CanvasController::stats() const noexcept {
    return {.tool = toolKind_,
            .sceneElements = scene_.elementCount(),
            .visibleElements = visible_.size(),
            .drawItems = lastDrawItems_,
            .selected = selection_.size(),
            .livePoints = preview_->stroke ? preview_->stroke->points().size() : 0,
            .batched = lastFrameBatched_,
            .batches = batches_.batches().size(),
            .batchesRebuilt = batches_.rebuiltLastUpdate(),
            .cache = cache_.stats()};
}

} // namespace studyapp::canvas
