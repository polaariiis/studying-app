// Application-level benchmarks (Phase 9): the user-facing operations measured end to end
// through the real session, persistence and Qt adapters — opening a workspace, search,
// planner edits, image import, text rasterisation, PDF inspection and tiles, export and
// bundles. Not run by CTest; build with STUDYAPP_BUILD_BENCHMARKS=ON (Release) and run
// studyapp_app_benchmarks. Runs on the offscreen Qt platform (no window, no GPU).

#include "PageExport.hpp"
#include "SessionDocumentRasterizer.hpp"
#include "SyntheticPage.hpp"

#include <studyapp/application/Planner.hpp>
#include <studyapp/application/Search.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/platform/QtTextLayout.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <QGuiApplication>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

#include <benchmark/benchmark.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace studyapp::bench {
namespace {

class NoLocks final : public application::WorkspaceLocker {
public:
    core::Result<application::LockStatus> inspect(const std::filesystem::path&) override {
        return application::LockStatus{};
    }
    core::Result<std::unique_ptr<application::WorkspaceLock>> acquire(const std::filesystem::path&,
                                                                      bool) override {
        return std::unique_ptr<application::WorkspaceLock>(std::make_unique<Held>());
    }

private:
    class Held final : public application::WorkspaceLock {};
};

/// A workspace on disk opened through a real session.
struct Fixture {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    NoLocks locker;
    std::unique_ptr<application::WorkspaceSession> session;
    core::NotebookId notebook;
    core::PageId page;
    core::LayerId layer;

    application::SessionServices services() { return {clock, ids, locker}; }
    std::filesystem::path root() const { return dir / "bench.studyws"; }

    Fixture() {
        session = std::move(*application::WorkspaceSession::create(root(), "Bench", services()));
        application::WorkspaceStructure structure(*session, clock, ids);
        auto created = structure.createNotebook("Notes");
        notebook = created->notebook;
        page = created->page;
        layer = session->workspace().layersOf(page).front();
    }

    /// Adds elements as one command (one transaction), built against a scratch copy.
    void addAll(const std::vector<document::ElementPayload>& payloads,
                const std::vector<core::DVec2>& positions) {
        document::Workspace scratch = session->workspace();
        document::Patch patch;
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            auto created = document::commands::createElement(
                scratch, layer, {.transform = {.position = positions[i]}, .payload = payloads[i]},
                ids);
            (void)scratch.apply(created->command.patch);
            for (const auto& change : created->command.patch.changes()) {
                patch.add(change);
            }
        }
        (void)session->execute(document::Command{"bulk", std::move(patch)});
    }

    void addStrokes(int count) {
        std::mt19937 random(20260930);
        std::uniform_real_distribution<double> position(0.0, 6000.0);
        std::vector<document::ElementPayload> payloads;
        std::vector<core::DVec2> positions;
        for (int i = 0; i < count; ++i) {
            payloads.emplace_back(
                document::Stroke{.baseWidth = 2.0F,
                                 .points = document::makeStrokePoints(handwritingStroke(random))});
            positions.push_back({position(random), position(random) * 0.6});
        }
        addAll(payloads, positions);
    }

    void addTextBoxes(int count) {
        static const char* words[] = {"membrane", "osmosis",  "diffusion", "mitochondria",
                                      "enzyme",   "catalyst", "gradient",  "photosynthesis"};
        std::vector<document::ElementPayload> payloads;
        std::vector<core::DVec2> positions;
        for (int i = 0; i < count; ++i) {
            std::string text =
                "Note " + std::to_string(i) + ": " + words[i % 8] + " and " + words[(i / 8) % 8];
            payloads.emplace_back(document::TextBox{.size = {240, 48}, .text = std::move(text)});
            positions.push_back({(i % 100) * 250.0, (i / 100) * 60.0});
        }
        addAll(payloads, positions);
    }
};

void writeFile(const std::filesystem::path& file, const std::vector<char>& bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path writePdf(const std::filesystem::path& file, int pages) {
    QPdfWriter writer(QString::fromStdU16String(file.u16string()));
    writer.setResolution(72);
    writer.setPageSize(QPageSize(QPageSize::Letter));
    QPainter painter(&writer);
    for (int p = 0; p < pages; ++p) {
        if (p > 0) {
            writer.newPage();
        }
        for (int i = 0; i < 60; ++i) {
            painter.drawLine(QPointF(40, 40 + i * 12), QPointF(560, 44 + i * 12));
        }
    }
    painter.end();
    return file;
}

// ---------------------------------------------------------------------------- workspace

void BM_OpenWorkspace(benchmark::State& state) {
    Fixture f;
    f.addStrokes(static_cast<int>(state.range(0)));
    (void)f.session->close();
    f.session.reset();
    for (auto _ : state) {
        auto opened = application::WorkspaceSession::open(f.root(), {}, f.services());
        benchmark::DoNotOptimize(opened->get()->workspace().elementCount());
        (void)(*opened)->close();
    }
}
BENCHMARK(BM_OpenWorkspace)->Arg(10'000)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------- search

void BM_Search(benchmark::State& state) {
    Fixture f;
    f.addTextBoxes(static_cast<int>(state.range(0)));
    std::size_t hits = 0;
    for (auto _ : state) {
        auto results = application::search(*f.session, "photo", 50);
        hits = results->size();
        benchmark::DoNotOptimize(hits);
    }
    state.counters["results"] = static_cast<double>(hits);
}
BENCHMARK(BM_Search)->Arg(1'000)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------- planner

void BM_PlannerCreateTask(benchmark::State& state) {
    Fixture f;
    application::Planner planner(*f.session, f.clock, f.ids);
    for (int i = 0; i < state.range(0); ++i) {
        (void)planner.createTask({.title = "Task " + std::to_string(i)});
    }
    int n = 0;
    for (auto _ : state) {
        auto created = planner.createTask({.title = "Measured " + std::to_string(++n)});
        benchmark::DoNotOptimize(created);
    }
}
BENCHMARK(BM_PlannerCreateTask)->Arg(100)->Arg(10'000)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------- assets

void BM_ImportImage(benchmark::State& state) {
    Fixture f;
    const int side = static_cast<int>(state.range(0));
    QImage image(side, side, QImage::Format_RGB32);
    std::mt19937 random(7);
    for (int y = 0; y < side; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < side; ++x) {
            line[x] = static_cast<QRgb>(random()); // incompressible: a photo-sized file
        }
    }
    const auto base = f.dir / "image.png";
    (void)image.save(QString::fromStdU16String(base.u16string()));
    std::ifstream in(base, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    int n = 0;
    for (auto _ : state) {
        state.PauseTiming();
        bytes.back() = static_cast<char>(++n); // new content each time: no dedupe
        const auto file = f.dir / ("image" + std::to_string(n) + ".png");
        writeFile(file, bytes);
        state.ResumeTiming();
        benchmark::DoNotOptimize(f.session->importAsset(file, "image/png"));
    }
    state.counters["MB"] = static_cast<double>(bytes.size()) / 1e6;
}
BENCHMARK(BM_ImportImage)->Arg(512)->Arg(4096)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------- text

void BM_TextRaster(benchmark::State& state) {
    platform::QtTextLayout layout;
    const float pixelsPerUnit = static_cast<float>(state.range(0)) / 10.0F;
    const std::string text = "Osmosis: water moves across a semipermeable membrane\n"
                             "from low to high solute concentration.";
    for (auto _ : state) {
        benchmark::DoNotOptimize(layout.rasterize(text, {240.0F, 64.0F}, pixelsPerUnit));
    }
}
BENCHMARK(BM_TextRaster)->Arg(10)->Arg(42)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------- PDF

void BM_PdfInspect(benchmark::State& state) {
    testing::TempDirectory dir;
    const auto file = writePdf(dir / "doc.pdf", static_cast<int>(state.range(0)));
    for (auto _ : state) {
        benchmark::DoNotOptimize(ui::inspectPdf(file));
    }
}
BENCHMARK(BM_PdfInspect)->Arg(200)->Unit(benchmark::kMillisecond);

void BM_PdfTile(benchmark::State& state) {
    Fixture f;
    const auto file = writePdf(f.dir / "doc.pdf", 20);
    const auto asset = *f.session->importAsset(file, "application/pdf");
    ui::SessionDocumentRasterizer rasterizer(*f.session);
    const int level = static_cast<int>(state.range(0));
    int page = 0;
    for (auto _ : state) {
        const canvas::DocumentTileKey key{.asset = asset, .page = page++ % 20, .level = level};
        (void)rasterizer.tile(key);
        rasterizer.waitForTiles();
        benchmark::DoNotOptimize(rasterizer.tile(key));
    }
}
BENCHMARK(BM_PdfTile)->Arg(-1)->Arg(1)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------- export

void BM_ExportPage(benchmark::State& state) {
    Fixture f;
    f.addStrokes(static_cast<int>(state.range(0)));
    const auto format = static_cast<ui::ExportFormat>(state.range(1));
    const char* extension[] = {".pdf", ".png", ".svg"};
    const auto target = f.dir / (std::string("page") + extension[state.range(1)]);
    const ui::ExportSources sources{
        .assetPath = [&](core::AssetId asset) -> std::optional<std::filesystem::path> {
            auto path = f.session->assetPath(asset);
            return path ? std::optional(*path) : std::nullopt;
        },
        .progress = {}};
    const std::vector<core::PageId> pages{f.page};
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            ui::exportPages(f.session->workspace(), pages, target, format, sources));
    }
    state.counters["MB"] = static_cast<double>(std::filesystem::file_size(target)) / 1e6;
}
BENCHMARK(BM_ExportPage)
    ->ArgsProduct({{100, 10'000}, {0, 1, 2}})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);

/// A bounded A4 page of 1 000 strokes (a dense handwritten page): the common case.
void BM_ExportA4Page(benchmark::State& state) {
    Fixture f;
    {
        auto format = document::commands::setPageFormat(
            f.session->workspace(), f.page,
            {.extent = document::PageExtent::Bounded,
             .size = document::kA4PortraitSize,
             .background = f.session->workspace().findPage(f.page)->background},
            f.clock);
        (void)f.session->execute(std::move(*format));
    }
    std::mt19937 random(11);
    std::uniform_real_distribution<double> x(20.0, 740.0);
    std::uniform_real_distribution<double> y(20.0, 1080.0);
    std::vector<document::ElementPayload> payloads;
    std::vector<core::DVec2> positions;
    for (int i = 0; i < 1000; ++i) {
        payloads.emplace_back(document::Stroke{
            .baseWidth = 2.0F, .points = document::makeStrokePoints(handwritingStroke(random))});
        positions.push_back({x(random), y(random)});
    }
    f.addAll(payloads, positions);
    const auto format = static_cast<ui::ExportFormat>(state.range(0));
    const char* extension[] = {".pdf", ".png", ".svg"};
    const auto target = f.dir / (std::string("a4") + extension[state.range(0)]);
    const ui::ExportSources sources{.assetPath = {}, .progress = {}};
    const std::vector<core::PageId> pages{f.page};
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            ui::exportPages(f.session->workspace(), pages, target, format, sources));
    }
    state.counters["MB"] = static_cast<double>(std::filesystem::file_size(target)) / 1e6;
}
BENCHMARK(BM_ExportA4Page)->Arg(0)->Arg(1)->Arg(2)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------- bundles

void BM_Bundle(benchmark::State& state) {
    Fixture f;
    f.addStrokes(10'000);
    const auto bundle = f.dir / "all.studybundle";
    const bool import = state.range(0) != 0;
    (void)f.session->exportBundle(bundle, f.notebook);
    for (auto _ : state) {
        if (import) {
            benchmark::DoNotOptimize(f.session->importBundle(bundle));
        } else {
            benchmark::DoNotOptimize(f.session->exportBundle(bundle, f.notebook));
        }
    }
}
BENCHMARK(BM_Bundle)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond)->Iterations(3);

} // namespace
} // namespace studyapp::bench

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
