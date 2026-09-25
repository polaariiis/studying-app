#include <studyapp/render/Tessellation.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace studyapp::render {
namespace {

void expectValid(const MeshData& mesh) {
    EXPECT_EQ(mesh.indices.size() % 3, 0U);
    for (const std::uint32_t index : mesh.indices) {
        ASSERT_LT(index, mesh.vertices.size());
    }
    for (const core::Vec2& v : mesh.vertices) {
        ASSERT_TRUE(std::isfinite(v.x) && std::isfinite(v.y));
        EXPECT_TRUE(mesh.bounds.contains(v));
    }
}

/// Largest distance of any vertex from the polyline, i.e. the drawn half width.
float maxDistanceFromSegment(const MeshData& mesh, core::Vec2 a, core::Vec2 b) {
    float worst = 0.0F;
    for (const core::Vec2& v : mesh.vertices) {
        const core::Vec2 ab = b - a;
        const float t = std::clamp((v - a).dot(ab) / ab.lengthSquared(), 0.0F, 1.0F);
        worst = std::max(worst, (v - (a + ab * t)).length());
    }
    return worst;
}

TEST(TessellationTest, EmptyAndUnusableInputGiveEmptyMeshes) {
    EXPECT_TRUE(tessellatePolyline({}).empty());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<WidthPoint> unusable{{{nan, 0}, 1}, {{0, 0}, 0}, {{1, 1}, -2}, {{2, 2}, nan}};
    EXPECT_TRUE(tessellatePolyline(unusable).empty());
}

TEST(TessellationTest, SinglePointIsADisc) {
    const std::vector<WidthPoint> dot{{{5, 5}, 2}};
    const MeshData mesh = tessellatePolyline(dot);
    expectValid(mesh);
    EXPECT_GE(mesh.triangleCount(), 6U);
    // An inscribed polygon: every vertex on the circle, extent between 2r·cos(π/n) and 2r.
    const float minimum = 4.0F * std::cos(3.14159265F / static_cast<float>(circleSegments(2, {})));
    for (const float extent : {mesh.bounds.width(), mesh.bounds.height()}) {
        EXPECT_LE(extent, 4.0F + 1e-4F);
        EXPECT_GE(extent, minimum - 1e-4F);
    }
    // Duplicates collapse to the same disc.
    const std::vector<WidthPoint> repeated{{{5, 5}, 2}, {{5, 5}, 2}, {{5, 5}, 1}};
    EXPECT_EQ(tessellatePolyline(repeated).triangleCount(), mesh.triangleCount());
}

TEST(TessellationTest, StraightLineRespectsWidthAndHasCompactTopology) {
    const std::vector<WidthPoint> points{{{0, 0}, 3}, {{50, 0}, 3}, {{100, 0}, 3}};
    const MeshData mesh = tessellatePolyline(points);
    expectValid(mesh);
    EXPECT_NEAR(mesh.bounds.min.y, -3.0F, 1e-4);
    EXPECT_NEAR(mesh.bounds.max.y, 3.0F, 1e-4);
    EXPECT_NEAR(mesh.bounds.min.x, -3.0F, 1e-3); // round caps extend by the radius
    EXPECT_NEAR(mesh.bounds.max.x, 103.0F, 1e-3);
    EXPECT_LE(maxDistanceFromSegment(mesh, {0, 0}, {100, 0}), 3.0F + 1e-4F);
    // Two quads (4 triangles) plus two cap fans; the collinear middle point shares its pair.
    const auto capTriangles = static_cast<std::size_t>(std::max(2, circleSegments(3, {}) / 2));
    EXPECT_EQ(mesh.triangleCount(), 4U + 2U * capTriangles);
}

TEST(TessellationTest, PressureChangesTheWidth) {
    const std::vector<WidthPoint> tapered{{{0, 0}, 4}, {{100, 0}, 1}};
    const MeshData mesh = tessellatePolyline(tapered);
    expectValid(mesh);
    float nearEnd = 0.0F;
    for (const core::Vec2& v : mesh.vertices) {
        if (v.x > 99.0F && v.x < 100.5F) {
            nearEnd = std::max(nearEnd, std::abs(v.y));
        }
    }
    EXPECT_LE(nearEnd, 1.0F + 1e-3F);
    EXPECT_NEAR(mesh.bounds.max.y, 4.0F, 1e-3);
}

TEST(TessellationTest, SharpTurnsAreRoundNotSpiky) {
    const std::vector<WidthPoint> zigzag{{{0, 0}, 2}, {{50, 0}, 2}, {{0, 1}, 2}};
    const MeshData mesh = tessellatePolyline(zigzag);
    expectValid(mesh);
    // A miter at an almost 180° turn would reach far past x = 52; round joins do not.
    EXPECT_LE(mesh.bounds.max.x, 52.0F + 1e-3F);
}

TEST(TessellationTest, GentleTurnsStayWithinTheBoundedMiter) {
    std::vector<WidthPoint> arc;
    for (int i = 0; i <= 90; ++i) {
        const float a = static_cast<float>(i) * 3.14159265F / 180.0F;
        arc.push_back({{100 * std::cos(a), 100 * std::sin(a)}, 2});
    }
    const MeshData mesh = tessellatePolyline(arc);
    expectValid(mesh);
    for (const core::Vec2& v : mesh.vertices) {
        EXPECT_LE(std::abs(v.length() - 100.0F), 2.0F * 1.04F + 0.01F);
    }
}

TEST(TessellationTest, DegenerateInputsDoNotCrash) {
    std::vector<WidthPoint> points;
    for (int i = 0; i < 1000; ++i) {
        points.push_back({{static_cast<float>(i % 3), static_cast<float>(i % 2)}, 0.5F});
    }
    expectValid(tessellatePolyline(points));
    const std::vector<WidthPoint> huge{{{0, 0}, 1e30F}, {{1, 0}, 1e30F}};
    expectValid(tessellatePolyline(huge));
}

TEST(TessellationTest, CircleSegmentsGrowWithScreenSize) {
    EXPECT_EQ(circleSegments(0.01F, {}), 6);
    EXPECT_LT(circleSegments(2, {.pixelsPerUnit = 1}), circleSegments(2, {.pixelsPerUnit = 16}));
    EXPECT_EQ(circleSegments(1e6F, {}), 96);
}

TEST(TessellationTest, ShapesAndOutlines) {
    const std::vector<core::Vec2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    const MeshData fill = tessellateConvexFill(square);
    expectValid(fill);
    EXPECT_EQ(fill.triangleCount(), 2U);
    EXPECT_TRUE(tessellateConvexFill(std::vector<core::Vec2>{{0, 0}, {1, 1}}).empty());

    const MeshData outline = tessellateRectOutline({{0, 0}, {10, 20}}, 1.0F);
    expectValid(outline);
    EXPECT_EQ(outline.triangleCount(), 8U);
    EXPECT_EQ(outline.bounds, (core::Rect{{-1, -1}, {11, 21}}));
    // Thinner than its line width: a solid block instead of a ring.
    EXPECT_EQ(tessellateRectOutline({{0, 0}, {1, 1}}, 2.0F).triangleCount(), 2U);

    const auto ellipse = ellipsePolygon({{0, 0}, {40, 20}});
    EXPECT_GE(ellipse.size(), 6U);
    for (const core::Vec2& p : ellipse) {
        const float x = (p.x - 20) / 20;
        const float y = (p.y - 10) / 10;
        EXPECT_NEAR(x * x + y * y, 1.0F, 1e-4);
    }

    MeshData combined = fill;
    appendMesh(combined, outline);
    expectValid(combined);
    EXPECT_EQ(combined.triangleCount(), 10U);
}

TEST(TessellationTest, AppendingManyMeshesIsLinear) {
    // Selection outlines of 10 000 elements are appended into one mesh every frame. The
    // buffers must grow geometrically: an exact reserve per append reallocated on every
    // call (quadratic, ≈ 130 ms per frame). Counted, not timed.
    const MeshData outline = tessellateRectOutline({{0, 0}, {10, 20}}, 1.0F);
    MeshData combined;
    int indexReallocations = 0;
    int vertexReallocations = 0;
    for (int i = 0; i < 10'000; ++i) {
        const std::size_t indexCapacity = combined.indices.capacity();
        const std::size_t vertexCapacity = combined.vertices.capacity();
        appendMesh(combined, outline);
        indexReallocations += combined.indices.capacity() != indexCapacity ? 1 : 0;
        vertexReallocations += combined.vertices.capacity() != vertexCapacity ? 1 : 0;
    }
    expectValid(combined);
    EXPECT_EQ(combined.triangleCount(), 80'000U);
    EXPECT_LT(indexReallocations, 64); // logarithmic in the number of appends
    EXPECT_LT(vertexReallocations, 64);
    // Indices of the last copy point at its own vertices.
    EXPECT_EQ(combined.indices.back() / outline.vertices.size(), 9'999U);
}

} // namespace
} // namespace studyapp::render
