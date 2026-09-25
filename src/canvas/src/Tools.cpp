#include "Tools.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/document/Commands.hpp>

#include <algorithm>
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
        if (mode_ == Mode::PendingMove &&
            (event.viewPos - startView_).length() >= kDragThresholdPx) {
            mode_ = Mode::Moving;
            context.preview.moving = true;
        }
        if (mode_ == Mode::Moving) {
            context.preview.moveOffset = world - startWorld_;
        } else if (mode_ == Mode::Marquee) {
            context.preview.marquee = core::DRect::fromPoints(startWorld_, world);
        }
        return;
    case PointerPhase::Up: {
        if (mode_ == Mode::Moving) {
            const core::DVec2 offset = world - startWorld_;
            const std::vector<core::ElementId> ids(context.selection.ids().begin(),
                                                   context.selection.ids().end());
            context.preview.moving = false;
            context.preview.moveOffset = {};
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

void SelectTool::cancel(ToolContext& context) {
    context.preview.marquee.reset();
    context.preview.moving = false;
    context.preview.moveOffset = {};
    mode_ = Mode::Idle;
}

// ---------------------------------------------------------------------------- eraser

void EraserTool::eraseAlong(const core::DVec2& a, const core::DVec2& b,
                            ToolContext& context) const {
    const double radius = context.camera.viewToWorldLength(kRadiusViewPx);
    std::vector<core::ElementId> candidates;
    context.scene.query(core::DRect::fromPoints(a, b).expanded(radius), candidates);
    for (const core::ElementId id : candidates) {
        const SceneEntry* entry = context.scene.find(id);
        const document::Element* element = context.document.workspace().findElement(id);
        if (entry == nullptr || element == nullptr || entry->layerLocked) {
            continue;
        }
        if (strokeTouchesSegment(*element, a, b, radius)) {
            context.preview.erased.insert(id);
        }
    }
}

void EraserTool::onPointer(const PointerEvent& event, ToolContext& context) {
    const core::DVec2 world = context.camera.viewToWorld(event.viewPos);
    context.preview.eraserCenter = world;
    context.preview.eraserRadiusWorld = context.camera.viewToWorldLength(kRadiusViewPx);
    switch (event.phase) {
    case PointerPhase::Down:
        if ((!isPrimary(event) && event.device != PointerDevice::Eraser) ||
            context.document.isReadOnly()) {
            return;
        }
        active_ = true;
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
        std::vector<core::ElementId> ids(context.preview.erased.begin(),
                                         context.preview.erased.end());
        context.preview.erased.clear();
        if (!ids.empty()) {
            // One gesture, one patch, one undo step (docs/CANVAS.md §5.2).
            context.commit(document::commands::deleteElements(context.document.workspace(), ids));
        }
        return;
    }
    case PointerPhase::Cancel:
        cancel(context);
        return;
    }
}

void EraserTool::cancel(ToolContext& context) {
    context.preview.erased.clear();
    active_ = false;
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
