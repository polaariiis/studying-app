#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>

namespace studyapp::core {

/// Two-component vector. `Vec2` (float) is used for element-local and GPU-facing data,
/// `DVec2` (double) for world coordinates on the infinite canvas (see docs/CANVAS.md §1).
///
/// An aggregate, so it supports `Vec2{1.f, 2.f}` and designated initializers.
template <std::floating_point T>
struct Vector2 {
    using value_type = T;

    T x{};
    T y{};

    constexpr Vector2& operator+=(const Vector2& rhs) noexcept {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }
    constexpr Vector2& operator-=(const Vector2& rhs) noexcept {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }
    constexpr Vector2& operator*=(T scalar) noexcept {
        x *= scalar;
        y *= scalar;
        return *this;
    }
    constexpr Vector2& operator/=(T scalar) noexcept {
        x /= scalar;
        y /= scalar;
        return *this;
    }

    [[nodiscard]] friend constexpr Vector2 operator+(Vector2 lhs, const Vector2& rhs) noexcept {
        return lhs += rhs;
    }
    [[nodiscard]] friend constexpr Vector2 operator-(Vector2 lhs, const Vector2& rhs) noexcept {
        return lhs -= rhs;
    }
    [[nodiscard]] friend constexpr Vector2 operator-(const Vector2& v) noexcept {
        return {-v.x, -v.y};
    }
    [[nodiscard]] friend constexpr Vector2 operator*(Vector2 v, T scalar) noexcept {
        return v *= scalar;
    }
    [[nodiscard]] friend constexpr Vector2 operator*(T scalar, Vector2 v) noexcept {
        return v *= scalar;
    }
    [[nodiscard]] friend constexpr Vector2 operator/(Vector2 v, T scalar) noexcept {
        return v /= scalar;
    }

    [[nodiscard]] friend constexpr bool operator==(const Vector2&,
                                                   const Vector2&) noexcept = default;

    [[nodiscard]] constexpr T dot(const Vector2& rhs) const noexcept {
        return x * rhs.x + y * rhs.y;
    }
    /// z-component of the 3D cross product; positive when `rhs` is counter-clockwise from
    /// `*this` in a y-up frame (clockwise on screen, where y points down).
    [[nodiscard]] constexpr T cross(const Vector2& rhs) const noexcept {
        return x * rhs.y - y * rhs.x;
    }
    [[nodiscard]] constexpr T lengthSquared() const noexcept { return dot(*this); }
    [[nodiscard]] T length() const noexcept { return std::hypot(x, y); }

    /// Unit vector in the same direction, or the zero vector if the length is zero.
    [[nodiscard]] Vector2 normalized() const noexcept {
        const T len = length();
        return len > T{0} ? *this / len : Vector2{};
    }
};

using Vec2 = Vector2<float>;
using DVec2 = Vector2<double>;

template <std::floating_point T>
[[nodiscard]] constexpr T distanceSquared(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return (b - a).lengthSquared();
}

template <std::floating_point T>
[[nodiscard]] T distance(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return (b - a).length();
}

/// Linear interpolation: `t = 0` gives `a`, `t = 1` gives `b`.
template <std::floating_point T>
[[nodiscard]] constexpr Vector2<T> lerp(const Vector2<T>& a, const Vector2<T>& b, T t) noexcept {
    return a + (b - a) * t;
}

template <std::floating_point T>
[[nodiscard]] constexpr Vector2<T> componentMin(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return {std::min(a.x, b.x), std::min(a.y, b.y)};
}

template <std::floating_point T>
[[nodiscard]] constexpr Vector2<T> componentMax(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return {std::max(a.x, b.x), std::max(a.y, b.y)};
}

/// Explicit precision conversion, e.g. `vectorCast<float>(worldPoint)`.
template <std::floating_point To, std::floating_point From>
[[nodiscard]] constexpr Vector2<To> vectorCast(const Vector2<From>& v) noexcept {
    return {static_cast<To>(v.x), static_cast<To>(v.y)};
}

} // namespace studyapp::core
