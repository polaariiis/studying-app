#pragma once

#include <studyapp/core/Color.hpp>
#include <studyapp/core/Vec2.hpp>
#include <studyapp/document/Element.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace studyapp::canvas {

// Freehand stroke input pipeline (docs/CANVAS.md §5.1):
//
//   raw samples → dedupe (< dedupeViewPx) → One-Euro smoothing → live preview
//   pen up      → final raw sample → Ramer–Douglas–Peucker (pressure-aware)
//               → element-local points relative to the first point
//
// Every threshold is given in *view* pixels and converted with the zoom at stroke start,
// so the pipeline behaves the same at every zoom level while the stored geometry is in
// world units. All steps are deterministic: the same samples always give the same stroke.

struct StrokeSample {
    core::DVec2 world{};
    float pressure = 1.0F;
    std::uint64_t timestampUs = 0;

    [[nodiscard]] friend bool operator==(const StrokeSample&, const StrokeSample&) = default;
};

/// One-Euro filter (Casiez, Roussel, Vogel, CHI 2012): a low-pass filter whose cutoff
/// rises with speed, so slow movements are smoothed strongly (jitter removed) while fast
/// movements lag little. Parameters are in view pixels and hertz.
struct OneEuroParams {
    double minCutoffHz = 2.0; ///< cutoff at rest: lower = smoother, more lag when slow
    double beta = 0.02;       ///< cutoff increase per view px/s of speed
    double derivativeCutoffHz = 1.0;
};

class OneEuroFilter {
public:
    OneEuroFilter(OneEuroParams params, double viewPixelsPerWorldUnit) noexcept
        : params_(params), scale_(viewPixelsPerWorldUnit) {}

    /// Filters one position. Samples without a usable time step (equal or decreasing
    /// timestamps, e.g. synthetic input) are assumed to be `kFallbackStepSeconds` apart.
    [[nodiscard]] core::DVec2 filter(const core::DVec2& world, std::uint64_t timestampUs) noexcept;

    static constexpr double kFallbackStepSeconds = 1.0 / 120.0;

private:
    OneEuroParams params_;
    double scale_;
    bool initialised_ = false;
    core::DVec2 value_{};
    core::DVec2 derivative_{};
    std::uint64_t lastTimestampUs_ = 0;
};

struct StrokeOptions {
    bool smoothing = true;
    OneEuroParams oneEuro{};
    double dedupeViewPx = 0.5;       ///< samples closer than this to the previous one are dropped
    double simplifyViewPx = 0.25;    ///< RDP tolerance (maximum deviation of dropped points)
    float pressureTolerance = 0.05F; ///< dropped points must also be this close in pressure
};

/// Indices of the points that Ramer–Douglas–Peucker keeps: a point is dropped only if it
/// lies within `toleranceWorld` of the chord between the kept neighbours *and* its
/// pressure is within `pressureTolerance` of the pressure interpolated along that chord.
/// First and last points are always kept; the result is sorted. Iterative (no recursion
/// depth limit on long strokes).
[[nodiscard]] std::vector<std::size_t> simplifyIndices(std::span<const StrokeSample> points,
                                                       double toleranceWorld,
                                                       float pressureTolerance);

/// Accumulates the samples of one gesture.
class StrokeBuilder {
public:
    StrokeBuilder(const StrokeSample& first, double zoom, StrokeOptions options = {});

    void add(const StrokeSample& sample);

    /// Smoothed points so far (world space), for the live preview.
    [[nodiscard]] std::span<const StrokeSample> points() const noexcept { return points_; }
    [[nodiscard]] std::size_t rawSampleCount() const noexcept { return rawCount_; }

    /// Ends the gesture: appends the last raw sample (so the stroke ends where the pointer
    /// lifted despite filter lag) and simplifies. Always returns at least one point.
    [[nodiscard]] std::vector<StrokeSample> finish() const;

private:
    StrokeOptions options_;
    double zoom_;
    OneEuroFilter filter_;
    std::vector<StrokeSample> points_;
    StrokeSample lastRaw_;
    std::size_t rawCount_ = 1;
};

/// Result of converting finished world points into a document stroke: the element's
/// position (the first point, world) and the payload with points relative to it.
struct StrokeGeometry {
    document::Transform transform;
    document::Stroke stroke;
};

struct PenStyle {
    document::Brush brush = document::Brush::Pen;
    core::Color color = core::Color::black();
    float width = 2.0F; ///< world units at pressure 1
};

/// Precondition: `points` is not empty.
[[nodiscard]] StrokeGeometry makeStrokeGeometry(std::span<const StrokeSample> points,
                                                const PenStyle& style);

} // namespace studyapp::canvas
