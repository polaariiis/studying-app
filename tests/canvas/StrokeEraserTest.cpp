// Partial eraser geometry (docs/CANVAS.md §5.2): exact cuts in element-local space,
// surviving points kept bit-exact, fragment policy, determinism.

#include <studyapp/canvas/StrokeEraser.hpp>

#include <studyapp/canvas/ElementGeometry.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace studyapp::canvas {
namespace {

using document::StrokePoint;

document::Stroke strokeOf(std::vector<StrokePoint> points, float width = 2.0F,
                          document::Brush brush = document::Brush::Pen) {
    return document::Stroke{.brush = brush,
                            .baseWidth = width,
                            .points = document::makeStrokePoints(std::move(points))};
}

std::vector<StrokePiece> erase(const document::Stroke& stroke,
                               std::initializer_list<EraserCapsule> capsules,
                               double minFragment = 0.0) {
    std::vector<StrokePiece> pieces{wholeStroke(stroke)};
    for (const EraserCapsule& capsule : capsules) {
        (void)erasePieces(pieces, stroke, capsule, minFragment);
    }
    return pieces;
}

TEST(StrokeEraserTest, SegmentIntervalsMatchTheCapsule) {
    // A horizontal segment through a disc of radius 5 at x = 50.
    auto hit = segmentInCapsule({0, 0}, {100, 0}, {50, 0}, {50, 0}, 5.0);
    EXPECT_NEAR(hit.t0, 0.45, 1e-12);
    EXPECT_NEAR(hit.t1, 0.55, 1e-12);
    // Crossing a vertical capsule [(50, -20), (50, 20)] of radius 5: the rectangle part.
    hit = segmentInCapsule({0, 10}, {100, 10}, {50, -20}, {50, 20}, 5.0);
    EXPECT_NEAR(hit.t0, 0.45, 1e-12);
    EXPECT_NEAR(hit.t1, 0.55, 1e-12);
    // Past the end cap: misses.
    EXPECT_TRUE(segmentInCapsule({0, 30}, {100, 30}, {50, -20}, {50, 20}, 5.0).empty());
    // Entirely inside.
    hit = segmentInCapsule({48, 0}, {52, 0}, {50, 0}, {50, 0}, 5.0);
    EXPECT_DOUBLE_EQ(hit.t0, 0.0);
    EXPECT_DOUBLE_EQ(hit.t1, 1.0);
    // A degenerate segment (a point) is inside or not.
    EXPECT_FALSE(segmentInCapsule({50, 3}, {50, 3}, {50, 0}, {50, 0}, 5.0).empty());
    EXPECT_TRUE(segmentInCapsule({50, 9}, {50, 9}, {50, 0}, {50, 0}, 5.0).empty());
}

TEST(StrokeEraserTest, ErasingTheMiddleOfALongSegmentSplitsItExactly) {
    // Two points only (as RDP leaves a straight line): the cut lies inside the segment.
    const auto stroke = strokeOf({{0, 0, 1}, {100, 0, 0.5F}}); // width 2
    const auto pieces = erase(stroke, {{.a = {50, -20}, .b = {50, 20}, .radius = 4}});
    ASSERT_EQ(pieces.size(), 2U);
    // Reach = eraser radius + ink radius (max of the ends: 1): the ink ends at the edge.
    EXPECT_EQ(pieces[0].points.front(), (StrokePoint{0, 0, 1}));
    EXPECT_NEAR(pieces[0].points.back().x, 45.0, 1e-4);
    EXPECT_NEAR(pieces[1].points.front().x, 55.0, 1e-4);
    EXPECT_EQ(pieces[1].points.back(), (StrokePoint{100, 0, 0.5F})); // bit-exact
    // Pressure is interpolated at the cut.
    EXPECT_NEAR(pieces[0].points.back().pressure, 1.0F - 0.5F * 0.45F, 1e-5);
    EXPECT_NEAR(pieces[0].bounds.max.x, 45.0, 1e-4);
}

TEST(StrokeEraserTest, BeginningEndAndSeveralRegions) {
    const auto stroke = strokeOf({{0, 0, 1}, {25, 0, 1}, {50, 0, 1}, {75, 0, 1}, {100, 0, 1}});
    auto pieces = erase(stroke, {{.a = {0, 0}, .b = {0, 0}, .radius = 9}});
    ASSERT_EQ(pieces.size(), 1U);
    EXPECT_NEAR(pieces[0].points.front().x, 10.0, 1e-4);
    EXPECT_EQ(pieces[0].points.back(), (StrokePoint{100, 0, 1}));
    pieces = erase(stroke, {{.a = {100, 0}, .b = {100, 0}, .radius = 9}});
    ASSERT_EQ(pieces.size(), 1U);
    EXPECT_NEAR(pieces[0].points.back().x, 90.0, 1e-4);
    // Two regions: three pieces; the untouched original points stay in between.
    pieces = erase(stroke, {{.a = {25, -5}, .b = {25, 5}, .radius = 4},
                            {.a = {75, -5}, .b = {75, 5}, .radius = 4}});
    ASSERT_EQ(pieces.size(), 3U);
    EXPECT_EQ(pieces[1].points.size(), 3U); // cut, (50, 0) kept exactly, cut
    EXPECT_EQ(pieces[1].points[1], (StrokePoint{50, 0, 1}));
    // Erasing everything leaves nothing.
    EXPECT_TRUE(erase(stroke, {{.a = {-10, 0}, .b = {110, 0}, .radius = 3}}).empty());
}

TEST(StrokeEraserTest, MissesAndRepeatsChangeNothing) {
    const auto stroke = strokeOf({{0, 0, 1}, {100, 0, 1}});
    std::vector<StrokePiece> pieces{wholeStroke(stroke)};
    const auto* data = pieces[0].points.data();
    EXPECT_FALSE(erasePieces(pieces, stroke, {.a = {50, 30}, .b = {60, 30}, .radius = 4}, 0.0));
    EXPECT_EQ(pieces[0].points.data(), data); // untouched pieces are not copied
    EXPECT_TRUE(erasePieces(pieces, stroke, {.a = {50, -5}, .b = {50, 5}, .radius = 4}, 0.0));
    const auto once = pieces;
    EXPECT_FALSE(erasePieces(pieces, stroke, {.a = {50, -5}, .b = {50, 5}, .radius = 4}, 0.0));
    EXPECT_EQ(pieces, once); // erasing the same place again
    // Deterministic: the same input gives the same output.
    EXPECT_EQ(erase(stroke, {{.a = {50, -5}, .b = {50, 5}, .radius = 4}}), once);
}

TEST(StrokeEraserTest, TinyFragmentsAreDropped) {
    const auto stroke = strokeOf({{0, 0, 1}, {100, 0, 1}}, 4.0F);
    // Leaves [0, 3] (shorter than the width 4) and [57, 100].
    auto pieces = erase(stroke, {{.a = {5, 0}, .b = {50, 0}, .radius = 3}});
    ASSERT_EQ(pieces.size(), 1U);
    EXPECT_NEAR(pieces[0].points.front().x, 55.0, 1e-4);
    // The caller's minimum (e.g. 1.5 view px when zoomed out) applies when longer.
    pieces = erase(stroke, {{.a = {50, 0}, .b = {50, 0}, .radius = 3}}, 60.0);
    EXPECT_TRUE(pieces.empty());
    // A dot is erased whole.
    EXPECT_TRUE(
        erase(strokeOf({{10, 10, 1}}), {{.a = {11, 10}, .b = {11, 10}, .radius = 1}}).empty());
}

TEST(StrokeEraserTest, SelfIntersectionsAndHighlighters) {
    // A loop crossing itself at (50, 0): erasing the crossing cuts both passes.
    const auto loop = strokeOf({{0, 0, 1}, {100, 0, 1}, {100, 40, 1}, {50, 40, 1}, {50, -40, 1}});
    const auto pieces = erase(loop, {{.a = {50, 0}, .b = {50, 0}, .radius = 3}});
    EXPECT_EQ(pieces.size(), 3U);
    // A highlighter's ink radius is half its width at any pressure: it is cut further out.
    const auto band = strokeOf({{0, 0, 0.1F}, {100, 0, 0.1F}}, 14.0F, document::Brush::Highlighter);
    const auto cut = erase(band, {{.a = {50, 0}, .b = {50, 0}, .radius = 4}});
    ASSERT_EQ(cut.size(), 2U);
    EXPECT_NEAR(cut[0].points.back().x, 50.0 - 4.0 - 7.0, 1e-4);
}

TEST(StrokeEraserTest, LongStrokesAreRejectedByBoundsAndSegments) {
    // A long wavy stroke; an eraser far away touches nothing.
    std::vector<StrokePoint> points;
    for (int i = 0; i < 5000; ++i) {
        points.push_back({static_cast<float>(i), static_cast<float>(std::sin(i * 0.01) * 10), 1});
    }
    const auto stroke = strokeOf(std::move(points));
    auto pieces = erase(stroke, {{.a = {2500, 200}, .b = {2600, 200}, .radius = 4}});
    ASSERT_EQ(pieces.size(), 1U);
    EXPECT_EQ(pieces[0].points.size(), 5000U);
    pieces = erase(stroke, {{.a = {2500, -50}, .b = {2500, 50}, .radius = 4}});
    ASSERT_EQ(pieces.size(), 2U);
    EXPECT_NEAR(pieces[0].points.back().x, 2495.0, 1e-3); // eraser 4 + ink 1
    EXPECT_NEAR(pieces[1].points.front().x, 2505.0, 1e-3);
    EXPECT_GT(pieces[0].points.size() + pieces[1].points.size(), 4985U);
}

} // namespace
} // namespace studyapp::canvas
