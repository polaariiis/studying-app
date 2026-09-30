// Stress benchmarks (docs/STRESS_TESTING.md, docs/CAPACITY.md): the canvas at growing
// stroke counts — 1 000 to 100 000 handwriting strokes on one page of the same area
// (SyntheticPage: ≈ 6000 × 3600 world units, so larger counts are also denser). Measures
// the first frame of the whole page, panning frames at zoom 1 and with the whole page in
// view, panning with everything selected, the meshes' memory and the process working set.
// CPU side only (RecordingRenderer); GPU cost is measured in the application.

#include "ProcessMemory.hpp"
#include "SyntheticPage.hpp"

#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/testing/RecordingRenderer.hpp>

#include <algorithm>
#include <benchmark/benchmark.h>
#include <chrono>
#include <memory>
#include <vector>

namespace studyapp::bench {
namespace {

/// Only the page of the current size is kept, so memory counters see one page at a time.
const SyntheticPage& pageOf(int strokes) {
    static std::unique_ptr<SyntheticPage> page;
    static int size = 0;
    if (!page || size != strokes) {
        page.reset();
        page = std::make_unique<SyntheticPage>(strokes);
        size = strokes;
    }
    return *page;
}

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

double meshMB(const testing::RecordingRenderer& renderer) {
    std::uint64_t bytes = 0;
    for (const auto& [index, mesh] : renderer.meshes) {
        bytes += mesh.byteSize();
    }
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

/// First frame with the whole page in view: every stroke tessellated and batched.
void BM_StressFirstFrame(benchmark::State& state) {
    const SyntheticPage& page = pageOf(static_cast<int>(state.range(0)));
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    double mesh = 0.0;
    double workingSet = 0.0;
    for (auto _ : state) {
        canvas::CanvasController controller(port, ids);
        testing::RecordingRenderer renderer;
        (void)renderer.initialize();
        controller.setViewport({1920, 1080}, 1.0);
        controller.setPage(page.page);
        controller.zoomBy(0.15);
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
        mesh = meshMB(renderer);
        workingSet = std::max(workingSet, workingSetMB());
    }
    state.counters["meshMB"] = mesh;
    state.counters["workingSetMB"] = workingSet;
}
BENCHMARK(BM_StressFirstFrame)
    ->Arg(1'000)
    ->Arg(5'000)
    ->Arg(10'000)
    ->Arg(25'000)
    ->Arg(50'000)
    ->Arg(100'000)
    ->Unit(benchmark::kMillisecond);

/// Frames while panning with warm caches; args: strokes, zoom in percent (100: about 1/40
/// of the page in view; 15: the whole page), all selected (0/1). Reports the median, 95th
/// and 99th percentile and the worst frame of the timed frames.
void BM_StressPanFrames(benchmark::State& state) {
    const SyntheticPage& page = pageOf(static_cast<int>(state.range(0)));
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    canvas::CanvasController controller(port, ids);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.zoomBy(static_cast<double>(state.range(1)) / 100.0);
    if (state.range(2) != 0) {
        controller.selectAll();
    }
    (void)controller.buildFrame(renderer); // warm the render cache
    std::vector<double> frameMs;
    double direction = 1.0;
    int frame = 0;
    for (auto _ : state) {
        if (++frame % 120 == 0) {
            direction = -direction;
        }
        const auto start = std::chrono::steady_clock::now();
        controller.panBy({12.0 * direction, 4.0 * direction});
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
        frameMs.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }
    std::sort(frameMs.begin(), frameMs.end());
    const auto at = [&](double quantile) {
        return frameMs[std::min(
            frameMs.size() - 1,
            static_cast<std::size_t>(quantile * static_cast<double>(frameMs.size())))];
    };
    state.counters["p50ms"] = at(0.50);
    state.counters["p95ms"] = at(0.95);
    state.counters["p99ms"] = at(0.99);
    state.counters["worstMs"] = frameMs.back();
    state.counters["drawItems"] = static_cast<double>(controller.stats().drawItems);
    state.counters["workingSetMB"] = workingSetMB();
}
BENCHMARK(BM_StressPanFrames)
    ->ArgsProduct({{1'000, 5'000, 10'000, 25'000, 50'000, 100'000}, {100, 15}, {0, 1}})
    ->Unit(benchmark::kMicrosecond);

} // namespace
} // namespace studyapp::bench
