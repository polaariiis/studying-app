#include <studyapp/canvas/Camera.hpp>

#include <gtest/gtest.h>

#include <random>

namespace studyapp::canvas {
namespace {

void expectNear(const core::DVec2& actual, const core::DVec2& expected, double tolerance) {
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
}

Camera makeCamera(core::DVec2 center, double zoom, core::DVec2 viewport = {800, 600},
                  double dpr = 1.0) {
    Camera camera;
    camera.setViewport(viewport, dpr);
    camera.setZoom(zoom);
    camera.setCenter(center);
    return camera;
}

TEST(CameraTest, CentreMapsToViewportCentre) {
    const Camera camera = makeCamera({100, 50}, 2.0);
    expectNear(camera.worldToView({100, 50}), {400, 300}, 1e-12);
    expectNear(camera.worldToView({110, 50}), {420, 300}, 1e-12); // 10 units at zoom 2
    expectNear(camera.viewToWorld({0, 0}), {-100, -100}, 1e-12);
}

TEST(CameraTest, WorldViewRoundTripAcrossZoomAndDistance) {
    std::mt19937_64 random(42);
    std::uniform_real_distribution<double> coordinate(-1e7, 1e7);
    std::uniform_real_distribution<double> zoomLog(-5.6, 6.0);
    for (int i = 0; i < 500; ++i) {
        const Camera camera =
            makeCamera({coordinate(random), coordinate(random)}, std::exp2(zoomLog(random)));
        const core::DVec2 world{coordinate(random), coordinate(random)};
        expectNear(camera.viewToWorld(camera.worldToView(world)), world, 1e-6);
        const core::DVec2 view{coordinate(random) * 1e-4, coordinate(random) * 1e-4};
        expectNear(camera.worldToView(camera.viewToWorld(view)), view, 1e-6);
    }
}

TEST(CameraTest, TransformMatchesPointFunctions) {
    const Camera camera = makeCamera({-3000.5, 42.25}, 0.37);
    const core::Affine2 transform = camera.worldToViewTransform();
    for (const core::DVec2 world :
         {core::DVec2{0, 0}, core::DVec2{-2999, 40}, core::DVec2{1e5, -7}}) {
        expectNear(transform.apply(world), camera.worldToView(world), 1e-9);
    }
}

TEST(CameraTest, CursorCentredZoomKeepsTheWorldPointUnderTheCursor) {
    Camera camera = makeCamera({250, -80}, 1.0);
    const core::DVec2 cursor{613.5, 97.25};
    const core::DVec2 before = camera.viewToWorld(cursor);
    camera.zoomAt(cursor, 1.2);
    expectNear(camera.viewToWorld(cursor), before, 1e-9);
    EXPECT_NEAR(camera.zoom(), 1.2, 1e-12);
    camera.zoomAt(cursor, 1.0 / 7.0);
    expectNear(camera.viewToWorld(cursor), before, 1e-9);
    // Clamped zoom still keeps the anchor.
    camera.zoomAt(cursor, 1e9);
    EXPECT_DOUBLE_EQ(camera.zoom(), Camera::kMaxZoom);
    expectNear(camera.viewToWorld(cursor), before, 1e-9);
    camera.zoomAt(cursor, 1e-9);
    EXPECT_DOUBLE_EQ(camera.zoom(), Camera::kMinZoom);
    expectNear(camera.viewToWorld(cursor), before, 1e-6);
}

TEST(CameraTest, PanMovesContentWithThePointer) {
    Camera camera = makeCamera({0, 0}, 4.0);
    const core::DVec2 world{10, 20};
    const core::DVec2 viewBefore = camera.worldToView(world);
    camera.panBy({30, -12});
    expectNear(camera.worldToView(world), viewBefore + core::DVec2{30, -12}, 1e-12);
}

TEST(CameraTest, PanComposesWithZoom) {
    Camera camera = makeCamera({0, 0}, 1.0);
    camera.zoomAt({100, 100}, 3.0);
    camera.panBy({50, 50});
    const core::DVec2 cursor{321, 123};
    const core::DVec2 world = camera.viewToWorld(cursor);
    camera.zoomAt(cursor, 0.5);
    expectNear(camera.viewToWorld(cursor), world, 1e-9);
    expectNear(camera.worldToView(world), cursor, 1e-9);
}

TEST(CameraTest, ViewportChangesKeepTheCentre) {
    Camera camera = makeCamera({500, 400}, 2.0);
    camera.setViewport({1920, 1080}, 1.5);
    expectNear(camera.worldToView({500, 400}), {960, 540}, 1e-12);
    EXPECT_DOUBLE_EQ(camera.devicePixelRatio(), 1.5);
    expectNear(camera.deviceSize(), {2880, 1620}, 1e-12);
    // Device coordinates are view coordinates scaled by the pixel ratio.
    expectNear(camera.viewToDevice({10, 20}), {15, 30}, 1e-12);
    expectNear(camera.deviceToView(camera.viewToDevice({10.25, 7.5})), {10.25, 7.5}, 1e-12);
    // Invalid sizes are ignored.
    camera.setViewport({0, -3}, 0.0);
    expectNear(camera.viewportSize(), {1920, 1080}, 0.0);
    EXPECT_DOUBLE_EQ(camera.devicePixelRatio(), 1.5);
}

TEST(CameraTest, VisibleRectCoversTheViewport) {
    const Camera camera = makeCamera({100, 100}, 2.0, {800, 600});
    const core::DRect visible = camera.visibleWorldRect();
    expectNear(visible.min, camera.viewToWorld({0, 0}), 1e-12);
    expectNear(visible.max, camera.viewToWorld({800, 600}), 1e-12);
    EXPECT_NEAR(visible.width(), 400.0, 1e-12);
}

TEST(CameraTest, FitRectAndReset) {
    Camera camera = makeCamera({0, 0}, 1.0, {1000, 800});
    camera.fitRect(core::DRect{{0, 0}, {794, 1123}}, 32.0);
    EXPECT_NEAR(camera.zoom(), (800.0 - 64.0) / 1123.0, 1e-12);
    expectNear(camera.center(), {397, 561.5}, 1e-12);
    camera.reset();
    EXPECT_DOUBLE_EQ(camera.zoom(), 1.0);
    expectNear(camera.worldToView({0, 0}), {0, 0}, 1e-12); // origin at the top-left
    expectNear(camera.worldToView({37, 11}), {37, 11}, 1e-12);
}

TEST(CameraTest, RejectsNonFiniteInput) {
    Camera camera = makeCamera({1, 2}, 3.0);
    camera.setCenter({std::nan(""), 0});
    camera.setZoom(std::numeric_limits<double>::infinity());
    camera.zoomAt({0, 0}, -2.0);
    expectNear(camera.center(), {1, 2}, 0.0);
    EXPECT_DOUBLE_EQ(camera.zoom(), 3.0);
}

} // namespace
} // namespace studyapp::canvas
