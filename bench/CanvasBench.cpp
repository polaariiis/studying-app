// Phase 4 canvas benchmarks (docs/CANVAS.md §3.1, §11; docs/RENDERING.md §10): stroke
// tessellation, spatial queries, hit testing, scene construction and the CPU side of a
// frame (culling, draw order, cached meshes) on a 10 000-stroke page. The GPU side is
// measured in the running application (`studyapp --bench-pan`).

#include "SyntheticPage.hpp"

#include <studyapp/canvas/CanvasController.hpp>
#include <studyapp/canvas/CanvasScene.hpp>
#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/canvas/ToolSettings.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/render/Tessellation.hpp>
#include <studyapp/testing/RecordingRenderer.hpp>

#include <algorithm>
#include <benchmark/benchmark.h>
#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

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

/// As BM_BuildFramePanning with every stroke selected (Select All): the selection overlay
/// is part of every frame.
void BM_BuildFramePanningAllSelected(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    canvas::CanvasController controller(port, ids);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.zoomBy(static_cast<double>(state.range(0)) / 100.0);
    controller.selectAll();
    (void)controller.buildFrame(renderer);
    double direction = 1.0;
    int frame = 0;
    for (auto _ : state) {
        if (++frame % 120 == 0) {
            direction = -direction;
        }
        controller.panBy({12.0 * direction, 4.0 * direction});
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.overlay.data());
    }
    state.counters["selected"] = static_cast<double>(controller.selection().size());
}
BENCHMARK(BM_BuildFramePanningAllSelected)->Arg(100)->Arg(15)->Unit(benchmark::kMicrosecond);

/// Wheel zooming with the whole page in view: each step of ×1.2 in, then back out. Zooming
/// in past a level-of-detail bucket needs finer meshes for the visible strokes.
void BM_BuildFrameZoomingWholePage(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    std::vector<double> frameMs;
    for (auto _ : state) {
        // One zoom-in / zoom-out gesture per iteration, 8 steps each way, starting from a
        // page whose meshes were built at the whole-page zoom.
        state.PauseTiming();
        auto controller = std::make_unique<canvas::CanvasController>(port, ids);
        testing::RecordingRenderer renderer;
        (void)renderer.initialize();
        controller->setViewport({1920, 1080}, 1.0);
        controller->setPage(page.page);
        controller->zoomBy(0.15);
        (void)controller->buildFrame(renderer);
        state.ResumeTiming();
        for (int step = 0; step < 16; ++step) {
            controller->zoomBy(step < 8 ? 1.2 : 1.0 / 1.2);
            const auto start = std::chrono::steady_clock::now();
            const render::RenderFrame built = controller->buildFrame(renderer);
            benchmark::DoNotOptimize(built.content.data());
            frameMs.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
    }
    std::sort(frameMs.begin(), frameMs.end());
    state.counters["worstFrameMs"] = frameMs.back();
    state.counters["medianFrameMs"] = frameMs[frameMs.size() / 2];
}
BENCHMARK(BM_BuildFrameZoomingWholePage)->Unit(benchmark::kMillisecond)->Iterations(3);

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
        controller.profiler().reset();
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
        for (const auto& section : controller.profiler().sections()) {
            state.counters["ms:" + std::string(section.name)] =
                std::chrono::duration<double, std::milli>(section.total).count();
        }
        // Memory of the meshes handed to the renderer (batches: per-vertex colours and
        // part numbers included).
        std::uint64_t bytes = 0;
        std::uint64_t partBytes = 0;
        for (const auto& [index, mesh] : renderer.meshes) {
            bytes += mesh.byteSize();
            partBytes += mesh.parts.size() * sizeof(std::uint32_t);
        }
        state.counters["meshMB"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
        state.counters["partMB"] = static_cast<double>(partBytes) / (1024.0 * 1024.0);
    }
}
BENCHMARK(BM_FirstFrameWholePage)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------- ink (Phase 6)

/// Writable port over a document::Editor, reporting every patch to the controller.
class EditPort final : public canvas::DocumentPort {
public:
    explicit EditPort(document::Workspace& workspace) : editor_(workspace) {}
    void attach(canvas::CanvasController& controller) { controller_ = &controller; }
    const document::Workspace& workspace() const override { return editor_.workspace(); }
    core::Result<void> execute(document::Command command) override {
        const document::Patch patch = command.patch;
        const auto start = std::chrono::steady_clock::now();
        if (auto done = editor_.execute(std::move(command)); !done) {
            return done;
        }
        const auto applied = std::chrono::steady_clock::now();
        if (controller_ != nullptr) {
            controller_->onDocumentChanged(patch);
        }
        applyUs += std::chrono::duration<double, std::micro>(applied - start).count();
        notifyUs +=
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - applied)
                .count();
        ++executed;
        return {};
    }
    double applyUs = 0.0;  ///< editor (workspace apply + history), summed
    double notifyUs = 0.0; ///< controller's scene update, summed
    int executed = 0;
    bool isReadOnly() const override { return false; }
    /// Undoes the last edit (benchmarks keep their page unchanged across iterations).
    void undo() {
        const document::Command* last = editor_.history().nextUndo();
        if (last == nullptr) {
            return;
        }
        const document::Patch inverse = last->patch.inverted();
        if (editor_.undo() && controller_ != nullptr) {
            controller_->onDocumentChanged(inverse);
        }
    }

private:
    document::Editor editor_;
    canvas::CanvasController* controller_ = nullptr;
};

double percentile(std::vector<double> values, double q) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1,
                           static_cast<std::size_t>(q * static_cast<double>(values.size())))];
}

/// One freehand gesture on a 10 000-stroke page at zoom 1, with a frame after every pointer
/// sample (the worst case; the widget coalesces samples into display-paced frames), then
/// the commit (command → editor → scene update) and the frame after it. Each iteration
/// adds one stroke. Args: tool (0 pen, 1 highlighter), samples per gesture.
void BM_InkGesture(benchmark::State& state) {
    static SyntheticPage page(kStrokes);
    EditPort port(page.workspace);
    canvas::CanvasController controller(port, page.ids); // ids continue the page's
    port.attach(controller);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.setTool(state.range(0) == 1 ? canvas::ToolKind::Highlighter : canvas::ToolKind::Pen);
    (void)controller.buildFrame(renderer);
    const auto samples = static_cast<int>(state.range(1));
    std::vector<double> sampleUs;
    std::vector<double> commitUs;
    std::map<std::string, double> commitSections;
    std::uint64_t t = 0;
    for (auto _ : state) {
        const auto at = [&](int i) {
            return core::DVec2{200.0 + 1400.0 * i / samples, 500.0 + 60.0 * std::sin(i * 0.05)};
        };
        controller.onPointer(
            {.phase = canvas::PointerPhase::Down, .viewPos = at(0), .timestampUs = t});
        for (int i = 1; i <= samples; ++i) {
            t += 4'167; // 240 Hz
            const auto start = std::chrono::steady_clock::now();
            controller.onPointer(
                {.phase = canvas::PointerPhase::Move, .viewPos = at(i), .timestampUs = t});
            const render::RenderFrame built = controller.buildFrame(renderer);
            benchmark::DoNotOptimize(built.content.data());
            sampleUs.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        controller.profiler().reset();
        const auto start = std::chrono::steady_clock::now();
        controller.onPointer(
            {.phase = canvas::PointerPhase::Up, .viewPos = at(samples), .timestampUs = t});
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
        commitUs.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count());
        for (const auto& section : controller.profiler().sections()) {
            commitSections[std::string(section.name)] +=
                std::chrono::duration<double, std::micro>(section.total).count();
        }
    }
    for (const auto& [name, us] : commitSections) {
        state.counters["commitUs:" + name] = us / static_cast<double>(state.iterations());
    }
    state.counters["sampleFrameUs_p50"] = percentile(sampleUs, 0.5);
    state.counters["sampleFrameUs_p99"] = percentile(sampleUs, 0.99);
    state.counters["sampleFrameUs_max"] = percentile(sampleUs, 1.0);
    state.counters["commitFrameUs_p50"] = percentile(commitUs, 0.5);
    state.counters["commitFrameUs_max"] = percentile(commitUs, 1.0);
    state.counters["commitFailed"] = controller.lastError().has_value() ? 1.0 : 0.0;
    state.counters["applyUs"] = port.applyUs / std::max(port.executed, 1);
    state.counters["sceneUs"] = port.notifyUs / std::max(port.executed, 1);
}
BENCHMARK(BM_InkGesture)
    ->Args({0, 240})
    ->Args({1, 240})
    ->Args({1, 2000})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(20);

/// BM_BuildFramePanning on a page where every 10th stroke is a highlighter stroke.
void BM_BuildFramePanningWithHighlighter(benchmark::State& state) {
    static const SyntheticPage page(kStrokes, 10);
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    canvas::CanvasController controller(port, ids);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.zoomBy(static_cast<double>(state.range(0)) / 100.0);
    (void)controller.buildFrame(renderer);
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
BENCHMARK(BM_BuildFramePanningWithHighlighter)->Arg(100)->Arg(15)->Unit(benchmark::kMicrosecond);

/// Switching between pen and highlighter and changing their settings with the whole
/// 10 000-stroke page in view (batched), plus the frame the tool change requests. Nothing
/// may be re-tessellated or re-batched: the counters must stay 0. Arg 0 draws the same
/// frames without any change (the baseline), arg 1 changes tool and settings every frame.
void BM_ToolAndSettingsChange(benchmark::State& state) {
    const SyntheticPage& page = page10k();
    ViewPort port(page.workspace);
    testing::SequentialIds ids;
    canvas::CanvasController controller(port, ids);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.zoomBy(0.15);
    (void)controller.buildFrame(renderer);
    std::uint64_t built = 0;
    std::uint64_t rebuiltBatches = 0;
    bool highlighter = false;
    for (auto _ : state) {
        if (state.range(0) == 0) {
            const render::RenderFrame frame = controller.buildFrame(renderer);
            benchmark::DoNotOptimize(frame.content.data());
            continue;
        }
        highlighter = !highlighter;
        canvas::ToolSettings settings = controller.toolSettings();
        settings.highlighter.width = highlighter ? 22.0F : 14.0F;
        settings.pen.width = highlighter ? 4.0F : 2.0F;
        controller.setToolSettings(settings);
        controller.setTool(highlighter ? canvas::ToolKind::Highlighter : canvas::ToolKind::Pen);
        const render::RenderFrame frame = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(frame.content.data());
        built += controller.stats().cache.builtLastFrame;
        rebuiltBatches += controller.stats().batchesRebuilt;
    }
    state.counters["meshesBuilt"] = static_cast<double>(built);
    state.counters["batchesRebuilt"] = static_cast<double>(rebuiltBatches);
}
BENCHMARK(BM_ToolAndSettingsChange)->Arg(0)->Arg(1)->Unit(benchmark::kMicrosecond);

/// A partial-eraser gesture (Phase 6, step 3) across a dense page: `samples` pointer
/// steps with a frame after each (the worst case), then the commit (one splitStrokes
/// patch) and its frame; undone after every iteration so the page stays the same.
/// Args: page (0: the 10 000-stroke page at zoom 1, 1: 300 long strokes of 3 000 points,
/// 2: the 10 000 strokes in writing order, as real notes are), samples.
void BM_PartialEraseGesture(benchmark::State& state) {
    static SyntheticPage dense(kStrokes);
    static SyntheticPage longStrokes(0);
    static const bool filled = [] {
        SyntheticPage& page = longStrokes;
        for (int k = 0; k < 300; ++k) {
            std::vector<document::StrokePoint> points;
            points.reserve(3000);
            for (int i = 0; i < 3000; ++i) {
                points.push_back({static_cast<float>(i) * 0.6F,
                                  static_cast<float>(20.0 * std::sin(i * 0.02 + k)), 1.0F});
            }
            auto created = document::commands::createElement(
                page.workspace, page.layer,
                {.transform = {.position = {0.0, 60.0 + 3.0 * k}},
                 .payload =
                     document::Stroke{.baseWidth = 2.0F,
                                      .points = document::makeStrokePoints(std::move(points))}},
                page.ids);
            (void)page.workspace.apply(created->command.patch);
        }
        return true;
    }();
    (void)filled;
    static SyntheticPage ordered(kStrokes, 0, true);
    SyntheticPage& page = state.range(0) == 0 ? dense : state.range(0) == 1 ? longStrokes : ordered;
    EditPort port(page.workspace);
    canvas::CanvasController controller(port, page.ids); // ids continue the page's
    port.attach(controller);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.setTool(canvas::ToolKind::Eraser);
    (void)controller.buildFrame(renderer);
    const auto samples = static_cast<int>(state.range(1));
    std::vector<double> sampleUs;
    std::vector<double> commitUs;
    std::uint64_t t = 0;
    std::size_t changes = 0;
    std::map<std::string, double> commitSections;
    std::map<std::string, double> sampleSections;
    for (auto _ : state) {
        const auto at = [&](int i) {
            return core::DVec2{300.0 + 1300.0 * i / samples, 80.0 + 900.0 * i / samples};
        };
        controller.profiler().reset();
        controller.onPointer(
            {.phase = canvas::PointerPhase::Down, .viewPos = at(0), .timestampUs = t});
        for (int i = 1; i <= samples; ++i) {
            t += 4'167;
            const auto start = std::chrono::steady_clock::now();
            controller.onPointer(
                {.phase = canvas::PointerPhase::Move, .viewPos = at(i), .timestampUs = t});
            const render::RenderFrame built = controller.buildFrame(renderer);
            benchmark::DoNotOptimize(built.content.data());
            sampleUs.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        for (const auto& section : controller.profiler().sections()) {
            sampleSections[std::string(section.name)] +=
                std::chrono::duration<double, std::micro>(section.total).count() / samples;
        }
        controller.profiler().reset();
        const auto start = std::chrono::steady_clock::now();
        controller.onPointer(
            {.phase = canvas::PointerPhase::Up, .viewPos = at(samples), .timestampUs = t});
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
        commitUs.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count());
        state.PauseTiming();
        for (const auto& section : controller.profiler().sections()) {
            commitSections[std::string(section.name)] +=
                std::chrono::duration<double, std::micro>(section.total).count();
        }
        changes = port.workspace().elementCount();
        port.undo();
        (void)controller.buildFrame(renderer);
        state.ResumeTiming();
    }
    state.counters["sampleFrameUs_p50"] = percentile(sampleUs, 0.5);
    state.counters["sampleFrameUs_p99"] = percentile(sampleUs, 0.99);
    state.counters["sampleFrameUs_max"] = percentile(sampleUs, 1.0);
    state.counters["commitFrameUs_p50"] = percentile(commitUs, 0.5);
    state.counters["commitFrameUs_max"] = percentile(commitUs, 1.0);
    state.counters["elementsAfterErase"] = static_cast<double>(changes);
    state.counters["applyUs"] = port.applyUs / std::max(port.executed, 1);
    state.counters["sceneUs"] = port.notifyUs / std::max(port.executed, 1);
    state.counters["commitFailed"] = controller.lastError().has_value() ? 1.0 : 0.0;
    for (const auto& [name, us] : commitSections) {
        state.counters["commitUs:" + name] = us / static_cast<double>(state.iterations());
    }
    for (const auto& [name, us] : sampleSections) {
        state.counters["sampleUs:" + name] = us / static_cast<double>(state.iterations());
    }
}
BENCHMARK(BM_PartialEraseGesture)
    ->Args({0, 120})
    ->Args({1, 120})
    ->Args({2, 120})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(10);

/// Copy and paste (Phase 6, step 9) on the 10 000-stroke page: copying the selection,
/// pasting it (command → editor → scene update) and the frame after the paste; undone after
/// every iteration. Arg: 0 selects the strokes in the view, 1 the whole page.
void BM_CopyPaste(benchmark::State& state) {
    static SyntheticPage page(kStrokes);
    EditPort port(page.workspace);
    canvas::CanvasController controller(port, page.ids); // ids continue the page's
    port.attach(controller);
    testing::RecordingRenderer renderer;
    (void)renderer.initialize();
    controller.setViewport({1920, 1080}, 1.0);
    controller.setPage(page.page);
    controller.setTool(canvas::ToolKind::Select);
    (void)controller.buildFrame(renderer);
    std::vector<double> copyUs;
    std::vector<double> pasteUs;
    std::vector<double> frameUs;
    std::size_t selected = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (state.range(0) == 1) {
            controller.selectAll();
        } else {
            controller.onPointer({.phase = canvas::PointerPhase::Down, .viewPos = {0, 0}});
            controller.onPointer({.phase = canvas::PointerPhase::Move, .viewPos = {1920, 1080}});
            controller.onPointer({.phase = canvas::PointerPhase::Up, .viewPos = {1920, 1080}});
        }
        selected = controller.selection().size();
        state.ResumeTiming();
        const auto start = std::chrono::steady_clock::now();
        (void)controller.copySelection();
        const auto copied = std::chrono::steady_clock::now();
        (void)controller.paste();
        const auto pasted = std::chrono::steady_clock::now();
        const render::RenderFrame built = controller.buildFrame(renderer);
        benchmark::DoNotOptimize(built.content.data());
        const auto framed = std::chrono::steady_clock::now();
        copyUs.push_back(std::chrono::duration<double, std::micro>(copied - start).count());
        pasteUs.push_back(std::chrono::duration<double, std::micro>(pasted - copied).count());
        frameUs.push_back(std::chrono::duration<double, std::micro>(framed - pasted).count());
        state.PauseTiming();
        port.undo();
        controller.clearSelection();
        (void)controller.buildFrame(renderer);
        state.ResumeTiming();
    }
    state.counters["selected"] = static_cast<double>(selected);
    state.counters["copyUs_p50"] = percentile(copyUs, 0.5);
    state.counters["pasteUs_p50"] = percentile(pasteUs, 0.5);
    state.counters["pasteUs_max"] = percentile(pasteUs, 1.0);
    state.counters["frameAfterUs_p50"] = percentile(frameUs, 0.5);
    state.counters["applyUs"] = port.applyUs / std::max(port.executed, 1);
    state.counters["sceneUs"] = port.notifyUs / std::max(port.executed, 1);
    state.counters["pasteFailed"] = controller.lastError().has_value() ? 1.0 : 0.0;
}
BENCHMARK(BM_CopyPaste)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond)->Iterations(10);

} // namespace
} // namespace studyapp::bench
