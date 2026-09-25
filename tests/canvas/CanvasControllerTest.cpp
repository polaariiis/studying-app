#include "CanvasTestSupport.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/canvas/RenderBatches.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace studyapp::canvas {
namespace {

using test::CanvasFixture;

void expectNear(const core::DVec2& actual, const core::DVec2& expected, double tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
}

/// World position of a stroke's first and last point.
std::pair<core::DVec2, core::DVec2> strokeEnds(const document::Element& element) {
    const auto& points = *std::get<document::Stroke>(element.payload).points;
    const core::Affine2 toWorld = document::localToWorld(element.transform);
    return {toWorld.apply({points.front().x, points.front().y}),
            toWorld.apply({points.back().x, points.back().y})};
}

// ---------------------------------------------------------------------------- pen

TEST(CanvasControllerTest, PenStrokeBecomesOneDocumentElementInWorldCoordinates) {
    CanvasFixture f;
    f.drag({100, 100}, {300, 180}, 20);
    const auto ids = f.elements();
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(f.port.executed, 1U);                    // one command for the whole gesture
    EXPECT_EQ(f.doc.editor.history().undoCount(), 4U); // notebook, section, page, stroke
    const document::Element& stroke = f.element(ids[0]);
    ASSERT_TRUE(std::holds_alternative<document::Stroke>(stroke.payload));
    const auto [start, end] = strokeEnds(stroke);
    expectNear(start, {100, 100}, 1e-9); // view == world at zoom 1, origin top-left
    expectNear(end, {300, 180}, 1e-9);
    // Simplification: a straight drag keeps only its ends.
    EXPECT_EQ(std::get<document::Stroke>(stroke.payload).points->size(), 2U);
}

TEST(CanvasControllerTest, DrawingStaysAlignedAfterZoomAndPan) {
    CanvasFixture f;
    f.controller.onWheel({.viewPos = {400, 300}, .angleDelta = {0, 480}}); // zoom in
    f.pointer(PointerPhase::Down, {50, 50}, PointerButton::Middle);        // middle-drag pans
    f.pointer(PointerPhase::Move, {130, 90}, PointerButton::Middle);
    f.pointer(PointerPhase::Up, {130, 90}, PointerButton::Middle);
    EXPECT_TRUE(f.elements().empty()); // panning never draws

    const Camera camera = f.controller.camera();
    EXPECT_GT(camera.zoom(), 1.5);
    f.drag({200, 250}, {420, 260}, 15);
    ASSERT_EQ(f.elements().size(), 1U);
    const auto [start, end] = strokeEnds(f.element(f.elements()[0]));
    // Input used exactly the camera the frame is rendered with.
    expectNear(start, camera.viewToWorld({200, 250}), 1e-9);
    expectNear(end, camera.viewToWorld({420, 260}), 1e-4); // float local offsets
    expectNear(camera.worldToView(start), {200, 250}, 1e-6);

    f.controller.onWheel({.viewPos = {10, 590}, .angleDelta = {0, -960}}); // zoom out
    const Camera zoomedOut = f.controller.camera();
    f.drag({30, 30}, {90, 30}, 5);
    ASSERT_EQ(f.elements().size(), 2U);
    expectNear(strokeEnds(f.element(f.elements()[1])).first, zoomedOut.viewToWorld({30, 30}), 1e-9);
}

TEST(CanvasControllerTest, StrokeWidthIsInWorldUnitsAndPressureIsKept) {
    CanvasFixture f;
    f.controller.penStyle().width = 6.0F;
    f.controller.onWheel({.viewPos = {0, 0}, .angleDelta = {0, 600}});
    f.pointer(PointerPhase::Down, {100, 100}, PointerButton::Primary, {}, 0.2F, PointerDevice::Pen);
    f.pointer(PointerPhase::Move, {200, 100}, PointerButton::Primary, {}, 0.9F, PointerDevice::Pen);
    f.pointer(PointerPhase::Up, {300, 100}, PointerButton::Primary, {}, 0.5F, PointerDevice::Pen);
    ASSERT_EQ(f.elements().size(), 1U);
    const auto& stroke = std::get<document::Stroke>(f.element(f.elements()[0]).payload);
    EXPECT_FLOAT_EQ(stroke.baseWidth, 6.0F); // independent of zoom
    EXPECT_FLOAT_EQ(stroke.points->front().pressure, 0.2F);
    EXPECT_LT(strokeRadius(stroke, 0.2F), strokeRadius(stroke, 0.9F));
}

TEST(CanvasControllerTest, LivePreviewIsNotPersistedUntilRelease) {
    CanvasFixture f;
    f.pointer(PointerPhase::Down, {10, 10});
    for (int i = 1; i < 30; ++i) {
        f.pointer(PointerPhase::Move, {10.0 + i * 5, 10.0 + std::sin(i) * 20});
    }
    EXPECT_TRUE(f.elements().empty());
    EXPECT_GT(f.controller.stats().livePoints, 10U);
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 1U); // the live stroke only
    f.controller.onKey({.key = Key::Escape});     // cancel
    f.pointer(PointerPhase::Up, {200, 10});
    EXPECT_TRUE(f.elements().empty());
    EXPECT_EQ(f.port.executed, 0U);
}

TEST(CanvasControllerTest, PenRespectsReadOnlyButtonsAndBoundedPages) {
    CanvasFixture f;
    f.port.readOnly = true;
    f.drag({10, 10}, {100, 100});
    f.port.readOnly = false;
    f.drag({10, 10}, {100, 100}, 5, PointerButton::Secondary);
    EXPECT_TRUE(f.elements().empty());

    CanvasFixture bounded({.extent = document::PageExtent::Bounded, .size = {200, 100}});
    const Camera& camera = bounded.controller.camera();
    const core::DVec2 outside = camera.worldToView({-20, 50});
    bounded.drag(outside, camera.worldToView({50, 50}));
    EXPECT_TRUE(bounded.elements().empty()); // starting off the page does nothing
    bounded.drag(camera.worldToView({20, 20}), camera.worldToView({300, 20}));
    EXPECT_EQ(bounded.elements().size(), 1U); // may continue past the edge
}

TEST(CanvasControllerTest, DrawUndoRedo) {
    CanvasFixture f;
    f.drag({10, 10}, {100, 10});
    const auto id = f.elements().at(0);
    const document::Element original = f.element(id);
    ASSERT_OK(f.port.undo());
    EXPECT_TRUE(f.elements().empty());
    EXPECT_EQ(f.controller.scene().elementCount(), 0U);
    ASSERT_OK(f.port.redo());
    ASSERT_EQ(f.elements().size(), 1U);
    EXPECT_EQ(f.element(id), original); // identical geometry and id
    EXPECT_EQ(f.controller.scene().elementCount(), 1U);
}

// ---------------------------------------------------------------------------- selection

struct SelectionFixture : CanvasFixture {
    std::vector<core::ElementId> strokes;

    SelectionFixture() {
        // Three horizontal strokes at y = 100, 200, 300.
        for (int i = 1; i <= 3; ++i) {
            drag({100, 100.0 * i}, {300, 100.0 * i}, 4);
        }
        strokes = elements();
        controller.setTool(ToolKind::Select);
    }
};

TEST(CanvasControllerTest, ClickSelectsTheStrokeUnderThePointer) {
    SelectionFixture f;
    f.click({200, 201}); // within the pick tolerance
    ASSERT_EQ(f.controller.selection().size(), 1U);
    EXPECT_TRUE(f.controller.selection().contains(f.strokes[1]));
    f.click({200, 250}); // empty space clears
    EXPECT_TRUE(f.controller.selection().empty());
    f.click({150, 100});
    f.click({150, 300}, {.shift = true}); // shift adds
    EXPECT_EQ(f.controller.selection().size(), 2U);
    f.click({150, 300}, {.shift = true}); // and toggles
    EXPECT_EQ(f.controller.selection().size(), 1U);
}

TEST(CanvasControllerTest, RectangleSelectionIntersectsOrContains) {
    SelectionFixture f;
    f.drag({250, 50}, {350, 250}, 5); // touches strokes 1 and 2
    EXPECT_EQ(f.controller.selection().size(), 2U);
    f.drag({250, 50}, {350, 350}, 5, PointerButton::Primary, {.alt = true});
    EXPECT_TRUE(f.controller.selection().empty()); // none fully contained
    f.drag({50, 50}, {350, 350}, 5, PointerButton::Primary, {.alt = true});
    EXPECT_EQ(f.controller.selection().size(), 3U);
}

TEST(CanvasControllerTest, RectangleSelectionUsesWorldCoordinatesWhenZoomed) {
    SelectionFixture f;
    f.controller.onWheel({.viewPos = {0, 0}, .angleDelta = {0, 480}});
    const Camera& camera = f.controller.camera();
    // Select only the first stroke by a rectangle given in world units.
    f.drag(camera.worldToView({90, 90}), camera.worldToView({310, 110}), 5);
    ASSERT_EQ(f.controller.selection().size(), 1U);
    EXPECT_TRUE(f.controller.selection().contains(f.strokes[0]));
}

TEST(CanvasControllerTest, MovingASelectionIsOneUndoableCommand) {
    SelectionFixture f;
    f.controller.onWheel({.viewPos = {0, 0}, .angleDelta = {0, 463}}); // zoom ≈ 2
    const double zoom = f.controller.camera().zoom();
    const Camera& camera = f.controller.camera();
    f.drag(camera.worldToView({50, 50}), camera.worldToView({350, 350}), 5); // select all 3
    ASSERT_EQ(f.controller.selection().size(), 3U);
    std::vector<core::DVec2> before;
    for (const auto id : f.strokes) {
        before.push_back(f.element(id).transform.position);
    }
    const auto executedBefore = f.port.executed;
    const core::DVec2 grab = camera.worldToView({200, 200});
    f.drag(grab, grab + core::DVec2{80, -40}, 12);
    EXPECT_EQ(f.port.executed, executedBefore + 1); // one command, not one per move event
    for (std::size_t i = 0; i < f.strokes.size(); ++i) {
        expectNear(f.element(f.strokes[i]).transform.position,
                   before[i] + core::DVec2{80 / zoom, -40 / zoom}, 1e-9);
    }
    EXPECT_EQ(f.controller.selection().size(), 3U); // selection survives the move
    ASSERT_OK(f.port.undo());
    for (std::size_t i = 0; i < f.strokes.size(); ++i) {
        expectNear(f.element(f.strokes[i]).transform.position, before[i], 0.0);
    }
    ASSERT_OK(f.port.redo());
    expectNear(f.element(f.strokes[0]).transform.position,
               before[0] + core::DVec2{80 / zoom, -40 / zoom}, 1e-9);
}

TEST(CanvasControllerTest, SmallJitterIsAClickNotAMove) {
    SelectionFixture f;
    f.click({200, 100});
    const auto executed = f.port.executed;
    f.drag({200, 100}, {201, 101}, 2); // below the drag threshold
    EXPECT_EQ(f.port.executed, executed);
}

TEST(CanvasControllerTest, DeletingASelectionIsOneUndoableCommand) {
    SelectionFixture f;
    f.drag({50, 150}, {350, 350}, 5); // strokes 2 and 3
    ASSERT_EQ(f.controller.selection().size(), 2U);
    f.controller.onKey({.key = Key::Delete});
    EXPECT_EQ(f.elements(), (std::vector<core::ElementId>{f.strokes[0]}));
    EXPECT_TRUE(f.controller.selection().empty());
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements(), f.strokes); // both back, same ids, same order
    ASSERT_OK(f.port.redo());
    EXPECT_EQ(f.elements().size(), 1U);
    // A selected element that disappears through undo is pruned from the selection.
    ASSERT_OK(f.port.undo()); // the two deleted strokes are back
    f.click({150, 300});
    ASSERT_TRUE(f.controller.selection().contains(f.strokes[2]));
    ASSERT_OK(f.port.undo()); // undoes the creation of stroke 3
    EXPECT_TRUE(f.controller.selection().empty());
}

// ---------------------------------------------------------------------------- eraser

TEST(CanvasControllerTest, StrokeEraserRemovesTouchedStrokesInOneCommand) {
    SelectionFixture f;
    f.controller.setTool(ToolKind::Eraser);
    const auto executed = f.port.executed;
    f.drag({200, 80}, {200, 220}, 10); // crosses strokes 1 and 2
    EXPECT_EQ(f.port.executed, executed + 1);
    EXPECT_EQ(f.elements(), (std::vector<core::ElementId>{f.strokes[2]}));
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements(), f.strokes);
    // The eraser ignores empty space and non-stroke content.
    f.drag({500, 500}, {600, 500}, 4);
    EXPECT_EQ(f.elements().size(), 3U);
}

TEST(CanvasControllerTest, PenEraserEndErasesWithAnyTool) {
    SelectionFixture f;
    f.controller.setTool(ToolKind::Pen);
    f.pointer(PointerPhase::Down, {200, 90}, PointerButton::Primary, {}, 1.0F,
              PointerDevice::Eraser);
    f.pointer(PointerPhase::Move, {200, 110}, PointerButton::Primary, {}, 1.0F,
              PointerDevice::Eraser);
    f.pointer(PointerPhase::Up, {200, 110}, PointerButton::Primary, {}, 1.0F,
              PointerDevice::Eraser);
    EXPECT_EQ(f.elements().size(), 2U);
}

TEST(CanvasControllerTest, EraserRingIsTheCursorNotRenderedContent) {
    CanvasFixture f;
    f.controller.setTool(ToolKind::Eraser);
    EXPECT_EQ(f.controller.cursor(), CursorShape::EraserRing);
    // Hovering draws nothing: the ring is the platform cursor, so no overlay can be left
    // behind at a stale position when the pointer leaves the canvas.
    for (int x = 100; x <= 700; x += 100) {
        f.pointer(PointerPhase::Move, {static_cast<double>(x), 300}, PointerButton::None);
    }
    (void)f.frame();
    EXPECT_TRUE(f.renderer.lastOverlay.empty());
    // Also while erasing, and for a pen's eraser end with another tool.
    f.pointer(PointerPhase::Down, {100, 100});
    EXPECT_EQ(f.controller.cursor(), CursorShape::EraserRing);
    f.pointer(PointerPhase::Up, {120, 100});
    f.controller.setTool(ToolKind::Pen);
    EXPECT_EQ(f.controller.cursor(), CursorShape::Crosshair);
    f.pointer(PointerPhase::Down, {100, 100}, PointerButton::Primary, {}, 1.0F,
              PointerDevice::Eraser);
    EXPECT_EQ(f.controller.cursor(), CursorShape::EraserRing);
    f.pointer(PointerPhase::Up, {120, 100}, PointerButton::Primary, {}, 1.0F,
              PointerDevice::Eraser);
    EXPECT_EQ(f.controller.cursor(), CursorShape::Crosshair);
}

TEST(CanvasControllerTest, HoverRequestsNoRepaint) {
    // Rendering is on demand: moving the pointer without a gesture changes nothing on the
    // canvas, so it must not cost a frame (it used to repaint on every mouse move).
    CanvasFixture f;
    int redraws = 0;
    f.controller.setRedrawCallback([&redraws] { ++redraws; });
    for (const ToolKind tool :
         {ToolKind::Pen, ToolKind::Select, ToolKind::Eraser, ToolKind::Pan, ToolKind::Zoom}) {
        f.controller.setTool(tool);
        redraws = 0;
        for (int x = 100; x <= 700; x += 50) {
            f.pointer(PointerPhase::Move, {static_cast<double>(x), 250}, PointerButton::None);
        }
        // A release or cancel without a gesture (e.g. focus loss while hovering) neither.
        f.pointer(PointerPhase::Up, {0, 0});
        f.controller.onPointer({.phase = PointerPhase::Cancel});
        EXPECT_EQ(redraws, 0) << toString(tool);
    }
    // A gesture repaints on every step, so the frame follows the pointer.
    f.controller.setTool(ToolKind::Pen);
    f.pointer(PointerPhase::Down, {100, 100});
    const int afterDown = redraws;
    EXPECT_GE(afterDown, 1);
    f.pointer(PointerPhase::Move, {150, 120});
    f.pointer(PointerPhase::Move, {200, 140});
    EXPECT_EQ(redraws, afterDown + 2);
    f.pointer(PointerPhase::Up, {200, 140});
    EXPECT_GE(redraws, afterDown + 3); // plus the committed stroke's patch
    EXPECT_EQ(f.elements().size(), 1U);
    f.controller.setRedrawCallback({});
}

// ---------------------------------------------------------------------------- navigation

TEST(CanvasControllerTest, SpacePanAndWheelZoomAroundTheCursor) {
    CanvasFixture f;
    const core::DVec2 cursor{600, 150};
    const core::DVec2 anchor = f.controller.camera().viewToWorld(cursor);
    f.controller.onWheel({.viewPos = cursor, .angleDelta = {0, 120}});
    expectNear(f.controller.camera().viewToWorld(cursor), anchor, 1e-9);
    EXPECT_NEAR(f.controller.camera().zoom(), std::pow(1.0015, 120), 1e-12);

    f.controller.onKey({.key = Key::Space, .pressed = true});
    const core::DVec2 centre = f.controller.camera().center();
    f.drag({100, 100}, {150, 100}, 3);
    EXPECT_TRUE(f.elements().empty());
    EXPECT_LT(f.controller.camera().center().x, centre.x); // content followed the pointer
    f.controller.onKey({.key = Key::Space, .pressed = false});
    f.drag({100, 100}, {150, 100}, 3);
    EXPECT_EQ(f.elements().size(), 1U);

    // Touchpad scrolling pans instead of zooming.
    const double zoom = f.controller.camera().zoom();
    f.controller.onWheel({.viewPos = cursor, .pixelDelta = {0, 40}});
    EXPECT_DOUBLE_EQ(f.controller.camera().zoom(), zoom);
    f.controller.onZoomGesture({.viewPos = cursor, .scaleFactor = 1.5});
    EXPECT_NEAR(f.controller.camera().zoom(), zoom * 1.5, 1e-12);
}

TEST(CanvasControllerTest, ResizeKeepsInputAligned) {
    CanvasFixture f;
    f.controller.setViewport({1600, 900}, 2.0); // e.g. window maximised on a HiDPI screen
    const Camera camera = f.controller.camera();
    EXPECT_DOUBLE_EQ(camera.deviceSize().x, 3200.0);
    f.drag({700, 400}, {900, 400}, 5);
    ASSERT_EQ(f.elements().size(), 1U);
    expectNear(strokeEnds(f.element(f.elements()[0])).first, camera.viewToWorld({700, 400}), 1e-9);
}

TEST(CanvasControllerTest, BoundedPagesAreFittedAndCannotBeLost) {
    CanvasFixture f({.extent = document::PageExtent::Bounded, .size = document::kA4PortraitSize});
    const core::DRect page{{0, 0}, document::kA4PortraitSize};
    const core::DRect visible = f.controller.camera().visibleWorldRect();
    EXPECT_TRUE(visible.contains(page)); // fitted with a margin
    f.controller.setTool(ToolKind::Pan);
    f.drag({400, 300}, {400 + 20'000, 300 + 20'000}, 4);
    EXPECT_TRUE(f.controller.camera().visibleWorldRect().intersects(page));
}

// ---------------------------------------------------------------------------- rendering

TEST(CanvasControllerTest, FramesCullDrawInOrderAndNeverShowStaleContent) {
    SelectionFixture f;
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 3U);
    EXPECT_EQ(f.controller.stats().visibleElements, 3U);

    // Pan so only nothing is visible: culled.
    f.controller.setTool(ToolKind::Pan);
    f.drag({400, 300}, {400 + 2000, 300}, 2);
    f.frame();
    EXPECT_TRUE(f.renderer.lastContent.empty());
    f.controller.resetView();

    // A move reuses the tessellated mesh; only the transform changes.
    f.controller.setTool(ToolKind::Select);
    f.frame();
    const auto created = f.renderer.created;
    f.click({200, 100});
    f.drag({200, 100}, {260, 100}, 4);
    f.frame();
    EXPECT_EQ(f.renderer.created, created + 1); // the selection overlay mesh only
    EXPECT_EQ(f.renderer.lastOverlay.size(), 1U);

    // Deleted content disappears and its GPU mesh is released.
    const std::size_t meshesBefore = f.renderer.meshes.size();
    f.controller.onKey({.key = Key::Delete});
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 2U);
    EXPECT_LT(f.renderer.meshes.size(), meshesBefore);
    EXPECT_TRUE(f.renderer.lastOverlay.empty());
}

TEST(CanvasControllerTest, BackgroundComesFromThePage) {
    CanvasFixture f({.extent = document::PageExtent::Bounded,
                     .size = {400, 300},
                     .background = {.color = core::Color::fromRgba(250, 250, 240),
                                    .pattern = document::BackgroundPattern::Grid,
                                    .spacing = 25}});
    f.frame();
    const render::Background& bg = f.renderer.lastBackground;
    EXPECT_EQ(bg.pattern, render::BackgroundPattern::Grid);
    EXPECT_TRUE(bg.bounded);
    EXPECT_FLOAT_EQ(bg.spacing, 25.0F);
    EXPECT_EQ(bg.paperColor, core::Color::fromRgba(250, 250, 240));
    const core::DVec2 centre = f.controller.camera().center();
    EXPECT_NEAR(bg.pageRect.min.x, -centre.x, 1e-3); // camera-relative
    EXPECT_GE(bg.patternPhase.x, 0.0F);
    EXPECT_LT(bg.patternPhase.x, 25.0F);

    auto changed = document::commands::setPageFormat(
        f.doc.workspace, f.page, {.extent = document::PageExtent::Infinite, .background = {}},
        f.doc.clock);
    ASSERT_OK(changed);
    ASSERT_OK(f.port.execute(std::move(*changed)));
    f.frame();
    EXPECT_FALSE(f.renderer.lastBackground.bounded);
    EXPECT_EQ(f.renderer.lastBackground.pattern, render::BackgroundPattern::None);
}

TEST(CanvasControllerTest, GraphicsResetReuploadsFromCpuCaches) {
    SelectionFixture f;
    f.frame();
    const auto built = f.controller.stats().cache.builtLastFrame;
    f.renderer.releaseAll();
    f.controller.onGraphicsReset();
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 3U);
    EXPECT_EQ(f.controller.stats().cache.builtLastFrame, 0U); // no re-tessellation
    EXPECT_GE(f.controller.stats().cache.uploadedLastFrame, 3U);
    (void)built;
}

/// A page with `count` small strokes on a grid, created directly through commands.
struct CrowdedFixture : CanvasFixture {
    std::vector<core::ElementId> strokes;

    explicit CrowdedFixture(int count) {
        for (int i = 0; i < count; ++i) {
            auto created = document::commands::createElement(
                doc.workspace, layer,
                {.transform = {.position = {(i % 50) * 20.0, (i / 50) * 20.0}},
                 .payload = document::test::makeStroke({{0, 0, 1}, {8, 4, 0.5F}, {12, 0, 1}})},
                doc.ids);
            EXPECT_TRUE(created.has_value());
            strokes.push_back(created->id);
            EXPECT_TRUE(port.execute(std::move(created->command)).has_value());
        }
    }

    std::uint64_t triangles(const std::vector<render::DrawItem>& items) const {
        std::uint64_t total = 0;
        for (const auto& item : items) {
            total += renderer.meshes.at(item.mesh.index).triangleCount();
        }
        return total;
    }
};

TEST(CanvasControllerTest, ManyVisibleElementsAreBatchedWithoutChangingWhatIsDrawn) {
    CrowdedFixture f(1300);
    // Everything visible (zoom ~0.74, detail bucket 0): runs of consecutive elements.
    f.controller.zoomToFit();
    f.frame();
    ASSERT_TRUE(f.controller.stats().batched);
    const std::size_t runs = (1300 + RenderBatches::kBatchSize - 1) / RenderBatches::kBatchSize;
    EXPECT_EQ(f.renderer.lastContent.size(), runs);
    std::uint64_t perElement = 0;
    for (const auto id : f.strokes) {
        for (const auto& part : buildElementMeshes(f.element(id), 1.0F)) {
            perElement += part.mesh.triangleCount();
        }
    }
    EXPECT_EQ(f.triangles(f.renderer.lastContent), perElement); // same geometry, fewer draws
    const auto& batches = f.controller.stats();
    EXPECT_EQ(batches.batches, runs);

    // Unchanged frames reuse every run.
    f.controller.panBy({3, 2});
    f.frame();
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 0U);

    // A deleted element disappears from its run (no stale geometry); only its run and the
    // runs after it are rebuilt.
    auto removed = document::commands::deleteElement(f.doc.workspace, f.strokes.back());
    ASSERT_OK(removed);
    ASSERT_OK(f.port.execute(std::move(*removed)));
    f.frame();
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 1U);
    const auto lastParts = buildElementMeshes(f.element(f.strokes.front()), 1.0F);
    EXPECT_EQ(f.triangles(f.renderer.lastContent),
              perElement - lastParts.front().mesh.triangleCount());

    // Few visible again: one item per element.
    f.controller.zoomBy(8.0);
    f.frame();
    EXPECT_FALSE(f.controller.stats().batched);
    EXPECT_LT(f.renderer.lastContent.size(), 1024U);
    EXPECT_GT(f.renderer.lastContent.size(), 0U);
}

TEST(CanvasControllerTest, BatchesTouchedByAPreviewAreDrawnPerElement) {
    CrowdedFixture f(1300);
    f.controller.zoomToFit();
    f.frame();
    const std::size_t runs = f.renderer.lastContent.size();
    // Start moving one element: its run falls back to per-element drawing, the others stay.
    f.controller.setTool(ToolKind::Select);
    const core::DVec2 at =
        f.controller.camera().worldToView(document::worldBounds(f.element(f.strokes[0])).center());
    f.controller.onPointer({.phase = PointerPhase::Down, .viewPos = at});
    f.controller.onPointer({.phase = PointerPhase::Move, .viewPos = at + core::DVec2{20, 0}});
    ASSERT_TRUE(f.controller.selection().contains(f.strokes[0]));
    f.frame();
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 0U); // previews never rebuild runs
    EXPECT_EQ(f.renderer.lastContent.size(), runs - 1 + RenderBatches::kBatchSize);
    f.controller.onPointer({.phase = PointerPhase::Up, .viewPos = at + core::DVec2{20, 0}});
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), runs);
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 1U); // the moved element's run
}

TEST(CanvasControllerTest, ToolSwitchCancelsTheGesture) {
    CanvasFixture f;
    f.pointer(PointerPhase::Down, {10, 10});
    f.pointer(PointerPhase::Move, {100, 10});
    EXPECT_TRUE(f.controller.isGestureActive());
    f.controller.setTool(ToolKind::Select);
    EXPECT_FALSE(f.controller.isGestureActive());
    f.pointer(PointerPhase::Up, {100, 10});
    EXPECT_TRUE(f.elements().empty());
    EXPECT_EQ(f.controller.cursor(), CursorShape::Arrow);
}

} // namespace
} // namespace studyapp::canvas
