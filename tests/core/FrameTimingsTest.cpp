#include <studyapp/core/FrameTimings.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace studyapp::core {
namespace {

TEST(FrameTimingsTest, SummaryOfKnownDistribution) {
    // 1..100 ms: nearest-rank percentiles are observed values.
    std::vector<double> values;
    for (int i = 100; i >= 1; --i) { // any order
        values.push_back(i);
    }
    const FrameTimeSummary s = summarizeFrameTimes(values);
    EXPECT_EQ(s.samples, 100U);
    EXPECT_DOUBLE_EQ(s.averageMs, 50.5);
    EXPECT_DOUBLE_EQ(s.medianMs, 50.0);
    EXPECT_DOUBLE_EQ(s.p95Ms, 95.0);
    EXPECT_DOUBLE_EQ(s.p99Ms, 99.0);
    EXPECT_DOUBLE_EQ(s.maxMs, 100.0);
    EXPECT_NEAR(s.stdDevMs, 28.866, 1e-3);
    EXPECT_NEAR(s.fps(), 1000.0 / 50.5, 1e-12);
}

TEST(FrameTimingsTest, EmptyAndSingleSample) {
    const FrameTimeSummary empty = summarizeFrameTimes({});
    EXPECT_EQ(empty.samples, 0U);
    EXPECT_EQ(empty.fps(), 0.0);
    const std::vector<double> one{16.7};
    const FrameTimeSummary s = summarizeFrameTimes(one);
    EXPECT_DOUBLE_EQ(s.medianMs, 16.7);
    EXPECT_DOUBLE_EQ(s.p99Ms, 16.7);
    EXPECT_DOUBLE_EQ(s.stdDevMs, 0.0);
}

TEST(FrameTimingsTest, SteadyCadenceHasNoJitterAndAlternationHas) {
    FrameTimings steady;
    FrameTimings alternating;
    double t = 0.0;
    for (int i = 0; i < 100; ++i) {
        steady.presented(i * 16.0);
        alternating.presented(t);
        t += (i % 2 == 0) ? 8.0 : 24.0; // same 16 ms average, 60/30-style oscillation
    }
    const auto s = steady.summary();
    const auto o = alternating.summary();
    EXPECT_DOUBLE_EQ(s.averageMs, 16.0);
    EXPECT_DOUBLE_EQ(s.stdDevMs, 0.0);
    EXPECT_NEAR(o.averageMs, 16.0, 0.2);
    EXPECT_GT(o.stdDevMs, 7.9); // the average hides it; jitter and p95 do not
    EXPECT_DOUBLE_EQ(o.p95Ms, 24.0);
}

TEST(FrameTimingsTest, IdleGapsAreNotFramesAndStartANewBurst) {
    // The old HUD averaged every paint interval: 59 frames at 7 ms plus one 3 s pause
    // between two mouse movements read as 57 ms per frame (17 fps).
    FrameTimings timings(240, 100.0);
    double t = 0.0;
    for (int i = 0; i < 30; ++i) {
        timings.presented(t);
        t += 7.0;
    }
    t += 3000.0; // idle: nothing asked for a frame
    timings.presented(t);
    EXPECT_TRUE(timings.startedAfterIdle());
    EXPECT_NEAR(timings.lastGapMs(), 3007.0, 1e-9);
    EXPECT_EQ(timings.summary().samples, 0U);
    for (int i = 0; i < 29; ++i) {
        t += 7.0;
        timings.presented(t);
    }
    const auto s = timings.summary();
    EXPECT_EQ(s.samples, 29U); // only the new burst
    EXPECT_DOUBLE_EQ(s.averageMs, 7.0);
    EXPECT_NEAR(s.fps(), 142.857, 1e-3);
    EXPECT_FALSE(timings.startedAfterIdle());
}

TEST(FrameTimingsTest, WindowKeepsTheNewestIntervals) {
    FrameTimings timings(4, 100.0);
    double t = 0.0;
    for (const double gap : {10.0, 10.0, 10.0, 10.0, 20.0, 30.0}) {
        timings.presented(t);
        t += gap;
    }
    timings.presented(t);
    const std::vector<double> kept(timings.intervals().begin(), timings.intervals().end());
    EXPECT_EQ(kept, (std::vector<double>{10.0, 10.0, 20.0, 30.0}));
    timings.reset();
    EXPECT_TRUE(timings.intervals().empty());
    EXPECT_EQ(timings.lastGapMs(), 0.0);
}

} // namespace
} // namespace studyapp::core
