#pragma once

#include <studyapp/core/Color.hpp>
#include <studyapp/core/Vec2.hpp>
#include <studyapp/document/Element.hpp>

#include <cstdint>
#include <optional>

namespace studyapp::canvas {

/// What the next shape looks like (ToolSettings::shape). Tool state, not document data:
/// every shape stores its own kind, colours and width.
struct ShapeStyle {
    document::ShapeKind kind = document::ShapeKind::Rectangle;
    core::Color color = core::Color::black(); ///< outline (lines and arrows: the line)
    float width = 2.0F;                       ///< outline width, world units
    bool fill = false; ///< rectangles and ellipses: a light tint of `color` inside

    [[nodiscard]] friend bool operator==(const ShapeStyle&, const ShapeStyle&) = default;
};

/// Opacity of the fill tint (the outline colour at this alpha).
inline constexpr std::uint8_t kShapeFillAlpha = 0x33;

/// A shape as the document stores it: the element transform and the payload.
struct ShapeGeometry {
    document::Transform transform;
    document::Shape shape;
};

/// The shape dragged from `from` to `to` (world). Rectangles and ellipses fill the box of
/// the two points (position = its top-left corner); lines and arrows run from `from` to
/// `to` (position = `from`, length along local +x, direction = rotation). `constrain`
/// makes boxes square and snaps line directions to multiples of 45°. nullopt when the
/// drag has no extent (both points equal, or a box of zero width and height).
[[nodiscard]] std::optional<ShapeGeometry> shapeFromDrag(const ShapeStyle& style,
                                                         const core::DVec2& from,
                                                         const core::DVec2& to, bool constrain);

} // namespace studyapp::canvas
