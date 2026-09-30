#include <studyapp/canvas/CanvasController.hpp>

#include "Tools.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/canvas/SelectionHandles.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/render/Tessellation.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
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

/// Raster resolution for text at `devicePixelsPerUnit`: the next power of √2 at or above it,
/// so a raster is re-made only when the zoom moves by a √2 step and is never more than
/// 1.41× (in each direction) finer than the screen needs.
float textPixelsPerUnit(double devicePixelsPerUnit) noexcept {
    if (!(devicePixelsPerUnit > 0.0) || !std::isfinite(devicePixelsPerUnit)) {
        return 1.0F;
    }
    const double halfSteps = std::ceil(2.0 * std::log2(devicePixelsPerUnit) - 1e-9);
    return static_cast<float>(std::exp2(std::clamp(halfSteps, -16.0, 16.0) / 2.0));
}

/// Whether `patch` changes what the canvas of `page` shows: elements, layers, or the page's
/// format and background. Study records (tasks, tags...) and other pages' titles do not.
bool changesCanvas(const document::Patch& patch, std::optional<core::PageId> page) {
    for (const document::AnyChange& change : patch.changes()) {
        const bool visible = std::visit(
            [&](const auto& c) {
                using Change = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<Change, document::ElementChange> ||
                              std::is_same_v<Change, document::LayerChange>) {
                    return true; // the scene decides; cheap either way
                } else if constexpr (std::is_same_v<Change, document::PageChange>) {
                    const auto& record = c.after ? *c.after : *c.before;
                    if (!page || record.id != *page) {
                        return false;
                    }
                    return !c.before || !c.after || c.before->extent != c.after->extent ||
                           c.before->size != c.after->size ||
                           !(c.before->background == c.after->background);
                } else {
                    return false;
                }
            },
            change);
        if (visible) {
            return true;
        }
    }
    return false;
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
    case ToolKind::Highlighter:
        return "Highlighter";
    case ToolKind::Shape:
        return "Shape";
    case ToolKind::Text:
        return "Text";
    case ToolKind::Connector:
        return "Connector";
    }
    return "Tool";
}

CanvasController::CanvasController(DocumentPort& document, core::IdGenerator& ids)
    : document_(&document), ids_(&ids), preview_(std::make_unique<detail::Preview>()) {
    cache_.setRefinementBudget(RenderCache::kDefaultRefinementBudget);
    tools_[indexOf(ToolKind::Pen)] = std::make_unique<detail::PenTool>();
    tools_[indexOf(ToolKind::Select)] = std::make_unique<detail::SelectTool>();
    tools_[indexOf(ToolKind::Eraser)] = std::make_unique<detail::EraserTool>();
    tools_[indexOf(ToolKind::Pan)] = std::make_unique<detail::PanTool>();
    tools_[indexOf(ToolKind::Zoom)] = std::make_unique<detail::ZoomTool>();
    tools_[indexOf(ToolKind::Highlighter)] =
        std::make_unique<detail::PenTool>(CursorShape::Highlighter);
    tools_[indexOf(ToolKind::Shape)] = std::make_unique<detail::ShapeTool>();
    tools_[indexOf(ToolKind::Text)] = std::make_unique<detail::TextTool>();
    tools_[indexOf(ToolKind::Connector)] = std::make_unique<detail::ConnectorTool>();
}

CanvasController::~CanvasController() {
    if (imageSource_ != nullptr) {
        imageSource_->setReadyHandler({}); // the source may outlive the controller
    }
    if (documents_ != nullptr) {
        documents_->setReadyHandler({});
    }
}

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
    endTextEdit();
    releaseImageTextures(); // another page shows other images
    releaseDocumentTiles();
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
    std::erase_if(preview_->partial,
                  [this](const auto& entry) { return scene_.find(entry.first) == nullptr; });
    if (textEdit_ && textEdit_->element && scene_.find(*textEdit_->element) == nullptr) {
        endTextEdit(); // the box being edited is gone (e.g. undo of its creation)
    }
    if (changesCanvas(patch, scene_.page())) {
        clampCamera();
        requestRedraw(); // planner edits (tasks, tags) cost no canvas frame
    }
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
    STUDYAPP_PROFILE_SCOPE(&profiler_, "tool");
    detail::ToolContext context{
        .camera = camera_,
        .scene = scene_,
        .document = *document_,
        .selection = selection_,
        .preview = *preview_,
        .pen = strokeStyle_,
        .eraserMode = toolSettings_.eraser,
        .shape = shapeStyle_,
        .strokeOptions = strokeOptions_,
        .ids = *ids_,
        .commit = [this](core::Result<document::Command> command) { commit(std::move(command)); },
        .beginTextEdit = [this](TextEdit edit) { beginTextEdit(std::move(edit)); },
        .textHeight = [this](std::string_view text, float width,
                             float fontSize) { return textHeightFor(text, width, fontSize); },
        .textSize = toolSettings_.textSize,
    };
    tool.onPointer(event, context);
}

void CanvasController::setToolSettings(const ToolSettings& settings) {
    toolSettings_ = sanitized(settings);
}

void CanvasController::onPointer(const PointerEvent& event) {
    if (gestureTool_ == nullptr && event.phase != PointerPhase::Down) {
        // Hovering (or a stray release/cancel): nothing on the canvas follows the pointer
        // (the eraser ring is the platform cursor), so there is nothing to redraw.
        return;
    }
    if (event.phase == PointerPhase::Down && gestureTool_ == nullptr) {
        // A stroke keeps the style it started with, even if the settings change mid-way.
        // (penStyle() hands out the settings for direct edits, so they are sanitized here.)
        const ToolSettings settings = sanitized(toolSettings_);
        strokeStyle_ = toolKind_ == ToolKind::Highlighter ? settings.highlighter : settings.pen;
        shapeStyle_ = settings.shape;
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
    // Only keys that change what the canvas shows ask for a frame. Space only arms panning
    // (a cursor change), and the widget reports it released on every focus loss.
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
                requestRedraw();
            } else if (!selection_.empty()) {
                selection_.clear();
                requestRedraw();
            }
        }
        break;
    case Key::Delete:
        if (event.pressed && !event.autoRepeat) {
            (void)deleteSelection(); // repaints when something was deleted
        }
        break;
    case Key::Other:
        break;
    }
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
            .pen = strokeStyle_,
            .eraserMode = toolSettings_.eraser,
            .shape = shapeStyle_,
            .strokeOptions = strokeOptions_,
            .ids = *ids_,
            .commit = [](core::Result<document::Command>) {},
            .beginTextEdit = [](TextEdit) {},
            .textHeight = [](std::string_view text, float,
                             float fontSize) { return fallbackTextHeight(text, fontSize); },
            .textSize = toolSettings_.textSize,
        };
        gestureTool_->cancel(context);
        gestureTool_ = nullptr;
    }
}

// ---------------------------------------------------------------------------- actions

core::Result<void> CanvasController::deleteSelection() {
    return removeSelection({});
}

core::Result<void> CanvasController::removeSelection(std::string_view label) {
    if (selection_.empty()) {
        return {};
    }
    const std::vector<core::ElementId> ids(selection_.ids().begin(), selection_.ids().end());
    auto command = document::commands::deleteElements(document_->workspace(), ids);
    if (!command) {
        return tl::unexpected(command.error());
    }
    if (!label.empty()) {
        command->label = std::string(label);
    }
    if (auto executed = document_->execute(std::move(*command)); !executed) {
        lastError_ = executed.error();
        return executed;
    }
    selection_.clear();
    requestRedraw();
    return {};
}

bool CanvasController::copySelection() {
    if (selection_.empty()) {
        return false;
    }
    const document::Workspace& workspace = document_->workspace();
    std::vector<document::Element> copied;
    copied.reserve(selection_.size());
    core::DRect bounds{};
    for (const core::ElementId id : scene_.drawOrder()) { // painter order, O(page)
        if (!selection_.contains(id)) {
            continue;
        }
        if (const document::Element* element = workspace.findElement(id)) {
            const core::DRect box = visualBounds(*element);
            bounds = copied.empty() ? box : bounds.united(box);
            copied.push_back(*element);
        }
    }
    if (copied.empty()) {
        return false;
    }
    clipboard_ = std::move(copied);
    clipboardBounds_ = bounds;
    clipboardPage_ = scene_.page();
    clipboardCut_ = false;
    pastesOn_.clear();
    return true;
}

core::Result<void> CanvasController::cutSelection() {
    if (document_->isReadOnly()) {
        return core::makeError(core::ErrorCode::InvalidArgument, "the workspace is read-only");
    }
    if (!copySelection()) {
        return {};
    }
    clipboardCut_ = true;
    return removeSelection("Cut");
}

core::Result<void> CanvasController::paste() {
    const auto page = scene_.page();
    const document::Workspace& workspace = document_->workspace();
    const auto layer = page ? detail::targetLayer(workspace, *page) : std::nullopt;
    if (clipboard_.empty() || !layer || document_->isReadOnly()) {
        return core::makeError(core::ErrorCode::InvalidArgument, "nothing to paste here");
    }
    const int pasted = pastesOn_[*page];
    const int steps = page == clipboardPage_ && !clipboardCut_ ? pasted + 1 : pasted;
    constexpr double kPasteStepViewPx = 16.0;
    const double step = camera_.viewToWorldLength(kPasteStepViewPx);
    core::DVec2 offset{step * steps, step * steps};
    const core::DRect view = camera_.visibleWorldRect();
    // On the source page the copies may overlap the view edge (they sit next to their
    // originals); elsewhere there is no reference position, so they must be fully visible.
    const core::DRect placed = clipboardBounds_.translated(offset);
    if (page == clipboardPage_ ? !view.intersects(placed) : !view.contains(placed)) {
        offset = view.center() - clipboardBounds_.center();
    }
    auto created = document::commands::pasteElements(workspace, *layer, clipboard_, offset, *ids_);
    if (!created) {
        return tl::unexpected(created.error());
    }
    std::vector<core::ElementId> copies = std::move(created->id);
    if (auto executed = document_->execute(std::move(created->command)); !executed) {
        lastError_ = executed.error();
        return executed;
    }
    lastError_.reset();
    ++pastesOn_[*page];
    selection_.set(std::move(copies));
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

bool CanvasController::revealElement(core::ElementId element) {
    const SceneEntry* entry = scene_.find(element);
    if (entry == nullptr) {
        return false;
    }
    selection_.set({element});
    if (!camera_.visibleWorldRect().contains(entry->bounds)) {
        camera_.setCenter(entry->bounds.center());
        clampCamera();
    }
    requestRedraw();
    return true;
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

void CanvasController::setView(const core::DVec2& center, double zoom) {
    camera_.setZoom(zoom);
    camera_.setCenter(center);
    needsInitialView_ = false; // also when the viewport is not known yet
    clampCamera();
    requestRedraw();
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

void CanvasController::updateErasePreview(render::Renderer& renderer, int lodBucket) {
    // Meshes of strokes the partial eraser cut: rebuilt only for strokes whose pieces
    // changed since the last frame (O(points of those strokes)); released once the
    // gesture ends. Everything else keeps its cached mesh.
    for (auto it = erasePreviewMeshes_.begin(); it != erasePreviewMeshes_.end();) {
        if (!preview_->partial.contains(it->first)) {
            renderer.destroyMesh(it->second);
            it = erasePreviewMeshes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [id, erased] : preview_->partial) {
        if (!erased.dirty) {
            continue;
        }
        erased.dirty = false;
        const document::Element* element = document_->workspace().findElement(id);
        const auto* stroke =
            element != nullptr ? std::get_if<document::Stroke>(&element->payload) : nullptr;
        if (stroke == nullptr) {
            continue;
        }
        render::MeshData mesh;
        for (const StrokePiece& piece : erased.pieces) {
            render::appendMesh(mesh, tessellateStrokePoints(*stroke, piece.points,
                                                            RenderCache::pixelsPerUnit(lodBucket)));
        }
        render::MeshHandle& handle = erasePreviewMeshes_[id];
        if (mesh.empty()) {
            if (handle.isValid()) {
                renderer.destroyMesh(handle);
            }
            erasePreviewMeshes_.erase(id);
        } else if (!uploadScratch(renderer, handle, mesh)) {
            erasePreviewMeshes_.erase(id);
        }
    }
}

// ---------------------------------------------------------------------------- text

void CanvasController::setTextLayout(TextLayout* layout) {
    if (layout == textLayout_) {
        return;
    }
    textLayout_ = layout;
    for (const auto& [id, cached] : textTextures_) {
        pendingTextureDestroy_.push_back(cached.texture);
    }
    textTextures_.clear();
    requestRedraw();
}

void CanvasController::beginTextEdit(TextEdit edit) {
    textEdit_ = std::move(edit);
    requestRedraw();
    if (textEditHandler_) {
        textEditHandler_();
    }
}

void CanvasController::endTextEdit() {
    if (!textEdit_) {
        return;
    }
    textEdit_.reset();
    requestRedraw();
    if (textEditHandler_) {
        textEditHandler_();
    }
}

void CanvasController::cancelTextEdit() {
    endTextEdit();
}

float CanvasController::textHeightFor(std::string_view text, float width, float fontSize) const {
    return textLayout_ != nullptr ? textLayout_->heightFor(text, width, fontSize)
                                  : fallbackTextHeight(text, fontSize);
}

core::Result<void> CanvasController::applyTextFontSize(float fontSize) {
    const float size = sanitizedTextFontSize(fontSize);
    if (textEdit_) {
        if (textEdit_->fontSize != size) {
            textEdit_->fontSize = size; // the UI's editor follows it (CanvasWidget)
            requestRedraw();
        }
        return {};
    }
    const document::Workspace& workspace = document_->workspace();
    std::vector<document::commands::TextFontSizeChange> changes;
    for (const core::ElementId id : selection_.ids()) {
        const document::Element* element = workspace.findElement(id);
        const auto* box =
            element != nullptr ? std::get_if<document::TextBox>(&element->payload) : nullptr;
        if (box == nullptr || box->fontSize == size) {
            continue;
        }
        changes.push_back({.element = id,
                           .fontSize = size,
                           .size = {box->size.x, textHeightFor(box->text, box->size.x, size)}});
    }
    if (changes.empty()) {
        return {};
    }
    lastError_.reset();
    commit(document::commands::setTextFontSize(workspace, changes));
    return lastError_ ? core::Result<void>{tl::unexpected(*lastError_)} : core::Result<void>{};
}

core::Result<void> CanvasController::finishTextEdit(std::string text) {
    if (!textEdit_) {
        return {};
    }
    const TextEdit edit = std::move(*textEdit_);
    endTextEdit();
    const bool blank = std::all_of(text.begin(), text.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    });
    const document::Workspace& workspace = document_->workspace();
    core::Result<document::Command> command = document::Command{};
    if (edit.element) {
        if (workspace.findElement(*edit.element) == nullptr) {
            return {};
        }
        command = blank ? document::commands::deleteElement(workspace, *edit.element)
                        : document::commands::editText(
                              workspace, *edit.element, text,
                              {edit.width, textHeightFor(text, edit.width, edit.fontSize)},
                              edit.fontSize);
    } else {
        if (blank) {
            return {}; // an empty new box is not created
        }
        const auto page = scene_.page();
        const auto layer = page ? detail::targetLayer(workspace, *page) : std::nullopt;
        if (!layer) {
            return {};
        }
        const core::Vec2 size{edit.width, textHeightFor(text, edit.width, edit.fontSize)};
        auto created = document::commands::createElement(
            workspace, *layer,
            {.transform = {.position = edit.position},
             .payload = document::TextBox{.size = size,
                                          .text = std::move(text),
                                          .fontSize = edit.fontSize}},
            *ids_);
        if (!created) {
            return tl::unexpected(created.error());
        }
        command = std::move(created->command);
    }
    if (!command) {
        return tl::unexpected(command.error());
    }
    commit(std::move(command));
    return lastError_ ? core::Result<void>{tl::unexpected(*lastError_)} : core::Result<void>{};
}

render::TextureHandle CanvasController::textTexture(const document::Element& element,
                                                    std::uint64_t version, float pixelsPerUnit,
                                                    render::Renderer& renderer) {
    const auto& box = std::get<document::TextBox>(element.payload);
    if (box.text.empty() || box.size.x <= 0.0F || box.size.y <= 0.0F) {
        return {};
    }
    // Never more than kMaxTextRasterPx per side: the resolution is capped instead.
    const float cap = static_cast<float>(kMaxTextRasterPx) / std::max(box.size.x, box.size.y);
    const float ppu = std::min(pixelsPerUnit, cap);
    auto [it, inserted] = textTextures_.try_emplace(element.id);
    TextTexture& cached = it->second;
    // The size is compared too: a resize preview has the element's content version but
    // another size, and its text wraps differently.
    const bool changed = inserted || cached.version != version || cached.size != box.size ||
                         cached.fontSize != box.fontSize || !cached.texture.isValid();
    bool refine = !changed && cached.pixelsPerUnit < ppu * 0.999F;
    const double cost = static_cast<double>(box.size.x) * static_cast<double>(box.size.y) *
                        static_cast<double>(ppu) * static_cast<double>(ppu);
    if (refine && textRefinedPixels_ > 0.0 && textRefinedPixels_ + cost > kTextRefinePixelBudget) {
        refine = false; // keep the coarser raster (stretched) this frame
        textRefinementPending_ = true;
    }
    if (changed || refine) {
        if (cached.texture.isValid()) {
            renderer.destroyTexture(cached.texture);
        }
        cached.texture =
            renderer.createTexture(textLayout_->rasterize(box.text, box.size, box.fontSize, ppu));
        cached.version = version;
        cached.size = box.size;
        cached.fontSize = box.fontSize;
        cached.pixelsPerUnit = ppu;
        ++textRasterized_;
        if (refine) {
            textRefinedPixels_ += cost;
        }
    }
    return cached.texture;
}

// ---------------------------------------------------------------------------- images

void CanvasController::setImageSource(ImageSource* source) {
    if (source == imageSource_) {
        return;
    }
    if (imageSource_ != nullptr) {
        imageSource_->setReadyHandler({});
    }
    imageSource_ = source;
    if (imageSource_ != nullptr) {
        imageSource_->setReadyHandler([this] { requestRedraw(); }); // decoded: draw it
    }
    releaseImageTextures();
    requestRedraw();
}

void CanvasController::releaseImageTextures() {
    for (const auto& [asset, cached] : imageTextures_) {
        pendingTextureDestroy_.push_back(cached.texture);
    }
    imageTextures_.clear();
    imageTextureBytes_ = 0;
}

render::TextureHandle CanvasController::imageTexture(const document::Image& image,
                                                     double devicePixelsPerUnit,
                                                     render::Renderer& renderer) {
    // The resolution this view needs: the image's longer side on screen, rounded up to a
    // power of two (so zooming re-decodes only per factor 2), within the texture limit.
    const double onScreen =
        static_cast<double>(std::max(image.size.x, image.size.y)) * devicePixelsPerUnit;
    int needed = 64;
    while (needed < kMaxImageTexturePx && static_cast<double>(needed) < onScreen) {
        needed *= 2;
    }
    auto [it, inserted] = imageTextures_.try_emplace(image.asset);
    ImageTexture& cached = it->second;
    cached.lastFrame = frameNumber_;
    if (cached.failed) {
        return {};
    }
    const bool refine = !inserted && !cached.full && cached.side < needed && cached.side > 0;
    const bool waiting = !inserted && cached.side == 0 && !cached.full; // first load pending
    if (!inserted && !refine && !waiting) {
        return cached.texture;
    }
    const double cost = static_cast<double>(needed) * static_cast<double>(needed);
    if (refine && imageRefinedPixels_ > 0.0 &&
        imageRefinedPixels_ + cost > kImageRefinePixelBudget) {
        imageRefinementPending_ = true; // keep the coarser texture (stretched) this frame
        return cached.texture;
    }
    std::optional<render::ImageData> loaded = imageSource_->load(image.asset, needed);
    if (!loaded) {
        // Decoding elsewhere; the source calls back when it is done. Until then the
        // coarser texture (or the neutral frame) is drawn.
        if (inserted) {
            cached.side = 0;
        }
        return cached.texture;
    }
    render::ImageData& pixels = *loaded;
    ++imagesDecoded_;
    if (refine) {
        imageRefinedPixels_ += cost;
    }
    if (pixels.empty()) {
        if (!cached.texture.isValid()) {
            cached.failed = true; // missing or unreadable: a frame instead, not retried
        }
        cached.full = true;
        return cached.texture;
    }
    const render::TextureHandle texture = renderer.createTexture(pixels);
    if (!texture.isValid()) {
        cached.full = true; // the backend refused it (too large): keep what we have
        return cached.texture;
    }
    if (cached.texture.isValid()) {
        renderer.destroyTexture(cached.texture);
        imageTextureBytes_ -= cached.bytes;
    }
    cached.texture = texture;
    cached.side = std::max(pixels.width, pixels.height);
    cached.full = cached.side < needed; // the asset is smaller than asked: all of it
    cached.bytes = pixels.byteSize() + pixels.byteSize() / 3;
    imageTextureBytes_ += cached.bytes;
    return cached.texture;
}

void CanvasController::trimImageTextures(render::Renderer& renderer) {
    if (imageTextureBytes_ <= kImageTextureBudgetBytes) {
        return;
    }
    // Over budget: release least recently drawn textures (not those of this frame).
    std::vector<std::pair<std::uint64_t, core::AssetId>> byAge;
    byAge.reserve(imageTextures_.size());
    for (const auto& [asset, cached] : imageTextures_) {
        if (cached.lastFrame != frameNumber_ && cached.texture.isValid()) {
            byAge.emplace_back(cached.lastFrame, asset);
        }
    }
    std::sort(byAge.begin(), byAge.end());
    for (const auto& [frame, asset] : byAge) {
        if (imageTextureBytes_ <= kImageTextureBudgetBytes) {
            break;
        }
        const auto it = imageTextures_.find(asset);
        renderer.destroyTexture(it->second.texture);
        imageTextureBytes_ -= it->second.bytes;
        imageTextures_.erase(it);
    }
}

core::Result<void> CanvasController::insertImage(core::AssetId asset, const core::Vec2& pixelSize) {
    const auto page = scene_.page();
    const document::Workspace& workspace = document_->workspace();
    const auto layer = page ? detail::targetLayer(workspace, *page) : std::nullopt;
    if (!layer || !(pixelSize.x > 0.0F) || !(pixelSize.y > 0.0F)) {
        return core::makeError(core::ErrorCode::InvalidArgument, "nowhere to insert the image");
    }
    const core::DRect view = camera_.visibleWorldRect();
    const double fit = std::min({1.0, 0.6 * view.width() / static_cast<double>(pixelSize.x),
                                 0.6 * view.height() / static_cast<double>(pixelSize.y)});
    const core::Vec2 size{static_cast<float>(pixelSize.x * fit),
                          static_cast<float>(pixelSize.y * fit)};
    const core::DVec2 topLeft =
        view.center() - core::DVec2{static_cast<double>(size.x), static_cast<double>(size.y)} * 0.5;
    auto created = document::commands::createElement(
        workspace, *layer,
        {.transform = {.position = topLeft},
         .payload = document::Image{.asset = asset, .size = size}},
        *ids_);
    if (!created) {
        return tl::unexpected(created.error());
    }
    const core::ElementId id = created->id;
    commit(std::move(created->command));
    if (lastError_) {
        return tl::unexpected(*lastError_);
    }
    selection_.set({id});
    requestRedraw();
    return {};
}

// ---------------------------------------------------------------------------- documents

void CanvasController::setDocumentRasterizer(DocumentRasterizer* rasterizer) {
    if (rasterizer == documents_) {
        return;
    }
    if (documents_ != nullptr) {
        documents_->setReadyHandler({});
    }
    documents_ = rasterizer;
    if (documents_ != nullptr) {
        documents_->setReadyHandler([this] { requestRedraw(); }); // rendered: draw it
    }
    releaseDocumentTiles();
    requestRedraw();
}

void CanvasController::releaseDocumentTiles() {
    for (const auto& [key, tile] : documentTiles_) {
        pendingTextureDestroy_.push_back(tile.texture);
    }
    documentTiles_.clear();
    documentTileBytes_ = 0;
    if (documents_ != nullptr) {
        documents_->keepOnly({}); // tiles queued for the page shown before are not needed
    }
}

render::TextureHandle CanvasController::documentTile(const DocumentTileKey& key,
                                                     render::Renderer& renderer) {
    wantedTiles_.push_back(key);
    auto [it, inserted] = documentTiles_.try_emplace(key);
    DocumentTile& tile = it->second;
    tile.lastFrame = frameNumber_;
    if (tile.texture.isValid() || tile.failed) {
        return tile.texture;
    }
    if (documentTilesUploaded_ >= kDocumentTileUploadsPerFrame) {
        documentTilesPending_ = true; // the next frame takes more
        return {};
    }
    std::optional<render::ImageData> pixels = documents_->tile(key);
    if (!pixels) {
        return {}; // being rendered; the rasterizer asks for a frame when it is done
    }
    if (pixels->empty()) {
        tile.failed = true; // unreadable document or page: the paper shows
        return {};
    }
    tile.texture = renderer.createTexture(*pixels);
    if (!tile.texture.isValid()) {
        tile.failed = true;
        return {};
    }
    tile.bytes = pixels->byteSize() + pixels->byteSize() / 3;
    documentTileBytes_ += tile.bytes;
    ++documentTilesUploaded_;
    return tile.texture;
}

void CanvasController::drawDocumentPage(const document::PageInfo& page,
                                        render::Renderer& renderer) {
    wantedTiles_.clear();
    const core::DVec2 centre = camera_.center();
    const auto drawTile = [&](const DocumentTileKey& key) {
        const render::TextureHandle texture = documentTile(key, renderer);
        if (!texture.isValid()) {
            return;
        }
        // The tile's rectangle on the page, in world units (clipped at the page's edges).
        const double tileWorld = kDocumentTilePx / documentScale(key.level);
        const core::DVec2 min{key.column * tileWorld, key.row * tileWorld};
        const core::DVec2 max{std::min(min.x + tileWorld, page.size.x),
                              std::min(min.y + tileWorld, page.size.y)};
        const core::Affine2 toRect = core::Affine2::translation(min - centre) *
                                     core::Affine2::scaling(max.x - min.x, max.y - min.y);
        content_.push_back({.mesh = {},
                            .transform = toRect.cast<float>(),
                            .color = core::Color::white(),
                            .texture = texture});
    };
    const core::AssetId asset = page.document->asset;
    const std::int32_t index = page.document->index;
    // The whole page, coarse: always there, under the finer tiles.
    const int preview = documentPreviewLevel(page.size);
    drawTile({.asset = asset, .page = index, .level = preview, .column = 0, .row = 0});
    // The tiles in view at the resolution the view needs.
    const int level = documentLevelFor(camera_.zoom() * camera_.devicePixelRatio());
    if (level > preview) {
        const core::DRect pageRect = core::DRect::fromOriginSize({0.0, 0.0}, page.size);
        const core::DRect view = camera_.visibleWorldRect();
        if (view.intersects(pageRect)) {
            const core::DRect shown = core::DRect::fromPoints(
                {std::max(view.min.x, 0.0), std::max(view.min.y, 0.0)},
                {std::min(view.max.x, page.size.x), std::min(view.max.y, page.size.y)});
            const double tileWorld = kDocumentTilePx / documentScale(level);
            const auto first = [&](double v) {
                return static_cast<std::int32_t>(std::floor(v / tileWorld));
            };
            const auto last = [&](double v, double limit) {
                return static_cast<std::int32_t>(std::ceil(std::min(v, limit) / tileWorld)) - 1;
            };
            for (std::int32_t row = first(shown.min.y); row <= last(shown.max.y, page.size.y);
                 ++row) {
                for (std::int32_t column = first(shown.min.x);
                     column <= last(shown.max.x, page.size.x); ++column) {
                    drawTile({.asset = asset,
                              .page = index,
                              .level = level,
                              .column = column,
                              .row = row});
                }
            }
        }
    }
    documents_->keepOnly(wantedTiles_);
}

void CanvasController::trimDocumentTiles(render::Renderer& renderer) {
    // Entries without a texture (still rendering, or failed) are forgotten once they are out
    // of view: bounded, and a failed tile is asked for again when it comes back into view.
    if (documentTiles_.size() > kDocumentTileEntries) {
        std::erase_if(documentTiles_, [&](const auto& entry) {
            return !entry.second.texture.isValid() && entry.second.lastFrame != frameNumber_;
        });
    }
    if (documentTileBytes_ <= kDocumentTextureBudgetBytes) {
        return;
    }
    std::vector<std::pair<std::uint64_t, DocumentTileKey>> byAge;
    byAge.reserve(documentTiles_.size());
    for (const auto& [key, tile] : documentTiles_) {
        if (tile.lastFrame != frameNumber_ && tile.texture.isValid()) {
            byAge.emplace_back(tile.lastFrame, key);
        }
    }
    std::sort(byAge.begin(), byAge.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [frame, key] : byAge) {
        if (documentTileBytes_ <= kDocumentTextureBudgetBytes) {
            break;
        }
        const auto it = documentTiles_.find(key);
        renderer.destroyTexture(it->second.texture);
        documentTileBytes_ -= it->second.bytes;
        documentTiles_.erase(it);
    }
}

void CanvasController::onGraphicsReset() noexcept {
    imageTextures_.clear();
    imageTextureBytes_ = 0;
    documentTiles_.clear();
    documentTileBytes_ = 0;
    textTextures_.clear();
    pendingTextureDestroy_.clear();
    erasePreviewMeshes_.clear();
    shapePreviewMeshes_ = {};
    followerMeshes_.clear();
    draftMeshes_.clear();
    handleMeshes_ = {};
    for (auto& [id, erased] : preview_->partial) {
        erased.dirty = true;
    }
    cache_.forgetGpuResources();
    batches_.forgetGpuResources();
    liveMesh_ = {};
    selectionMesh_ = {};
    marqueeMesh_ = {};
}

render::RenderFrame CanvasController::buildFrame(render::Renderer& renderer) {
    STUDYAPP_PROFILE_SCOPE(&profiler_, "build frame");
    cache_.beginFrame();
    for (const core::ElementId id : scene_.takeRemoved()) {
        cache_.evict(id);
        if (const auto text = textTextures_.find(id); text != textTextures_.end()) {
            pendingTextureDestroy_.push_back(text->second.texture);
            textTextures_.erase(text);
        }
    }
    cache_.flush(renderer);
    for (const render::TextureHandle texture : pendingTextureDestroy_) {
        if (texture.isValid()) {
            renderer.destroyTexture(texture);
        }
    }
    pendingTextureDestroy_.clear();
    textRasterized_ = 0;
    textRefinedPixels_ = 0.0;
    textRefinementPending_ = false;
    ++frameNumber_;
    imageRefinedPixels_ = 0.0;
    imagesDecoded_ = 0;
    imageRefinementPending_ = false;
    documentTilesUploaded_ = 0;
    documentTilesPending_ = false;
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

    // The page's document (PDF) behind everything else.
    if (info != nullptr && info->document && documents_ != nullptr) {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "document page");
        drawDocumentPage(*info, renderer);
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
    {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "erase preview");
        updateErasePreview(renderer, lod);
    }
    std::size_t draftMesh = 0;
    const auto drawElement = [&](core::ElementId id) {
        const document::Element* element = workspace.findElement(id);
        const SceneEntry* entry = scene_.find(id);
        if (element == nullptr || entry == nullptr) {
            return;
        }
        if (preview_->resized && preview_->resized->id == id) {
            // Being resized: drawn at its new geometry. Images stretch their texture and text
            // boxes are laid out for their new size (below); other kinds are tessellated for
            // this frame.
            element = &*preview_->resized;
            if (!std::holds_alternative<document::TextBox>(element->payload) &&
                !std::holds_alternative<document::Image>(element->payload)) {
                const core::Affine2f transform =
                    (toCameraRelative * meshToWorld(*element)).cast<float>();
                for (const MeshPart& part :
                     buildElementMeshes(*element, RenderCache::pixelsPerUnit(lod))) {
                    if (draftMesh == draftMeshes_.size()) {
                        draftMeshes_.emplace_back();
                    }
                    render::MeshHandle& handle = draftMeshes_[draftMesh];
                    if (uploadScratch(renderer, handle, part.mesh)) {
                        ++draftMesh;
                        content_.push_back({.mesh = handle,
                                            .transform = transform,
                                            .color = part.color,
                                            .opacity = entry->layerOpacity});
                    }
                }
                return;
            }
        }
        if (textLayout_ != nullptr && std::holds_alternative<document::TextBox>(element->payload)) {
            if (textEdit_ && textEdit_->element == id) {
                return; // shown by the editor overlay while it is edited
            }
            const render::TextureHandle texture = textTexture(
                *element, entry->contentVersion,
                textPixelsPerUnit(camera_.zoom() * camera_.devicePixelRatio()), renderer);
            if (texture.isValid()) {
                core::Affine2 toWorld = meshToWorld(*element);
                if (preview_->moving && selection_.contains(id)) {
                    toWorld = moveOffset * toWorld;
                }
                const auto& box = std::get<document::TextBox>(element->payload);
                const core::Affine2 toBox = core::Affine2::scaling(static_cast<double>(box.size.x),
                                                                   static_cast<double>(box.size.y));
                content_.push_back({.mesh = {},
                                    .transform = (toCameraRelative * toWorld * toBox).cast<float>(),
                                    .color = core::Color::white(),
                                    .opacity = entry->layerOpacity,
                                    .texture = texture});
            }
            return;
        }
        if (textEdit_ && textEdit_->element == id) {
            return;
        }
        if (const auto* image = std::get_if<document::Image>(&element->payload);
            image != nullptr && imageSource_ != nullptr) {
            const render::TextureHandle texture =
                imageTexture(*image, camera_.zoom() * camera_.devicePixelRatio(), renderer);
            if (texture.isValid()) {
                core::Affine2 toWorld = meshToWorld(*element);
                if (preview_->moving && selection_.contains(id)) {
                    toWorld = moveOffset * toWorld;
                }
                const core::Affine2 toBox = core::Affine2::scaling(
                    static_cast<double>(image->size.x), static_cast<double>(image->size.y));
                content_.push_back({.mesh = {},
                                    .transform = (toCameraRelative * toWorld * toBox).cast<float>(),
                                    .color = core::Color::white(),
                                    .opacity = entry->layerOpacity,
                                    .texture = texture});
                return;
            }
            // Missing or unreadable: the neutral frame below.
        }
        if (preview_->moving && preview_->followers.contains(id)) {
            // Attached to what is being moved: its attached ends follow the offset.
            document::Element follower = *element;
            auto& connector = std::get<document::Connector>(follower.payload);
            for (document::ConnectorEnd* end : {&connector.start, &connector.end}) {
                if (end->attachedTo && selection_.contains(*end->attachedTo)) {
                    end->position += preview_->moveOffset;
                }
            }
            const std::vector<MeshPart> parts =
                buildElementMeshes(follower, RenderCache::pixelsPerUnit(lod));
            if (!parts.empty() && uploadScratch(renderer, followerMeshes_[id], parts[0].mesh)) {
                content_.push_back(
                    {.mesh = followerMeshes_[id],
                     .transform = (toCameraRelative * meshToWorld(follower)).cast<float>(),
                     .color = parts[0].color,
                     .opacity = entry->layerOpacity});
            }
            return;
        }
        if (preview_->erased.contains(id)) {
            // Being erased: nothing, or (partial eraser) what is left of it, in its place.
            if (const auto piece = erasePreviewMeshes_.find(id);
                piece != erasePreviewMeshes_.end()) {
                content_.push_back(
                    {.mesh = piece->second,
                     .transform = (toCameraRelative * meshToWorld(*element)).cast<float>(),
                     .color = std::get<document::Stroke>(element->payload).color,
                     .opacity = entry->layerOpacity});
            }
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
            // Missing meshes built in parallel first (e.g. hundreds of long strokes cut by
            // one erase); drawElement() then finds them in the cache.
            prebuildRequests_.clear();
            for (const CanvasScene::OrderedId& item : visible_) {
                const SceneEntry* entry = scene_.find(item.id);
                const document::Element* element = workspace.findElement(item.id);
                // Text boxes and images are drawn from textures (meshes only as fallback).
                if (entry != nullptr && element != nullptr &&
                    !std::holds_alternative<document::TextBox>(element->payload) &&
                    !std::holds_alternative<document::Image>(element->payload)) {
                    prebuildRequests_.push_back(
                        {.element = element, .version = entry->contentVersion});
                }
            }
            cache_.prebuild(prebuildRequests_, lod);
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
                // previews never force a rebuild. A run that moves as a whole (every
                // member selected, e.g. Select All) is still one draw: its mesh moves by
                // the preview offset. Per element, dragging a selected 10 000-stroke page
                // took 8 000 draw calls and 135 ms frames.
                bool erased = false;
                bool anySelected = false;
                bool allSelected = true;
                if (!preview_->erased.empty() || preview_->moving || preview_->resized) {
                    for (const core::ElementId id : batch.members) {
                        erased = erased || preview_->erased.contains(id) ||
                                 (preview_->moving && preview_->followers.contains(id)) ||
                                 (preview_->resized && preview_->resized->id == id);
                        const bool selected = preview_->moving && selection_.contains(id);
                        anySelected = anySelected || selected;
                        allSelected = allSelected && selected;
                    }
                }
                const bool movesWhole = anySelected && allSelected && !erased;
                if (!batch.gpu.isValid() || erased || (anySelected && !movesWhole)) {
                    for (std::size_t i = first; i < v; ++i) {
                        drawElement(visible_[i].id);
                    }
                    continue;
                }
                const core::DVec2 origin =
                    batch.origin + (movesWhole ? preview_->moveOffset : core::DVec2{});
                content_.push_back(
                    {.mesh = batch.gpu,
                     .transform =
                         (toCameraRelative * core::Affine2::translation(origin)).cast<float>(),
                     .color = core::Color::white(),
                     .opacity = batch.opacity});
            }
        }
    }

    // Live stroke: tessellated every frame from the smoothed samples, camera-relative.
    if (preview_->stroke) {
        STUDYAPP_PROFILE_SCOPE(&profiler_, "live stroke");
        const document::Stroke style{.brush = strokeStyle_.brush,
                                     .color = strokeStyle_.color,
                                     .baseWidth = strokeStyle_.width,
                                     .points = {}};
        std::vector<render::WidthPoint> points;
        points.reserve(preview_->stroke->points().size());
        for (const StrokeSample& sample : preview_->stroke->points()) {
            points.push_back(
                {toFloat(sample.world - centre), strokeRadius(style, sample.pressure)});
        }
        const render::MeshData mesh = render::tessellatePolyline(
            points, {.pixelsPerUnit = static_cast<float>(camera_.zoom())});
        if (uploadScratch(renderer, liveMesh_, mesh)) {
            content_.push_back({.mesh = liveMesh_, .transform = {}, .color = strokeStyle_.color});
        }
    }

    // The shape being dragged, on top of the content.
    if (preview_->shape) {
        const std::vector<MeshPart> parts =
            buildElementMeshes(*preview_->shape, RenderCache::pixelsPerUnit(lod));
        const core::Affine2f transform =
            (toCameraRelative * meshToWorld(*preview_->shape)).cast<float>();
        for (std::size_t i = 0; i < parts.size() && i < shapePreviewMeshes_.size(); ++i) {
            if (uploadScratch(renderer, shapePreviewMeshes_[i], parts[i].mesh)) {
                content_.push_back({.mesh = shapePreviewMeshes_[i],
                                    .transform = transform,
                                    .color = parts[i].color});
            }
        }
    }

    // Overlays in view space. Selection outlines are built for the current zoom relative to
    // an anchor and only translated while panning: rebuilding 10 000 of them per frame cost
    // ≈ 8 ms (docs/PERFORMANCE.md).
    if (!selection_.empty()) {
        const core::DVec2 offset = preview_->moving ? preview_->moveOffset : core::DVec2{};
        const SelectionOutlineKey key{.selection = selection_.revision(),
                                      .scene = scene_.generation(),
                                      .zoom = camera_.zoom(),
                                      .offset = offset};
        if (!(selectionOutlineKey_ && *selectionOutlineKey_ == key && selectionMesh_.isValid())) {
            render::MeshData mesh;
            mesh.vertices.reserve(8 * selection_.size()); // one 8-vertex outline per element
            mesh.indices.reserve(24 * selection_.size());
            for (const core::ElementId id : selection_.ids()) {
                if (const SceneEntry* entry = scene_.find(id)) {
                    const core::Rect view = viewRectOf(camera_, entry->bounds.translated(offset));
                    render::appendMesh(mesh,
                                       render::tessellateRectOutline(
                                           view.expanded(static_cast<float>(kSelectionPaddingPx)),
                                           kOverlayHalfWidthPx));
                }
            }
            selectionOutlineAnchor_ = camera_.center();
            selectionOutlineAnchorView_ = camera_.worldToView(selectionOutlineAnchor_);
            selectionOutlineKey_ =
                uploadScratch(renderer, selectionMesh_, mesh) ? std::optional(key) : std::nullopt;
        }
        if (selectionOutlineKey_) {
            // Where the anchor is on screen now, relative to where the mesh was built.
            const core::DVec2 shift =
                camera_.worldToView(selectionOutlineAnchor_) - selectionOutlineAnchorView_;
            overlay_.push_back({.mesh = selectionMesh_,
                                .transform = core::Affine2::translation(shift).cast<float>(),
                                .color = colors_.selection});
        }
    }
    // Handles of a single selected element (view space).
    if (selection_.size() == 1 && !preview_->moving && !document_->isReadOnly()) {
        const core::ElementId id = *selection_.ids().begin();
        const document::Element* element = preview_->resized && preview_->resized->id == id
                                               ? &*preview_->resized
                                               : workspace.findElement(id);
        const SceneEntry* entry = scene_.find(id);
        render::MeshData outer;
        render::MeshData inner;
        if (element != nullptr && entry != nullptr && !entry->layerLocked && !element->locked) {
            const auto half = static_cast<float>(detail::SelectTool::kHandleViewPx * 0.5);
            for (const Handle& handle : handlesFor(*element)) {
                const core::Vec2 at = toFloat(camera_.worldToView(handle.world));
                const core::Rect box{at - core::Vec2{half, half}, at + core::Vec2{half, half}};
                const std::array<core::Vec2, 4> square{box.min, core::Vec2{box.max.x, box.min.y},
                                                       box.max, core::Vec2{box.min.x, box.max.y}};
                render::appendMesh(outer, render::tessellateConvexFill(square));
                const core::Rect light = box.expanded(-1.25F);
                const std::array<core::Vec2, 4> hole{
                    light.min, core::Vec2{light.max.x, light.min.y}, light.max,
                    core::Vec2{light.min.x, light.max.y}};
                render::appendMesh(inner, render::tessellateConvexFill(hole));
            }
        }
        if (uploadScratch(renderer, handleMeshes_[0], outer)) {
            overlay_.push_back(
                {.mesh = handleMeshes_[0], .transform = {}, .color = colors_.selection});
        }
        if (uploadScratch(renderer, handleMeshes_[1], inner)) {
            overlay_.push_back(
                {.mesh = handleMeshes_[1], .transform = {}, .color = core::Color::white()});
        }
    }
    if (preview_->marquee) {
        const render::MeshData mesh = render::tessellateRectOutline(
            viewRectOf(camera_, *preview_->marquee), kOverlayHalfWidthPx);
        if (uploadScratch(renderer, marqueeMesh_, mesh)) {
            overlay_.push_back({.mesh = marqueeMesh_, .transform = {}, .color = colors_.marquee});
        }
    }
    lastDrawItems_ = content_.size();
    trimImageTextures(renderer);
    trimDocumentTiles(renderer);
    if (!preview_->moving && !followerMeshes_.empty()) {
        for (const auto& [id, mesh] : followerMeshes_) {
            renderer.destroyMesh(mesh);
        }
        followerMeshes_.clear();
    }
    if (cache_.refinementPending() || textRefinementPending_ || imageRefinementPending_ ||
        documentTilesPending_) {
        // Some meshes were drawn at a coarser level of detail than this zoom asks for; the
        // next frames refine them within the budget, then rendering is idle again.
        requestRedraw();
    }
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
            .cache = cache_.stats(),
            .textTextures = textTextures_.size(),
            .textRasterizedLastFrame = textRasterized_,
            .imageTextures = imageTextures_.size(),
            .imageTextureBytes = imageTextureBytes_,
            .imagesDecodedLastFrame = imagesDecoded_,
            .documentTiles = documentTiles_.size(),
            .documentTileBytes = documentTileBytes_,
            .documentTilesUploadedLastFrame = documentTilesUploaded_};
}

} // namespace studyapp::canvas
