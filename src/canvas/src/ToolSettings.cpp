#include <studyapp/canvas/ToolSettings.hpp>

#include <algorithm>
#include <cmath>

namespace studyapp::canvas {

namespace {

PenStyle sanitized(PenStyle style, const PenStyle& defaults) noexcept {
    style.width = std::isfinite(style.width) ? std::clamp(style.width, kMinPenWidth, kMaxPenWidth)
                                             : defaults.width;
    if (style.color.a == 0) {
        style.color.a = defaults.color.a;
    }
    return style;
}

} // namespace

ToolSettings sanitized(ToolSettings settings) noexcept {
    settings.pen = sanitized(settings.pen, PenStyle{});
    settings.pen.brush = document::Brush::Pen;
    settings.highlighter = sanitized(settings.highlighter, kDefaultHighlighter);
    settings.highlighter.brush = document::Brush::Highlighter;
    ShapeStyle& shape = settings.shape;
    shape.width = std::isfinite(shape.width) ? std::clamp(shape.width, kMinPenWidth, kMaxPenWidth)
                                             : ShapeStyle{}.width;
    if (shape.color.a == 0) {
        shape.color.a = 255;
    }
    if (!document::isKnownShapeKind(shape.kind)) {
        shape.kind = document::ShapeKind::Rectangle;
    }
    return settings;
}

} // namespace studyapp::canvas
