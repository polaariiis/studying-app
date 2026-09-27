#pragma once

#include <studyapp/core/Rect.hpp>
#include <studyapp/core/Vec2.hpp>
#include <studyapp/document/Element.hpp>

#include <optional>
#include <span>
#include <vector>

namespace studyapp::canvas {

// Partial (vector-preserving) erasing of strokes (docs/CANVAS.md §5.2). Pure, deterministic
// geometry in element-local coordinates: the ink the eraser covers is cut out of a stroke's
// centre line, which leaves runs of the original points ("pieces"). Surviving points are
// kept bit-exact; only the new end points where the eraser boundary crosses a segment are
// computed (position and pressure interpolated along the segment). Nothing is rasterised.

/// A run of consecutive points of one stroke, with the bounds of the points (local).
struct StrokePiece {
    std::vector<document::StrokePoint> points;
    core::DRect bounds = core::DRect::emptyBounds();

    [[nodiscard]] friend bool operator==(const StrokePiece&, const StrokePiece&) = default;
};

/// The area one eraser step covers: every point within `radius` of the segment [a, b]
/// (a disc when a == b). Element-local coordinates.
struct EraserCapsule {
    core::DVec2 a{};
    core::DVec2 b{};
    double radius = 0.0;
};

/// The whole stroke as one piece.
[[nodiscard]] StrokePiece wholeStroke(const document::Stroke& stroke);

/// The first cut of a stroke, straight from its stored points (no copy of a stroke the
/// capsule misses): nullopt if the capsule misses it, otherwise what is left (possibly
/// nothing). Same rules as erasePieces().
[[nodiscard]] std::optional<std::vector<StrokePiece>>
eraseStroke(const document::Stroke& stroke, const EraserCapsule& capsule, double minFragmentLength);

/// Erases the capsule from `pieces` of `stroke` (its style decides the ink width): a
/// segment is cut where its centre line comes within capsule radius + ink radius of the
/// capsule axis, so the remaining ink ends at the eraser's edge. A piece left shorter than
/// max(stroke width, `minFragmentLength`) is dropped (no stray dots). Pieces the capsule
/// does not reach are left untouched (not even copied). Returns true if anything changed.
///
/// Cost: O(points of the pieces whose bounds the capsule reaches); other pieces are
/// rejected by their bounds.
bool erasePieces(std::vector<StrokePiece>& pieces, const document::Stroke& stroke,
                 const EraserCapsule& capsule, double minFragmentLength);

/// Parameter interval [t0, t1] of the segment p0 → p1 whose points lie within `radius` of
/// the segment [a, b]; empty (t0 > t1) if none. Exposed for tests.
struct ParameterInterval {
    double t0 = 1.0;
    double t1 = 0.0;
    [[nodiscard]] bool empty() const noexcept { return t0 > t1; }
};
[[nodiscard]] ParameterInterval segmentInCapsule(const core::DVec2& p0, const core::DVec2& p1,
                                                 const core::DVec2& a, const core::DVec2& b,
                                                 double radius) noexcept;

} // namespace studyapp::canvas
