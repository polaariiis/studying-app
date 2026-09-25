#pragma once

#include <studyapp/core/Vec2.hpp>

#include <concepts>
#include <limits>

namespace studyapp::core {

/// Axis-aligned rectangle stored as closed interval [min, max] on both axes.
///
/// A rectangle is *empty* when `min > max` on either axis. `emptyBounds()` returns the
/// canonical empty rectangle (min = +inf, max = -inf), which is the identity for
/// `united()` and is convenient for accumulating bounds. Zero-area rectangles (a single
/// point or a line) are not empty.
template <std::floating_point T>
struct BasicRect {
    using value_type = T;
    using vector_type = Vector2<T>;

    vector_type min{};
    vector_type max{};

    [[nodiscard]] static constexpr BasicRect emptyBounds() noexcept {
        constexpr T inf = std::numeric_limits<T>::infinity();
        return {{inf, inf}, {-inf, -inf}};
    }

    /// Smallest rectangle containing both points (in any order).
    [[nodiscard]] static constexpr BasicRect fromPoints(const vector_type& a,
                                                        const vector_type& b) noexcept {
        return {componentMin(a, b), componentMax(a, b)};
    }

    /// Rectangle from an origin and a size; negative sizes are normalised.
    [[nodiscard]] static constexpr BasicRect fromOriginSize(const vector_type& origin,
                                                            const vector_type& size) noexcept {
        return fromPoints(origin, origin + size);
    }

    [[nodiscard]] constexpr bool isEmpty() const noexcept { return min.x > max.x || min.y > max.y; }
    [[nodiscard]] constexpr T width() const noexcept { return isEmpty() ? T{0} : max.x - min.x; }
    [[nodiscard]] constexpr T height() const noexcept { return isEmpty() ? T{0} : max.y - min.y; }
    [[nodiscard]] constexpr vector_type size() const noexcept { return {width(), height()}; }
    [[nodiscard]] constexpr T area() const noexcept { return width() * height(); }
    [[nodiscard]] constexpr vector_type center() const noexcept { return (min + max) / T{2}; }

    [[nodiscard]] constexpr bool contains(const vector_type& p) const noexcept {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }

    /// True if `other` lies entirely inside this rectangle. An empty rectangle is contained
    /// in every rectangle; an empty rectangle contains nothing non-empty.
    [[nodiscard]] constexpr bool contains(const BasicRect& other) const noexcept {
        if (other.isEmpty()) {
            return true;
        }
        return !isEmpty() && other.min.x >= min.x && other.max.x <= max.x && other.min.y >= min.y &&
               other.max.y <= max.y;
    }

    /// True if the rectangles share at least one point (touching edges intersect).
    [[nodiscard]] constexpr bool intersects(const BasicRect& other) const noexcept {
        return !isEmpty() && !other.isEmpty() && min.x <= other.max.x && other.min.x <= max.x &&
               min.y <= other.max.y && other.min.y <= max.y;
    }

    [[nodiscard]] constexpr BasicRect united(const BasicRect& other) const noexcept {
        return {componentMin(min, other.min), componentMax(max, other.max)};
    }

    [[nodiscard]] constexpr BasicRect including(const vector_type& p) const noexcept {
        return {componentMin(min, p), componentMax(max, p)};
    }

    /// Overlapping region, or `emptyBounds()` if the rectangles do not intersect.
    [[nodiscard]] constexpr BasicRect intersected(const BasicRect& other) const noexcept {
        if (!intersects(other)) {
            return emptyBounds();
        }
        return {componentMax(min, other.min), componentMin(max, other.max)};
    }

    /// Grows (or, for negative `margin`, shrinks) the rectangle on every side.
    [[nodiscard]] constexpr BasicRect expanded(T margin) const noexcept {
        if (isEmpty()) {
            return *this;
        }
        return {{min.x - margin, min.y - margin}, {max.x + margin, max.y + margin}};
    }

    [[nodiscard]] constexpr BasicRect translated(const vector_type& offset) const noexcept {
        return {min + offset, max + offset};
    }

    [[nodiscard]] friend constexpr bool operator==(const BasicRect&,
                                                   const BasicRect&) noexcept = default;
};

using Rect = BasicRect<float>;
using DRect = BasicRect<double>;

} // namespace studyapp::core
