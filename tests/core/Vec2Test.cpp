#include <studyapp/core/Vec2.hpp>

#include <gtest/gtest.h>

#include <cmath>

namespace studyapp::core {
namespace {

TEST(Vec2Test, DefaultConstructsToZero) {
    constexpr Vec2 v;
    EXPECT_EQ(v.x, 0.0F);
    EXPECT_EQ(v.y, 0.0F);
}

TEST(Vec2Test, ArithmeticOperators) {
    constexpr Vec2 a{1.0F, 2.0F};
    constexpr Vec2 b{3.0F, -4.0F};

    EXPECT_EQ(a + b, (Vec2{4.0F, -2.0F}));
    EXPECT_EQ(a - b, (Vec2{-2.0F, 6.0F}));
    EXPECT_EQ(-a, (Vec2{-1.0F, -2.0F}));
    EXPECT_EQ(a * 2.0F, (Vec2{2.0F, 4.0F}));
    EXPECT_EQ(2.0F * a, (Vec2{2.0F, 4.0F}));
    EXPECT_EQ(b / 2.0F, (Vec2{1.5F, -2.0F}));

    Vec2 c = a;
    c += b;
    c -= Vec2{1.0F, 1.0F};
    c *= 3.0F;
    c /= 3.0F;
    EXPECT_EQ(c, (Vec2{3.0F, -3.0F}));
}

TEST(Vec2Test, IsUsableInConstantExpressions) {
    constexpr Vec2 sum = Vec2{1.0F, 1.0F} + Vec2{2.0F, 3.0F};
    static_assert(sum == Vec2{3.0F, 4.0F});
    static_assert(Vec2{3.0F, 4.0F}.lengthSquared() == 25.0F);
    static_assert(Vec2{1.0F, 0.0F}.dot(Vec2{0.0F, 1.0F}) == 0.0F);
    SUCCEED();
}

TEST(Vec2Test, DotAndCross) {
    constexpr Vec2 a{2.0F, 3.0F};
    constexpr Vec2 b{4.0F, -1.0F};
    EXPECT_FLOAT_EQ(a.dot(b), 5.0F);
    EXPECT_FLOAT_EQ(a.cross(b), -14.0F);
    EXPECT_FLOAT_EQ(b.cross(a), 14.0F);
}

TEST(Vec2Test, LengthAndNormalization) {
    constexpr Vec2 v{3.0F, 4.0F};
    EXPECT_FLOAT_EQ(v.length(), 5.0F);
    EXPECT_FLOAT_EQ(v.lengthSquared(), 25.0F);

    const Vec2 unit = v.normalized();
    EXPECT_FLOAT_EQ(unit.x, 0.6F);
    EXPECT_FLOAT_EQ(unit.y, 0.8F);
    EXPECT_FLOAT_EQ(unit.length(), 1.0F);
}

TEST(Vec2Test, NormalizingZeroVectorYieldsZero) {
    EXPECT_EQ(Vec2{}.normalized(), Vec2{});
}

TEST(Vec2Test, DistanceAndLerp) {
    constexpr Vec2 a{1.0F, 1.0F};
    constexpr Vec2 b{4.0F, 5.0F};
    EXPECT_FLOAT_EQ(distance(a, b), 5.0F);
    EXPECT_FLOAT_EQ(distanceSquared(a, b), 25.0F);
    EXPECT_EQ(lerp(a, b, 0.0F), a);
    EXPECT_EQ(lerp(a, b, 1.0F), b);
    EXPECT_EQ(lerp(a, b, 0.5F), (Vec2{2.5F, 3.0F}));
}

TEST(Vec2Test, ComponentMinMax) {
    constexpr Vec2 a{1.0F, 5.0F};
    constexpr Vec2 b{3.0F, 2.0F};
    EXPECT_EQ(componentMin(a, b), (Vec2{1.0F, 2.0F}));
    EXPECT_EQ(componentMax(a, b), (Vec2{3.0F, 5.0F}));
}

TEST(DVec2Test, KeepsPrecisionFarFromOrigin) {
    // World coordinates are doubles so the infinite canvas stays precise far from the
    // origin (docs/CANVAS.md, "floating origin").
    constexpr DVec2 far{1.0e9, -1.0e9};
    constexpr DVec2 offset{0.001, 0.001};
    const DVec2 moved = far + offset;
    EXPECT_NEAR(moved.x - far.x, 0.001, 1e-6);
    EXPECT_NEAR(moved.y - far.y, 0.001, 1e-6);

    // The same arithmetic in float loses the offset entirely.
    const Vec2 farF = vectorCast<float>(far);
    const Vec2 movedF = farF + vectorCast<float>(offset);
    EXPECT_EQ(movedF, farF);
}

TEST(DVec2Test, ArithmeticAndLength) {
    constexpr DVec2 a{0.5, -1.5};
    constexpr DVec2 b{2.5, 0.5};
    EXPECT_EQ(a + b, (DVec2{3.0, -1.0}));
    EXPECT_DOUBLE_EQ(distance(a, b), std::sqrt(8.0));
    EXPECT_DOUBLE_EQ((DVec2{6.0, 8.0}).length(), 10.0);
}

TEST(DVec2Test, VectorCastConvertsPrecision) {
    constexpr Vec2 f{1.5F, -2.25F};
    constexpr DVec2 d = vectorCast<double>(f);
    static_assert(d == DVec2{1.5, -2.25});
    EXPECT_EQ(vectorCast<float>(d), f);
}

} // namespace
} // namespace studyapp::core
