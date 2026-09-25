#include <studyapp/core/Geometry.hpp>

#include <studyapp/core/Affine2.hpp>
#include <studyapp/core/Profiler.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numbers>

namespace studyapp::core {
namespace {

void expectNear(const DVec2& actual, const DVec2& expected, double tolerance = 1e-9) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
}

TEST(Affine2Test, ComposesRightToLeft) {
    const Affine2 t = Affine2::translation({10, 0}) * Affine2::scaling(2, 3);
    expectNear(t.apply({1, 1}), {12, 3}); // scale first, then translate
    const Affine2 u = Affine2::scaling(2, 3) * Affine2::translation({10, 0});
    expectNear(u.apply({1, 1}), {22, 3});
}

TEST(Affine2Test, RotationTurnsXTowardsY) {
    const Affine2 r = Affine2::rotation(std::numbers::pi / 2);
    expectNear(r.apply({1, 0}), {0, 1});
    expectNear(r.apply({0, 1}), {-1, 0});
}

TEST(Affine2Test, InverseRoundTrips) {
    const Affine2 t =
        Affine2::translation({-5, 7}) * Affine2::rotation(0.7) * Affine2::scaling(-2, 0.5);
    const auto inverse = t.inverse();
    ASSERT_TRUE(inverse.has_value());
    for (const DVec2 p : {DVec2{0, 0}, DVec2{3, -4}, DVec2{1e6, 2.5}}) {
        expectNear(inverse->apply(t.apply(p)), p, 1e-6);
    }
    EXPECT_FALSE(Affine2::scaling(0, 1).inverse().has_value());
    // The linear part ignores translation.
    expectNear(Affine2::translation({9, 9}).applyVector({1, 2}), {1, 2});
}

TEST(GeometryTest, DistanceToSegment) {
    EXPECT_DOUBLE_EQ(distanceSquaredToSegment<double>({5, 3}, {0, 0}, {10, 0}), 9.0);
    EXPECT_DOUBLE_EQ(distanceSquaredToSegment<double>({-3, 4}, {0, 0}, {10, 0}), 25.0);
    EXPECT_DOUBLE_EQ(distanceSquaredToSegment<double>({2, 2}, {1, 1}, {1, 1}), 2.0); // point
    EXPECT_DOUBLE_EQ(closestParameterOnSegment<double>({5, 3}, {0, 0}, {10, 0}), 0.5);
}

TEST(GeometryTest, SegmentIntersection) {
    EXPECT_TRUE(segmentsIntersect<double>({0, 0}, {10, 10}, {0, 10}, {10, 0}));
    EXPECT_FALSE(segmentsIntersect<double>({0, 0}, {1, 1}, {2, 2}, {3, 3}));   // collinear gap
    EXPECT_TRUE(segmentsIntersect<double>({0, 0}, {2, 2}, {1, 1}, {3, 3}));    // overlap
    EXPECT_TRUE(segmentsIntersect<double>({0, 0}, {10, 0}, {10, 0}, {10, 5})); // touching end
    EXPECT_DOUBLE_EQ(distanceSquaredBetweenSegments<double>({0, 0}, {10, 0}, {0, 3}, {10, 3}), 9.0);
    EXPECT_DOUBLE_EQ(distanceSquaredBetweenSegments<double>({0, 0}, {10, 10}, {0, 10}, {10, 0}),
                     0.0);
}

TEST(GeometryTest, SegmentRectangle) {
    const DRect rect{{0, 0}, {10, 10}};
    EXPECT_TRUE(segmentIntersectsRect<double>({5, 5}, {6, 6}, rect));      // inside
    EXPECT_TRUE(segmentIntersectsRect<double>({-5, 5}, {15, 5}, rect));    // crosses
    EXPECT_FALSE(segmentIntersectsRect<double>({-5, -5}, {-1, 20}, rect)); // beside
    EXPECT_FALSE(segmentIntersectsRect<double>({-5, 5}, {5, -5}, DRect::emptyBounds()));
    EXPECT_TRUE(segmentIntersectsRect<double>({-5, 5}, {5, -5}, rect)); // clips a corner
}

TEST(ProfilerTest, AccumulatesNamedSections) {
    Profiler profiler;
    profiler.record("frame", std::chrono::milliseconds(2));
    profiler.record("frame", std::chrono::milliseconds(4));
    profiler.record("query", std::chrono::milliseconds(1));
    ASSERT_EQ(profiler.sections().size(), 2U);
    const Profiler::Section* frame = profiler.find("frame");
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->count, 2U);
    EXPECT_DOUBLE_EQ(frame->lastMs(), 4.0);
    EXPECT_DOUBLE_EQ(frame->averageMs(), 3.0);
    EXPECT_EQ(frame->max, std::chrono::milliseconds(4));
    {
        STUDYAPP_PROFILE_SCOPE(&profiler, "scoped");
    }
    EXPECT_EQ(profiler.find("scoped") != nullptr, STUDYAPP_ENABLE_PROFILING != 0);
    profiler.reset();
    EXPECT_TRUE(profiler.sections().empty());
}

} // namespace
} // namespace studyapp::core
