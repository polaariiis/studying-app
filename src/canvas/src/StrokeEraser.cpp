#include <studyapp/canvas/StrokeEraser.hpp>

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/core/Geometry.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace studyapp::canvas {

namespace {

using core::DRect;
using core::DVec2;
using document::StrokePoint;

constexpr double kMinCutParameter = 1e-9;

DVec2 positionOf(const StrokePoint& p) noexcept {
    return {static_cast<double>(p.x), static_cast<double>(p.y)};
}

DRect boundsOf(const std::vector<StrokePoint>& points) noexcept {
    DRect bounds = DRect::emptyBounds();
    for (const StrokePoint& p : points) {
        bounds = bounds.including(positionOf(p));
    }
    return bounds;
}

double lengthOf(const std::vector<StrokePoint>& points) noexcept {
    double length = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        length += core::distance(positionOf(points[i - 1]), positionOf(points[i]));
    }
    return length;
}

void unite(ParameterInterval& acc, double t0, double t1) noexcept {
    if (t0 <= t1) {
        acc.t0 = std::min(acc.t0, t0);
        acc.t1 = std::max(acc.t1, t1);
    }
}

/// Parameters where p0 + t·d lies within `radius` of `centre`.
void uniteDisc(ParameterInterval& acc, const DVec2& p0, const DVec2& d, const DVec2& centre,
               double radius) noexcept {
    const DVec2 f = p0 - centre;
    const double a = d.dot(d);
    const double c = f.dot(f) - radius * radius;
    if (a == 0.0) {
        if (c <= 0.0) {
            unite(acc, -std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity());
        }
        return;
    }
    const double b = 2.0 * d.dot(f);
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) {
        return;
    }
    const double root = std::sqrt(discriminant);
    unite(acc, (-b - root) / (2.0 * a), (-b + root) / (2.0 * a));
}

/// Narrows [lo, hi] to the parameters where x0 + t·dx lies in [min, max].
void clampLinear(double& lo, double& hi, double x0, double dx, double min, double max) noexcept {
    if (dx == 0.0) {
        if (x0 < min || x0 > max) {
            lo = 1.0;
            hi = 0.0;
        }
        return;
    }
    double t0 = (min - x0) / dx;
    double t1 = (max - x0) / dx;
    if (t0 > t1) {
        std::swap(t0, t1);
    }
    lo = std::max(lo, t0);
    hi = std::min(hi, t1);
}

StrokePoint pointAt(const StrokePoint& p, const StrokePoint& q, double t) noexcept {
    const auto tf = static_cast<float>(t);
    return {p.x + (q.x - p.x) * tf, p.y + (q.y - p.y) * tf,
            std::clamp(p.pressure + (q.pressure - p.pressure) * tf, 0.0F, 1.0F)};
}

void append(std::vector<StrokePoint>& points, const StrokePoint& p) {
    if (points.empty() || points.back() != p) {
        points.push_back(p);
    }
}

/// Cuts one run of points the capsule may reach. Appends what survives to `out`; returns
/// false (appending nothing) if the capsule misses it.
bool cutPoints(std::span<const StrokePoint> points, const document::Stroke& stroke,
               const EraserCapsule& capsule, double minLength, std::vector<StrokePiece>& out) {
    const auto reach = [&](const StrokePoint& p, const StrokePoint& q) {
        return capsule.radius + static_cast<double>(std::max(strokeRadius(stroke, p.pressure),
                                                             strokeRadius(stroke, q.pressure)));
    };
    if (points.size() == 1) {
        const double r = reach(points[0], points[0]);
        return core::distanceSquaredToSegment(positionOf(points[0]), capsule.a, capsule.b) <=
               r * r; // a dot is either erased whole or untouched
    }
    const DRect capsuleBounds = DRect::fromPoints(capsule.a, capsule.b);
    // Intervals per segment; bounds reject most segments of a long stroke cheaply.
    std::vector<ParameterInterval> cuts(points.size() - 1);
    bool hit = false;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double r = reach(points[i], points[i + 1]);
        const DVec2 p0 = positionOf(points[i]);
        const DVec2 p1 = positionOf(points[i + 1]);
        if (!DRect::fromPoints(p0, p1).intersects(capsuleBounds.expanded(r))) {
            continue;
        }
        // A contact of zero length (e.g. an end left exactly on the eraser's edge by an
        // earlier cut) removes no ink: it is not a cut.
        if (const ParameterInterval cut = segmentInCapsule(p0, p1, capsule.a, capsule.b, r);
            cut.t1 - cut.t0 > kMinCutParameter) {
            cuts[i] = cut;
            hit = true;
        }
    }
    if (!hit) {
        return false;
    }
    std::vector<StrokePoint> current;
    const auto flush = [&] {
        if (current.size() >= 2 && lengthOf(current) >= minLength) {
            DRect bounds = boundsOf(current);
            out.push_back({.points = std::move(current), .bounds = bounds});
        }
        current.clear();
    };
    if (cuts[0].empty() || cuts[0].t0 > 0.0) {
        current.push_back(points[0]);
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const ParameterInterval& cut = cuts[i];
        if (cut.empty()) {
            if (current.empty()) {
                current.push_back(points[i]); // the previous cut ended right at this point
            }
            append(current, points[i + 1]);
            continue;
        }
        if (cut.t0 > 0.0) {
            if (current.empty()) {
                current.push_back(points[i]);
            }
            append(current, pointAt(points[i], points[i + 1], cut.t0));
        }
        flush();
        if (cut.t1 < 1.0) {
            current.push_back(pointAt(points[i], points[i + 1], cut.t1));
            append(current, points[i + 1]);
        }
    }
    flush();
    return true;
}

} // namespace

ParameterInterval segmentInCapsule(const DVec2& p0, const DVec2& p1, const DVec2& a, const DVec2& b,
                                   double radius) noexcept {
    // The capsule is the union of two discs and the rectangle between them; the points of
    // a segment inside a convex set form one interval, so the union of the three
    // intervals is that interval.
    ParameterInterval acc;
    const DVec2 d = p1 - p0;
    uniteDisc(acc, p0, d, a, radius);
    uniteDisc(acc, p0, d, b, radius);
    const DVec2 axis = b - a;
    const double length = axis.length();
    if (length > 0.0) {
        const DVec2 u = axis / length;
        const DVec2 n{-u.y, u.x};
        const DVec2 f = p0 - a;
        double lo = -std::numeric_limits<double>::infinity();
        double hi = std::numeric_limits<double>::infinity();
        clampLinear(lo, hi, f.dot(u), d.dot(u), 0.0, length);
        clampLinear(lo, hi, f.dot(n), d.dot(n), -radius, radius);
        unite(acc, lo, hi);
    }
    if (acc.empty()) {
        return acc;
    }
    acc.t0 = std::max(acc.t0, 0.0);
    acc.t1 = std::min(acc.t1, 1.0);
    return acc;
}

std::optional<std::vector<StrokePiece>> eraseStroke(const document::Stroke& stroke,
                                                    const EraserCapsule& capsule,
                                                    double minFragmentLength) {
    std::vector<StrokePiece> pieces;
    if (!stroke.points ||
        !cutPoints(*stroke.points, stroke, capsule,
                   std::max(static_cast<double>(stroke.baseWidth), minFragmentLength), pieces)) {
        return std::nullopt;
    }
    return pieces;
}

StrokePiece wholeStroke(const document::Stroke& stroke) {
    StrokePiece piece;
    if (stroke.points) {
        piece.points = *stroke.points;
        piece.bounds = boundsOf(piece.points);
    }
    return piece;
}

bool erasePieces(std::vector<StrokePiece>& pieces, const document::Stroke& stroke,
                 const EraserCapsule& capsule, double minFragmentLength) {
    const double maxInk = static_cast<double>(stroke.baseWidth) * 0.5;
    const DRect reach = DRect::fromPoints(capsule.a, capsule.b).expanded(capsule.radius + maxInk);
    const double minLength = std::max(static_cast<double>(stroke.baseWidth), minFragmentLength);
    bool changed = false;
    std::vector<StrokePiece> result;
    result.reserve(pieces.size() + 1);
    for (StrokePiece& piece : pieces) {
        if (!piece.bounds.intersects(reach) ||
            !cutPoints(piece.points, stroke, capsule, minLength, result)) {
            result.push_back(std::move(piece)); // untouched: moved, not copied
            continue;
        }
        changed = true;
    }
    pieces = std::move(result);
    return changed;
}

} // namespace studyapp::canvas
