#include <studyapp/core/Rect.hpp>

#include <gtest/gtest.h>

namespace studyapp::core {
namespace {

TEST(RectTest, FromPointsNormalizesCorners) {
    const Rect r = Rect::fromPoints({5.0F, 1.0F}, {2.0F, 4.0F});
    EXPECT_EQ(r.min, (Vec2{2.0F, 1.0F}));
    EXPECT_EQ(r.max, (Vec2{5.0F, 4.0F}));
    EXPECT_FLOAT_EQ(r.width(), 3.0F);
    EXPECT_FLOAT_EQ(r.height(), 3.0F);
    EXPECT_FLOAT_EQ(r.area(), 9.0F);
    EXPECT_EQ(r.center(), (Vec2{3.5F, 2.5F}));
}

TEST(RectTest, FromOriginSizeHandlesNegativeSize) {
    const Rect r = Rect::fromOriginSize({10.0F, 10.0F}, {-4.0F, 2.0F});
    EXPECT_EQ(r, (Rect{{6.0F, 10.0F}, {10.0F, 12.0F}}));
    EXPECT_EQ(r.size(), (Vec2{4.0F, 2.0F}));
}

TEST(RectTest, EmptyBoundsIsEmptyAndIdentityForUnion) {
    constexpr Rect empty = Rect::emptyBounds();
    EXPECT_TRUE(empty.isEmpty());
    EXPECT_FLOAT_EQ(empty.width(), 0.0F);
    EXPECT_FLOAT_EQ(empty.area(), 0.0F);

    const Rect r{{1.0F, 2.0F}, {3.0F, 4.0F}};
    EXPECT_EQ(empty.united(r), r);
    EXPECT_EQ(r.united(empty), r);
}

TEST(RectTest, ZeroAreaRectIsNotEmpty) {
    const Rect point = Rect::fromPoints({1.0F, 1.0F}, {1.0F, 1.0F});
    EXPECT_FALSE(point.isEmpty());
    EXPECT_TRUE(point.contains(Vec2{1.0F, 1.0F}));
}

TEST(RectTest, AccumulatesBoundsFromPoints) {
    Rect bounds = Rect::emptyBounds();
    for (const Vec2 p : {Vec2{3.0F, -1.0F}, Vec2{-2.0F, 5.0F}, Vec2{0.0F, 0.0F}}) {
        bounds = bounds.including(p);
    }
    EXPECT_EQ(bounds, (Rect{{-2.0F, -1.0F}, {3.0F, 5.0F}}));
}

TEST(RectTest, ContainsPointOnClosedInterval) {
    const Rect r{{0.0F, 0.0F}, {10.0F, 5.0F}};
    EXPECT_TRUE(r.contains(Vec2{0.0F, 0.0F}));
    EXPECT_TRUE(r.contains(Vec2{10.0F, 5.0F}));
    EXPECT_TRUE(r.contains(Vec2{5.0F, 2.5F}));
    EXPECT_FALSE(r.contains(Vec2{10.1F, 2.0F}));
    EXPECT_FALSE(r.contains(Vec2{5.0F, -0.1F}));
}

TEST(RectTest, ContainsRect) {
    const Rect outer{{0.0F, 0.0F}, {10.0F, 10.0F}};
    EXPECT_TRUE(outer.contains(Rect{{2.0F, 2.0F}, {8.0F, 8.0F}}));
    EXPECT_TRUE(outer.contains(outer));
    EXPECT_FALSE(outer.contains(Rect{{5.0F, 5.0F}, {11.0F, 8.0F}}));
    EXPECT_TRUE(outer.contains(Rect::emptyBounds()));
    EXPECT_FALSE(Rect::emptyBounds().contains(outer));
}

TEST(RectTest, IntersectsIncludesTouchingEdges) {
    const Rect a{{0.0F, 0.0F}, {2.0F, 2.0F}};
    EXPECT_TRUE(a.intersects(Rect{{1.0F, 1.0F}, {3.0F, 3.0F}}));
    EXPECT_TRUE(a.intersects(Rect{{2.0F, 0.0F}, {4.0F, 2.0F}}));
    EXPECT_FALSE(a.intersects(Rect{{2.1F, 0.0F}, {4.0F, 2.0F}}));
    EXPECT_FALSE(a.intersects(Rect::emptyBounds()));
}

TEST(RectTest, Intersected) {
    const Rect a{{0.0F, 0.0F}, {4.0F, 4.0F}};
    const Rect b{{2.0F, 1.0F}, {6.0F, 3.0F}};
    EXPECT_EQ(a.intersected(b), (Rect{{2.0F, 1.0F}, {4.0F, 3.0F}}));
    EXPECT_TRUE(a.intersected(Rect{{5.0F, 5.0F}, {6.0F, 6.0F}}).isEmpty());
}

TEST(RectTest, ExpandedAndTranslated) {
    const Rect r{{1.0F, 1.0F}, {3.0F, 2.0F}};
    EXPECT_EQ(r.expanded(1.0F), (Rect{{0.0F, 0.0F}, {4.0F, 3.0F}}));
    EXPECT_EQ(r.translated({2.0F, -1.0F}), (Rect{{3.0F, 0.0F}, {5.0F, 1.0F}}));
    EXPECT_TRUE(Rect::emptyBounds().expanded(5.0F).isEmpty());
}

TEST(DRectTest, WorksWithDoublePrecisionWorldCoordinates) {
    const DRect r = DRect::fromOriginSize({1.0e9, 1.0e9}, {0.5, 0.25});
    EXPECT_DOUBLE_EQ(r.width(), 0.5);
    EXPECT_DOUBLE_EQ(r.height(), 0.25);
    EXPECT_TRUE(r.contains(DVec2{1.0e9 + 0.25, 1.0e9 + 0.125}));
    EXPECT_FALSE(r.contains(DVec2{1.0e9 + 0.75, 1.0e9}));
}

TEST(DRectTest, IsUsableInConstantExpressions) {
    constexpr DRect r = DRect::fromPoints({0.0, 0.0}, {2.0, 2.0});
    static_assert(r.area() == 4.0);
    static_assert(r.contains(DVec2{1.0, 1.0}));
    static_assert(DRect::emptyBounds().isEmpty());
    SUCCEED();
}

} // namespace
} // namespace studyapp::core
