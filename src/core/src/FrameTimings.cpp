#include <studyapp/core/FrameTimings.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace studyapp::core {

namespace {

/// Nearest-rank percentile of sorted values: the smallest value with at least `fraction`
/// of the samples at or below it.
double nearestRank(const std::vector<double>& sorted, double fraction) {
    const auto count = static_cast<double>(sorted.size());
    const auto rank = static_cast<std::size_t>(std::ceil(fraction * count));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

} // namespace

FrameTimeSummary summarizeFrameTimes(std::span<const double> valuesMs) {
    FrameTimeSummary summary;
    if (valuesMs.empty()) {
        return summary;
    }
    std::vector<double> sorted(valuesMs.begin(), valuesMs.end());
    std::sort(sorted.begin(), sorted.end());
    const auto count = static_cast<double>(sorted.size());
    summary.samples = sorted.size();
    summary.averageMs = std::accumulate(sorted.begin(), sorted.end(), 0.0) / count;
    summary.medianMs = nearestRank(sorted, 0.5);
    summary.p95Ms = nearestRank(sorted, 0.95);
    summary.p99Ms = nearestRank(sorted, 0.99);
    summary.maxMs = sorted.back();
    double squares = 0.0;
    for (const double value : sorted) {
        squares += (value - summary.averageMs) * (value - summary.averageMs);
    }
    summary.stdDevMs = std::sqrt(squares / count);
    return summary;
}

FrameTimings::FrameTimings(std::size_t window, double idleGapMs)
    : window_(std::max<std::size_t>(window, 1)), idleGapMs_(idleGapMs) {
    intervals_.reserve(window_);
}

void FrameTimings::presented(double timeMs) {
    if (hasPresent_) {
        lastGapMs_ = timeMs - lastPresentMs_;
        if (lastGapMs_ > idleGapMs_) {
            intervals_.clear(); // idle time, not a frame: a new burst starts here
        } else {
            if (intervals_.size() == window_) {
                intervals_.erase(intervals_.begin());
            }
            intervals_.push_back(lastGapMs_);
        }
    }
    lastPresentMs_ = timeMs;
    hasPresent_ = true;
}

void FrameTimings::reset() {
    hasPresent_ = false;
    lastGapMs_ = 0.0;
    intervals_.clear();
}

} // namespace studyapp::core
