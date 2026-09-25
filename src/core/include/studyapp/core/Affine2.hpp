#pragma once

#include <studyapp/core/Vec2.hpp>

#include <cmath>
#include <concepts>
#include <optional>

namespace studyapp::core {

/// 2D affine transform (linear part + translation), column-vector convention:
///
///   | a  c  tx |   | x |
///   | b  d  ty | * | y |
///   | 0  0  1  |   | 1 |
///
/// `lhs * rhs` applies `rhs` first. Used for element local → world transforms and for the
/// camera-relative draw transforms handed to the renderer (docs/RENDERING.md §5).
template <std::floating_point T>
struct BasicAffine2 {
    using value_type = T;
    using vector_type = Vector2<T>;

    T a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    [[nodiscard]] static constexpr BasicAffine2 identity() noexcept { return {}; }
    [[nodiscard]] static constexpr BasicAffine2 translation(const vector_type& t) noexcept {
        return {1, 0, 0, 1, t.x, t.y};
    }
    [[nodiscard]] static constexpr BasicAffine2 scaling(T sx, T sy) noexcept {
        return {sx, 0, 0, sy, 0, 0};
    }
    /// Rotation by `radians`; positive angles turn +x towards +y (clockwise on screen,
    /// where y points down).
    [[nodiscard]] static BasicAffine2 rotation(T radians) noexcept {
        const T cs = std::cos(radians);
        const T sn = std::sin(radians);
        return {cs, sn, -sn, cs, 0, 0};
    }

    [[nodiscard]] constexpr vector_type apply(const vector_type& p) const noexcept {
        return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
    }
    /// Applies the linear part only (directions, extents).
    [[nodiscard]] constexpr vector_type applyVector(const vector_type& v) const noexcept {
        return {a * v.x + c * v.y, b * v.x + d * v.y};
    }

    [[nodiscard]] constexpr T determinant() const noexcept { return a * d - b * c; }

    /// Inverse transform, or nullopt if the transform is singular.
    [[nodiscard]] constexpr std::optional<BasicAffine2> inverse() const noexcept {
        const T det = determinant();
        if (det == T{0}) {
            return std::nullopt;
        }
        const T ia = d / det;
        const T ib = -b / det;
        const T ic = -c / det;
        const T id = a / det;
        return BasicAffine2{ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }

    [[nodiscard]] friend constexpr BasicAffine2 operator*(const BasicAffine2& l,
                                                          const BasicAffine2& r) noexcept {
        return {l.a * r.a + l.c * r.b,          l.b * r.a + l.d * r.b,
                l.a * r.c + l.c * r.d,          l.b * r.c + l.d * r.d,
                l.a * r.tx + l.c * r.ty + l.tx, l.b * r.tx + l.d * r.ty + l.ty};
    }

    template <std::floating_point U>
    [[nodiscard]] constexpr BasicAffine2<U> cast() const noexcept {
        return {static_cast<U>(a), static_cast<U>(b),  static_cast<U>(c),
                static_cast<U>(d), static_cast<U>(tx), static_cast<U>(ty)};
    }

    [[nodiscard]] friend constexpr bool operator==(const BasicAffine2&,
                                                   const BasicAffine2&) noexcept = default;
};

using Affine2 = BasicAffine2<double>;
using Affine2f = BasicAffine2<float>;

} // namespace studyapp::core
