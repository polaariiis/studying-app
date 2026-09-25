#pragma once

#include <studyapp/core/Vec2.hpp>

#include <cstdint>

namespace studyapp::canvas {

// Engine-level input events (docs/CANVAS.md §0, §4). The UI layer converts Qt events into
// these plain values; nothing in `canvas` ever sees a Qt type. All positions are in view
// space (logical widget pixels, origin top-left), never device pixels.

enum class PointerDevice : std::uint8_t {
    Mouse,
    Pen,
    Eraser, ///< the eraser end of a pen
    Touch,
};

enum class PointerPhase : std::uint8_t {
    Down,
    Move,
    Up,
    Cancel, ///< the gesture was interrupted (focus loss, device left proximity)
};

enum class PointerButton : std::uint8_t {
    None, ///< e.g. moves without a button, pen hover
    Primary,
    Secondary,
    Middle,
};

struct Modifiers {
    bool shift = false;
    bool control = false; ///< Ctrl, or Cmd on macOS
    bool alt = false;

    [[nodiscard]] friend bool operator==(const Modifiers&, const Modifiers&) = default;
};

struct PointerEvent {
    PointerPhase phase = PointerPhase::Move;
    PointerDevice device = PointerDevice::Mouse;
    /// The button that changed state (Down/Up) or that is held during a drag (Move).
    PointerButton button = PointerButton::Primary;
    core::DVec2 viewPos{};
    float pressure = 1.0F; ///< 0..1; devices without pressure report 1
    Modifiers modifiers{};
    std::uint64_t timestampUs = 0; ///< monotonic; used by stroke smoothing
    std::uint64_t pointerId = 0;   ///< distinguishes concurrent pointers (touch, pens)
};

struct WheelEvent {
    core::DVec2 viewPos{};
    /// Mouse wheel notches ×120 (Qt convention); 0 for pixel-precise devices.
    core::DVec2 angleDelta{};
    /// Scroll distance in view pixels from touchpads; zero for classic wheels.
    core::DVec2 pixelDelta{};
    Modifiers modifiers{};
};

/// A pinch/zoom gesture step (touchpad or touch screen).
struct ZoomGestureEvent {
    core::DVec2 viewPos{};
    double scaleFactor = 1.0; ///< relative to the previous step
};

enum class Key : std::uint8_t {
    Space,
    Escape,
    Delete, ///< Delete or Backspace
    Other,
};

struct KeyEvent {
    Key key = Key::Other;
    bool pressed = true;
    bool autoRepeat = false;
    Modifiers modifiers{};
};

/// Cursor the canvas wants; the UI maps it to a platform cursor.
enum class CursorShape : std::uint8_t {
    Arrow,
    Crosshair,
    OpenHand,
    ClosedHand,
    SizeAll,
    ZoomIn,
    /// A ring of kEraserRadiusViewPx around the hot spot: the stroke eraser's reach. Shown
    /// as the platform cursor, not drawn by the renderer, so it moves with the pointer
    /// without the one to two frames of latency of anything presented through the
    /// compositor, and it disappears when the pointer leaves the canvas.
    EraserRing,
};

/// Radius of the stroke eraser, in view pixels (the size of CursorShape::EraserRing).
inline constexpr double kEraserRadiusViewPx = 8.0;

} // namespace studyapp::canvas
