#include <studyapp/canvas/StrokeBuilder.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

namespace studyapp::canvas {
namespace {

StrokeSample at(double x, double y, std::uint64_t t, float pressure = 1.0F) {
    return {.world = {x, y}, .pressure = pressure, .timestampUs = t};
}

std::vector<StrokeSample> line(std::size_t count, double step = 1.0) {
    std::vector<StrokeSample> points;
    for (std::size_t i = 0; i < count; ++i) {
        points.push_back(at(static_cast<double>(i) * step, 0.0, i * 8'000));
    }
    return points;
}

// ---------------------------------------------------------------------------- One-Euro

TEST(OneEuroFilterTest, FirstSampleIsUnchangedAndOutputIsDeterministic) {
    const auto run = [] {
        OneEuroFilter filter({}, 1.0);
        std::vector<core::DVec2> out;
        std::mt19937 random(7);
        std::normal_distribution<double> noise(0.0, 0.8);
        for (int i = 0; i < 100; ++i) {
            out.push_back(filter.filter({i * 2.0 + noise(random), noise(random)},
                                        static_cast<std::uint64_t>(i) * 8'000));
        }
        return out;
    };
    const auto a = run();
    const auto b = run();
    EXPECT_EQ(a, b);
    OneEuroFilter filter({}, 1.0);
    EXPECT_EQ(filter.filter({3, 4}, 0), (core::DVec2{3, 4}));
}

TEST(OneEuroFilterTest, ReducesJitterAtRest) {
    OneEuroFilter filter({}, 1.0);
    std::mt19937 random(1);
    std::normal_distribution<double> noise(0.0, 1.0);
    double rawSpread = 0.0;
    double filteredSpread = 0.0;
    for (int i = 0; i < 400; ++i) {
        const core::DVec2 raw{noise(random), noise(random)};
        const core::DVec2 filtered = filter.filter(raw, static_cast<std::uint64_t>(i) * 8'000);
        if (i >= 50) {
            rawSpread += raw.lengthSquared();
            filteredSpread += filtered.lengthSquared();
        }
    }
    EXPECT_LT(filteredSpread, rawSpread * 0.25);
}

TEST(OneEuroFilterTest, FollowsFastMovementWithLittleLag) {
    OneEuroFilter filter({}, 1.0);
    core::DVec2 last{};
    for (int i = 0; i <= 60; ++i) { // 2000 px/s along x
        last = filter.filter({i * 16.0, 0.0}, static_cast<std::uint64_t>(i) * 8'000);
    }
    EXPECT_GT(last.x, 60 * 16.0 - 40.0); // lags less than 40 px behind
}

TEST(OneEuroFilterTest, HandlesMissingTimestamps) {
    OneEuroFilter filter({}, 1.0);
    (void)filter.filter({0, 0}, 5);
    const core::DVec2 next = filter.filter({10, 0}, 5); // same timestamp
    EXPECT_TRUE(std::isfinite(next.x));
    EXPECT_GT(next.x, 0.0);
    EXPECT_LT(next.x, 10.0);
}

// ---------------------------------------------------------------------------- RDP

TEST(SimplifyTest, StraightLineKeepsOnlyTheEnds) {
    const auto points = line(100);
    const auto kept = simplifyIndices(points, 0.25, 0.05F);
    EXPECT_EQ(kept, (std::vector<std::size_t>{0, 99}));
}

TEST(SimplifyTest, ShortAndAlreadySimpleStrokesAreUnchanged) {
    EXPECT_TRUE(simplifyIndices({}, 0.25, 0.05F).empty());
    const std::vector<StrokeSample> one{at(1, 1, 0)};
    EXPECT_EQ(simplifyIndices(one, 0.25, 0.05F), (std::vector<std::size_t>{0}));
    const std::vector<StrokeSample> corner{at(0, 0, 0), at(10, 0, 1), at(10, 10, 2)};
    EXPECT_EQ(simplifyIndices(corner, 0.25, 0.05F), (std::vector<std::size_t>{0, 1, 2}));
}

TEST(SimplifyTest, NoisyLineStaysWithinTolerance) {
    std::mt19937 random(3);
    std::uniform_real_distribution<double> noise(-0.1, 0.1);
    std::vector<StrokeSample> points;
    for (int i = 0; i < 200; ++i) {
        points.push_back(at(i, noise(random), static_cast<std::uint64_t>(i)));
    }
    const auto kept = simplifyIndices(points, 0.25, 0.05F);
    EXPECT_LT(kept.size(), 10U);
    EXPECT_EQ(kept.front(), 0U);
    EXPECT_EQ(kept.back(), 199U);
}

TEST(SimplifyTest, PreservesShapeOfACurve) {
    std::vector<StrokeSample> points;
    for (int i = 0; i <= 360; ++i) {
        const double a = i * std::acos(-1.0) / 180.0;
        points.push_back(at(100 * std::cos(a), 100 * std::sin(a), static_cast<std::uint64_t>(i)));
    }
    const double tolerance = 0.25;
    const auto kept = simplifyIndices(points, tolerance, 1.0F);
    EXPECT_LT(kept.size(), points.size() / 3);
    // Every dropped point lies within the tolerance of its chord.
    for (std::size_t k = 0; k + 1 < kept.size(); ++k) {
        for (std::size_t i = kept[k] + 1; i < kept[k + 1]; ++i) {
            const auto& a = points[kept[k]].world;
            const auto& b = points[kept[k + 1]].world;
            const auto& p = points[i].world;
            const double t = std::clamp((p - a).dot(b - a) / (b - a).lengthSquared(), 0.0, 1.0);
            EXPECT_LE((p - (a + (b - a) * t)).length(), tolerance + 1e-9);
        }
    }
}

TEST(SimplifyTest, KeepsPressureChanges) {
    std::vector<StrokeSample> points = line(21);
    points[10].pressure = 0.2F; // straight in space, but the width dips mid-way
    const auto kept = simplifyIndices(points, 0.25, 0.05F);
    EXPECT_NE(std::find(kept.begin(), kept.end(), 10U), kept.end());
}

TEST(SimplifyTest, LargePointSetsAreHandledIteratively) {
    std::vector<StrokeSample> points;
    for (int i = 0; i < 10'000; ++i) { // zig-zag: nothing can be dropped (worst case)
        points.push_back(at(i, (i % 2) * 10.0, static_cast<std::uint64_t>(i)));
    }
    EXPECT_EQ(simplifyIndices(points, 0.25, 0.05F).size(), points.size());
}

// ---------------------------------------------------------------------------- builder

TEST(StrokeBuilderTest, DedupesTinyMovesAndEndsAtTheLastSample) {
    StrokeBuilder builder(at(0, 0, 0), 1.0);
    builder.add(at(0.1, 0.1, 8'000)); // < 0.5 px: dropped
    EXPECT_EQ(builder.rawSampleCount(), 1U);
    for (int i = 1; i <= 50; ++i) {
        builder.add(at(i * 4.0, 0.0, static_cast<std::uint64_t>(i) * 8'000));
    }
    const auto points = builder.finish();
    ASSERT_GE(points.size(), 2U);
    EXPECT_EQ(points.front().world, (core::DVec2{0, 0}));
    EXPECT_EQ(points.back().world, (core::DVec2{200, 0})); // the pen-up position, not a lagged one
}

TEST(StrokeBuilderTest, ThresholdsScaleWithZoom) {
    // At zoom 10 a 0.1 world-unit move is 1 view px: kept.
    StrokeBuilder zoomed(at(0, 0, 0), 10.0);
    zoomed.add(at(0.1, 0, 8'000));
    EXPECT_EQ(zoomed.rawSampleCount(), 2U);
    // At zoom 1 the same move is dropped.
    StrokeBuilder normal(at(0, 0, 0), 1.0);
    normal.add(at(0.1, 0, 8'000));
    EXPECT_EQ(normal.rawSampleCount(), 1U);
}

TEST(StrokeBuilderTest, SingleTapGivesOnePoint) {
    const StrokeBuilder builder(at(5, 5, 0, 0.4F), 1.0);
    const auto points = builder.finish();
    ASSERT_EQ(points.size(), 1U);
    EXPECT_FLOAT_EQ(points[0].pressure, 0.4F);
}

TEST(StrokeBuilderTest, IgnoresNonFiniteSamplesAndClampsPressure) {
    StrokeBuilder builder(at(0, 0, 0, 7.0F), 1.0, {.smoothing = false});
    builder.add(at(std::nan(""), 1, 8'000));
    builder.add(at(10, 0, 16'000, std::nanf("")));
    const auto points = builder.finish();
    ASSERT_EQ(points.size(), 2U);
    EXPECT_FLOAT_EQ(points[0].pressure, 1.0F);
    EXPECT_FLOAT_EQ(points[1].pressure, 1.0F);
}

TEST(StrokeBuilderTest, SmoothingCanBeDisabled) {
    StrokeBuilder builder(at(0, 0, 0), 1.0, {.smoothing = false});
    builder.add(at(5, 3, 8'000));
    builder.add(at(10, 0, 16'000));
    const auto points = builder.finish();
    ASSERT_EQ(points.size(), 3U);
    EXPECT_EQ(points[1].world, (core::DVec2{5, 3}));
}

TEST(StrokeGeometryTest, PointsAreRelativeToTheFirstPoint) {
    const std::vector<StrokeSample> points{at(1000.5, -20, 0, 0.5F), at(1010.5, -15, 1, 1.0F)};
    const StrokeGeometry geometry = makeStrokeGeometry(
        points,
        {.brush = document::Brush::Pencil, .color = core::Color::fromRgba(1, 2, 3), .width = 3.0F});
    EXPECT_EQ(geometry.transform.position, (core::DVec2{1000.5, -20}));
    ASSERT_EQ(geometry.stroke.points->size(), 2U);
    EXPECT_EQ((*geometry.stroke.points)[0], (document::StrokePoint{0, 0, 0.5F}));
    EXPECT_EQ((*geometry.stroke.points)[1], (document::StrokePoint{10, 5, 1.0F}));
    EXPECT_EQ(geometry.stroke.brush, document::Brush::Pencil);
    EXPECT_FLOAT_EQ(geometry.stroke.baseWidth, 3.0F);
}

} // namespace
} // namespace studyapp::canvas
