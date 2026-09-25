#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace studyapp::core {

/// Distribution of a set of frame intervals (or frame costs), in milliseconds.
struct FrameTimeSummary {
    std::size_t samples = 0;
    double averageMs = 0.0;
    double medianMs = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
    double stdDevMs = 0.0; ///< jitter: population standard deviation around the average
    /// 1000 / average, or 0 without samples.
    [[nodiscard]] double fps() const noexcept { return averageMs > 0.0 ? 1000.0 / averageMs : 0.0; }
};

/// Summarises `valuesMs` (any order). Percentiles use the nearest-rank method, so each is
/// an observed value. Empty input gives an all-zero summary.
[[nodiscard]] FrameTimeSummary summarizeFrameTimes(std::span<const double> valuesMs);

/// Presentation cadence of an on-demand renderer.
///
/// Frames are only drawn when something changed, so the time between two presented frames
/// is either a frame interval (while interaction keeps requesting frames) or idle time. An
/// interval longer than `idleGapMs` is idle time: it is not a sample and it starts a new
/// burst, so the statistics always describe the current (or most recent) burst of
/// activity. Without this rule a pause between two mouse movements counts as one very slow
/// frame and the average says nothing about rendering. Not thread-safe.
class FrameTimings {
public:
    explicit FrameTimings(std::size_t window = 240, double idleGapMs = 100.0);

    /// A frame was presented at `timeMs` (monotonic clock, milliseconds).
    void presented(double timeMs);
    void reset();

    /// Consecutive-frame intervals of the current burst (at most `window`, newest last).
    [[nodiscard]] std::span<const double> intervals() const noexcept { return intervals_; }
    [[nodiscard]] FrameTimeSummary summary() const { return summarizeFrameTimes(intervals_); }
    /// Time between the last two presented frames, idle gaps included (0 before two frames).
    [[nodiscard]] double lastGapMs() const noexcept { return lastGapMs_; }
    /// True when the last presented frame followed idle time (no interval recorded yet).
    [[nodiscard]] bool startedAfterIdle() const noexcept { return intervals_.empty(); }
    [[nodiscard]] double idleGapMs() const noexcept { return idleGapMs_; }

private:
    std::size_t window_;
    double idleGapMs_;
    double lastPresentMs_ = 0.0;
    bool hasPresent_ = false;
    double lastGapMs_ = 0.0;
    std::vector<double> intervals_;
};

} // namespace studyapp::core
