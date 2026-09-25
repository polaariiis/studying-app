#include <studyapp/document/Element.hpp>

#include "TestWorkspace.hpp"

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

} // namespace
} // namespace studyapp::document::test
