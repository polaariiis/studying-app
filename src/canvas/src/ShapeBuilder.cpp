#include <studyapp/canvas/ShapeBuilder.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace studyapp::canvas {

std::optional<ShapeGeometry> shapeFromDrag(const ShapeStyle& style, const core::DVec2& from,
                                           const core::DVec2& to, bool constrain) {
    using document::ShapeKind;
    core::DVec2 delta = to - from;
    const bool isLine = style.kind == ShapeKind::Line || style.kind == ShapeKind::Arrow;
    document::Shape shape{.kind = style.kind,
                          .size = {},
                          .strokeColor = style.color,
                          .strokeWidth = style.width,
                          .fillColor = std::nullopt};
    if (isLine) {
        double length = delta.length();
        if (!(length > 0.0) || !std::isfinite(length)) {
            return std::nullopt;
        }
        double angle = std::atan2(delta.y, delta.x);
        if (constrain) {
            constexpr double step = std::numbers::pi / 4.0;
            angle = std::round(angle / step) * step;
        }
        shape.size = {static_cast<float>(length), 0.0F};
        return ShapeGeometry{.transform = {.position = from, .rotation = static_cast<float>(angle)},
                             .shape = shape};
    }
    if (constrain) {
        const double side = std::max(std::abs(delta.x), std::abs(delta.y));
        delta = {std::copysign(side, delta.x), std::copysign(side, delta.y)};
    }
    const core::DVec2 corner{std::min(from.x, from.x + delta.x),
                             std::min(from.y, from.y + delta.y)};
    shape.size = {static_cast<float>(std::abs(delta.x)), static_cast<float>(std::abs(delta.y))};
    if (!(shape.size.x > 0.0F || shape.size.y > 0.0F) || !std::isfinite(shape.size.x) ||
        !std::isfinite(shape.size.y)) {
        return std::nullopt;
    }
    if (style.fill) {
        core::Color tint = style.color;
        tint.a = kShapeFillAlpha;
        shape.fillColor = tint;
    }
    return ShapeGeometry{.transform = {.position = corner}, .shape = shape};
}

} // namespace studyapp::canvas
