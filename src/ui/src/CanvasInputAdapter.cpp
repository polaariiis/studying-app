#include "CanvasInputAdapter.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QWheelEvent>

#include <cstdint>

namespace studyapp::ui {

namespace {

core::DVec2 toDVec2(const QPointF& point) noexcept {
    return {point.x(), point.y()};
}

std::uint64_t toMicroseconds(quint64 milliseconds) noexcept {
    return static_cast<std::uint64_t>(milliseconds) * 1000U;
}

canvas::PointerButton toButton(Qt::MouseButton button) noexcept {
    switch (button) {
    case Qt::LeftButton:
        return canvas::PointerButton::Primary;
    case Qt::RightButton:
        return canvas::PointerButton::Secondary;
    case Qt::MiddleButton:
        return canvas::PointerButton::Middle;
    default:
        return canvas::PointerButton::None;
    }
}

/// For moves: the button that is held (primary first), None when hovering.
canvas::PointerButton heldButton(Qt::MouseButtons buttons) noexcept {
    if (buttons.testFlag(Qt::LeftButton)) {
        return canvas::PointerButton::Primary;
    }
    if (buttons.testFlag(Qt::MiddleButton)) {
        return canvas::PointerButton::Middle;
    }
    if (buttons.testFlag(Qt::RightButton)) {
        return canvas::PointerButton::Secondary;
    }
    return canvas::PointerButton::None;
}

} // namespace

canvas::Modifiers toModifiers(Qt::KeyboardModifiers modifiers) noexcept {
    return {.shift = modifiers.testFlag(Qt::ShiftModifier),
            .control = modifiers.testFlag(Qt::ControlModifier),
            .alt = modifiers.testFlag(Qt::AltModifier)};
}

std::optional<canvas::PointerEvent> toPointerEvent(const QMouseEvent& event) {
    canvas::PointerEvent out{.device = canvas::PointerDevice::Mouse,
                             .viewPos = toDVec2(event.position()),
                             .pressure = 1.0F, // mice have no pressure
                             .modifiers = toModifiers(event.modifiers()),
                             .timestampUs = toMicroseconds(event.timestamp())};
    switch (event.type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        out.phase = canvas::PointerPhase::Down;
        out.button = toButton(event.button());
        break;
    case QEvent::MouseButtonRelease:
        out.phase = canvas::PointerPhase::Up;
        out.button = toButton(event.button());
        break;
    case QEvent::MouseMove:
        out.phase = canvas::PointerPhase::Move;
        out.button = heldButton(event.buttons());
        break;
    default:
        return std::nullopt;
    }
    if (out.button == canvas::PointerButton::None && out.phase != canvas::PointerPhase::Move) {
        return std::nullopt; // e.g. extra mouse buttons
    }
    return out;
}

std::optional<canvas::PointerEvent> toPointerEvent(const QTabletEvent& event) {
    const bool eraser = event.pointerType() == QPointingDevice::PointerType::Eraser;
    canvas::PointerEvent out{.device = eraser ? canvas::PointerDevice::Eraser
                                              : canvas::PointerDevice::Pen,
                             .viewPos = toDVec2(event.position()),
                             .pressure = static_cast<float>(event.pressure()),
                             .modifiers = toModifiers(event.modifiers()),
                             .timestampUs = toMicroseconds(event.timestamp())};
    switch (event.type()) {
    case QEvent::TabletPress:
        out.phase = canvas::PointerPhase::Down;
        out.button = event.button() == Qt::RightButton ? canvas::PointerButton::Secondary
                                                       : canvas::PointerButton::Primary;
        break;
    case QEvent::TabletRelease:
        out.phase = canvas::PointerPhase::Up;
        out.button = event.button() == Qt::RightButton ? canvas::PointerButton::Secondary
                                                       : canvas::PointerButton::Primary;
        break;
    case QEvent::TabletMove:
        out.phase = canvas::PointerPhase::Move;
        out.button = event.buttons() == Qt::NoButton ? canvas::PointerButton::None
                                                     : canvas::PointerButton::Primary;
        break;
    default:
        return std::nullopt;
    }
    return out;
}

canvas::WheelEvent toWheelEvent(const QWheelEvent& event) {
    return {.viewPos = toDVec2(event.position()),
            .angleDelta = {static_cast<double>(event.angleDelta().x()),
                           static_cast<double>(event.angleDelta().y())},
            .pixelDelta = {static_cast<double>(event.pixelDelta().x()),
                           static_cast<double>(event.pixelDelta().y())},
            .modifiers = toModifiers(event.modifiers())};
}

std::optional<canvas::ZoomGestureEvent> toZoomGesture(const QNativeGestureEvent& event) {
    if (event.gestureType() != Qt::ZoomNativeGesture) {
        return std::nullopt;
    }
    // Qt reports the zoom step as a delta around 0 (e.g. 0.05 = 5 % larger).
    return canvas::ZoomGestureEvent{.viewPos = toDVec2(event.position()),
                                    .scaleFactor = 1.0 + event.value()};
}

std::optional<canvas::KeyEvent> toKeyEvent(const QKeyEvent& event, bool pressed) {
    canvas::Key key = canvas::Key::Other;
    switch (event.key()) {
    case Qt::Key_Space:
        key = canvas::Key::Space;
        break;
    case Qt::Key_Escape:
        key = canvas::Key::Escape;
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        key = canvas::Key::Delete;
        break;
    default:
        return std::nullopt;
    }
    return canvas::KeyEvent{.key = key,
                            .pressed = pressed,
                            .autoRepeat = event.isAutoRepeat(),
                            .modifiers = toModifiers(event.modifiers())};
}

} // namespace studyapp::ui
