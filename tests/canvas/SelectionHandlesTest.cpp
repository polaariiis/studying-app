// Selection handles (Phase 6, step 8): which elements have which handles, and what
// dragging them does to the geometry.

#include <studyapp/canvas/SelectionHandles.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

namespace studyapp::canvas {
namespace {

using document::ShapeKind;

document::Element box(document::ElementPayload payload, core::DVec2 at = {100, 100}) {
    return document::Element{.id = {},
                             .layer = {},
                             .z = core::FractionalIndex::first(),
                             .transform = {.position = at},
                             .locked = false,
                             .payload = std::move(payload)};
}

core::Vec2 sizeOf(const document::Element& element) {
    if (const auto* shape = std::get_if<document::Shape>(&element.payload)) {
        return shape->size;
    }
    if (const auto* image = std::get_if<document::Image>(&element.payload)) {
        return image->size;
    }
    return std::get<document::TextBox>(element.payload).size;
}

TEST(SelectionHandlesTest, WhichElementsHaveWhichHandles) {
    EXPECT_EQ(handlesFor(box(document::Shape{.size = {100, 50}})).size(), 8U);
    EXPECT_EQ(handlesFor(box(document::Image{.asset = {}, .size = {40, 30}})).size(), 8U);
    const auto text = handlesFor(box(document::TextBox{.size = {200, 40}, .text = "t"}));
    ASSERT_EQ(text.size(), 2U); // width only
    EXPECT_EQ(text[0].kind, HandleKind::Left);
    EXPECT_EQ(text[1].world, (core::DVec2{300, 120}));
    const auto line = handlesFor(box(document::Shape{.kind = ShapeKind::Line, .size = {50, 0}}));
    ASSERT_EQ(line.size(), 2U);
    EXPECT_EQ(line[1].world, (core::DVec2{150, 100}));
    const auto link = handlesFor(
        box(document::Connector{.start = {.position = {1, 2}}, .end = {.position = {3, 4}}}));
    ASSERT_EQ(link.size(), 2U);
    EXPECT_EQ(link[1].world, (core::DVec2{3, 4}));
    // Strokes and rotated boxes have none.
    EXPECT_TRUE(handlesFor(box(document::Stroke{.points = document::makeStrokePoints({{0, 0, 1}})}))
                    .empty());
    document::Element rotated = box(document::Shape{.size = {10, 10}});
    rotated.transform.rotation = 0.3F;
    EXPECT_TRUE(handlesFor(rotated).empty());
}

TEST(SelectionHandlesTest, BoxesResizeFromTheOppositeCornerOrEdge) {
    const auto rect = box(document::Shape{.size = {100, 50}});
    auto dragged = dragHandle(rect, HandleKind::BottomRight, {250, 200}, {});
    ASSERT_TRUE(dragged.has_value());
    EXPECT_EQ(dragged->transform.position, (core::DVec2{100, 100}));
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{150, 100}));
    dragged = dragHandle(rect, HandleKind::Left, {50, 999}, {}); // edges ignore the other axis
    EXPECT_EQ(dragged->transform.position, (core::DVec2{50, 100}));
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{150, 50}));
    // Past the opposite side: the box flips instead of turning negative.
    dragged = dragHandle(rect, HandleKind::Right, {60, 120}, {});
    EXPECT_EQ(dragged->transform.position, (core::DVec2{60, 100}));
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{40, 50}));
    // Never smaller than the minimum.
    dragged = dragHandle(rect, HandleKind::Right, {100, 120}, {.minSize = 4});
    EXPECT_EQ(sizeOf(*dragged).x, 4.0F);
    // Shift keeps the aspect ratio on corners.
    dragged = dragHandle(rect, HandleKind::BottomRight, {400, 160}, {.constrain = true});
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{300, 150}));
}

TEST(SelectionHandlesTest, ImagesKeepTheirAspectAndTextBoxesTheirLayoutHeight) {
    const auto image = box(document::Image{.asset = {}, .size = {80, 40}});
    auto dragged = dragHandle(image, HandleKind::BottomRight, {300, 130}, {});
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{200, 100})); // the larger ratio wins
    dragged = dragHandle(image, HandleKind::BottomRight, {300, 130}, {.constrain = true});
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{200, 30})); // Shift: free
    const auto text = box(document::TextBox{.size = {200, 40}, .text = "some text"});
    // Two lines below 150 wide, each as high as the font size plus 4.
    const auto layout = [](std::string_view, float width, float fontSize) {
        return (width < 150.0F ? 2.0F : 1.0F) * (fontSize + 4.0F);
    };
    dragged = dragHandle(text, HandleKind::Right, {200, 500}, {.textHeight = layout});
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{100, 40})); // narrower: two lines at 16
    EXPECT_EQ(dragged->transform.position, (core::DVec2{100, 100}));
    // The box's own font size is laid out (1.2-TXT-02), and it is kept.
    const auto large = box(document::TextBox{.size = {200, 36}, .text = "x", .fontSize = 32});
    dragged = dragHandle(large, HandleKind::Right, {200, 500}, {.textHeight = layout});
    EXPECT_EQ(sizeOf(*dragged), (core::Vec2{100, 72}));
    EXPECT_EQ(std::get<document::TextBox>(dragged->payload).fontSize, 32.0F);
}

TEST(SelectionHandlesTest, LineEndsAndConnectorEnds) {
    const auto line = box(document::Shape{.kind = ShapeKind::Arrow, .size = {100, 0}});
    auto dragged = dragHandle(line, HandleKind::End, {100, 200}, {});
    ASSERT_TRUE(dragged.has_value());
    EXPECT_EQ(dragged->transform.position, (core::DVec2{100, 100}));
    EXPECT_NEAR(dragged->transform.rotation, std::numbers::pi / 2.0, 1e-6);
    EXPECT_FLOAT_EQ(sizeOf(*dragged).x, 100.0F);
    dragged = dragHandle(line, HandleKind::Start, {0, 100}, {});
    EXPECT_EQ(dragged->transform.position, (core::DVec2{0, 100}));
    EXPECT_FLOAT_EQ(sizeOf(*dragged).x, 200.0F);
    dragged = dragHandle(line, HandleKind::End, {200, 190}, {.constrain = true});
    EXPECT_NEAR(dragged->transform.rotation, std::numbers::pi / 4.0, 1e-6);

    core::Uuid::Bytes bytes{};
    bytes[6] = 0x70;
    bytes[8] = 0x80;
    bytes[15] = 1;
    const core::ElementId attached{core::Uuid{bytes}};
    const auto link =
        box(document::Connector{.start = {.position = {0, 0}, .attachedTo = attached},
                                .end = {.position = {10, 0}, .attachedTo = attached}});
    dragged = dragHandle(link, HandleKind::End, {50, 60}, {});
    const auto& connector = std::get<document::Connector>(dragged->payload);
    EXPECT_EQ(connector.end.position, (core::DVec2{50, 60}));
    EXPECT_FALSE(connector.end.attachedTo.has_value()); // the tool re-attaches on release
    EXPECT_EQ(connector.start.attachedTo, attached);
    EXPECT_FALSE(dragHandle(link, HandleKind::TopLeft, {0, 0}, {}).has_value());
}

} // namespace
} // namespace studyapp::canvas
