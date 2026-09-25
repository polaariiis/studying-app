#include <studyapp/canvas/StrokeBuilder.hpp>

#include <studyapp/core/Geometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace studyapp::canvas {

namespace {

double smoothingFactor(double stepSeconds, double cutoffHz) noexcept {
    const double tau = 1.0 / (2.0 * std::numbers::pi * cutoffHz);
    return 1.0 / (1.0 + tau / stepSeconds);
}

float sanitisePressure(float pressure) noexcept {
    return std::isfinite(pressure) ? std::clamp(pressure, 0.0F, 1.0F) : 1.0F;
}

StrokeSample sanitise(StrokeSample sample) noexcept {
    sample.pressure = sanitisePressure(sample.pressure);
    return sample;
}

} // namespace

// ---------------------------------------------------------------------------- One-Euro

core::DVec2 OneEuroFilter::filter(const core::DVec2& world, std::uint64_t timestampUs) noexcept {
    if (!initialised_) {
        initialised_ = true;
        value_ = world;
        derivative_ = {};
        lastTimestampUs_ = timestampUs;
        return world;
    }
    const double step = timestampUs > lastTimestampUs_
                            ? static_cast<double>(timestampUs - lastTimestampUs_) / 1'000'000.0
                            : kFallbackStepSeconds;
    lastTimestampUs_ = std::max(lastTimestampUs_, timestampUs);

    // Velocity estimate (world/s), itself low-pass filtered.
    const core::DVec2 rawDerivative = (world - value_) / step;
    const double alphaD = smoothingFactor(step, params_.derivativeCutoffHz);
    derivative_ = derivative_ + (rawDerivative - derivative_) * alphaD;

    // Adaptive cutoff from the speed in view pixels per second.
    const double speedView = derivative_.length() * scale_;
    const double cutoff = params_.minCutoffHz + params_.beta * speedView;
    const double alpha = smoothingFactor(step, cutoff);
    value_ = value_ + (world - value_) * alpha;
    return value_;
}

// ---------------------------------------------------------------------------- RDP

std::vector<std::size_t> simplifyIndices(std::span<const StrokeSample> points,
                                         double toleranceWorld, float pressureTolerance) {
    const std::size_t n = points.size();
    if (n <= 2) {
        std::vector<std::size_t> all(n);
        for (std::size_t i = 0; i < n; ++i) {
            all[i] = i;
        }
        return all;
    }
    std::vector<bool> keep(n, false);
    keep.front() = true;
    keep.back() = true;
    const double toleranceSq = toleranceWorld * toleranceWorld;

    std::vector<std::pair<std::size_t, std::size_t>> ranges{{0, n - 1}};
    while (!ranges.empty()) {
        const auto [first, last] = ranges.back();
        ranges.pop_back();
        if (last <= first + 1) {
            continue;
        }
        const StrokeSample& a = points[first];
        const StrokeSample& b = points[last];
        std::size_t worst = first;
        double worstScore = 0.0;
        for (std::size_t i = first + 1; i < last; ++i) {
            const StrokeSample& p = points[i];
            const double distanceSq = core::distanceSquaredToSegment(p.world, a.world, b.world);
            const double t = core::closestParameterOnSegment(p.world, a.world, b.world);
            const double expectedPressure =
                static_cast<double>(a.pressure) +
                (static_cast<double>(b.pressure) - static_cast<double>(a.pressure)) * t;
            const double pressureError =
                std::abs(static_cast<double>(p.pressure) - expectedPressure);
            // Normalised so that either criterion alone can force a split (score > 1).
            const double score = std::max(
                toleranceSq > 0.0 ? distanceSq / toleranceSq : distanceSq * 1e300,
                pressureTolerance > 0.0F ? pressureError / static_cast<double>(pressureTolerance)
                                         : pressureError * 1e300);
            if (score > worstScore) {
                worstScore = score;
                worst = i;
            }
        }
        if (worstScore > 1.0) {
            keep[worst] = true;
            ranges.emplace_back(first, worst);
            ranges.emplace_back(worst, last);
        }
    }
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < n; ++i) {
        if (keep[i]) {
            indices.push_back(i);
        }
    }
    return indices;
}

// ---------------------------------------------------------------------------- builder

StrokeBuilder::StrokeBuilder(const StrokeSample& first, double zoom, StrokeOptions options)
    : options_(options), zoom_(zoom > 0.0 && std::isfinite(zoom) ? zoom : 1.0),
      filter_(options.oneEuro, zoom_), lastRaw_(sanitise(first)) {
    StrokeSample start = lastRaw_;
    if (options_.smoothing) {
        start.world = filter_.filter(start.world, start.timestampUs);
    }
    points_.push_back(start);
}

void StrokeBuilder::add(const StrokeSample& raw) {
    const StrokeSample sample = sanitise(raw);
    if (!std::isfinite(sample.world.x) || !std::isfinite(sample.world.y)) {
        return;
    }
    const double dedupeWorld = options_.dedupeViewPx / zoom_;
    if ((sample.world - lastRaw_.world).lengthSquared() < dedupeWorld * dedupeWorld) {
        return; // too close to the previous sample to change the line
    }
    lastRaw_ = sample;
    ++rawCount_;
    StrokeSample filtered = sample;
    if (options_.smoothing) {
        filtered.world = filter_.filter(sample.world, sample.timestampUs);
    }
    points_.push_back(filtered);
}

std::vector<StrokeSample> StrokeBuilder::finish() const {
    std::vector<StrokeSample> points = points_;
    const double dedupeWorld = options_.dedupeViewPx / zoom_;
    if (options_.smoothing && rawCount_ > 1 &&
        (lastRaw_.world - points.back().world).lengthSquared() >= dedupeWorld * dedupeWorld) {
        points.push_back(lastRaw_);
    }
    const auto keep =
        simplifyIndices(points, options_.simplifyViewPx / zoom_, options_.pressureTolerance);
    std::vector<StrokeSample> simplified;
    simplified.reserve(keep.size());
    for (const std::size_t index : keep) {
        simplified.push_back(points[index]);
    }
    return simplified;
}

// ---------------------------------------------------------------------------- geometry

StrokeGeometry makeStrokeGeometry(std::span<const StrokeSample> points, const PenStyle& style) {
    const core::DVec2 origin = points.front().world;
    std::vector<document::StrokePoint> local;
    local.reserve(points.size());
    for (const StrokeSample& sample : points) {
        const core::DVec2 offset = sample.world - origin;
        local.push_back({static_cast<float>(offset.x), static_cast<float>(offset.y),
                         sanitisePressure(sample.pressure)});
    }
    return StrokeGeometry{
        .transform = {.position = origin, .rotation = 0.0F, .scale = {1.0F, 1.0F}},
        .stroke = {.brush = style.brush,
                   .color = style.color,
                   .baseWidth = style.width,
                   .points = document::makeStrokePoints(std::move(local))},
    };
}

} // namespace studyapp::canvas
