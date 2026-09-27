// Shapes (Phase 6, step 4): drag → document geometry, arrow meshes, hit tests and visual
// bounds.

#include <studyapp/canvas/ShapeBuilder.hpp>

#include <studyapp/canvas/ElementGeometry.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

namespace studyapp::canvas {
namespace {

using document::ShapeKind;

document::Element elementOf(const ShapeGeometry& geometry) {
    return document::Element{.id = {},
                             .layer = {},
                             .z = core::FractionalIndex::first(),
                             .transform = geometry.transform,
                             .locked = false,
                             .payload = geometry.shape};
}

TEST(ShapeBuilderTest, BoxesFillTheDraggedRectangleInAnyDirection) {
    const ShapeStyle style{.kind = ShapeKind::Ellipse, .width = 3.0F, .fill = true};
    const auto shape = shapeFromDrag(style, {100, 80}, {40, 120}, false);
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->transform.position, (core::DVec2{40, 80}));
    EXPECT_FLOAT_EQ(shape->transform.rotation, 0.0F);
    EXPECT_EQ(shape->shape.kind, ShapeKind::Ellipse);
    EXPECT_EQ(shape->shape.size, (core::Vec2{60, 40}));
    EXPECT_FLOAT_EQ(shape->shape.strokeWidth, 3.0F);
    ASSERT_TRUE(shape->shape.fillColor.has_value());
    EXPECT_EQ(shape->shape.fillColor->a, kShapeFillAlpha);
    // Shift: a square (circle), still anchored at the press point.
    const auto square = shapeFromDrag(style, {100, 80}, {40, 120}, true);
    EXPECT_EQ(square->shape.size, (core::Vec2{60, 60}));
    EXPECT_EQ(square->transform.position, (core::DVec2{40, 80}));
    // No extent: nothing. A flat box (one side 0) is still a shape.
    EXPECT_FALSE(shapeFromDrag(style, {5, 5}, {5, 5}, false).has_value());
    EXPECT_TRUE(shapeFromDrag({}, {0, 0}, {10, 0}, false).has_value());
    EXPECT_FALSE(shapeFromDrag({}, {0, 0}, {10, 0}, false)->shape.fillColor.has_value());
}

TEST(ShapeBuilderTest, LinesAndArrowsRunFromThePressAlongTheirRotation) {
    const ShapeStyle arrow{.kind = ShapeKind::Arrow};
    const auto shape = shapeFromDrag(arrow, {10, 10}, {10, 60}, false);
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->transform.position, (core::DVec2{10, 10}));
    EXPECT_NEAR(shape->transform.rotation, std::numbers::pi / 2.0, 1e-6);
    EXPECT_EQ(shape->shape.size, (core::Vec2{50, 0}));
    EXPECT_FALSE(shape->shape.fillColor.has_value()); // lines are never filled
    // The far end is where the pointer was.
    const core::DVec2 end = document::localToWorld(shape->transform).apply({50, 0});
    EXPECT_NEAR(end.x, 10.0, 1e-4);
    EXPECT_NEAR(end.y, 60.0, 1e-4);
    // Shift snaps to 45°.
    const auto snapped = shapeFromDrag({.kind = ShapeKind::Line}, {0, 0}, {100, 80}, true);
    EXPECT_NEAR(snapped->transform.rotation, std::numbers::pi / 4.0, 1e-6);
    EXPECT_FALSE(shapeFromDrag(arrow, {3, 3}, {3, 3}, false).has_value());
}

TEST(ShapeBuilderTest, ArrowsDrawAHeadAndAreHitThere) {
    const auto shape =
        shapeFromDrag({.kind = ShapeKind::Arrow, .width = 2.0F}, {0, 0}, {100, 0}, false);
    const document::Element element = elementOf(*shape);
    const auto parts = buildElementMeshes(element, 1.0F);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_FALSE(parts[0].mesh.empty());
    // The head (8 long, 4 half-wide for width 2) widens the mesh near the end.
    EXPECT_NEAR(parts[0].mesh.bounds.max.x, 100.0, 1e-3);
    EXPECT_NEAR(parts[0].mesh.bounds.max.y, 4.0, 1e-3);
    EXPECT_TRUE(hitTest(element, {97, 2.5}, 0.5));  // on the head
    EXPECT_FALSE(hitTest(element, {50, 2.5}, 0.5)); // beside the shaft
    EXPECT_TRUE(hitTest(element, {50, 0.5}, 0.5));  // on the shaft
    // Visual bounds include the head and the outline, beyond the zero-height box.
    const core::DRect bounds = visualBounds(element);
    EXPECT_NEAR(bounds.min.y, -4.0, 1e-6);
    EXPECT_NEAR(bounds.max.y, 4.0, 1e-6);
    const auto box = shapeFromDrag({.width = 6.0F}, {0, 0}, {10, 10}, false);
    EXPECT_NEAR(visualBounds(elementOf(*box)).min.x, -3.0, 1e-6);
}

} // namespace
} // namespace studyapp::canvas
