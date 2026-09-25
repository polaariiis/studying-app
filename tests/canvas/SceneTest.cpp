#include "CanvasTestSupport.hpp"

#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/canvas/RenderCache.hpp>
#include <studyapp/canvas/SpatialGrid.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

namespace studyapp::canvas {
namespace {

using document::test::makeConnector;
using document::test::makeStroke;
using document::test::TestWorkspace;

core::ElementId idOf(std::uint64_t n) {
    core::Uuid::Bytes bytes{};
    bytes[15] = static_cast<std::uint8_t>(n & 0xFF);
    bytes[14] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
    bytes[6] = 0x70;
    return core::ElementId{core::Uuid{bytes}};
}

// ---------------------------------------------------------------------------- grid

TEST(SpatialGridTest, QueriesMatchBruteForce) {
    std::mt19937_64 random(11);
    std::uniform_real_distribution<double> position(-5000, 5000);
    std::uniform_real_distribution<double> size(0, 300);
    SpatialGrid grid(512.0, 16);
    std::vector<std::pair<core::ElementId, core::DRect>> all;
    for (std::uint64_t i = 1; i <= 2000; ++i) {
        const core::DVec2 min{position(random), position(random)};
        // A few huge elements exercise the "large" list.
        const double extent = i % 250 == 0 ? 20'000.0 : size(random);
        const core::DRect bounds{min, min + core::DVec2{extent, extent * 0.5}};
        grid.upsert(idOf(i), bounds);
        all.emplace_back(idOf(i), bounds);
    }
    EXPECT_GT(grid.largeCount(), 0U);
    for (int q = 0; q < 200; ++q) {
        const core::DVec2 min{position(random), position(random)};
        const core::DRect query{min, min + core::DVec2{size(random) * 3, size(random) * 2}};
        std::vector<core::ElementId> expected;
        for (const auto& [id, bounds] : all) {
            if (bounds.intersects(query)) {
                expected.push_back(id);
            }
        }
        std::sort(expected.begin(), expected.end());
        std::vector<core::ElementId> actual;
        grid.query(query, actual);
        ASSERT_EQ(actual, expected) << "query " << q;
    }
}

TEST(SpatialGridTest, UpdateAndRemove) {
    SpatialGrid grid(100.0);
    grid.upsert(idOf(1), {{0, 0}, {10, 10}});
    grid.upsert(idOf(1), {{1000, 1000}, {1010, 1010}}); // moved far away
    std::vector<core::ElementId> out;
    grid.query({{-5, -5}, {20, 20}}, out);
    EXPECT_TRUE(out.empty());
    grid.query({{1000, 1000}, {1001, 1001}}, out);
    EXPECT_EQ(out.size(), 1U);
    grid.remove(idOf(1));
    EXPECT_EQ(grid.size(), 0U);
    EXPECT_EQ(grid.cellCount(), 0U);
    // Empty bounds are not indexed; point queries work.
    grid.upsert(idOf(2), core::DRect::emptyBounds());
    EXPECT_EQ(grid.size(), 0U);
    grid.upsert(idOf(3), {{5, 5}, {5, 5}});
    out.clear();
    grid.query({{5, 5}, {5, 5}}, out);
    EXPECT_EQ(out.size(), 1U);
}

TEST(SpatialGridTest, FarAwayCoordinatesDoNotOverflow) {
    SpatialGrid grid;
    const core::DRect far{{1e18, -1e18}, {1e18 + 10, -1e18 + 10}};
    grid.upsert(idOf(1), far);
    std::vector<core::ElementId> out;
    grid.query(far, out);
    EXPECT_EQ(out.size(), 1U);
}

// ---------------------------------------------------------------------------- scene

struct SceneTest : ::testing::Test {
    TestWorkspace doc;
    core::PageId page;
    core::LayerId bottom;
    core::LayerId top;
    CanvasScene scene;

    void SetUp() override {
        const auto notebook = doc.addNotebook("N");
        const auto section = doc.addSection(notebook, "S");
        page = doc.addPage(section, "P");
        bottom = doc.firstLayer(page);
        top = doc.addLayer(page, "Top");
    }

    /// Runs a command and forwards its patch to the scene, as the controller does.
    void run(core::Result<document::Command> command) {
        ASSERT_TRUE(command.has_value()) << command.error().message;
        const document::Patch patch = command->patch;
        ASSERT_OK(doc.editor.execute(std::move(*command)));
        scene.onPatch(doc.workspace, patch);
    }

    core::ElementId add(core::LayerId layer, document::ElementPayload payload,
                        core::DVec2 at = {}) {
        auto created = document::commands::createElement(
            doc.workspace, layer, {.transform = {.position = at}, .payload = std::move(payload)},
            doc.ids);
        EXPECT_TRUE(created.has_value());
        const auto id = created->id;
        run(std::move(created->command));
        return id;
    }
};

TEST_F(SceneTest, DrawOrderFollowsLayersThenZ) {
    scene.rebuild(doc.workspace, page);
    const auto a = add(top, makeStroke(), {0, 0});
    const auto b = add(bottom, makeStroke(), {0, 0});
    const auto c = add(top, makeStroke(), {0, 0});
    const auto d = add(bottom, makeStroke(), {0, 0});
    const std::vector<core::ElementId> expected{b, d, a, c};
    EXPECT_EQ(std::vector<core::ElementId>(scene.drawOrder().begin(), scene.drawOrder().end()),
              expected);
    std::vector<core::ElementId> visible;
    scene.query({{-100, -100}, {100, 100}}, visible);
    EXPECT_EQ(visible, expected); // query results come back in draw order
}

TEST_F(SceneTest, RebuildMatchesIncrementalUpdates) {
    scene.rebuild(doc.workspace, page);
    std::vector<core::ElementId> ids;
    for (int i = 0; i < 20; ++i) {
        ids.push_back(add(i % 2 == 0 ? bottom : top, makeStroke(), {i * 30.0, i * 7.0}));
    }
    run(document::commands::moveElements(doc.workspace, std::vector{ids[3], ids[4]}, {500, 0}));
    run(document::commands::deleteElements(doc.workspace, std::vector{ids[7], ids[8]}));
    CanvasScene rebuilt;
    rebuilt.rebuild(doc.workspace, page);
    EXPECT_EQ(std::vector<core::ElementId>(scene.drawOrder().begin(), scene.drawOrder().end()),
              std::vector<core::ElementId>(rebuilt.drawOrder().begin(), rebuilt.drawOrder().end()));
    for (const core::ElementId id : rebuilt.drawOrder()) {
        ASSERT_NE(scene.find(id), nullptr);
        EXPECT_EQ(scene.find(id)->bounds, rebuilt.find(id)->bounds);
    }
    EXPECT_EQ(scene.find(ids[7]), nullptr);
}

TEST_F(SceneTest, VersionsChangeOnlyWithThePayload) {
    scene.rebuild(doc.workspace, page);
    const auto id = add(bottom, makeStroke());
    const auto version = scene.find(id)->contentVersion;
    run(document::commands::moveElements(doc.workspace, std::vector{id}, {10, 10}));
    EXPECT_EQ(scene.find(id)->contentVersion, version); // a move reuses the mesh
    EXPECT_EQ(scene.find(id)->bounds, document::worldBounds(*doc.workspace.findElement(id)));
    const document::Element before = *doc.workspace.findElement(id);
    document::Element after = before;
    std::get<document::Stroke>(after.payload).color = core::Color::white();
    run(document::Command{"restyle", document::Patch({document::updated(before, after)})});
    EXPECT_NE(scene.find(id)->contentVersion, version);
}

TEST_F(SceneTest, HiddenLayersAreNotQueriedAndRemovalsAreReported) {
    scene.rebuild(doc.workspace, page);
    const auto hidden = add(top, makeStroke());
    const auto shown = add(bottom, makeStroke());
    const document::Layer before = *doc.workspace.findLayer(top);
    document::Layer after = before;
    after.visible = false;
    run(document::Command{"hide", document::Patch({document::updated(before, after)})});
    std::vector<core::ElementId> visible;
    scene.query({{-100, -100}, {100, 100}}, visible);
    EXPECT_EQ(visible, (std::vector<core::ElementId>{shown}));
    ASSERT_NE(scene.find(hidden), nullptr);
    EXPECT_FALSE(scene.find(hidden)->layerVisible);

    (void)scene.takeRemoved();
    run(document::commands::deleteElement(doc.workspace, shown));
    EXPECT_EQ(scene.takeRemoved(), (std::vector<core::ElementId>{shown}));
    EXPECT_TRUE(scene.takeRemoved().empty());
}

TEST_F(SceneTest, IgnoresOtherPagesAndClearsWhenThePageGoes) {
    scene.rebuild(doc.workspace, page);
    const auto section = doc.workspace.findPage(page)->section;
    const auto other = doc.addPage(section, "Other");
    add(doc.firstLayer(other), makeStroke());
    EXPECT_EQ(scene.elementCount(), 0U);
    add(bottom, makeStroke());
    EXPECT_EQ(scene.elementCount(), 1U);
    run(document::commands::deletePage(doc.workspace, page));
    EXPECT_FALSE(scene.page().has_value());
    EXPECT_EQ(scene.elementCount(), 0U);
}

// ---------------------------------------------------------------------------- render cache

TEST_F(SceneTest, RenderCacheRebuildsOnlyForNewVersionsOrFinerDetail) {
    scene.rebuild(doc.workspace, page);
    test::RecordingRenderer renderer;
    RenderCache cache;
    const auto id = add(bottom, makeStroke());
    const document::Element& element = *doc.workspace.findElement(id);

    cache.beginFrame();
    const auto& first = cache.ensure(element, 1, 0, &renderer);
    EXPECT_EQ(cache.stats().builtLastFrame, 1U);
    ASSERT_EQ(first.gpu.size(), 1U);
    const render::MeshHandle handle = first.gpu[0];

    cache.beginFrame();
    (void)cache.ensure(element, 1, -2, &renderer); // coarser detail: reuse
    EXPECT_EQ(cache.stats().builtLastFrame, 0U);
    (void)cache.ensure(element, 1, 3, &renderer); // finer detail: rebuild
    EXPECT_EQ(cache.stats().builtLastFrame, 1U);
    (void)cache.ensure(element, 2, 3, &renderer); // new content version: rebuild
    EXPECT_EQ(cache.stats().builtLastFrame, 2U);
    cache.flush(renderer);
    EXPECT_FALSE(renderer.meshes.contains(handle.index)); // replaced meshes are destroyed

    cache.evict(id);
    cache.flush(renderer);
    EXPECT_TRUE(renderer.meshes.empty()); // nothing kept alive for a deleted element
    EXPECT_EQ(cache.stats().entries, 0U);
    EXPECT_EQ(cache.stats().cpuBytes, 0U);
}

TEST_F(SceneTest, RenderCacheSpreadsRefinementOverFramesButNeverDelaysContent) {
    scene.rebuild(doc.workspace, page);
    test::RecordingRenderer renderer;
    RenderCache cache;
    cache.setRefinementBudget(2);
    std::vector<core::ElementId> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(add(bottom, makeStroke(), {i * 20.0, 0.0}));
    }
    const auto ensureAll = [&](int lod) {
        for (const core::ElementId id : ids) {
            (void)cache.ensure(*doc.workspace.findElement(id), 1, lod, &renderer);
        }
    };
    cache.beginFrame();
    ensureAll(0); // new content is built regardless of the budget
    EXPECT_EQ(cache.stats().builtLastFrame, 5U);
    EXPECT_FALSE(cache.refinementPending());

    // Zooming in two buckets: two refinements per frame, the rest keep their coarse mesh.
    int frames = 0;
    std::uint32_t refined = 0;
    do {
        cache.beginFrame();
        ensureAll(2);
        EXPECT_LE(cache.stats().refinedLastFrame, 2U);
        refined += cache.stats().refinedLastFrame;
        ++frames;
    } while (cache.refinementPending() && frames < 10);
    EXPECT_EQ(refined, 5U);
    EXPECT_EQ(frames, 3); // 2 + 2 + 1, and the third frame reports nothing pending
    cache.beginFrame();
    ensureAll(2);
    EXPECT_EQ(cache.stats().builtLastFrame, 0U); // all at the finer detail now
    EXPECT_FALSE(cache.refinementPending());

    // A content change is never deferred, even with the budget used up.
    cache.setRefinementBudget(0);
    cache.beginFrame();
    (void)cache.ensure(*doc.workspace.findElement(ids[0]), 2, 2, &renderer);
    EXPECT_EQ(cache.stats().builtLastFrame, 1U);
    ensureAll(3);
    EXPECT_TRUE(cache.refinementPending());
    EXPECT_EQ(cache.stats().refinedLastFrame, 0U);
    for (const core::ElementId id : ids) { // coarse, but every element still has a mesh
        const auto& entry = cache.ensure(*doc.workspace.findElement(id), 1, 3, &renderer);
        EXPECT_FALSE(entry.gpu.empty());
    }
}

TEST(RenderCacheTest, LodBuckets) {
    EXPECT_EQ(RenderCache::lodBucketFor(1.0), 0);
    EXPECT_EQ(RenderCache::lodBucketFor(1.5), 1);
    EXPECT_EQ(RenderCache::lodBucketFor(0.3), -1);
    EXPECT_EQ(RenderCache::lodBucketFor(1e9), 8);
    EXPECT_FLOAT_EQ(RenderCache::pixelsPerUnit(3), 8.0F);
}

// ---------------------------------------------------------------------------- geometry

TEST(ElementGeometryTest, StrokeHitTestUsesTheInk) {
    document::Element stroke{.id = idOf(1),
                             .layer = {},
                             .z = core::FractionalIndex::first(),
                             .transform = {.position = {100, 100}},
                             .locked = false,
                             .payload = makeStroke({{0, 0, 1}, {100, 0, 1}})};
    // baseWidth 2: radius 1 at full pressure.
    EXPECT_TRUE(hitTest(stroke, {150, 100}, 0.0));
    EXPECT_TRUE(hitTest(stroke, {150, 101.5}, 1.0)); // near miss within tolerance
    EXPECT_FALSE(hitTest(stroke, {150, 103}, 1.0));  // miss
    EXPECT_FALSE(hitTest(stroke, {150, 50}, 4.0));   // inside the bounds' row but far away

    // Transformed: scaled ×2 and rotated a quarter turn.
    stroke.transform = {.position = {0, 0}, .rotation = 1.5707964F, .scale = {2, 2}};
    EXPECT_TRUE(hitTest(stroke, {0, 150}, 0.5));
    EXPECT_FALSE(hitTest(stroke, {150, 0}, 0.5));
}

TEST(ElementGeometryTest, RectangleAndEraserPredicates) {
    const document::Element stroke{.id = idOf(1),
                                   .layer = {},
                                   .z = core::FractionalIndex::first(),
                                   .transform = {},
                                   .locked = false,
                                   .payload = makeStroke({{0, 0, 1}, {100, 100, 1}})};
    EXPECT_TRUE(intersectsRect(stroke, {{40, 40}, {60, 60}}));
    // The bounds overlap, but the diagonal ink misses this corner rectangle.
    EXPECT_FALSE(intersectsRect(stroke, {{80, 0}, {100, 20}}));
    EXPECT_TRUE(strokeTouchesSegment(stroke, {0, 100}, {100, 0}, 0.1));   // crossing path
    EXPECT_FALSE(strokeTouchesSegment(stroke, {0, 100}, {20, 100}, 5.0)); // far path
    EXPECT_TRUE(strokeTouchesSegment(stroke, {50, 56}, {50, 56}, 5.0));   // eraser disc
}

TEST(ElementGeometryTest, EveryKindProducesDrawableMeshes) {
    const auto make = [](document::ElementPayload payload) {
        return document::Element{.id = idOf(1),
                                 .layer = {},
                                 .z = core::FractionalIndex::first(),
                                 .transform = {},
                                 .locked = false,
                                 .payload = std::move(payload)};
    };
    EXPECT_EQ(buildElementMeshes(make(makeStroke()), 1.0F).size(), 1U);
    const document::Shape filled{.kind = document::ShapeKind::Ellipse,
                                 .size = {20, 10},
                                 .strokeColor = core::Color::black(),
                                 .strokeWidth = 1,
                                 .fillColor = core::Color::white()};
    EXPECT_EQ(buildElementMeshes(make(filled), 1.0F).size(), 2U); // fill + outline
    EXPECT_EQ(
        buildElementMeshes(make(document::TextBox{.size = {50, 20}, .text = "x"}), 1.0F).size(),
        1U);
    EXPECT_EQ(buildElementMeshes(make(makeConnector(std::nullopt, std::nullopt)), 1.0F).size(), 1U);
    for (const auto& part : buildElementMeshes(make(filled), 4.0F)) {
        EXPECT_FALSE(part.mesh.empty());
    }
    const document::Element connector = make(makeConnector(std::nullopt, std::nullopt));
    EXPECT_EQ(meshToWorld(connector).apply({0, 0}), (core::DVec2{0, 0}));
    EXPECT_TRUE(hitTest(connector, {50, 0.5}, 0.0));
}

} // namespace
} // namespace studyapp::canvas
