#include <studyapp/canvas/ElementGeometry.hpp>

#include <studyapp/core/Geometry.hpp>
#include <studyapp/render/Tessellation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace studyapp::canvas {

namespace {

using core::DRect;
using core::DVec2;
using document::Connector;
using document::Element;
using document::Image;
using document::Shape;
using document::ShapeKind;
using document::Stroke;
using document::TextBox;

/// Arrowhead of an arrow shape `length` long drawn `width` wide: its length along the
/// shaft and half its width. Grows with the line width, never longer than 60 % of the
/// arrow.
struct ArrowHead {
    float length;
    float halfWidth;
};
ArrowHead arrowHead(float length, float width) noexcept {
    const float head = std::min(std::max(3.5F * width, 8.0F), 0.6F * length);
    return {head, head * 0.5F};
}

/// Neutral frame for kinds whose content is drawn from Phase 6 on.
constexpr core::Color kPlaceholderColor = core::Color::fromRgba(0xA3, 0xA3, 0xA3);
constexpr float kPlaceholderHalfWidth = 0.75F;

DVec2 toDouble(const core::Vec2& v) noexcept {
    return {static_cast<double>(v.x), static_cast<double>(v.y)};
}

/// Scale factor of a transform for lengths (geometric mean of the axis scales).
double lengthScale(const core::Affine2& t) noexcept {
    return std::sqrt(std::abs(t.determinant()));
}

std::vector<render::WidthPoint> closedOutline(std::span<const core::Vec2> polygon, float radius) {
    std::vector<render::WidthPoint> points;
    for (const core::Vec2& p : polygon) {
        points.push_back({p, radius});
    }
    if (!polygon.empty()) {
        points.push_back({polygon.front(), radius});
    }
    return points;
}

std::vector<core::Vec2> shapePolygon(const Shape& shape,
                                     const render::TessellationOptions& options) {
    const core::Rect box = core::Rect::fromPoints({0.0F, 0.0F}, shape.size);
    if (shape.kind == ShapeKind::Ellipse) {
        return render::ellipsePolygon(box, options);
    }
    return {box.min, {box.max.x, box.min.y}, box.max, {box.min.x, box.max.y}};
}

std::vector<MeshPart> placeholderFrame(const core::Vec2& size, bool cross) {
    const core::Rect box = core::Rect::fromPoints({0.0F, 0.0F}, size);
    MeshPart part{.mesh = render::tessellateRectOutline(box, kPlaceholderHalfWidth),
                  .color = kPlaceholderColor};
    if (cross) {
        const std::vector<render::WidthPoint> a{{box.min, kPlaceholderHalfWidth},
                                                {box.max, kPlaceholderHalfWidth}};
        const std::vector<render::WidthPoint> b{{{box.max.x, box.min.y}, kPlaceholderHalfWidth},
                                                {{box.min.x, box.max.y}, kPlaceholderHalfWidth}};
        render::appendMesh(part.mesh, render::tessellatePolyline(a));
        render::appendMesh(part.mesh, render::tessellatePolyline(b));
    }
    return {std::move(part)};
}

/// Distance test against a polyline of (world point, radius) pairs.
template <class PointAt, class RadiusAt>
bool polylineWithin(std::size_t count, PointAt pointAt, RadiusAt radiusAt, const DVec2& a,
                    const DVec2& b, double extra) {
    if (count == 0) {
        return false;
    }
    if (count == 1) {
        const double reach = radiusAt(0) + extra;
        return core::distanceSquaredToSegment(pointAt(0), a, b) <= reach * reach;
    }
    const DRect query = DRect::fromPoints(a, b);
    DVec2 previous = pointAt(0);
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const DVec2 next = pointAt(i + 1);
        const double reach = std::max(radiusAt(i), radiusAt(i + 1)) + extra;
        // Most segments of a long stroke are far away: a box test first.
        if (DRect::fromPoints(previous, next).intersects(query.expanded(reach)) &&
            core::distanceSquaredBetweenSegments(previous, next, a, b) <= reach * reach) {
            return true;
        }
        previous = next;
    }
    return false;
}

bool strokeWithin(const Element& element, const Stroke& stroke, const DVec2& a, const DVec2& b,
                  double extra) {
    if (!stroke.points || stroke.points->empty()) {
        return false;
    }
    const core::Affine2 toWorld = document::localToWorld(element.transform);
    const double scale = lengthScale(toWorld);
    const auto& points = *stroke.points;
    return polylineWithin(
        points.size(), [&](std::size_t i) { return toWorld.apply({points[i].x, points[i].y}); },
        [&](std::size_t i) {
            return static_cast<double>(strokeRadius(stroke, points[i].pressure)) * scale;
        },
        a, b, extra);
}

} // namespace

float strokeRadius(const Stroke& stroke, float pressure) noexcept {
    if (stroke.brush == document::Brush::Highlighter) {
        return stroke.baseWidth * 0.5F; // an even band: pressure is recorded, not applied
    }
    const float p = std::isfinite(pressure) ? std::clamp(pressure, 0.0F, 1.0F) : 1.0F;
    return stroke.baseWidth * 0.5F * (0.3F + 0.7F * p);
}

render::MeshData tessellateStrokePoints(const Stroke& stroke,
                                        std::span<const document::StrokePoint> points,
                                        float pixelsPerUnit) {
    std::vector<render::WidthPoint> widthPoints;
    widthPoints.reserve(points.size());
    for (const auto& p : points) {
        widthPoints.push_back({{p.x, p.y}, strokeRadius(stroke, p.pressure)});
    }
    return render::tessellatePolyline(widthPoints, {.pixelsPerUnit = pixelsPerUnit});
}

core::DVec2 attachPoint(const Element& target, const DVec2& towards) {
    const DRect box = visualBounds(target);
    const DVec2 centre = box.center();
    const DVec2 d = towards - centre;
    if (box.isEmpty() || (d.x == 0.0 && d.y == 0.0)) {
        return centre;
    }
    // Where the ray from the centre towards `towards` leaves the box.
    const DVec2 half = box.size() * 0.5;
    const double tx = d.x != 0.0 ? half.x / std::abs(d.x) : std::numeric_limits<double>::infinity();
    const double ty = d.y != 0.0 ? half.y / std::abs(d.y) : std::numeric_limits<double>::infinity();
    return centre + d * std::min(tx, ty);
}

core::DRect visualBounds(const Element& element) {
    if (const auto* connector = std::get_if<Connector>(&element.payload)) {
        const double length = (connector->end.position - connector->start.position).length();
        const double head =
            length > 0.0 ? static_cast<double>(
                               arrowHead(static_cast<float>(length), connector->width).halfWidth)
                         : 0.0;
        return document::worldBounds(element).expanded(head);
    }
    const DRect bounds = document::worldBounds(element);
    const auto* shape = std::get_if<Shape>(&element.payload);
    if (shape == nullptr || bounds.isEmpty() || !shape->strokeColor) {
        return bounds;
    }
    double reach = static_cast<double>(shape->strokeWidth) * 0.5;
    if (shape->kind == ShapeKind::Arrow) {
        reach = std::max(
            reach, static_cast<double>(arrowHead(shape->size.x, shape->strokeWidth).halfWidth));
    }
    return bounds.expanded(reach * lengthScale(document::localToWorld(element.transform)));
}

core::Affine2 meshToWorld(const Element& element) noexcept {
    if (const auto* connector = std::get_if<Connector>(&element.payload)) {
        return core::Affine2::translation(connector->start.position);
    }
    return document::localToWorld(element.transform);
}

std::vector<MeshPart> buildElementMeshes(const Element& element, float pixelsPerUnit) {
    const render::TessellationOptions options{.pixelsPerUnit = pixelsPerUnit};
    return std::visit(
        [&](const auto& payload) -> std::vector<MeshPart> {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, Stroke>) {
                if (!payload.points) {
                    return {MeshPart{.mesh = {}, .color = payload.color}};
                }
                return {MeshPart{
                    .mesh = tessellateStrokePoints(payload, *payload.points, pixelsPerUnit),
                    .color = payload.color}};
            } else if constexpr (std::is_same_v<T, Shape>) {
                std::vector<MeshPart> parts;
                const float radius = payload.strokeWidth * 0.5F;
                if (payload.kind == ShapeKind::Line) {
                    if (payload.strokeColor && radius > 0.0F) {
                        const std::vector<render::WidthPoint> line{{{0.0F, 0.0F}, radius},
                                                                   {payload.size, radius}};
                        parts.push_back(
                            {render::tessellatePolyline(line, options), *payload.strokeColor});
                    }
                    return parts;
                }
                if (payload.kind == ShapeKind::Arrow) {
                    // Along +x (the element rotation gives the direction): a shaft that
                    // ends inside a filled head, one mesh (a part covers a pixel once).
                    if (!payload.strokeColor || radius <= 0.0F || payload.size.x <= 0.0F) {
                        return parts;
                    }
                    const float length = payload.size.x;
                    const ArrowHead head = arrowHead(length, payload.strokeWidth);
                    const std::vector<render::WidthPoint> shaft{
                        {{0.0F, 0.0F}, radius}, {{length - head.length * 0.5F, 0.0F}, radius}};
                    render::MeshData mesh = render::tessellatePolyline(shaft, options);
                    const std::array<core::Vec2, 3> tip{
                        core::Vec2{length, 0.0F}, core::Vec2{length - head.length, -head.halfWidth},
                        core::Vec2{length - head.length, head.halfWidth}};
                    render::appendMesh(mesh, render::tessellateConvexFill(tip));
                    parts.push_back({std::move(mesh), *payload.strokeColor});
                    return parts;
                }
                const auto polygon = shapePolygon(payload, options);
                if (payload.fillColor) {
                    parts.push_back({render::tessellateConvexFill(polygon), *payload.fillColor});
                }
                if (payload.strokeColor && radius > 0.0F) {
                    parts.push_back(
                        {render::tessellatePolyline(closedOutline(polygon, radius), options),
                         *payload.strokeColor});
                }
                return parts;
            } else if constexpr (std::is_same_v<T, Connector>) {
                // A straight line from start to end with an arrowhead at the end (the
                // schema's default end cap), in the space of meshToWorld (start at 0).
                const DVec2 end = payload.end.position - payload.start.position;
                const float radius = payload.width * 0.5F;
                const auto length = static_cast<float>(end.length());
                const core::Vec2 tip{static_cast<float>(end.x), static_cast<float>(end.y)};
                if (!(length > 0.0F)) {
                    const std::vector<render::WidthPoint> dot{{{0.0F, 0.0F}, radius}};
                    return {MeshPart{.mesh = render::tessellatePolyline(dot, options),
                                     .color = payload.color}};
                }
                const ArrowHead head = arrowHead(length, payload.width);
                const core::Vec2 u = tip / length;
                const core::Vec2 n{-u.y, u.x};
                const core::Vec2 base = tip - u * head.length;
                const std::vector<render::WidthPoint> shaft{
                    {{0.0F, 0.0F}, radius}, {tip - u * (head.length * 0.5F), radius}};
                render::MeshData mesh = render::tessellatePolyline(shaft, options);
                const std::array<core::Vec2, 3> triangle{tip, base + n * head.halfWidth,
                                                         base - n * head.halfWidth};
                render::appendMesh(mesh, render::tessellateConvexFill(triangle));
                return {MeshPart{.mesh = std::move(mesh), .color = payload.color}};
            } else if constexpr (std::is_same_v<T, TextBox>) {
                return placeholderFrame(payload.size, false);
            } else {
                static_assert(std::is_same_v<T, Image>);
                return placeholderFrame(payload.size, true);
            }
        },
        element.payload);
}

bool hitTest(const Element& element, const DVec2& world, double toleranceWorld) {
    return std::visit(
        [&](const auto& payload) -> bool {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, Stroke>) {
                return strokeWithin(element, payload, world, world, toleranceWorld);
            } else if constexpr (std::is_same_v<T, Connector>) {
                const double reach = static_cast<double>(payload.width) * 0.5 + toleranceWorld;
                return core::distanceSquaredToSegment(world, payload.start.position,
                                                      payload.end.position) <= reach * reach;
            } else {
                const core::Affine2 toWorld = document::localToWorld(element.transform);
                const auto toLocal = toWorld.inverse();
                if (!toLocal) {
                    return false;
                }
                const DVec2 local = toLocal->apply(world);
                const double tolerance = toleranceWorld / std::max(lengthScale(toWorld), 1e-12);
                const DRect box = DRect::fromPoints({0.0, 0.0}, toDouble(payload.size));
                if constexpr (std::is_same_v<T, Shape>) {
                    const double reach = static_cast<double>(payload.strokeWidth) * 0.5 + tolerance;
                    if (payload.kind == ShapeKind::Line) {
                        return core::distanceSquaredToSegment(
                                   local, {0.0, 0.0}, toDouble(payload.size)) <= reach * reach;
                    }
                    if (payload.kind == ShapeKind::Arrow) {
                        const double length = static_cast<double>(payload.size.x);
                        const ArrowHead head = arrowHead(payload.size.x, payload.strokeWidth);
                        const double headReach = static_cast<double>(head.halfWidth) + tolerance;
                        return core::distanceSquaredToSegment(local, {0.0, 0.0}, {length, 0.0}) <=
                                   reach * reach ||
                               core::distanceSquaredToSegment(
                                   local, {length - static_cast<double>(head.length), 0.0},
                                   {length, 0.0}) <= headReach * headReach;
                    }
                    if (payload.kind == ShapeKind::Ellipse) {
                        const DVec2 c = box.center();
                        const DVec2 r = box.size() * 0.5 + DVec2{reach, reach};
                        const DVec2 d = local - c;
                        const double outer = (d.x * d.x) / (r.x * r.x) + (d.y * d.y) / (r.y * r.y);
                        return outer <= 1.0; // filled or not: inside the outer boundary
                    }
                    return box.expanded(reach).contains(local);
                } else {
                    return box.expanded(tolerance).contains(local);
                }
            }
        },
        element.payload);
}

bool intersectsRect(const Element& element, const DRect& world) {
    const auto* stroke = std::get_if<Stroke>(&element.payload);
    if (stroke == nullptr) {
        return document::worldBounds(element).intersects(world);
    }
    if (!stroke->points || stroke->points->empty() ||
        !document::worldBounds(element).intersects(world)) {
        return false;
    }
    const core::Affine2 toWorld = document::localToWorld(element.transform);
    const auto& points = *stroke->points;
    const DRect inflated =
        world.expanded(static_cast<double>(strokeRadius(*stroke, 1.0F)) * lengthScale(toWorld));
    DVec2 previous = toWorld.apply({points[0].x, points[0].y});
    if (points.size() == 1) {
        return inflated.contains(previous);
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        const DVec2 next = toWorld.apply({points[i].x, points[i].y});
        if (core::segmentIntersectsRect(previous, next, inflated)) {
            return true;
        }
        previous = next;
    }
    return false;
}

bool strokeTouchesSegment(const Element& element, const DVec2& a, const DVec2& b,
                          double radiusWorld) {
    const auto* stroke = std::get_if<Stroke>(&element.payload);
    return stroke != nullptr && strokeWithin(element, *stroke, a, b, radiusWorld);
}

} // namespace studyapp::canvas
