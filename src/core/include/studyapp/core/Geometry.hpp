#pragma once

#include <studyapp/core/Rect.hpp>
#include <studyapp/core/Vec2.hpp>

#include <algorithm>
#include <concepts>

namespace studyapp::core {

// Small exact 2D predicates used for hit testing and erasing (docs/CANVAS.md §6).
// Header-only templates over float/double; no allocation, no state.

/// Squared distance from `p` to the segment [a, b] (a point if a == b).
template <std::floating_point T>
[[nodiscard]] constexpr T distanceSquaredToSegment(const Vector2<T>& p, const Vector2<T>& a,
                                                   const Vector2<T>& b) noexcept {
    const Vector2<T> ab = b - a;
    const T lengthSq = ab.lengthSquared();
    if (lengthSq == T{0}) {
        return (p - a).lengthSquared();
    }
    const T t = std::clamp((p - a).dot(ab) / lengthSq, T{0}, T{1});
    return (p - (a + ab * t)).lengthSquared();
}

/// Parameter t in [0, 1] of the point on [a, b] closest to `p`.
template <std::floating_point T>
[[nodiscard]] constexpr T closestParameterOnSegment(const Vector2<T>& p, const Vector2<T>& a,
                                                    const Vector2<T>& b) noexcept {
    const Vector2<T> ab = b - a;
    const T lengthSq = ab.lengthSquared();
    return lengthSq == T{0} ? T{0} : std::clamp((p - a).dot(ab) / lengthSq, T{0}, T{1});
}

/// True if the closed segments [a, b] and [c, d] share at least one point.
template <std::floating_point T>
[[nodiscard]] constexpr bool segmentsIntersect(const Vector2<T>& a, const Vector2<T>& b,
                                               const Vector2<T>& c, const Vector2<T>& d) noexcept {
    const auto orientation = [](const Vector2<T>& p, const Vector2<T>& q, const Vector2<T>& r) {
        const T value = (q - p).cross(r - p);
        return value > T{0} ? 1 : (value < T{0} ? -1 : 0);
    };
    const auto onSegment = [](const Vector2<T>& p, const Vector2<T>& q, const Vector2<T>& r) {
        // r is collinear with [p, q]: is it within the segment's box?
        return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) &&
               std::min(p.y, q.y) <= r.y && r.y <= std::max(p.y, q.y);
    };
    const int o1 = orientation(a, b, c);
    const int o2 = orientation(a, b, d);
    const int o3 = orientation(c, d, a);
    const int o4 = orientation(c, d, b);
    if (o1 != o2 && o3 != o4) {
        return true;
    }
    return (o1 == 0 && onSegment(a, b, c)) || (o2 == 0 && onSegment(a, b, d)) ||
           (o3 == 0 && onSegment(c, d, a)) || (o4 == 0 && onSegment(c, d, b));
}

/// Squared distance between the segments [a, b] and [c, d] (0 if they intersect).
template <std::floating_point T>
[[nodiscard]] constexpr T distanceSquaredBetweenSegments(const Vector2<T>& a, const Vector2<T>& b,
                                                         const Vector2<T>& c,
                                                         const Vector2<T>& d) noexcept {
    if (segmentsIntersect(a, b, c, d)) {
        return T{0};
    }
    return std::min({distanceSquaredToSegment(a, c, d), distanceSquaredToSegment(b, c, d),
                     distanceSquaredToSegment(c, a, b), distanceSquaredToSegment(d, a, b)});
}

/// True if the segment [a, b] touches the (non-empty) rectangle.
template <std::floating_point T>
[[nodiscard]] constexpr bool segmentIntersectsRect(const Vector2<T>& a, const Vector2<T>& b,
                                                   const BasicRect<T>& rect) noexcept {
    if (rect.isEmpty()) {
        return false;
    }
    if (rect.contains(a) || rect.contains(b)) {
        return true;
    }
    if (!rect.intersects(BasicRect<T>::fromPoints(a, b))) {
        return false;
    }
    const Vector2<T> tl = rect.min;
    const Vector2<T> tr{rect.max.x, rect.min.y};
    const Vector2<T> br = rect.max;
    const Vector2<T> bl{rect.min.x, rect.max.y};
    return segmentsIntersect(a, b, tl, tr) || segmentsIntersect(a, b, tr, br) ||
           segmentsIntersect(a, b, br, bl) || segmentsIntersect(a, b, bl, tl);
}

} // namespace studyapp::core
