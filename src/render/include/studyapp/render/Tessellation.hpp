#pragma once

#include <studyapp/core/Rect.hpp>
#include <studyapp/core/Vec2.hpp>
#include <studyapp/render/Renderer.hpp>

#include <span>
#include <vector>

namespace studyapp::render {

// CPU tessellation (docs/RENDERING.md §4): pure, deterministic functions producing indexed
// triangle meshes. Geometry is never a bitmap; it is rebuilt from the logical points.

/// A polyline vertex with its half width (radius) in the same units as the position.
struct WidthPoint {
    core::Vec2 position;
    float radius = 1.0F;
};

struct TessellationOptions {
    /// View pixels per mesh unit. Only used to choose how many segments round caps and
    /// joins get, so they look round on screen without wasting triangles.
    float pixelsPerUnit = 1.0F;
    /// Maximum distance between a true circle and its polygon, in view pixels.
    float roundTolerancePx = 0.25F;
};

/// Variable-width polyline with round caps and round joins:
///
///   * each segment is a quad whose ends take the radius of their points;
///   * gentle turns (< 30 degrees) share one vertex pair offset along the averaged normal
///     (a bounded miter, at most 1.04 x radius), so a smooth stroke costs two vertices per
///     point;
///   * sharper turns get a round fan on the outer side, so there are no miter spikes;
///   * both ends get semicircular caps; a single point (or a zero-length polyline) becomes
///     a disc.
///
/// Non-finite points and non-positive radii are skipped and consecutive duplicates are
/// merged. An input without usable points gives an empty mesh. The output never contains
/// NaN or infinity.
[[nodiscard]] MeshData tessellatePolyline(std::span<const WidthPoint> points,
                                          const TessellationOptions& options = {});

/// Filled convex polygon (triangle fan), e.g. rectangles and ellipses.
[[nodiscard]] MeshData tessellateConvexFill(std::span<const core::Vec2> polygon);

/// Axis-aligned rectangle outline of constant `halfWidth` (selection frames, marquees).
[[nodiscard]] MeshData tessellateRectOutline(const core::Rect& rect, float halfWidth);

/// Points on the ellipse inscribed in `bounds`, with enough segments for `options`.
[[nodiscard]] std::vector<core::Vec2> ellipsePolygon(const core::Rect& bounds,
                                                     const TessellationOptions& options = {});

/// Appends `source` to `target`, rebasing its indices.
void appendMesh(MeshData& target, const MeshData& source);

/// Number of segments a full circle of `radius` needs for `options` (6..96).
[[nodiscard]] int circleSegments(float radius, const TessellationOptions& options) noexcept;

} // namespace studyapp::render
