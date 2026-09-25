#include <studyapp/document/Element.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

namespace studyapp::document::test {
namespace {

TEST(ElementTest, KindMatchesPayload) {
    EXPECT_EQ(kindOf(makeStroke()), ElementKind::Stroke);
    EXPECT_EQ(kindOf(makeText("t")), ElementKind::TextBox);
    EXPECT_EQ(kindOf(Shape{}), ElementKind::Shape);
    EXPECT_EQ(kindOf(Image{}), ElementKind::Image);
    EXPECT_EQ(kindOf(makeConnector(std::nullopt, std::nullopt)), ElementKind::Connector);
    EXPECT_EQ(toString(ElementKind::TextBox), "text box");
}

TEST(ElementTest, KindValuesAreStable) {
    // These values will be persisted (docs/DATABASE_SCHEMA.md: element.kind).
    EXPECT_EQ(static_cast<int>(ElementKind::Stroke), 1);
    EXPECT_EQ(static_cast<int>(ElementKind::TextBox), 2);
    EXPECT_EQ(static_cast<int>(ElementKind::Shape), 3);
    EXPECT_EQ(static_cast<int>(ElementKind::Image), 4);
    EXPECT_EQ(static_cast<int>(ElementKind::Connector), 5);
}

TEST(ElementTest, StrokeEqualityComparesPointValues) {
    const Stroke a = makeStroke({{1, 2, 1}});
    Stroke sameValues = makeStroke({{1, 2, 1}});
    Stroke shared = a;
    EXPECT_EQ(a, sameValues);           // different arrays, equal values
    EXPECT_EQ(a.points, shared.points); // copying an element shares the points
    sameValues.color = core::Color::white();
    EXPECT_NE(a, sameValues);
    EXPECT_NE(a, makeStroke({{1, 2, 0.5F}}));
    EXPECT_NE(a, Stroke{});
}

TEST(ElementTest, CopyingAnElementDoesNotCopyStrokePoints) {
    const Element element{.id = core::ElementId{},
                          .layer = core::LayerId{},
                          .z = core::FractionalIndex::first(),
                          .payload = makeStroke(std::vector<StrokePoint>(10'000))};
    const Element copy = element; // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(std::get<Stroke>(copy.payload).points.get(),
              std::get<Stroke>(element.payload).points.get());
    EXPECT_EQ(copy, element);
}

TEST(ElementTest, LocalBoundsPerKind) {
    EXPECT_EQ(localBounds(TextBox{.size = {120, 40}, .text = "t"}),
              (core::DRect{{0, 0}, {120, 40}}));
    EXPECT_EQ(localBounds(Shape{.size = {10, 20}}), (core::DRect{{0, 0}, {10, 20}}));
    EXPECT_EQ(localBounds(Image{.asset = {}, .size = {30, 5}}), (core::DRect{{0, 0}, {30, 5}}));
    // Stroke: point extent inflated by half the base width (2 -> 1).
    EXPECT_EQ(localBounds(makeStroke({{0, 0, 1}, {10, 5, 1}, {-4, 2, 1}})),
              (core::DRect{{-5, -1}, {11, 6}}));
    // Connector: world end positions inflated by half the width.
    EXPECT_EQ(localBounds(makeConnector(std::nullopt, std::nullopt)),
              (core::DRect{{-1, -1}, {101, 1}}));
}

TEST(ElementTest, WorldBoundsApplyScaleRotationAndTranslation) {
    Element element{.id = {},
                    .layer = {},
                    .z = core::FractionalIndex::first(),
                    .transform = {.position = {100, 50}, .rotation = 0, .scale = {2, 3}},
                    .locked = false,
                    .payload = Shape{.size = {10, 20}}};
    EXPECT_EQ(worldBounds(element), (core::DRect{{100, 50}, {120, 110}}));

    // A quarter turn maps local (x, y) to (-y, x).
    element.transform = {.position = {0, 0}, .rotation = 1.5707963267948966F, .scale = {1, 1}};
    const core::DRect rotated = worldBounds(element);
    EXPECT_NEAR(rotated.min.x, -20.0, 1e-5);
    EXPECT_NEAR(rotated.max.x, 0.0, 1e-5);
    EXPECT_NEAR(rotated.min.y, 0.0, 1e-5);
    EXPECT_NEAR(rotated.max.y, 10.0, 1e-5);

    // Negative scale mirrors but still yields ordered bounds.
    element.transform = {.position = {0, 0}, .rotation = 0, .scale = {-1, 1}};
    EXPECT_EQ(worldBounds(element), (core::DRect{{-10, 0}, {0, 20}}));
}

TEST(ElementTest, ConnectorWorldBoundsIgnoreTransform) {
    Element element{.id = {},
                    .layer = {},
                    .z = core::FractionalIndex::first(),
                    .transform = {.position = {1000, 1000}, .rotation = 1, .scale = {5, 5}},
                    .locked = false,
                    .payload = makeConnector(std::nullopt, std::nullopt)};
    EXPECT_EQ(worldBounds(element), localBounds(element.payload));
}

} // namespace
} // namespace studyapp::document::test
