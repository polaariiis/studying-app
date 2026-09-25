// Phase 4 canvas benchmarks (docs/CANVAS.md §3.1, §11; docs/RENDERING.md §10): stroke
// tessellation, spatial queries, hit testing, scene construction and the CPU side of a
// frame (culling, draw order, cached meshes) on a 10 000-stroke page. The GPU side is
// measured in the running application (`studyapp --bench-pan`).

#include "SyntheticPage.hpp"

#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/render/Tessellation.hpp>
#include <studyapp/testing/RecordingRenderer.hpp>

#include <benchmark/benchmark.h>
#include <memory>
#include <random>

namespace studyapp::bench {
namespace {

constexpr int kStrokes = 10'000;

const SyntheticPage& page10k() {
    static const SyntheticPage page(kStrokes);
    return page;
}

/// Read-only port: the benchmarks only look at the document.
class ViewPort final : public canvas::DocumentPort {
public:
    explicit ViewPort(const document::Workspace& workspace) : workspace_(&workspace) {}
    const document::Workspace& workspace() const override { return *workspace_; }
    core::Result<void> execute(document::Command) override {
        return core::makeError(core::ErrorCode::Unsupported, "read-only benchmark");
    }
    bool isReadOnly() const override { return true; }

private:
    const document::Workspace* workspace_;
};

// ---------------------------------------------------------------------------- tessellation

void BM_TessellateStroke(benchmark::State& state) {
    std::mt19937 random(1);
    const document::Element element{
        .id = {},
        .layer = {},
        .z = core::FractionalIndex::first(),
        .transform = {},
        .locked = false,
        .payload = document::Stroke{
            .baseWidth = 2.0F, .points = document::makeStrokePoints(handwritingStroke(random))}};
    const auto points = std::get<document::Stroke>(element.payload).points->size();
    for (auto _ : state) {
        auto meshes = canvas::buildElementMeshes(element, 1.0F);
        benchmark::DoNotOptimize(meshes);
    }
    state.counters["points"] = static_cast<double>(points);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(points));
}
BENCHMARK(BM_TessellateStroke);

void BM_TessellatePageAllStrokes(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    for (auto _ : state) {
        std::size_t triangles = 0;
        for (const core::ElementId id : page.workspace.elementsOf(page.layer)) {
            for (const auto& part :
                 canvas::buildElementMeshes(*page.workspace.findElement(id), 1.0F)) {
                triangles += part.mesh.triangleCount();
            }
        }
        benchmark::DoNotOptimize(triangles);
        state.counters["triangles"] = static_cast<double>(triangles);
    }
}
BENCHMARK(BM_TessellatePageAllStrokes)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------- scene

void BM_SceneRebuild(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    for (auto _ : state) {
        canvas::CanvasScene scene;
        scene.rebuild(page.workspace, page.page);
        benchmark::DoNotOptimize(scene.elementCount());
    }
}
BENCHMARK(BM_SceneRebuild)->Unit(benchmark::kMillisecond);

void BM_VisibleQuery(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    canvas::CanvasScene scene;
    scene.rebuild(page.workspace, page.page);
    // A 1920×1080 viewport at zoom state.range(0)/100.
    const double zoom = static_cast<double>(state.range(0)) / 100.0;
    const core::DVec2 half{960.0 / zoom, 540.0 / zoom};
    std::vector<core::ElementId> visible;
    std::mt19937 random(2);
    std::uniform_real_distribution<double> centre(0.0, 3600.0);
    for (auto _ : state) {
        const core::DVec2 c{centre(random) * 1.6, centre(random)};
        visible.clear();
        scene.query({c - half, c + half}, visible);
        benchmark::DoNotOptimize(visible.data());
    }
    state.counters["visible"] = static_cast<double>(visible.size());
}
BENCHMARK(BM_VisibleQuery)->Arg(25)->Arg(100)->Arg(400);

void BM_HitTestTopmost(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    canvas::CanvasScene scene;
    scene.rebuild(page.workspace, page.page);
    std::mt19937 random(3);
    std::uniform_real_distribution<double> position(0.0, 6000.0);
    std::vector<core::ElementId> candidates;
    std::size_t hits = 0;
    for (auto _ : state) {
        const core::DVec2 p{position(random), position(random) * 0.6};
        candidates.clear();
        scene.query(core::DRect{p, p}.expanded(4.0), candidates);
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if (canvas::hitTest(*page.workspace.findElement(*it), p, 4.0)) {
                ++hits;
                break;
            }
        }
    }
    benchmark::DoNotOptimize(hits);
}
BENCHMARK(BM_HitTestTopmost);

// ---------------------------------------------------------------------------- frames

/// CPU cost of one frame (cull + order + draw items) with warm caches, while panning.
void BM_BuildFramePanning(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    canvas::CanvasController controller(port, ids);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.zoomBy(static_cast<double>(state.range(0)) / 100.0);
    (void)controller.buildFrame(renderer); // warm the render cache
    double direction = 1.0;
    int frame = 0;
    for (auto _ : state) {
        if (++frame % 120 == 0) {
            direction = -direction;
        }
        controller.panBy({12.0 * direction, 4.0 * direction});
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
    }
    state.counters["drawItems"] = static_cast<double>(controller.stats().drawItems);
}
// Zoom 1 (≈ 200 strokes visible), zoomed out to 0.3 (≈ 30 %), and the whole page (0.15).
BENCHMARK(BM_BuildFramePanning)->Arg(100)->Arg(30)->Arg(15)->Unit(benchmark::kMicrosecond);

/// First frame of a freshly opened 10 000-stroke page: every visible stroke tessellated.
void BM_FirstFrameWholePage(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    for (auto _ : state) {
        canvas::CanvasController controller(port, ids);
        testing::RecordingRenderer renderer;
        (void)renderer.initialize();
        controller.setViewport({1920, 1080}, 1.0);
        controller.setPage(page.page);
        controller.zoomBy(0.15);
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
    }
}
BENCHMARK(BM_FirstFrameWholePage)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace studyapp::bench
