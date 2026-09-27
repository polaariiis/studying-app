#include "CanvasTestSupport.hpp"

#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/canvas/RenderBatches.hpp>
#include <studyapp/canvas/SelectionHandles.hpp>
#include <studyapp/document/StudyCommands.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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
    f.controller.setToolSettings({.eraser = EraserMode::WholeStroke});
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

TEST(CanvasControllerTest, PartialEraserCutsStrokesAsOneUndoableCommand) {
    SelectionFixture f; // three horizontal strokes from x = 100 to 300
    EXPECT_EQ(f.controller.toolSettings().eraser, EraserMode::Partial); // the default
    f.controller.setTool(ToolKind::Eraser);
    const document::Workspace before = f.doc.workspace;
    const auto executed = f.port.executed;

    // During the gesture nothing is written; cut strokes are drawn from their pieces.
    f.pointer(PointerPhase::Down, {200, 80});
    f.pointer(PointerPhase::Move, {200, 150});
    f.pointer(PointerPhase::Move, {200, 220});
    (void)f.frame();
    EXPECT_EQ(f.port.executed, executed);
    EXPECT_EQ(f.renderer.lastContent.size(), 3U); // stroke 3 + the pieces of strokes 1 and 2
    f.pointer(PointerPhase::Up, {200, 220});
    EXPECT_EQ(f.port.executed, executed + 1); // one gesture, one command

    const auto ids = f.elements();
    ASSERT_EQ(ids.size(), 5U);
    EXPECT_EQ(ids[0], f.strokes[0]); // the first piece keeps the id and the draw order
    EXPECT_EQ(ids[2], f.strokes[1]);
    EXPECT_EQ(ids[4], f.strokes[2]);
    const double reach = kEraserRadiusViewPx + 1.0; // eraser + ink radius at zoom 1
    for (std::size_t i = 0; i < 4; ++i) {
        const core::DRect bounds = document::worldBounds(f.element(ids[i]));
        // A gap around x = 200 (bounds include the ink radius 1).
        EXPECT_TRUE(bounds.max.x <= 200.0 - reach + 1.0 + 1e-3 ||
                    bounds.min.x >= 200.0 + reach - 1.0 - 1e-3)
            << i;
    }
    ASSERT_OK(f.port.undo());
    EXPECT_TRUE(f.doc.workspace == before);
    ASSERT_OK(f.port.redo());
    EXPECT_EQ(f.elements().size(), 5U);
    // The preview meshes are released; the pieces are ordinary cached elements now.
    (void)f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 5U);
    EXPECT_EQ(f.renderer.meshes.size(), 5U);
}

TEST(CanvasControllerTest, PartialEraserCancelAndMissChangeNothing) {
    SelectionFixture f;
    f.controller.setTool(ToolKind::Eraser);
    const auto executed = f.port.executed;
    f.pointer(PointerPhase::Down, {200, 80});
    f.pointer(PointerPhase::Move, {200, 150});
    f.controller.onKey({.key = Key::Escape});
    f.pointer(PointerPhase::Up, {200, 150});
    f.drag({500, 500}, {600, 500}, 4); // empty space
    EXPECT_EQ(f.port.executed, executed);
    EXPECT_EQ(f.elements(), f.strokes);
    (void)f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 3U);
}

TEST(CanvasControllerTest, PenEraserEndErasesWithAnyTool) {
    SelectionFixture f;
    f.controller.setToolSettings({.eraser = EraserMode::WholeStroke});
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
    // Keys that change nothing visible neither: Space (the widget also reports it released
    // whenever the canvas loses focus, e.g. to the navigation tree), Escape without a
    // gesture or selection, Delete without a selection.
    f.controller.onKey({.key = Key::Space, .pressed = true});
    f.controller.onKey({.key = Key::Space, .pressed = false});
    f.controller.onKey({.key = Key::Escape, .pressed = true});
    f.controller.onKey({.key = Key::Delete, .pressed = true});
    EXPECT_EQ(redraws, 0);
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

TEST(CanvasControllerTest, PlannerEditsCostNoCanvasFrame) {
    // Tasks, tags and other study records are not drawn: their patches repaint nothing.
    // The page's own format does.
    CanvasFixture f;
    int redraws = 0;
    f.controller.setRedrawCallback([&redraws] { ++redraws; });
    auto task = document::commands::createTask(f.doc.workspace, {.title = "Revise"}, f.doc.clock,
                                               f.doc.ids);
    ASSERT_OK(task);
    ASSERT_OK(f.port.execute(std::move(task->command)));
    auto tag = document::commands::createTag(f.doc.workspace, "Exam", f.doc.clock, f.doc.ids);
    ASSERT_OK(tag);
    ASSERT_OK(f.port.execute(std::move(tag->command)));
    auto tagged = document::commands::setPageTags(f.doc.workspace, f.page, {tag->id}, f.doc.clock);
    ASSERT_OK(tagged);
    ASSERT_OK(f.port.execute(std::move(*tagged)));
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(redraws, 0);
    auto format = document::commands::setPageFormat(
        f.doc.workspace, f.page,
        {.extent = document::PageExtent::Infinite,
         .background = {.pattern = document::BackgroundPattern::Grid}},
        f.doc.clock);
    ASSERT_OK(format);
    ASSERT_OK(f.port.execute(std::move(*format)));
    EXPECT_EQ(redraws, 1);
    f.controller.setRedrawCallback({});
}

TEST(CanvasControllerTest, ZoomingInRefinesDetailOverFramesThenGoesIdle) {
    // A batched page (> 1024 visible strokes) zoomed in past two detail buckets: meshes are
    // refined at most `budget` per frame, each refining frame asks for the next one, and
    // rendering stops asking once everything is at the new detail.
    CanvasFixture f;
    for (int i = 0; i < 1200; ++i) {
        auto created = document::commands::createElement(
            f.doc.workspace, f.layer,
            {.transform = {.position = {(i % 40) * 39.0, (i / 40) * 39.0}},
             .payload = document::test::makeStroke()},
            f.doc.ids);
        ASSERT_TRUE(created.has_value());
        ASSERT_OK(f.port.execute(std::move(created->command)));
    }
    f.controller.zoomToFit();
    f.controller.zoomBy(0.25);
    (void)f.frame(); // everything built at the coarse bucket
    ASSERT_TRUE(f.controller.stats().batched);
    ASSERT_FALSE(f.controller.stats().cache.refinementPending);

    constexpr std::size_t budget = 100;
    f.controller.setRefinementBudget(budget);
    int redraws = 0;
    f.controller.setRedrawCallback([&redraws] { ++redraws; });
    f.controller.zoomBy(4.0); // two buckets finer; the whole page stays in view
    const auto drawnTriangles = [&f] {
        std::size_t total = 0;
        for (const render::DrawItem& item : f.renderer.lastContent) {
            total += f.renderer.meshes.at(item.mesh.index).triangleCount();
        }
        return total;
    };
    const std::size_t triangles = drawnTriangles();
    int frames = 0;
    std::uint32_t refined = 0;
    while (frames < 50) {
        redraws = 0;
        (void)f.frame();
        ++frames;
        const auto stats = f.controller.stats();
        EXPECT_LE(stats.cache.refinedLastFrame, budget);
        EXPECT_EQ(stats.visibleElements, 1200U); // every frame shows the whole page
        refined += stats.cache.refinedLastFrame;
        if (!stats.cache.refinementPending) {
            EXPECT_EQ(redraws, 0); // done: back to rendering on demand
            break;
        }
        EXPECT_EQ(redraws, 1); // one more frame to continue refining
    }
    EXPECT_EQ(refined, 1200U);
    EXPECT_EQ(frames, 12);                  // 12 × 100
    EXPECT_GE(drawnTriangles(), triangles); // finer (rounder) meshes
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
    const std::size_t runs = f.renderer.lastContent.size();
    EXPECT_GE(runs, (1300 + RenderBatches::kBatchSize - 1) / RenderBatches::kBatchSize);
    EXPECT_LE(runs, 1300 / RenderBatches::kMinBatchSize + 1);
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

TEST(CanvasControllerTest, InsertionsInTheMiddleRebuildOnlyNearbyRuns) {
    // Run boundaries follow the elements, not their positions: splitting a stroke in the
    // middle of a crowded page (new elements inserted there) rebuilds only its run and at
    // most its neighbour, not every later run.
    CrowdedFixture f(1300);
    f.controller.zoomToFit();
    f.frame();
    const std::size_t runs = f.renderer.lastContent.size();
    ASSERT_GT(runs, 8U);
    const std::vector<document::commands::StrokePieces> split{
        {.stroke = f.strokes[650],
         .pieces = {document::makeStrokePoints({{0, 0, 1}, {4, 2, 1}}),
                    document::makeStrokePoints({{8, 4, 0.5F}, {12, 0, 1}})}}};
    auto command = document::commands::splitStrokes(f.doc.workspace, split, f.doc.ids);
    ASSERT_OK(command);
    ASSERT_OK(f.port.execute(std::move(*command)));
    f.frame();
    EXPECT_LE(f.controller.stats().batchesRebuilt, 2U);
    EXPECT_GE(f.controller.stats().batchesRebuilt, 1U);
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
    // The moved element's run is drawn element by element (at most kBatchSize items).
    EXPECT_GT(f.renderer.lastContent.size(), runs);
    EXPECT_LE(f.renderer.lastContent.size(), runs - 1 + RenderBatches::kBatchSize);
    f.controller.onPointer({.phase = PointerPhase::Up, .viewPos = at + core::DVec2{20, 0}});
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), runs);
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 1U); // the moved element's run
}

TEST(CanvasControllerTest, ResizePreviewIsDrawnOnABatchedPage) {
    // A shape among many strokes: its run is batched, but dragging its handle still shows
    // the new size (the run is drawn element by element meanwhile, not from its old mesh).
    CrowdedFixture f(1300);
    auto created = document::commands::createElement(
        f.doc.workspace, f.layer,
        {.transform = {.position = {1100, 0}}, .payload = document::Shape{.size = {100, 60}}},
        f.doc.ids);
    ASSERT_OK(created);
    const core::ElementId rect = created->id;
    ASSERT_OK(f.port.execute(std::move(created->command)));
    f.controller.zoomToFit();
    f.frame();
    ASSERT_TRUE(f.controller.stats().batched);
    const std::size_t runs = f.renderer.lastContent.size();
    f.controller.setTool(ToolKind::Select);
    const Camera& camera = f.controller.camera();
    const core::DVec2 edge = camera.worldToView({1100, 30});
    f.controller.onPointer({.phase = PointerPhase::Down, .viewPos = edge});
    f.controller.onPointer({.phase = PointerPhase::Up, .viewPos = edge});
    ASSERT_TRUE(f.controller.selection().contains(rect));

    const core::DVec2 corner = camera.worldToView({1200, 60});
    const core::DVec2 dragged = camera.worldToView({1250, 100});
    f.controller.onPointer({.phase = PointerPhase::Down, .viewPos = corner});
    f.controller.onPointer({.phase = PointerPhase::Move, .viewPos = dragged});
    f.frame();
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 0U); // previews never rebuild runs
    EXPECT_GT(f.renderer.lastContent.size(), runs);     // the resized element's run, per element
    const bool previewDrawn = std::any_of(
        f.renderer.lastContent.begin(), f.renderer.lastContent.end(), [&](const auto& d) {
            const auto it = f.renderer.meshes.find(d.mesh.index);
            return it != f.renderer.meshes.end() && it->second.bounds.max.x > 148.0F &&
                   it->second.bounds.max.x < 152.0F; // the 150-wide outline being dragged
        });
    EXPECT_TRUE(previewDrawn);
    f.controller.onPointer({.phase = PointerPhase::Up, .viewPos = dragged});
    EXPECT_EQ(std::get<document::Shape>(f.element(rect).payload).size, (core::Vec2{150, 100}));
}

TEST(CanvasControllerTest, RunsMovingAsAWholeStayOneDrawDuringTheMovePreview) {
    CrowdedFixture f(1300);
    f.controller.zoomToFit();
    f.frame();
    const std::size_t runs = f.renderer.lastContent.size();
    const std::vector<render::DrawItem> before = f.renderer.lastContent;
    const std::uint64_t triangles = f.triangles(before);
    // Select All, then drag one of the strokes: every run moves as a whole.
    f.controller.setTool(ToolKind::Select);
    f.controller.selectAll();
    const core::DVec2 at =
        f.controller.camera().worldToView(document::worldBounds(f.element(f.strokes[0])).center());
    f.controller.onPointer({.phase = PointerPhase::Down, .viewPos = at});
    f.controller.onPointer({.phase = PointerPhase::Move, .viewPos = at + core::DVec2{30, 12}});
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), runs); // still one draw per run
    EXPECT_EQ(f.controller.stats().batchesRebuilt, 0U);
    EXPECT_EQ(f.triangles(f.renderer.lastContent), triangles);
    const double zoom = f.controller.camera().zoom();
    for (std::size_t i = 0; i < runs; ++i) { // moved by the preview offset, in world units
        EXPECT_NEAR(f.renderer.lastContent[i].transform.tx - before[i].transform.tx, 30 / zoom,
                    1e-3);
        EXPECT_NEAR(f.renderer.lastContent[i].transform.ty - before[i].transform.ty, 12 / zoom,
                    1e-3);
    }
    // Releasing commits one move; the runs are rebuilt at the new positions.
    f.controller.onPointer({.phase = PointerPhase::Up, .viewPos = at + core::DVec2{30, 12}});
    f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), runs);
    EXPECT_EQ(f.triangles(f.renderer.lastContent), triangles);
    expectNear(document::worldBounds(f.element(f.strokes[0])).center(),
               f.controller.camera().viewToWorld(at + core::DVec2{30, 12}), 1e-6);
}

TEST(CanvasControllerTest, SetViewRestoresAViewAfterSwitchingPages) {
    CanvasFixture f;
    const auto second = f.doc.addPage(f.doc.workspace.findPage(f.page)->section, "Second");
    f.controller.zoomBy(3.0);
    f.controller.panBy({-120, 40});
    const Camera left = f.controller.camera();

    f.controller.setPage(second); // a page opens at its initial view
    EXPECT_NEAR(f.controller.camera().zoom(), 1.0, 1e-12);
    f.controller.setPage(f.page);
    f.controller.setView(left.center(), left.zoom()); // back where the user left it
    expectNear(f.controller.camera().center(), left.center(), 1e-9);
    EXPECT_NEAR(f.controller.camera().zoom(), left.zoom(), 1e-12);
    // Drawing is aligned with the restored view.
    f.drag({200, 200}, {300, 200}, 5);
    ASSERT_EQ(f.elements().size(), 1U);
    expectNear(strokeEnds(f.element(f.elements()[0])).first, left.viewToWorld({200, 200}), 1e-9);

    // Invalid zooms are clamped like any other zoom.
    f.controller.setView({0, 0}, 1e9);
    EXPECT_LE(f.controller.camera().zoom(), 64.0);
}

TEST(CanvasControllerTest, SwitchingPagesReleasesThePreviousPagesMeshes) {
    // Page navigation (Phase 5): showing another page reports the old page's elements as
    // removed, so their meshes are released (CPU and GPU) instead of piling up while the
    // user browses pages.
    CanvasFixture f;
    for (int i = 0; i < 5; ++i) {
        f.drag({50.0 + i * 60, 100}, {90.0 + i * 60, 140}, 4);
    }
    (void)f.frame();
    ASSERT_EQ(f.controller.stats().cache.entries, 5U);
    const std::size_t meshesOnFirstPage = f.renderer.meshes.size();
    ASSERT_GE(meshesOnFirstPage, 5U);

    const auto second = f.doc.addPage(f.doc.workspace.findPage(f.page)->section, "Second");
    f.controller.setPage(second);
    (void)f.frame();
    EXPECT_EQ(f.controller.stats().cache.entries, 0U);
    EXPECT_LT(f.renderer.meshes.size(), meshesOnFirstPage); // GPU meshes destroyed too

    // Back on the first page everything is drawn again, correctly.
    f.controller.setPage(f.page);
    (void)f.frame();
    EXPECT_EQ(f.controller.stats().cache.entries, 5U);
    EXPECT_EQ(f.renderer.lastContent.size(), 5U);
}

TEST(CanvasControllerTest, ToolSettingsDecideNewStrokesOnly) {
    CanvasFixture f;
    const core::Color blue = core::Color::fromRgba(0x2F, 0x5F, 0xA8);
    f.controller.setToolSettings(
        {.pen = {.brush = document::Brush::Pen, .color = blue, .width = 4.0F}});
    f.drag({100, 100}, {300, 120}, 8);
    ASSERT_EQ(f.elements().size(), 1U);
    const auto first = f.elements()[0];
    const auto& stroke = std::get<document::Stroke>(f.element(first).payload);
    EXPECT_EQ(stroke.brush, document::Brush::Pen);
    EXPECT_EQ(stroke.color, blue);
    EXPECT_FLOAT_EQ(stroke.baseWidth, 4.0F);

    // Changing the settings is not an edit: nothing written, existing ink unchanged.
    const auto executed = f.port.executed;
    const document::Element before = f.element(first);
    f.controller.setToolSettings({.pen = {.color = core::Color::black(), .width = 1.0F}});
    EXPECT_EQ(f.port.executed, executed);
    EXPECT_TRUE(f.element(first) == before);

    // A stroke keeps the style it started with; the live preview draws in it.
    const core::Color red = core::Color::fromRgba(0xB3, 0x36, 0x2F);
    f.controller.setToolSettings({.pen = {.color = red, .width = 3.0F}});
    f.pointer(PointerPhase::Down, {100, 300});
    f.pointer(PointerPhase::Move, {200, 320});
    (void)f.frame();
    ASSERT_FALSE(f.renderer.lastContent.empty());
    EXPECT_EQ(f.renderer.lastContent.back().color, red); // the live stroke
    f.controller.setToolSettings({.pen = {.color = blue, .width = 9.0F}});
    f.pointer(PointerPhase::Up, {300, 330});
    ASSERT_EQ(f.elements().size(), 2U);
    const auto& second = std::get<document::Stroke>(f.element(f.elements()[1]).payload);
    EXPECT_EQ(second.color, red);
    EXPECT_FLOAT_EQ(second.baseWidth, 3.0F);

    // Undo/redo restore the stroke with its style (it is document data).
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements().size(), 1U);
    ASSERT_OK(f.port.redo());
    EXPECT_EQ(std::get<document::Stroke>(f.element(f.elements()[1]).payload).color, red);
}

TEST(CanvasControllerTest, ToolSettingsAreSanitized) {
    CanvasFixture f;
    f.controller.setToolSettings({.pen = {.width = 0.0F}});
    EXPECT_FLOAT_EQ(f.controller.toolSettings().pen.width, kMinPenWidth);
    f.controller.setToolSettings({.pen = {.width = 1000.0F}});
    EXPECT_FLOAT_EQ(f.controller.toolSettings().pen.width, kMaxPenWidth);
    f.controller.setToolSettings({.pen = {.width = std::numeric_limits<float>::quiet_NaN()}});
    EXPECT_FLOAT_EQ(f.controller.toolSettings().pen.width, PenStyle{}.width);
    f.controller.setToolSettings({.pen = {.color = core::Color::transparent()}});
    EXPECT_EQ(f.controller.toolSettings().pen.color.a, 255); // would draw nothing
    // Even a style set directly is sanitized when a stroke starts.
    f.controller.penStyle().width = -5.0F;
    f.drag({10, 10}, {60, 10}, 3);
    ASSERT_EQ(f.elements().size(), 1U);
    EXPECT_FLOAT_EQ(std::get<document::Stroke>(f.element(f.elements()[0]).payload).baseWidth,
                    kMinPenWidth);
}

// ---------------------------------------------------------------------------- highlighter

TEST(CanvasControllerTest, HighlighterDrawsWithItsOwnStyleAndThePenKeepsItsOwn) {
    CanvasFixture f;
    EXPECT_EQ(f.controller.toolSettings().highlighter, kDefaultHighlighter);
    const auto undoSteps = [&] {
        return f.doc.editor.history().undoCount();
    };
    const std::size_t stepsBefore = undoSteps();

    f.controller.setTool(ToolKind::Highlighter);
    EXPECT_EQ(f.controller.tool(), ToolKind::Highlighter);
    EXPECT_EQ(f.controller.cursor(), CursorShape::Highlighter);
    f.drag({100, 100}, {400, 100}, 8);
    f.controller.setTool(ToolKind::Pen);
    EXPECT_EQ(f.controller.cursor(), CursorShape::Crosshair);
    f.drag({100, 200}, {400, 200}, 8);
    f.controller.setTool(ToolKind::Highlighter);
    f.drag({100, 300}, {400, 300}, 8);
    ASSERT_EQ(f.elements().size(), 3U);

    const auto strokeOf = [&](std::size_t i) -> const document::Stroke& {
        return std::get<document::Stroke>(f.element(f.elements()[i]).payload);
    };
    for (const std::size_t i : {0U, 2U}) {
        EXPECT_EQ(strokeOf(i).brush, document::Brush::Highlighter);
        EXPECT_EQ(strokeOf(i).color, kDefaultHighlighter.color); // translucent: alpha stored
        EXPECT_LT(strokeOf(i).color.a, 255);
        EXPECT_FLOAT_EQ(strokeOf(i).baseWidth, kDefaultHighlighter.width);
    }
    EXPECT_EQ(strokeOf(1).brush, document::Brush::Pen);
    EXPECT_EQ(strokeOf(1).color, core::Color::black());
    EXPECT_FLOAT_EQ(strokeOf(1).baseWidth, PenStyle{}.width);

    // One undo step per stroke; switching tools added none.
    EXPECT_EQ(undoSteps(), stepsBefore + 3);
    ASSERT_OK(f.port.undo());
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements().size(), 1U);
    ASSERT_OK(f.port.redo());
    ASSERT_OK(f.port.redo());
    ASSERT_EQ(f.elements().size(), 3U);
    EXPECT_EQ(strokeOf(1).brush, document::Brush::Pen);
    EXPECT_EQ(strokeOf(2).brush, document::Brush::Highlighter);
    EXPECT_EQ(strokeOf(2).color, kDefaultHighlighter.color);
}

TEST(CanvasControllerTest, HighlighterSettingsDecideNewStrokesOnly) {
    CanvasFixture f;
    const core::Color red = core::Color::fromRgba(0xE0, 0x60, 0x60, 0x60);
    const core::Color yellow = core::Color::fromRgba(0xF2, 0xC9, 0x4C, 0x50);
    f.controller.setTool(ToolKind::Highlighter);
    f.controller.setToolSettings({.highlighter = {.color = red, .width = 20.0F}});
    f.drag({100, 100}, {400, 100}, 8);
    ASSERT_EQ(f.elements().size(), 1U);
    const document::Element first = f.element(f.elements()[0]);

    // Changing the settings writes nothing, redraws nothing and leaves the stroke alone.
    int redraws = 0;
    f.controller.setRedrawCallback([&] { ++redraws; });
    const auto executed = f.port.executed;
    f.controller.setToolSettings({.highlighter = {.color = yellow, .width = 30.0F}});
    EXPECT_EQ(f.port.executed, executed);
    EXPECT_EQ(redraws, 0);
    EXPECT_TRUE(f.element(f.elements()[0]) == first);

    // The live preview draws in the new style, translucent; the committed stroke too.
    f.pointer(PointerPhase::Down, {100, 200});
    f.pointer(PointerPhase::Move, {250, 200});
    (void)f.frame();
    ASSERT_FALSE(f.renderer.lastContent.empty());
    EXPECT_EQ(f.renderer.lastContent.back().color, yellow);
    EXPECT_EQ(f.elements().size(), 1U); // the preview is not document content
    f.pointer(PointerPhase::Up, {400, 200});
    ASSERT_EQ(f.elements().size(), 2U);
    const auto& second = std::get<document::Stroke>(f.element(f.elements()[1]).payload);
    EXPECT_EQ(second.color, yellow);
    EXPECT_FLOAT_EQ(second.baseWidth, 30.0F);
    const auto& firstStroke = std::get<document::Stroke>(f.element(f.elements()[0]).payload);
    EXPECT_EQ(firstStroke.color, red);
    EXPECT_FLOAT_EQ(firstStroke.baseWidth, 20.0F);
    // The pen's settings were not touched by the highlighter's.
    EXPECT_EQ(f.controller.toolSettings().pen, PenStyle{});
}

TEST(CanvasControllerTest, HighlighterIsAnEvenBandAtAnyPressure) {
    CanvasFixture f;
    f.controller.setTool(ToolKind::Highlighter);
    f.pointer(PointerPhase::Down, {100, 100}, PointerButton::Primary, {}, 0.2F, PointerDevice::Pen);
    for (int i = 1; i <= 8; ++i) {
        f.pointer(PointerPhase::Move, {100.0 + 40.0 * i, 100}, PointerButton::Primary, {},
                  0.2F + 0.1F * static_cast<float>(i), PointerDevice::Pen);
    }
    f.pointer(PointerPhase::Up, {420, 100}, PointerButton::Primary, {}, 1.0F, PointerDevice::Pen);
    ASSERT_EQ(f.elements().size(), 1U);
    const document::Element& element = f.element(f.elements()[0]);
    const auto& stroke = std::get<document::Stroke>(element.payload);
    EXPECT_FLOAT_EQ(stroke.points->front().pressure, 0.2F); // recorded as drawn
    EXPECT_FLOAT_EQ(strokeRadius(stroke, 0.2F), kDefaultHighlighter.width * 0.5F);
    EXPECT_FLOAT_EQ(strokeRadius(stroke, 1.0F), kDefaultHighlighter.width * 0.5F);
    const auto parts = buildElementMeshes(element, 1.0F);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_NEAR(parts[0].mesh.bounds.size().y, kDefaultHighlighter.width, 0.5); // full width
    EXPECT_EQ(parts[0].color, kDefaultHighlighter.color);

    // The pen still follows pressure.
    const document::Stroke pen{.brush = document::Brush::Pen, .baseWidth = 2.0F};
    EXPECT_LT(strokeRadius(pen, 0.2F), strokeRadius(pen, 1.0F));
}

TEST(CanvasControllerTest, HighlighterSettingsAreSanitized) {
    CanvasFixture f;
    f.controller.setToolSettings(
        {.highlighter = {.brush = document::Brush::Pen,
                         .color = core::Color::fromRgba(0xF2, 0xC9, 0x4C, 0),
                         .width = std::numeric_limits<float>::infinity()}});
    const PenStyle& style = f.controller.toolSettings().highlighter;
    EXPECT_EQ(style.brush, document::Brush::Highlighter);
    EXPECT_EQ(style.color, core::Color::fromRgba(0xF2, 0xC9, 0x4C, kHighlighterAlpha));
    EXPECT_FLOAT_EQ(style.width, kDefaultHighlighter.width);
    for (const auto& [input, expected] :
         {std::pair{0.0F, kMinPenWidth}, std::pair{-5.0F, kMinPenWidth}, std::pair{8.0F, 8.0F},
          std::pair{1000.0F, kMaxPenWidth}}) {
        f.controller.setToolSettings({.highlighter = {.width = input}});
        EXPECT_FLOAT_EQ(f.controller.toolSettings().highlighter.width, expected) << input;
    }
    // Each tool draws its own brush.
    f.controller.setToolSettings({.pen = {.brush = document::Brush::Highlighter}});
    EXPECT_EQ(f.controller.toolSettings().pen.brush, document::Brush::Pen);
    // An opaque highlighter is the user's choice and is kept.
    f.controller.setToolSettings({.highlighter = {.color = core::Color::black()}});
    EXPECT_EQ(f.controller.toolSettings().highlighter.color, core::Color::black());
}

TEST(CanvasControllerTest, ToolAndSettingChangesRebuildNoGeometry) {
    CanvasFixture f;
    f.drag({100, 100}, {400, 120}, 8);
    f.controller.setTool(ToolKind::Highlighter);
    f.drag({100, 200}, {400, 220}, 8);
    (void)f.frame();
    const std::size_t created = f.renderer.created;
    f.controller.setToolSettings({.pen = {.width = 6.0F}, .highlighter = {.width = 30.0F}});
    f.controller.setTool(ToolKind::Pen);
    f.controller.setTool(ToolKind::Highlighter);
    (void)f.frame();
    EXPECT_EQ(f.controller.stats().cache.builtLastFrame, 0U);
    EXPECT_EQ(f.controller.stats().cache.uploadedLastFrame, 0U);
    EXPECT_EQ(f.renderer.created, created);

    // Batched pages too: every run is reused.
    CrowdedFixture crowded(1300);
    crowded.controller.zoomToFit();
    (void)crowded.frame();
    ASSERT_TRUE(crowded.controller.stats().batched);
    crowded.controller.setToolSettings({.highlighter = {.width = 30.0F}});
    crowded.controller.setTool(ToolKind::Highlighter);
    (void)crowded.frame();
    EXPECT_EQ(crowded.controller.stats().batchesRebuilt, 0U);
    EXPECT_EQ(crowded.controller.stats().cache.builtLastFrame, 0U);
}

TEST(CanvasControllerTest, BatchesNumberTheirPartsSoEachCoversAPixelOnce) {
    // The renderer draws each part of a mesh at most once per pixel (translucent ink stays
    // even where a stroke overlaps itself). Batches number every member's mesh parts in
    // draw order, so a batched page looks the same as the page drawn element by element.
    CrowdedFixture f(1300);
    f.controller.zoomToFit();
    (void)f.frame();
    ASSERT_TRUE(f.controller.stats().batched);
    const auto& batches = f.controller.stats().batches;
    ASSERT_EQ(f.renderer.lastContent.size(), batches);
    std::size_t members = 0;
    for (const render::DrawItem& item : f.renderer.lastContent) {
        const render::MeshData& mesh = f.renderer.meshes.at(item.mesh.index);
        ASSERT_EQ(mesh.parts.size(), mesh.vertices.size());
        EXPECT_EQ(mesh.parts.front(), 0U);
        EXPECT_TRUE(std::ranges::is_sorted(mesh.parts));
        // Every member is a stroke with one mesh part: one part number each, no gaps.
        std::vector<std::uint32_t> distinct(mesh.parts.begin(), mesh.parts.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        EXPECT_EQ(distinct.size(), static_cast<std::size_t>(mesh.parts.back()) + 1);
        members += distinct.size();
    }
    EXPECT_EQ(members, 1300U);

    // Drawn element by element, a mesh is one part (no part numbers needed).
    f.controller.zoomBy(8.0);
    (void)f.frame();
    ASSERT_FALSE(f.controller.stats().batched);
    for (const render::DrawItem& item : f.renderer.lastContent) {
        EXPECT_TRUE(f.renderer.meshes.at(item.mesh.index).parts.empty());
    }
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

TEST(CanvasControllerTest, PartialEraserKeepsHighlighterStyleAndOnlyTouchesNearbyStrokes) {
    CrowdedFixture f(1300); // small strokes 20 units apart
    f.controller.setTool(ToolKind::Highlighter);
    f.drag({30, 700}, {600, 700}, 20);
    const core::ElementId band = f.elements().back();
    f.controller.setTool(ToolKind::Eraser);
    f.drag({300, 680}, {300, 720}, 4);
    const document::Command* last = f.doc.editor.history().nextUndo();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->label, "Erase");
    EXPECT_EQ(last->patch.size(), 2U); // the band is split in two; no crowded stroke touched
    const auto ids = f.elements();
    const auto& first = std::get<document::Stroke>(f.element(band).payload);
    const auto& second = std::get<document::Stroke>(f.element(ids.back()).payload);
    for (const document::Stroke* piece : {&first, &second}) {
        EXPECT_EQ(piece->brush, document::Brush::Highlighter);
        EXPECT_EQ(piece->color, kDefaultHighlighter.color);
        EXPECT_FLOAT_EQ(piece->baseWidth, kDefaultHighlighter.width);
    }
    // Erasing through the crowded strokes cuts only the ones under the eraser.
    f.drag({5, 5}, {5, 60}, 4);
    last = f.doc.editor.history().nextUndo();
    ASSERT_NE(last, nullptr);
    EXPECT_GT(last->patch.size(), 0U);
    EXPECT_LE(last->patch.size(), 12U);
}

// ---------------------------------------------------------------------------- shapes

TEST(CanvasControllerTest, ShapeToolCreatesEachKindAsOneUndoableCommand) {
    CanvasFixture f;
    f.controller.setTool(ToolKind::Shape);
    const auto undoSteps = [&] {
        return f.doc.editor.history().undoCount();
    };
    const std::size_t before = undoSteps();
    int y = 100;
    for (const auto kind : {document::ShapeKind::Rectangle, document::ShapeKind::Ellipse,
                            document::ShapeKind::Line, document::ShapeKind::Arrow}) {
        ShapeStyle style{
            .kind = kind, .color = core::Color::fromRgba(0x2F, 0x5F, 0xA8), .width = 3.0F};
        f.controller.setToolSettings({.shape = style});
        // The preview is drawn during the drag; nothing is written until release.
        f.pointer(PointerPhase::Down, {100, static_cast<double>(y)});
        f.pointer(PointerPhase::Move, {200, static_cast<double>(y + 40)});
        (void)f.frame();
        EXPECT_FALSE(f.renderer.lastContent.empty());
        EXPECT_EQ(f.renderer.lastContent.back().color, style.color);
        EXPECT_EQ(undoSteps(), before + static_cast<std::size_t>((y - 100) / 100));
        f.pointer(PointerPhase::Up, {200, static_cast<double>(y + 40)});
        y += 100;
    }
    ASSERT_EQ(f.elements().size(), 4U);
    EXPECT_EQ(undoSteps(), before + 4);
    const auto& rect = std::get<document::Shape>(f.element(f.elements()[0]).payload);
    EXPECT_EQ(rect.kind, document::ShapeKind::Rectangle);
    EXPECT_EQ(rect.size, (core::Vec2{100, 40}));
    EXPECT_FLOAT_EQ(rect.strokeWidth, 3.0F);
    const auto& arrow = std::get<document::Shape>(f.element(f.elements()[3]).payload);
    EXPECT_EQ(arrow.kind, document::ShapeKind::Arrow);
    EXPECT_NEAR(arrow.size.x, std::hypot(100.0, 40.0), 1e-3);
    // The preview is gone after release; every shape is an ordinary element.
    (void)f.frame();
    EXPECT_EQ(f.renderer.lastContent.size(), 4U);

    // A click is not a shape; undo/redo work as for any element.
    f.click({500, 500});
    EXPECT_EQ(f.elements().size(), 4U);
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements().size(), 3U);
    ASSERT_OK(f.port.redo());
    EXPECT_EQ(f.elements().size(), 4U);

    // Select (on the outline), move and delete like other content.
    f.controller.setTool(ToolKind::Select);
    f.click({130, 101}); // the rectangle's top edge (away from its handles)
    ASSERT_EQ(f.controller.selection().size(), 1U);
    EXPECT_TRUE(f.controller.selection().contains(f.elements()[0]));
    f.drag({130, 101}, {150, 121}, 4);
    EXPECT_EQ(f.element(f.elements()[0]).transform.position, (core::DVec2{120, 120}));
    ASSERT_OK(f.controller.deleteSelection());
    EXPECT_EQ(f.elements().size(), 3U);
}

TEST(CanvasControllerTest, ShapeStyleIsSanitizedAndDecidesNewShapesOnly) {
    CanvasFixture f;
    f.controller.setToolSettings({.shape = {.kind = static_cast<document::ShapeKind>(4),
                                            .color = core::Color::transparent(),
                                            .width = 1000.0F}});
    EXPECT_EQ(f.controller.toolSettings().shape.kind, document::ShapeKind::Rectangle);
    EXPECT_EQ(f.controller.toolSettings().shape.color.a, 255);
    EXPECT_FLOAT_EQ(f.controller.toolSettings().shape.width, kMaxPenWidth);
    f.controller.setToolSettings({.shape = {.kind = document::ShapeKind::Ellipse}});
    f.controller.setTool(ToolKind::Shape);
    f.drag({100, 100}, {200, 180}, 4);
    ASSERT_EQ(f.elements().size(), 1U);
    const document::Element made = f.element(f.elements()[0]);
    int redraws = 0;
    f.controller.setRedrawCallback([&] { ++redraws; });
    f.controller.setToolSettings({.shape = {.kind = document::ShapeKind::Line, .width = 9.0F}});
    EXPECT_EQ(redraws, 0);
    EXPECT_TRUE(f.element(f.elements()[0]) == made);
}

// ---------------------------------------------------------------------------- text

/// A deterministic text layout: 20 units per line plus padding; opaque rasters.
struct FakeTextLayout final : TextLayout {
    int rasterized = 0;
    float lastPixelsPerUnit = 0.0F;
    float heightFor(std::string_view text, float /*width*/) override {
        return 20.0F * static_cast<float>(std::count(text.begin(), text.end(), '\n') + 1) + 8.0F;
    }
    render::ImageData rasterize(std::string_view /*text*/, const core::Vec2& size,
                                float pixelsPerUnit) override {
        ++rasterized;
        lastPixelsPerUnit = pixelsPerUnit;
        render::ImageData image{
            .width = std::max(1, static_cast<int>(std::ceil(size.x * pixelsPerUnit))),
            .height = std::max(1, static_cast<int>(std::ceil(size.y * pixelsPerUnit))),
            .pixels = {}};
        image.pixels.assign(static_cast<std::size_t>(image.width * image.height) * 4U, 255);
        return image;
    }
};

std::size_t textureItems(const std::vector<render::DrawItem>& items) {
    return static_cast<std::size_t>(
        std::count_if(items.begin(), items.end(),
                      [](const render::DrawItem& item) { return item.texture.isValid(); }));
}

TEST(CanvasControllerTest, TextToolEditsInTheUiAndWritesOneCommandWhenFinished) {
    CanvasFixture f;
    FakeTextLayout layout;
    f.controller.setTextLayout(&layout);
    int notified = 0;
    f.controller.setTextEditHandler([&] { ++notified; });
    f.controller.setTool(ToolKind::Text);
    EXPECT_EQ(f.controller.cursor(), CursorShape::IBeam);
    const std::size_t steps = f.doc.editor.history().undoCount();

    f.click({100, 120});
    ASSERT_TRUE(f.controller.textEdit().has_value());
    EXPECT_EQ(notified, 1);
    EXPECT_FALSE(f.controller.textEdit()->element.has_value());
    EXPECT_EQ(f.controller.textEdit()->position, (core::DVec2{100, 120}));
    EXPECT_FLOAT_EQ(f.controller.textEdit()->width, kDefaultTextWidth);
    EXPECT_TRUE(f.elements().empty()); // nothing is written while editing

    const std::string text = "Hello\nW\xC3\xB6rld \xE2\x9C\x93"; // "Hello\nWörld ✓"
    ASSERT_OK(f.controller.finishTextEdit(text));
    EXPECT_FALSE(f.controller.textEdit().has_value());
    EXPECT_EQ(notified, 2);
    ASSERT_EQ(f.elements().size(), 1U);
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
    const document::Element& made = f.element(f.elements()[0]);
    const auto& box = std::get<document::TextBox>(made.payload);
    EXPECT_EQ(box.text, text);                                   // UTF-8 kept byte for byte
    EXPECT_EQ(box.size, (core::Vec2{kDefaultTextWidth, 48.0F})); // two lines of the fake layout
    EXPECT_EQ(made.transform.position, (core::DVec2{100, 120}));

    // Drawn as one texture, rasterised once; frames that change nothing re-use it.
    (void)f.frame();
    EXPECT_EQ(textureItems(f.renderer.lastContent), 1U);
    EXPECT_EQ(layout.rasterized, 1);
    f.controller.panBy({10, 5});
    (void)f.frame();
    (void)f.frame();
    EXPECT_EQ(layout.rasterized, 1);

    // A blank new box is not created; cancelling writes nothing.
    f.click({400, 400});
    ASSERT_OK(f.controller.finishTextEdit("  \n "));
    f.click({400, 400});
    f.controller.cancelTextEdit();
    EXPECT_EQ(f.elements().size(), 1U);
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
}

TEST(CanvasControllerTest, EditingATextBoxUpdatesItInPlaceAndClearingRemovesIt) {
    CanvasFixture f;
    FakeTextLayout layout;
    f.controller.setTextLayout(&layout);
    f.controller.setTool(ToolKind::Text);
    f.drag({100, 100}, {260, 100}, 4); // a drag sets the width
    ASSERT_TRUE(f.controller.textEdit().has_value());
    EXPECT_FLOAT_EQ(f.controller.textEdit()->width, 160.0F);
    ASSERT_OK(f.controller.finishTextEdit("first"));
    const core::ElementId id = f.elements().at(0);
    (void)f.frame();
    EXPECT_EQ(layout.rasterized, 1);

    // Clicking the box edits it: it is hidden meanwhile, the editor starts with its text.
    f.click({150, 110});
    ASSERT_TRUE(f.controller.textEdit().has_value());
    EXPECT_EQ(f.controller.textEdit()->element, id);
    EXPECT_EQ(f.controller.textEdit()->text, "first");
    (void)f.frame();
    EXPECT_EQ(textureItems(f.renderer.lastContent), 0U);
    ASSERT_OK(f.controller.finishTextEdit("first\nsecond"));
    ASSERT_EQ(f.elements().size(), 1U);
    EXPECT_EQ(f.elements()[0], id); // same element, new text and height
    const auto& box = std::get<document::TextBox>(f.element(id).payload);
    EXPECT_EQ(box.text, "first\nsecond");
    EXPECT_EQ(box.size, (core::Vec2{160.0F, 48.0F}));
    (void)f.frame();
    EXPECT_EQ(layout.rasterized, 2); // changed content is rasterised again

    // Unchanged text writes nothing; clearing the text removes the box.
    const std::size_t steps = f.doc.editor.history().undoCount();
    f.click({150, 110});
    ASSERT_OK(f.controller.finishTextEdit("first\nsecond"));
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps);
    f.click({150, 110});
    ASSERT_OK(f.controller.finishTextEdit(""));
    EXPECT_TRUE(f.elements().empty());
    (void)f.frame();
    EXPECT_TRUE(f.renderer.textures.empty()); // its texture is released
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(std::get<document::TextBox>(f.element(id).payload).text, "first\nsecond");

    // Undoing the box's creation while it is being edited ends the edit.
    f.controller.beginTextEdit({.element = id, .position = {100, 100}, .width = 160, .text = ""});
    ASSERT_OK(f.port.undo()); // the edit "first" -> "first\nsecond"
    ASSERT_OK(f.port.undo()); // the creation
    EXPECT_FALSE(f.controller.textEdit().has_value());
}

TEST(CanvasControllerTest, TextRastersFollowTheZoomWithinABudget) {
    CanvasFixture f;
    FakeTextLayout layout;
    f.controller.setTextLayout(&layout);
    for (int i = 0; i < 6; ++i) {
        auto created = document::commands::createElement(
            f.doc.workspace, f.layer,
            {.transform = {.position = {200.0 + 10.0 * i, 150.0}},
             .payload = document::TextBox{.size = {400, 300}, .text = "t"}},
            f.doc.ids);
        ASSERT_OK(created);
        ASSERT_OK(f.port.execute(std::move(created->command)));
    }
    (void)f.frame();
    EXPECT_EQ(layout.rasterized, 6); // new content: all at once, at zoom 1 (1 px per unit)
    EXPECT_FLOAT_EQ(layout.lastPixelsPerUnit, 1.0F);
    int redraws = 0;
    f.controller.setRedrawCallback([&] { ++redraws; });
    f.controller.zoomBy(1.3); // the next √2 step: 400×300 at 1.41 px per unit ≈ 0.24 Mpx
    redraws = 0;
    (void)f.frame();
    EXPECT_NEAR(layout.lastPixelsPerUnit, std::sqrt(2.0F), 1e-5);
    EXPECT_EQ(f.controller.stats().textRasterizedLastFrame, 4U); // within 1 Mpx
    EXPECT_GT(redraws, 0);                                       // more frames are due
    (void)f.frame();
    EXPECT_EQ(layout.rasterized, 12);
    redraws = 0;
    (void)f.frame();
    EXPECT_EQ(f.controller.stats().textRasterizedLastFrame, 0U);
    EXPECT_EQ(redraws, 0); // then idle again
    // Panning or a zoom within the same step rasterises nothing.
    f.controller.panBy({30, 10});
    f.controller.zoomBy(1.05);
    (void)f.frame();
    EXPECT_EQ(layout.rasterized, 12);
    // Switching pages releases the textures.
    f.controller.setPage(std::nullopt);
    (void)f.frame();
    EXPECT_TRUE(f.renderer.textures.empty());
}

TEST(CanvasControllerTest, TextBoxesKeepPaintersOrderInBatches) {
    CrowdedFixture f(1300);
    FakeTextLayout layout;
    f.controller.setTextLayout(&layout);
    auto created = document::commands::createElement(
        f.doc.workspace, f.layer,
        {.transform = {.position = {100, 100}},
         .payload = document::TextBox{.size = {60, 20}, .text = "note"}},
        f.doc.ids);
    ASSERT_OK(created);
    ASSERT_OK(f.port.execute(std::move(created->command)));
    f.controller.zoomToFit();
    (void)f.frame();
    ASSERT_TRUE(f.controller.stats().batched);
    EXPECT_EQ(textureItems(f.renderer.lastContent), 1U);
    // The text box is last in the draw order, so it is drawn last.
    EXPECT_TRUE(f.renderer.lastContent.back().texture.isValid());
}

// ---------------------------------------------------------------------------- images

/// Decodes square images of the size asked for (up to `fullSide`); some assets "fail".
struct FakeImageSource final : ImageSource {
    void notify() const { notifyReady(); }
    int loads = 0;
    int fullSide = 8192;
    std::vector<core::AssetId> broken;
    std::vector<int> sides;    ///< maxSide of every load
    bool asynchronous = false; ///< the first load of each request is "not ready"
    std::vector<std::pair<core::AssetId, int>> pending;
    std::optional<render::ImageData> load(core::AssetId asset, int maxSide) override {
        if (asynchronous) {
            const std::pair<core::AssetId, int> key{asset, maxSide};
            if (std::find(pending.begin(), pending.end(), key) == pending.end()) {
                pending.push_back(key);
                return std::nullopt;
            }
        }
        ++loads;
        sides.push_back(maxSide);
        if (std::find(broken.begin(), broken.end(), asset) != broken.end()) {
            return render::ImageData{};
        }
        const int side = std::min(maxSide, fullSide);
        render::ImageData image{.width = side, .height = side, .pixels = {}};
        image.pixels.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U,
                            200);
        return image;
    }
};

core::ElementId addImage(CanvasFixture& f, core::AssetId asset, core::DVec2 at, core::Vec2 size) {
    auto created = document::commands::createElement(
        f.doc.workspace, f.layer,
        {.transform = {.position = at}, .payload = document::Image{.asset = asset, .size = size}},
        f.doc.ids);
    EXPECT_TRUE(created.has_value());
    const core::ElementId id = created->id;
    EXPECT_TRUE(f.port.execute(std::move(created->command)).has_value());
    return id;
}

TEST(CanvasControllerTest, ImagesShareOneTexturePerAssetAtTheResolutionTheViewNeeds) {
    CanvasFixture f;
    FakeImageSource images;
    f.controller.setImageSource(&images);
    const core::AssetId photo{f.doc.ids.next()};
    const core::AssetId other{f.doc.ids.next()};
    addImage(f, photo, {50, 50}, {200, 150});
    addImage(f, photo, {300, 50}, {100, 75}); // the same asset again
    addImage(f, other, {50, 300}, {60, 60});
    (void)f.frame();
    EXPECT_EQ(textureItems(f.renderer.lastContent), 3U);
    EXPECT_EQ(f.renderer.textures.size(), 2U); // one texture per asset
    EXPECT_EQ(images.loads, 2);
    EXPECT_EQ(images.sides, (std::vector<int>{256, 64})); // powers of two ≥ the screen size
    // Unchanged frames and panning decode nothing; zooming out keeps the finer texture.
    f.controller.panBy({20, 10});
    f.controller.zoomBy(0.5);
    (void)f.frame();
    (void)f.frame();
    EXPECT_EQ(images.loads, 2);
    // Zooming in past a factor two decodes the finer resolution once (the small image has
    // left the view by now; only what is drawn is decoded).
    f.controller.zoomBy(4.0);
    (void)f.frame();
    EXPECT_EQ(images.loads, 3);
    EXPECT_EQ(images.sides.back(), 512);
    EXPECT_EQ(f.renderer.textures.size(), 2U); // replaced, not added
    EXPECT_EQ(f.controller.stats().imageTextures, 2U);
    // Another page shows other images: the textures are released.
    f.controller.setPage(std::nullopt);
    (void)f.frame();
    EXPECT_TRUE(f.renderer.textures.empty());
}

TEST(CanvasControllerTest, ImagesDecodedElsewhereAppearWhenReady) {
    CanvasFixture f;
    FakeImageSource images;
    images.asynchronous = true;
    f.controller.setImageSource(&images);
    addImage(f, core::AssetId{f.doc.ids.next()}, {300, 200}, {200, 150});
    (void)f.frame(); // not decoded yet: the neutral frame, no stall
    EXPECT_EQ(textureItems(f.renderer.lastContent), 0U);
    EXPECT_EQ(f.renderer.lastContent.size(), 1U);
    int redraws = 0;
    f.controller.setRedrawCallback([&] { ++redraws; });
    images.notify(); // the decode finished: the canvas asks for a frame
    EXPECT_EQ(redraws, 1);
    (void)f.frame();
    EXPECT_EQ(textureItems(f.renderer.lastContent), 1U);
    EXPECT_EQ(images.loads, 1);
}

TEST(CanvasControllerTest, MissingImagesAreDrawnAsAFrameAndNotRetried) {
    CanvasFixture f;
    FakeImageSource images;
    const core::AssetId gone{f.doc.ids.next()};
    images.broken.push_back(gone);
    f.controller.setImageSource(&images);
    addImage(f, gone, {300, 200}, {200, 150});
    (void)f.frame();
    EXPECT_EQ(textureItems(f.renderer.lastContent), 0U);
    ASSERT_EQ(f.renderer.lastContent.size(), 1U); // the neutral frame (a mesh)
    EXPECT_FALSE(f.renderer.lastContent[0].texture.isValid());
    (void)f.frame();
    f.controller.zoomBy(3.0);
    (void)f.frame();
    EXPECT_EQ(images.loads, 1); // not retried
}

TEST(CanvasControllerTest, ImageTexturesStayWithinTheMemoryBudget) {
    CanvasFixture f;
    FakeImageSource images;
    f.controller.setImageSource(&images);
    // Four huge images far apart: each needs a 4096² texture (≈ 85 MB with mipmaps).
    std::vector<core::DVec2> places;
    for (int i = 0; i < 4; ++i) {
        const core::DVec2 at{20000.0 * i, 0.0};
        places.push_back(at);
        addImage(f, core::AssetId{f.doc.ids.next()}, at, {5000, 5000});
    }
    for (const core::DVec2& at : places) {
        f.controller.setView(at + core::DVec2{2500, 2500}, 1.0); // one image in view
        (void)f.frame();
    }
    EXPECT_EQ(images.loads, 4);
    EXPECT_LE(f.controller.stats().imageTextureBytes, kImageTextureBudgetBytes);
    EXPECT_EQ(f.renderer.textures.size(), 3U); // the least recently drawn one was released
    // Coming back decodes it again.
    f.controller.setView(places[0] + core::DVec2{2500, 2500}, 1.0);
    (void)f.frame();
    EXPECT_EQ(images.loads, 5);
}

TEST(CanvasControllerTest, InsertImageFitsTheViewAsOneSelectedElement) {
    CanvasFixture f;
    const core::AssetId asset{f.doc.ids.next()};
    const std::size_t steps = f.doc.editor.history().undoCount();
    ASSERT_OK(f.controller.insertImage(asset, {200, 100}));
    ASSERT_EQ(f.elements().size(), 1U);
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
    const document::Element& small = f.element(f.elements()[0]);
    EXPECT_EQ(std::get<document::Image>(small.payload).size, (core::Vec2{200, 100}));
    EXPECT_EQ(small.transform.position, (core::DVec2{300, 250})); // centred in 800×600
    EXPECT_TRUE(f.controller.selection().contains(f.elements()[0]));
    // A large image is scaled down to 60 % of the view, keeping its aspect ratio.
    ASSERT_OK(f.controller.insertImage(asset, {4000, 1000}));
    const auto& large = std::get<document::Image>(f.element(f.elements()[1]).payload);
    EXPECT_FLOAT_EQ(large.size.x, 480.0F);
    EXPECT_FLOAT_EQ(large.size.y, 120.0F);
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements().size(), 1U);
    EXPECT_FALSE(f.controller.insertImage(asset, {0, 10}).has_value());
}

// ---------------------------------------------------------------------------- connectors

/// Two rectangles to connect: A at (100, 100)-(200, 160), B at (400, 100)-(500, 160).
struct ConnectorFixture : CanvasFixture {
    core::ElementId a;
    core::ElementId b;
    ConnectorFixture() {
        controller.setTool(ToolKind::Shape);
        drag({100, 100}, {200, 160}, 4);
        drag({400, 100}, {500, 160}, 4);
        a = elements().at(0);
        b = elements().at(1);
        controller.setTool(ToolKind::Connector);
    }
    const document::Connector& connector(core::ElementId id) const {
        return std::get<document::Connector>(element(id).payload);
    }
};

TEST(CanvasControllerTest, ConnectorsAttachAtTheEdgesOfTheElementsTheyJoin) {
    ConnectorFixture f;
    const std::size_t steps = f.doc.editor.history().undoCount();
    f.pointer(PointerPhase::Down, {150, 130});
    f.pointer(PointerPhase::Move, {300, 130});
    (void)f.frame();
    EXPECT_EQ(f.elements().size(), 2U); // a preview only
    f.pointer(PointerPhase::Move, {450, 130});
    f.pointer(PointerPhase::Up, {450, 130});
    ASSERT_EQ(f.elements().size(), 3U);
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
    const auto& link = f.connector(f.elements()[2]);
    EXPECT_EQ(link.start.attachedTo, f.a);
    EXPECT_EQ(link.end.attachedTo, f.b);
    // Where the line leaves each box (bounds include half the 2-unit outline).
    EXPECT_NEAR(link.start.position.x, 201.0, 1e-6);
    EXPECT_NEAR(link.start.position.y, 130.0, 1e-6);
    EXPECT_NEAR(link.end.position.x, 399.0, 1e-6);
    // Drawn with an arrowhead at the end.
    const auto parts = buildElementMeshes(f.element(f.elements()[2]), 1.0F);
    ASSERT_EQ(parts.size(), 1U);
    EXPECT_NEAR(parts[0].mesh.bounds.max.x, 198.0, 1e-3); // from the start at 201 to 399
    EXPECT_GT(parts[0].mesh.bounds.max.y, 3.0F);          // the head is wider than the line

    // Free ends where there is nothing to attach to; a click makes nothing.
    f.drag({150, 300}, {450, 350}, 4);
    ASSERT_EQ(f.elements().size(), 4U);
    EXPECT_FALSE(f.connector(f.elements()[3]).start.attachedTo.has_value());
    EXPECT_FALSE(f.connector(f.elements()[3]).end.attachedTo.has_value());
    f.click({600, 500});
    EXPECT_EQ(f.elements().size(), 4U);
}

TEST(CanvasControllerTest, AttachedConnectorsFollowMovesInThePreviewAndTheCommand) {
    ConnectorFixture f;
    f.drag({150, 130}, {450, 130}, 4);
    const core::ElementId link = f.elements().at(2);
    const document::Connector before = f.connector(link);
    f.controller.setTool(ToolKind::Select);
    f.click({130, 100}); // A's top edge (away from its handles)
    ASSERT_TRUE(f.controller.selection().contains(f.a));
    f.pointer(PointerPhase::Down, {130, 100});
    f.pointer(PointerPhase::Move, {130, 150});
    (void)f.frame();
    // The connector is drawn in place with its start moved by the offset (50 down).
    const auto followerItem = std::find_if(
        f.renderer.lastContent.begin(), f.renderer.lastContent.end(),
        [&](const render::DrawItem& item) {
            return f.renderer.meshes.contains(item.mesh.index) &&
                   std::abs(item.transform.ty -
                            static_cast<float>(before.start.position.y + 50.0 -
                                               f.controller.camera().center().y)) < 1e-3F;
        });
    EXPECT_NE(followerItem, f.renderer.lastContent.end());
    f.pointer(PointerPhase::Up, {130, 150});
    const document::Connector& after = f.connector(link);
    EXPECT_EQ(after.start.position, (before.start.position + core::DVec2{0, 50}));
    EXPECT_EQ(after.end.position, before.end.position); // B did not move
    EXPECT_EQ(after.start.attachedTo, f.a);
    ASSERT_OK(f.port.undo());
    EXPECT_TRUE(f.connector(link) == before);
    (void)f.frame(); // the follower's preview mesh is released
    // + the selection outline and the two handle meshes (scratch meshes, kept for re-use)
    EXPECT_EQ(f.renderer.meshes.size(), f.elements().size() + 3U);
}

TEST(CanvasControllerTest, DraggingAConnectorEndDetachesAndReattachesIt) {
    ConnectorFixture f;
    f.drag({150, 130}, {450, 130}, 4);
    const core::ElementId link = f.elements().at(2);
    const core::DVec2 end = f.connector(link).end.position;
    // Grab the end and drop it on empty space: detached there.
    f.drag(end, {450, 400}, 4);
    EXPECT_EQ(f.elements().size(), 3U);
    EXPECT_FALSE(f.connector(link).end.attachedTo.has_value());
    EXPECT_EQ(f.connector(link).end.position, (core::DVec2{450, 400}));
    EXPECT_EQ(f.connector(link).start.attachedTo, f.a);
    // And back onto B: attached again, at B's edge facing A.
    f.drag({450, 400}, {460, 140}, 4);
    EXPECT_EQ(f.connector(link).end.attachedTo, f.b);
    EXPECT_NEAR(f.connector(link).end.position.x, 399.0, 1.0);
    // Deleting B detaches the end (the connector stays).
    f.controller.setTool(ToolKind::Select);
    f.click({450, 100});
    ASSERT_TRUE(f.controller.selection().contains(f.b));
    ASSERT_OK(f.controller.deleteSelection());
    EXPECT_FALSE(f.connector(link).end.attachedTo.has_value());
}

TEST(CanvasControllerTest, MovingAnElementTouchesOnlyItsOwnConnectors) {
    // 500 pairs of boxes, each pair joined: moving one box changes the box and its one
    // connector (from the workspace's attachment index), nothing else.
    CanvasFixture f;
    std::vector<core::ElementId> boxes;
    for (int i = 0; i < 1000; ++i) {
        auto created = document::commands::createElement(
            f.doc.workspace, f.layer,
            {.transform = {.position = {(i % 40) * 30.0, (i / 40) * 30.0}},
             .payload = document::Shape{.size = {10, 10}}},
            f.doc.ids);
        ASSERT_OK(created);
        boxes.push_back(created->id);
        ASSERT_OK(f.port.execute(std::move(created->command)));
    }
    for (int i = 0; i < 1000; i += 2) {
        auto created = document::commands::createElement(
            f.doc.workspace, f.layer,
            {.payload = document::Connector{.start = {.position = {}, .attachedTo = boxes[i]},
                                            .end = {.position = {}, .attachedTo = boxes[i + 1]}}},
            f.doc.ids);
        ASSERT_OK(created);
        ASSERT_OK(f.port.execute(std::move(created->command)));
    }
    EXPECT_EQ(f.doc.workspace.connectorsAttachedTo(boxes[10]).size(), 1U);
    const std::vector<core::ElementId> moved{boxes[10]};
    auto move = document::commands::moveElements(f.doc.workspace, moved, {5, 5});
    ASSERT_OK(move);
    EXPECT_EQ(move->patch.size(), 2U);
    ASSERT_OK(f.doc.workspace.validate());
}

// ---------------------------------------------------------------------------- handles

TEST(CanvasControllerTest, CopyAndPasteDuplicateAMixedSelectionAsOneCommand) {
    ConnectorFixture f;                // rectangles A and B
    f.drag({150, 130}, {450, 130}, 4); // a connector A → B
    f.controller.setTool(ToolKind::Pen);
    f.drag({100, 300}, {300, 320});
    ASSERT_EQ(f.elements().size(), 4U);
    const std::vector<core::ElementId> originals = f.elements();
    f.controller.setTool(ToolKind::Select);

    EXPECT_FALSE(f.controller.canPaste());
    EXPECT_FALSE(f.controller.paste());         // nothing copied yet
    EXPECT_FALSE(f.controller.copySelection()); // nothing selected
    const std::size_t steps = f.doc.editor.history().undoCount();
    f.controller.selectAll();
    ASSERT_TRUE(f.controller.copySelection());
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps); // copying writes nothing
    ASSERT_OK(f.controller.paste());
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
    EXPECT_EQ(f.doc.editor.history().nextUndo()->label, "Paste");
    ASSERT_EQ(f.elements().size(), 8U);
    const std::vector<core::ElementId> all = f.elements();
    const std::vector<core::ElementId> copies(all.begin() + 4, all.end());
    // The copies are selected, drawn above the originals in the same order, 16 view px
    // down and right, and the copied connector joins the copies of A and B.
    EXPECT_EQ(f.controller.selection().size(), 4U);
    for (std::size_t i = 0; i < copies.size(); ++i) {
        EXPECT_TRUE(f.controller.selection().contains(copies[i]));
        EXPECT_EQ(f.element(copies[i]).kind(), f.element(originals[i]).kind());
    }
    EXPECT_EQ(f.element(copies[0]).transform.position,
              (f.element(f.a).transform.position + core::DVec2{16, 16}));
    const auto& link = f.connector(copies[2]);
    EXPECT_EQ(link.start.attachedTo, copies[0]);
    EXPECT_EQ(link.end.attachedTo, copies[1]);
    EXPECT_EQ(link.start.position,
              (f.connector(originals[2]).start.position + core::DVec2{16, 16}));
    (void)f.frame();
    EXPECT_EQ(f.controller.scene().elementCount(), 8U);

    // Pasting again steps further; undo takes back one paste at a time.
    ASSERT_OK(f.controller.paste());
    ASSERT_EQ(f.elements().size(), 12U);
    EXPECT_EQ(f.element(f.elements()[8]).transform.position,
              (f.element(f.a).transform.position + core::DVec2{32, 32}));
    ASSERT_OK(f.port.undo());
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(f.elements(), originals);
    EXPECT_TRUE(f.controller.selection().empty()); // the copies are gone
}

TEST(CanvasControllerTest, CutIsOneStepAndPasteOnAnotherPageKeepsOrCentresThePosition) {
    ConnectorFixture f;
    f.drag({150, 130}, {450, 130}, 4); // a connector A → B
    const core::ElementId link = f.elements().at(2);
    f.controller.setTool(ToolKind::Select);
    f.click({130, 100}); // A only
    ASSERT_TRUE(f.controller.selection().contains(f.a));
    const document::Element a = f.element(f.a);
    const std::size_t steps = f.doc.editor.history().undoCount();
    ASSERT_OK(f.controller.cutSelection());
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
    EXPECT_EQ(f.doc.editor.history().nextUndo()->label, "Cut");
    EXPECT_EQ(f.doc.workspace.findElement(f.a), nullptr);
    EXPECT_FALSE(f.connector(link).start.attachedTo.has_value()); // detached, as a delete does

    // After a cut the first paste is in place; the connector was not copied, so it stays free.
    ASSERT_OK(f.controller.paste());
    const core::ElementId back = f.elements().back();
    EXPECT_NE(back, f.a);
    EXPECT_EQ(f.element(back).transform, a.transform);
    EXPECT_EQ(f.element(back).payload, a.payload);
    EXPECT_FALSE(f.connector(link).start.attachedTo.has_value());

    // On another page the copy keeps its position while that is in view ...
    const auto second = f.doc.addPage(f.doc.workspace.findPage(f.page)->section, "Second");
    f.controller.setPage(second);
    ASSERT_TRUE(f.controller.canPaste()); // the clipboard outlives page switches
    ASSERT_OK(f.controller.paste());
    const auto onSecond = f.doc.workspace.elementsOf(f.doc.firstLayer(second));
    ASSERT_EQ(onSecond.size(), 1U);
    EXPECT_EQ(f.element(onSecond[0]).transform.position, a.transform.position);
    // ... and is centred in the view when it would not be entirely in view there.
    f.controller.panBy({-150, 0}); // A's position is now half out of view
    ASSERT_OK(f.controller.paste());
    const auto more = f.doc.workspace.elementsOf(f.doc.firstLayer(second));
    ASSERT_EQ(more.size(), 2U);
    const core::DRect view = f.controller.camera().visibleWorldRect();
    EXPECT_TRUE(view.contains(visualBounds(f.element(more[1]))));

    // Read-only: nothing is cut or pasted.
    f.port.readOnly = true;
    f.controller.selectAll();
    EXPECT_FALSE(f.controller.cutSelection());
    EXPECT_FALSE(f.controller.paste());
    EXPECT_EQ(f.doc.workspace.elementsOf(f.doc.firstLayer(second)).size(), 2U);
}

TEST(CanvasControllerTest, DraggingAHandleResizesAsOneCommandWithAPreview) {
    ConnectorFixture f;                // A (100,100)-(200,160) and B, both rectangles
    f.drag({150, 130}, {450, 130}, 4); // a connector A → B
    const core::ElementId link = f.elements().at(2);
    f.controller.setTool(ToolKind::Select);
    f.click({130, 100});
    ASSERT_TRUE(f.controller.selection().contains(f.a));
    (void)f.frame();
    // Eight handles are drawn as an overlay (squares and light centres).
    ASSERT_GE(f.renderer.lastOverlay.size(), 3U);
    const std::size_t overlay = f.renderer.lastOverlay.size();
    f.port.readOnly = true; // read-only: no handles (they could not be dragged)
    (void)f.frame();
    EXPECT_EQ(f.renderer.lastOverlay.size(), overlay - 2);
    f.port.readOnly = false;
    (void)f.frame();
    const std::size_t steps = f.doc.editor.history().undoCount();
    const document::Connector before = f.connector(link);

    // Drag the bottom-right handle: the preview shows the new size, nothing is written.
    f.pointer(PointerPhase::Down, {200, 160});
    f.pointer(PointerPhase::Move, {250, 200});
    EXPECT_EQ(f.controller.cursor(), CursorShape::SizeAll);
    (void)f.frame();
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps);
    const auto preview = std::find_if(f.renderer.lastContent.begin(), f.renderer.lastContent.end(),
                                      [&](const render::DrawItem& d) {
                                          const auto it = f.renderer.meshes.find(d.mesh.index);
                                          return it != f.renderer.meshes.end() &&
                                                 it->second.bounds.max.x > 148.0F &&
                                                 it->second.bounds.max.x <
                                                     152.0F; // a 150-wide outline being dragged
                                      });
    EXPECT_NE(preview, f.renderer.lastContent.end());
    f.pointer(PointerPhase::Up, {250, 200});
    EXPECT_EQ(f.doc.editor.history().undoCount(), steps + 1);
    const auto& shape = std::get<document::Shape>(f.element(f.a).payload);
    EXPECT_EQ(shape.size, (core::Vec2{150, 100}));
    EXPECT_EQ(f.element(f.a).transform.position, (core::DVec2{100, 100}));
    // The attached connector end kept its relative place on A's (visual) bounds.
    EXPECT_GT(f.connector(link).start.position.x, before.start.position.x + 40.0);
    ASSERT_OK(f.port.undo());
    EXPECT_EQ(std::get<document::Shape>(f.element(f.a).payload).size, (core::Vec2{100, 60}));
    EXPECT_TRUE(f.connector(link) == before);
}

TEST(CanvasControllerTest, ConnectorAndLineHandlesMoveTheirEnds) {
    ConnectorFixture f;
    f.drag({150, 130}, {450, 130}, 4);
    const core::ElementId link = f.elements().at(2);
    f.controller.setTool(ToolKind::Select);
    f.click({300, 130}); // on the connector
    ASSERT_TRUE(f.controller.selection().contains(link));
    const core::DVec2 end = f.connector(link).end.position;
    f.drag(end, {300, 400}, 4); // off B: detached
    EXPECT_FALSE(f.connector(link).end.attachedTo.has_value());
    EXPECT_EQ(f.connector(link).end.position, (core::DVec2{300, 400}));
    f.drag({300, 400}, {450, 140}, 4); // back onto B: attached at its edge
    EXPECT_EQ(f.connector(link).end.attachedTo, f.b);
    // A line's end handle changes its length and direction.
    f.controller.setTool(ToolKind::Shape);
    f.controller.setToolSettings({.shape = {.kind = document::ShapeKind::Line}});
    f.drag({100, 500}, {300, 500}, 4);
    const core::ElementId line = f.elements().back();
    f.controller.setTool(ToolKind::Select);
    f.click({200, 500});
    ASSERT_TRUE(f.controller.selection().contains(line));
    f.drag({300, 500}, {100, 700}, 4);
    const document::Element& moved = f.element(line);
    EXPECT_EQ(moved.transform.position, (core::DVec2{100, 500}));
    EXPECT_NEAR(std::get<document::Shape>(moved.payload).size.x, 200.0F, 1e-3);
    EXPECT_NEAR(moved.transform.rotation, std::numbers::pi / 2.0, 1e-6);
    // Now at an angle (its direction is its rotation): its handles still work.
    f.drag({100, 700}, {250, 500}, 4);
    const document::Element& again = f.element(line);
    EXPECT_EQ(again.transform.position, (core::DVec2{100, 500}));
    EXPECT_NEAR(std::get<document::Shape>(again.payload).size.x, 150.0F, 1e-3);
    EXPECT_NEAR(again.transform.rotation, 0.0, 1e-6);
    // An arrow drawn diagonally has handles on both ends from the start.
    f.controller.setTool(ToolKind::Shape);
    f.controller.setToolSettings({.shape = {.kind = document::ShapeKind::Arrow}});
    f.drag({500, 500}, {600, 600}, 4);
    const core::ElementId arrow = f.elements().back();
    ASSERT_EQ(handlesFor(f.element(arrow)).size(), 2U);
    f.controller.setTool(ToolKind::Select);
    f.click({550, 550});
    ASSERT_TRUE(f.controller.selection().contains(arrow));
    f.drag({500, 500}, {500, 600}, 4); // the start: the arrow now points right
    EXPECT_EQ(f.element(arrow).transform.position, (core::DVec2{500, 600}));
    EXPECT_NEAR(f.element(arrow).transform.rotation, 0.0, 1e-6);
}

} // namespace
} // namespace studyapp::canvas
