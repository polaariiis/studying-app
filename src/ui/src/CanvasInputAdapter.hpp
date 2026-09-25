#pragma once

#include <studyapp/canvas/Input.hpp>

#include <QtCore/qnamespace.h>

#include <optional>

class QKeyEvent;
class QMouseEvent;
class QNativeGestureEvent;
class QTabletEvent;
class QWheelEvent;

namespace studyapp::ui {

// Qt event → canvas event normalisation (docs/ARCHITECTURE.md §3.1). This is the only code
// that sees both Qt input types and canvas input types; nothing Qt-specific crosses into
// the canvas. Positions stay in logical (view) pixels — Qt's event positions already are —
// and never in device pixels.

[[nodiscard]] std::optional<canvas::PointerEvent> toPointerEvent(const QMouseEvent& event);
[[nodiscard]] std::optional<canvas::PointerEvent> toPointerEvent(const QTabletEvent& event);
[[nodiscard]] canvas::WheelEvent toWheelEvent(const QWheelEvent& event);
[[nodiscard]] std::optional<canvas::ZoomGestureEvent>
toZoomGesture(const QNativeGestureEvent& event);
[[nodiscard]] std::optional<canvas::KeyEvent> toKeyEvent(const QKeyEvent& event, bool pressed);
[[nodiscard]] canvas::Modifiers toModifiers(Qt::KeyboardModifiers modifiers) noexcept;

} // namespace studyapp::ui
