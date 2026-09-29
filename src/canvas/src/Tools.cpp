#include "Tools.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/document/Commands.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

namespace studyapp::canvas::detail {

namespace {

/// Pointer travel (view px) before a press on a selected element becomes a move.
constexpr double kDragThresholdPx = 3.0;
/// Pick tolerance for click selection (view px).
constexpr double kPickTolerancePx = 4.0;

bool isPrimary(const PointerEvent& event) noexcept {
    return event.button == PointerButton::Primary;
}

StrokeSample sampleOf(const PointerEvent& event, const Camera& camera) noexcept {
    return {.world = camera.viewToWorld(event.viewPos),
            .pressure = event.pressure,
            .timestampUs = event.timestampUs};
}

bool insidePage(const document::Workspace& workspace, std::optional<core::PageId> page,
                const core::DVec2& world) {
    const document::PageInfo* info = page ? workspace.findPage(*page) : nullptr;
    if (info == nullptr) {
        return false;
    }
    return info->extent == document::PageExtent::Infinite ||
           core::DRect::fromOriginSize({0.0, 0.0}, info->size).contains(world);
}

} // namespace

std::optional<core::LayerId> targetLayer(const document::Workspace& workspace, core::PageId page) {
    const auto layers = workspace.layersOf(page);
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const document::Layer* layer = workspace.findLayer(*it);
        if (layer != nullptr && layer->visible && !layer->locked) {
            return *it;
        }
    }
    return std::nullopt;
}

std::optional<core::ElementId> topmostAt(const CanvasScene& scene,
                                         const document::Workspace& workspace,
                                         const core::DVec2& world, double toleranceWorld) {
    std::vector<core::ElementId> candidates;
    scene.query(core::DRect{world, world}.expanded(toleranceWorld), candidates);
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const SceneEntry* entry = scene.find(*it);
        const document::Element* element = workspace.findElement(*it);
        if (entry == nullptr || element == nullptr || entry->layerLocked) {
            continue;
        }
        if (hitTest(*element, world, toleranceWorld)) {
            return *it;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------- pen

void PenTool::onPointer(const PointerEvent& event, ToolContext& context) {
    switch (event.phase) {
    case PointerPhase::Down: {
        if (!isPrimary(event) || context.document.isReadOnly()) {
            return;
        }
        const auto page = context.scene.page();
        const StrokeSample sample = sampleOf(event, context.camera);
        // Bounded pages: ink starts on the page (it may continue past the edge).
        if (!page || !insidePage(context.document.workspace(), page, sample.world) ||
            !targetLayer(context.document.workspace(), *page)) {
            return;
        }
        context.preview.stroke.emplace(sample, context.camera.zoom(), context.strokeOptions);
        active_ = true;
        return;
    }
    case PointerPhase::Move:
        if (active_) {
            context.preview.stroke->add(sampleOf(event, context.camera));
        }
        return;
    case PointerPhase::Up: {
        if (!active_) {
            return;
        }
        context.preview.stroke->add(sampleOf(event, context.camera));
        const std::vector<StrokeSample> points = context.preview.stroke->finish();
        context.preview.stroke.reset();
        active_ = false;
        const auto page = context.scene.page();
        const auto layer = page ? targetLayer(context.document.workspace(), *page) : std::nullopt;
        if (!layer) {
            return;
        }
        StrokeGeometry geometry = makeStrokeGeometry(points, context.pen);
        auto created = document::commands::createElement(
            context.document.workspace(), *layer,
            document::commands::NewElement{.transform = geometry.transform,
                                           .payload = std::move(geometry.stroke)},
            context.ids);
        if (!created) {
            context.commit(tl::unexpected(created.error()));
            return;
        }
        context.commit(std::move(created->command));
        return;
    }
    case PointerPhase::Cancel:
        cancel(context);
        return;
    }
}

void PenTool::cancel(ToolContext& context) {
    context.preview.stroke.reset();
    active_ = false;
}

// ---------------------------------------------------------------------------- select

void SelectTool::onPointer(const PointerEvent& event, ToolContext& context) {
    const core::DVec2 world = context.camera.viewToWorld(event.viewPos);
    switch (event.phase) {
    case PointerPhase::Down: {
        if (!isPrimary(event)) {
            return;
        }
        startView_ = event.viewPos;
        startWorld_ = world;
        additive_ = event.modifiers.shift;
        if (!context.document.isReadOnly() && beginResize(world, context)) {
            return; // a handle of the selected element
        }
        const auto hit = topmostAt(context.scene, context.document.workspace(), world,
                                   context.camera.viewToWorldLength(kPickTolerancePx));
        if (hit) {
            if (additive_) {
                context.selection.toggle(*hit);
            } else if (!context.selection.contains(*hit)) {
                context.selection.set({*hit});
            }
            mode_ = context.selection.contains(*hit) && !context.document.isReadOnly()
                        ? Mode::PendingMove
                        : Mode::Idle;
            return;
        }
        if (!additive_) {
            context.selection.clear();
        }
        mode_ = Mode::Marquee;
        context.preview.marquee = core::DRect{world, world};
        return;
    }
    case PointerPhase::Move:
        if (mode_ == Mode::Resizing) {
            if (const document::Element* element =
                    context.document.workspace().findElement(resizing_)) {
                context.preview.resized =
                    dragHandle(*element, handle_, world,
                               {.constrain = event.modifiers.shift,
                                .minSize = context.camera.viewToWorldLength(4.0),
                                .textHeight = context.textHeight});
            }
            return;
        }
        if (mode_ == Mode::PendingMove &&
            (event.viewPos - startView_).length() >= kDragThresholdPx) {
            mode_ = Mode::Moving;
            context.preview.moving = true;
            // Connectors attached to what moves (and not moved themselves) follow it in the
            // preview: found once here from the attachment index, not per frame.
            const document::Workspace& workspace = context.document.workspace();
            for (const core::ElementId id : context.selection.ids()) {
                for (const core::ElementId connector : workspace.connectorsAttachedTo(id)) {
                    if (!context.selection.contains(connector)) {
                        context.preview.followers.insert(connector);
                    }
                }
            }
        }
        if (mode_ == Mode::Moving) {
            context.preview.moveOffset = world - startWorld_;
        } else if (mode_ == Mode::Marquee) {
            context.preview.marquee = core::DRect::fromPoints(startWorld_, world);
        }
        return;
    case PointerPhase::Up: {
        if (mode_ == Mode::Resizing) {
            commitResize(world, event.modifiers.shift, context);
            return;
        }
        if (mode_ == Mode::Moving) {
            const core::DVec2 offset = world - startWorld_;
            const std::vector<core::ElementId> ids(context.selection.ids().begin(),
                                                   context.selection.ids().end());
            context.preview.moving = false;
            context.preview.moveOffset = {};
            context.preview.followers.clear();
            mode_ = Mode::Idle;
            if (!ids.empty()) {
                context.commit(
                    document::commands::moveElements(context.document.workspace(), ids, offset));
            }
            return;
        }
        if (mode_ == Mode::Marquee) {
            const core::DRect rect = core::DRect::fromPoints(startWorld_, world);
            // Intersection selects anything the rectangle touches; Alt selects only
            // elements whose bounds lie entirely inside it.
            const bool containment = event.modifiers.alt;
            std::vector<core::ElementId> candidates;
            context.scene.query(rect, candidates);
            std::vector<core::ElementId> picked;
            for (const core::ElementId id : candidates) {
                const SceneEntry* entry = context.scene.find(id);
                const document::Element* element = context.document.workspace().findElement(id);
                if (entry == nullptr || element == nullptr || entry->layerLocked) {
                    continue;
                }
                const bool selected =
                    containment ? rect.contains(entry->bounds) : intersectsRect(*element, rect);
                if (selected) {
                    picked.push_back(id);
                }
            }
            if (additive_) {
                for (const core::ElementId id : picked) {
                    context.selection.add(id);
                }
            } else {
                context.selection.set(std::move(picked));
            }
            context.preview.marquee.reset();
        }
        mode_ = Mode::Idle;
        return;
    }
    case PointerPhase::Cancel:
        cancel(context);
        return;
    }
}

bool SelectTool::beginResize(const core::DVec2& world, ToolContext& context) {
    if (context.selection.size() != 1) {
        return false;
    }
    const core::ElementId id = *context.selection.ids().begin();
    const SceneEntry* entry = context.scene.find(id);
    const document::Element* element = context.document.workspace().findElement(id);
    if (entry == nullptr || element == nullptr || entry->layerLocked || element->locked) {
        return false;
    }
    const double grab = context.camera.viewToWorldLength(kHandleGrabViewPx);
    double best = grab * grab;
    std::optional<HandleKind> chosen;
    for (const Handle& handle : handlesFor(*element)) { // at most 8
        const double d = core::distanceSquared(handle.world, world);
        if (d <= best) {
            best = d;
            chosen = handle.kind;
        }
    }
    if (!chosen) {
        return false;
    }
    mode_ = Mode::Resizing;
    resizing_ = id;
    handle_ = *chosen;
    context.preview.resized = *element;
    return true;
}

void SelectTool::commitResize(const core::DVec2& world, bool constrain, ToolContext& context) {
    mode_ = Mode::Idle;
    context.preview.resized.reset();
    const document::Workspace& workspace = context.document.workspace();
    const document::Element* element = workspace.findElement(resizing_);
    if (element == nullptr || core::distance(world, startWorld_) == 0.0) {
        return; // a click on a handle changes nothing
    }
    auto result = dragHandle(*element, handle_, world,
                             {.constrain = constrain,
                              .minSize = context.camera.viewToWorldLength(4.0),
                              .textHeight = context.textHeight});
    if (!result) {
        return;
    }
    if (const auto* connector = std::get_if<document::Connector>(&result->payload)) {
        // A connector end: attach it to what it is dropped on, as the connector tool does.
        document::ConnectorEnd start = connector->start;
        document::ConnectorEnd end = connector->end;
        document::ConnectorEnd& moved = handle_ == HandleKind::Start ? start : end;
        const document::ConnectorEnd& other = handle_ == HandleKind::Start ? end : start;
        if (const auto target = attachTarget(context, world, other.attachedTo)) {
            const document::Element& onto = *workspace.findElement(*target);
            moved.attachedTo = *target;
            moved.position = attachPoint(
                onto, other.attachedTo
                          ? visualBounds(*workspace.findElement(*other.attachedTo)).center()
                          : other.position);
        }
        context.commit(document::commands::setConnectorEnds(workspace, resizing_, start, end));
        return;
    }
    const core::Vec2 size = std::visit(
        [](const auto& payload) -> core::Vec2 {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, document::Shape> ||
                          std::is_same_v<T, document::Image> ||
                          std::is_same_v<T, document::TextBox>) {
                return payload.size;
            } else {
                return {};
            }
        },
        result->payload);
    context.commit(
        document::commands::resizeElement(workspace, resizing_, result->transform, size));
}

void SelectTool::cancel(ToolContext& context) {
    context.preview.resized.reset();
    context.preview.marquee.reset();
    context.preview.moving = false;
    context.preview.moveOffset = {};
    context.preview.followers.clear();
    mode_ = Mode::Idle;
}

// ---------------------------------------------------------------------------- eraser

void EraserTool::eraseAlong(const core::DVec2& a, const core::DVec2& b,
                            ToolContext& context) const {
    const double radius = context.camera.viewToWorldLength(kRadiusViewPx);
    const double minFragment = context.camera.viewToWorldLength(kMinFragmentViewPx);
    std::vector<core::ElementId> candidates; // broad phase: the grid cells the step covers
    context.scene.query(core::DRect::fromPoints(a, b).expanded(radius), candidates);
    for (const core::ElementId id : candidates) {
        const SceneEntry* entry = context.scene.find(id);
        const document::Element* element = context.document.workspace().findElement(id);
        if (entry == nullptr || element == nullptr || entry->layerLocked) {
            continue;
        }
        if (!partial_) {
            if (!context.preview.erased.contains(id) &&
                strokeTouchesSegment(*element, a, b, radius)) {
                context.preview.erased.insert(id);
            }
            continue;
        }
        const auto* stroke = std::get_if<document::Stroke>(&element->payload);
        if (stroke == nullptr) {
            continue;
        }
        // Cut in element-local space (strokes keep their stored points and transform).
        const core::Affine2 toWorld = document::localToWorld(element->transform);
        const auto toLocal = toWorld.inverse();
        if (!toLocal) {
            continue;
        }
        const double scale = std::max(std::sqrt(std::abs(toWorld.determinant())), 1e-12);
        const EraserCapsule capsule{
            .a = toLocal->apply(a), .b = toLocal->apply(b), .radius = radius / scale};
        if (const auto it = context.preview.partial.find(id); it != context.preview.partial.end()) {
            if (erasePieces(it->second.pieces, *stroke, capsule, minFragment / scale)) {
                it->second.dirty = true;
            }
        } else if (auto pieces = eraseStroke(*stroke, capsule, minFragment / scale)) {
            // First cut: only now is a working copy of what is left made.
            context.preview.partial.emplace(id, Preview::ErasedPieces{std::move(*pieces)});
            context.preview.erased.insert(id);
        }
    }
}

void EraserTool::commit(ToolContext& context) {
    Preview& preview = context.preview;
    // Sorted ids: the same gesture always gives the same patch.
    std::vector<core::ElementId> ids(preview.erased.begin(), preview.erased.end());
    std::sort(ids.begin(), ids.end());
    if (ids.empty()) {
        preview.clear();
        return;
    }
    if (!partial_) {
        preview.clear();
        // One gesture, one patch, one undo step (docs/CANVAS.md §5.2).
        context.commit(document::commands::deleteElements(context.document.workspace(), ids));
        return;
    }
    std::vector<document::commands::StrokePieces> splits;
    splits.reserve(ids.size());
    for (const core::ElementId id : ids) {
        document::commands::StrokePieces split{.stroke = id, .pieces = {}};
        auto& pieces = preview.partial.at(id).pieces;
        split.pieces.reserve(pieces.size());
        for (StrokePiece& piece : pieces) {
            split.pieces.push_back(document::makeStrokePoints(std::move(piece.points)));
        }
        splits.push_back(std::move(split));
    }
    preview.clear();
    context.commit(
        document::commands::splitStrokes(context.document.workspace(), splits, context.ids));
}

void EraserTool::onPointer(const PointerEvent& event, ToolContext& context) {
    const core::DVec2 world = context.camera.viewToWorld(event.viewPos);
    switch (event.phase) {
    case PointerPhase::Down:
        if ((!isPrimary(event) && event.device != PointerDevice::Eraser) ||
            context.document.isReadOnly()) {
            return;
        }
        active_ = true;
        partial_ = context.eraserMode == EraserMode::Partial;
        lastWorld_ = world;
        eraseAlong(world, world, context);
        return;
    case PointerPhase::Move:
        if (active_) {
            eraseAlong(lastWorld_, world, context);
            lastWorld_ = world;
        }
        return;
    case PointerPhase::Up: {
        if (!active_) {
            return;
        }
        eraseAlong(lastWorld_, world, context);
        active_ = false;
        commit(context);
        return;
    }
    case PointerPhase::Cancel:
        cancel(context);
        return;
    }
}

void EraserTool::cancel(ToolContext& context) {
    context.preview.erased.clear();
    context.preview.partial.clear();
    active_ = false;
}

// ---------------------------------------------------------------------------- shape

void ShapeTool::updatePreview(const core::DVec2& world, bool constrain,
                              ToolContext& context) const {
    auto geometry = shapeFromDrag(context.shape, startWorld_, world, constrain);
    if (!geometry) {
        context.preview.shape.reset();
        return;
    }
    context.preview.shape = document::Element{.id = {},
                                              .layer = {},
                                              .z = core::FractionalIndex::first(),
                                              .transform = geometry->transform,
                                              .locked = false,
                                              .payload = geometry->shape};
}

void ShapeTool::onPointer(const PointerEvent& event, ToolContext& context) {
    const core::DVec2 world = context.camera.viewToWorld(event.viewPos);
    switch (event.phase) {
    case PointerPhase::Down: {
        if (!isPrimary(event) || context.document.isReadOnly()) {
            return;
        }
        const auto page = context.scene.page();
        if (!page || !insidePage(context.document.workspace(), page, world) ||
            !targetLayer(context.document.workspace(), *page)) {
            return;
        }
        active_ = true;
        startWorld_ = world;
        startView_ = event.viewPos;
        context.preview.shape.reset();
        return;
    }
    case PointerPhase::Move:
        if (active_) {
            updatePreview(world, event.modifiers.shift, context);
        }
        return;
    case PointerPhase::Up: {
        if (!active_) {
            return;
        }
        active_ = false;
        context.preview.shape.reset();
        if (core::distance(event.viewPos, startView_) < kMinDragViewPx) {
            return; // a click
        }
        auto geometry = shapeFromDrag(context.shape, startWorld_, world, event.modifiers.shift);
        const auto page = context.scene.page();
        const auto layer = page ? targetLayer(context.document.workspace(), *page) : std::nullopt;
        if (!geometry || !layer) {
            return;
        }
        auto created = document::commands::createElement(
            context.document.workspace(), *layer,
            document::commands::NewElement{.transform = geometry->transform,
                                           .payload = geometry->shape},
            context.ids);
        if (!created) {
            context.commit(tl::unexpected(created.error()));
            return;
        }
        context.commit(std::move(created->command));
        return;
    }
    case PointerPhase::Cancel:
        cancel(context);
        return;
    }
}

void ShapeTool::cancel(ToolContext& context) {
    context.preview.shape.reset();
    active_ = false;
}

// ---------------------------------------------------------------------------- text

void TextTool::onPointer(const PointerEvent& event, ToolContext& context) {
    const core::DVec2 world = context.camera.viewToWorld(event.viewPos);
    switch (event.phase) {
    case PointerPhase::Down: {
        if (!isPrimary(event) || context.document.isReadOnly()) {
            return;
        }
        const auto page = context.scene.page();
        if (!page || !insidePage(context.document.workspace(), page, world) ||
            !targetLayer(context.document.workspace(), *page)) {
            return;
        }
        active_ = true;
        startWorld_ = world;
        startView_ = event.viewPos;
        return;
    }
    case PointerPhase::Move:
        return;
    case PointerPhase::Up: {
        if (!active_) {
            return;
        }
        active_ = false;
        const double dragged = std::abs(event.viewPos.x - startView_.x);
        if (dragged < ShapeTool::kMinDragViewPx) {
            // A click: edit the text box under the pointer, or start a new one there.
            const double tolerance = context.camera.viewToWorldLength(4.0);
            if (const auto hit = topmostAt(context.scene, context.document.workspace(), startWorld_,
                                           tolerance)) {
                const document::Element& element = *context.document.workspace().findElement(*hit);
                if (const auto* box = std::get_if<document::TextBox>(&element.payload)) {
                    context.beginTextEdit({.element = *hit,
                                           .position = element.transform.position,
                                           .width = box->size.x,
                                           .text = box->text});
                    return;
                }
            }
            context.beginTextEdit({.position = startWorld_});
            return;
        }
        const auto width = static_cast<float>(std::abs(world.x - startWorld_.x));
        context.beginTextEdit({.position = {std::min(world.x, startWorld_.x), startWorld_.y},
                               .width = std::max(width, kMinTextWidth)});
        return;
    }
    case PointerPhase::Cancel:
        active_ = false;
        return;
    }
}

// ---------------------------------------------------------------------------- connector

std::optional<core::ElementId> attachTarget(const ToolContext& context, const core::DVec2& world,
                                            std::optional<core::ElementId> except) {
    const double tolerance = context.camera.viewToWorldLength(kPickTolerancePx);
    std::vector<core::ElementId> candidates;
    context.scene.query(core::DRect{world, world}.expanded(tolerance), candidates);
    const document::Workspace& workspace = context.document.workspace();
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const SceneEntry* entry = context.scene.find(*it);
        const document::Element* element = workspace.findElement(*it);
        if (entry == nullptr || element == nullptr || entry->layerLocked || *it == except ||
            std::holds_alternative<document::Connector>(element->payload)) {
            continue;
        }
        // Boxes attach anywhere inside; strokes and lines near their ink.
        if (hitTest(*element, world, tolerance) ||
            document::worldBounds(*element).contains(world)) {
            return *it;
        }
    }
    return std::nullopt;
}

document::Connector ConnectorTool::draft(const core::DVec2& world,
                                         const ToolContext& context) const {
    const document::Workspace& workspace = context.document.workspace();
    const auto centreOf = [&](core::ElementId id) {
        return visualBounds(*workspace.findElement(id)).center();
    };
    const auto endAt = [&](std::optional<core::ElementId> attached, const core::DVec2& free,
                           const core::DVec2& towards) {
        return document::ConnectorEnd{
            .position = attached ? attachPoint(*workspace.findElement(*attached), towards) : free,
            .attachedTo = attached};
    };
    if (mode_ == Mode::MovingEnd) {
        const auto& current =
            std::get<document::Connector>(workspace.findElement(editing_)->payload);
        const document::ConnectorEnd& fixed = editingStart_ ? current.end : current.start;
        const auto target = attachTarget(context, world, fixed.attachedTo);
        const core::DVec2 fixedAt = fixed.attachedTo ? centreOf(*fixed.attachedTo) : fixed.position;
        document::ConnectorEnd moved = endAt(target, world, fixedAt);
        document::ConnectorEnd kept = fixed;
        if (fixed.attachedTo) { // re-aim the fixed end at the moved one
            kept.position = attachPoint(*workspace.findElement(*fixed.attachedTo),
                                        target ? centreOf(*target) : world);
        }
        document::Connector result = current;
        result.start = editingStart_ ? moved : kept;
        result.end = editingStart_ ? kept : moved;
        return result;
    }
    const auto endTarget = attachTarget(context, world, startAttached_);
    const core::DVec2 endTowards = endTarget ? centreOf(*endTarget) : world;
    const core::DVec2 startTowards = startAttached_ ? centreOf(*startAttached_) : startWorld_;
    return document::Connector{.start = endAt(startAttached_, startWorld_, endTowards),
                               .end = endAt(endTarget, world, startTowards),
                               .color = context.shape.color,
                               .width = context.shape.width};
}

void ConnectorTool::onPointer(const PointerEvent& event, ToolContext& context) {
    const core::DVec2 world = context.camera.viewToWorld(event.viewPos);
    const document::Workspace& workspace = context.document.workspace();
    switch (event.phase) {
    case PointerPhase::Down: {
        if (!isPrimary(event) || context.document.isReadOnly()) {
            return;
        }
        const auto page = context.scene.page();
        if (!page || !targetLayer(workspace, *page)) {
            return;
        }
        startWorld_ = world;
        startView_ = event.viewPos;
        // An end of an existing connector under the pointer: drag that end.
        const double grab = context.camera.viewToWorldLength(kEndGrabViewPx);
        std::vector<core::ElementId> candidates;
        context.scene.query(core::DRect{world, world}.expanded(grab), candidates);
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            const SceneEntry* entry = context.scene.find(*it);
            const document::Element* element = workspace.findElement(*it);
            const auto* connector =
                element != nullptr ? std::get_if<document::Connector>(&element->payload) : nullptr;
            if (entry == nullptr || connector == nullptr || entry->layerLocked) {
                continue;
            }
            for (const bool start : {false, true}) {
                const core::DVec2 at = start ? connector->start.position : connector->end.position;
                if (core::distance(at, world) <= grab) {
                    mode_ = Mode::MovingEnd;
                    editing_ = *it;
                    editingStart_ = start;
                    context.preview.erased.insert(editing_); // shown as the draft meanwhile
                    return;
                }
            }
        }
        if (!insidePage(workspace, page, world)) {
            return;
        }
        mode_ = Mode::Creating;
        startAttached_ = attachTarget(context, world, std::nullopt);
        return;
    }
    case PointerPhase::Move: {
        if (mode_ == Mode::Idle) {
            return;
        }
        context.preview.shape =
            document::Element{.id = mode_ == Mode::MovingEnd ? editing_ : core::ElementId{},
                              .layer = {},
                              .z = core::FractionalIndex::first(),
                              .transform = {},
                              .locked = false,
                              .payload = draft(world, context)};
        return;
    }
    case PointerPhase::Up: {
        if (mode_ == Mode::Idle) {
            return;
        }
        const Mode mode = std::exchange(mode_, Mode::Idle);
        context.preview.shape.reset();
        if (mode == Mode::MovingEnd) {
            context.preview.erased.erase(editing_);
            mode_ = Mode::MovingEnd; // draft() reads it
            const document::Connector result = draft(world, context);
            mode_ = Mode::Idle;
            context.commit(document::commands::setConnectorEnds(workspace, editing_, result.start,
                                                                result.end));
            return;
        }
        if (core::distance(event.viewPos, startView_) < ShapeTool::kMinDragViewPx) {
            return; // a click is not a connector
        }
        mode_ = Mode::Creating;
        const document::Connector connector = draft(world, context);
        mode_ = Mode::Idle;
        const auto page = context.scene.page();
        const auto layer = page ? targetLayer(workspace, *page) : std::nullopt;
        if (!layer) {
            return;
        }
        auto created = document::commands::createElement(
            workspace, *layer, document::commands::NewElement{.payload = connector}, context.ids);
        if (!created) {
            context.commit(tl::unexpected(created.error()));
            return;
        }
        context.commit(std::move(created->command));
        return;
    }
    case PointerPhase::Cancel:
        cancel(context);
        return;
    }
}

void ConnectorTool::cancel(ToolContext& context) {
    if (mode_ == Mode::MovingEnd) {
        context.preview.erased.erase(editing_);
    }
    context.preview.shape.reset();
    mode_ = Mode::Idle;
}

// ---------------------------------------------------------------------------- pan / zoom

void PanTool::onPointer(const PointerEvent& event, ToolContext& context) {
    switch (event.phase) {
    case PointerPhase::Down:
        active_ = true;
        lastView_ = event.viewPos;
        return;
    case PointerPhase::Move:
        if (active_) {
            context.camera.panBy(event.viewPos - lastView_);
            lastView_ = event.viewPos;
        }
        return;
    case PointerPhase::Up:
    case PointerPhase::Cancel:
        active_ = false;
        return;
    }
}

void PanTool::cancel(ToolContext& /*context*/) {
    active_ = false;
}

void ZoomTool::onPointer(const PointerEvent& event, ToolContext& context) {
    if (event.phase != PointerPhase::Down) {
        return;
    }
    const bool zoomOut = event.modifiers.alt || event.button == PointerButton::Secondary;
    context.camera.zoomAt(event.viewPos, zoomOut ? 1.0 / kStep : kStep);
}

} // namespace studyapp::canvas::detail
