#pragma once

#include <studyapp/core/Affine2.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/core/Rect.hpp>
#include <studyapp/core/Vec2.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/render/Renderer.hpp>

#include <vector>

namespace studyapp::canvas {

// Geometry of document elements for the canvas: how wide a stroke is at a given pressure,
// which meshes represent an element, and the exact (narrow-phase) hit tests
// (docs/CANVAS.md §6). All inputs and outputs are world or element-local coordinates;
// tolerances are world lengths (callers convert view pixels with Camera).
//
// Phase 4 draws strokes fully. Shapes and connectors get simple outlines/fills; text boxes
// and images (whose content rendering arrives in Phase 6) are shown as a neutral frame so
// they remain visible and selectable.

/// Radius (half width, local units) of a stroke at `pressure` in [0, 1]. Pressure scales
/// the width between 30 % and 100 % of `baseWidth`; devices without pressure report 1.
[[nodiscard]] float strokeRadius(const document::Stroke& stroke, float pressure) noexcept;

/// One single-colour mesh of an element, in the space meshToWorld() maps from.
struct MeshPart {
    render::MeshData mesh;
    core::Color color;
};

/// Meshes of an element. `pixelsPerUnit` (view pixels per world unit, bucketed by the
/// caller) only affects how round caps, joins and ellipses are subdivided.
[[nodiscard]] std::vector<MeshPart> buildElementMeshes(const document::Element& element,
                                                       float pixelsPerUnit);

/// Transform from the space of buildElementMeshes() to world: the element transform, or
/// for connectors (defined by world end positions) a translation to the start point.
[[nodiscard]] core::Affine2 meshToWorld(const document::Element& element) noexcept;

/// True if `world` lies on the element's visible geometry, within `toleranceWorld`.
[[nodiscard]] bool hitTest(const document::Element& element, const core::DVec2& world,
                           double toleranceWorld);

/// True if the element's geometry touches the world rectangle (strokes: any part of the
/// ink; other kinds: their world bounds).
[[nodiscard]] bool intersectsRect(const document::Element& element, const core::DRect& world);

/// True if a stroke's ink comes within `radiusWorld` of the world segment [a, b] (the
/// stroke eraser's path). Always false for other element kinds.
[[nodiscard]] bool strokeTouchesSegment(const document::Element& element, const core::DVec2& a,
                                        const core::DVec2& b, double radiusWorld);

} // namespace studyapp::canvas
