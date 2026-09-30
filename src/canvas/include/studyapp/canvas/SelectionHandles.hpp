#pragma once

#include <studyapp/core/Vec2.hpp>
#include <studyapp/document/Element.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace studyapp::canvas {

// Selection handles (docs/CANVAS.md §7): what a single selected element can be resized or
// re-shaped with, and the geometry a handle drag gives. Pure functions on world
// coordinates; the select tool turns the result into one command on release.
//
//   * shapes (rectangle, ellipse) and images: 8 handles on the box (corners and edges);
//   * text boxes: left and right (the width; the height follows the text layout);
//   * lines, arrows and connectors: their two ends.
// Rotated or scaled elements and strokes have no handles (Phase 6 has no rotation tool).

enum class HandleKind : std::uint8_t {
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Start, ///< a line's or connector's start
    End,   ///< a line's or connector's end
};

struct Handle {
    HandleKind kind{};
    core::DVec2 world;
};

/// Handles of `element` when it is the only selected element (empty: none).
[[nodiscard]] std::vector<Handle> handlesFor(const document::Element& element);

struct HandleDragOptions {
    bool constrain = false; ///< Shift: keep the aspect ratio (images: free instead); lines: 45°
    double minSize = 1.0;   ///< world units; boxes never get smaller
    /// Height of a text box's text at a width and font size (the canvas's text layout).
    std::function<float(std::string_view, float, float)> textHeight;
};

/// `element` with `handle` dragged to `world`: boxes keep the opposite corner or edge
/// (a drag past it flips the box); images keep their aspect ratio on corner handles unless
/// constrained; connector ends move freely and are detached (the tool re-attaches them).
/// nullopt if the element has no such handle.
[[nodiscard]] std::optional<document::Element> dragHandle(const document::Element& element,
                                                          HandleKind handle,
                                                          const core::DVec2& world,
                                                          const HandleDragOptions& options);

} // namespace studyapp::canvas
