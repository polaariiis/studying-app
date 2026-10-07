// Workspace maintenance on large workspaces (1.2-PERF-01, backlog V-07; docs/PERFORMANCE.md
// §3.11): Check Workspace and its two parts, Back Up Now, the daily backup when a session
// closes, bundle export / extract / import and Save a Copy, on deterministic ~1 GB and ~5 GB
// workspaces. These operations run on the GUI thread today, synchronously (MainWindow), so
// the time measured here is the time the window does not respond.
//
// The workspaces are generated once into STUDYAPP_MAINTENANCE_DIR (default: the system temp
// directory + "/studyapp-maintenance-bench"), outside the repository, and reused; delete the
// directory to free the space (≈ 6 GB plus the largest operation's output). Run each
// benchmark in its own process (--benchmark_filter) so that its peak-memory counter is its
// own; docs/BENCHMARKS.md lists the commands.

#include "ProcessMemory.hpp"
#include "SyntheticPage.hpp"

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/persistence/AssetStore.hpp>
#include <studyapp/persistence/Backups.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Sha256.hpp>
#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/persistence/Zip.hpp>

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/resource.h>
#endif

namespace studyapp::bench {
namespace {

namespace fs = std::filesystem;

/// Pages of handwriting, image elements and imported PDFs; asset bytes are pseudo-random
/// (incompressible, like the JPEG and PDF data they stand for). 1 GB → 5 GB scales every
/// count by five.
struct Spec {
    int gigabytes = 1;
    int pages = 20;
    int strokesPerPage = 2'000;
    int images = 120;
    int imageMiB = 5;
    int pdfs = 16;
    int pdfMiB = 20;
    int pdfPages = 12;
};

Spec specFor(int gigabytes) {
    Spec spec;
    spec.gigabytes = gigabytes;
    spec.pages *= gigabytes;
    spec.images *= gigabytes;
    spec.pdfs *= gigabytes;
    return spec;
}

double peakMemoryMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0); // bytes
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0; // KiB
#endif
#endif
}

class NoLocks final : public application::WorkspaceLocker {
public:
    core::Result<application::LockStatus> inspect(const fs::path&) override {
        return application::LockStatus{};
    }
    core::Result<std::unique_ptr<application::WorkspaceLock>> acquire(const fs::path&,
                                                                      bool) override {
        return std::unique_ptr<application::WorkspaceLock>(std::make_unique<Held>());
    }

private:
    class Held final : public application::WorkspaceLock {};
};

struct Services {
    core::SystemClock clock;
    core::UuidV7Generator ids{clock};
    NoLocks locker;
    application::SessionServices get() { return {clock, ids, locker}; }
};

fs::path benchRoot() {
#ifdef _WIN32
    wchar_t* chosen = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&chosen, &length, L"STUDYAPP_MAINTENANCE_DIR") == 0 && chosen != nullptr) {
        const fs::path path(chosen);
        std::free(chosen);
        return path;
    }
#else
    if (const char* chosen = std::getenv("STUDYAPP_MAINTENANCE_DIR"); chosen != nullptr) {
        return fs::path(chosen);
    }
#endif
    return fs::temp_directory_path() / "studyapp-maintenance-bench";
}

fs::path fixtureRoot(const Spec& spec) {
    return benchRoot() / ("ws-" + std::to_string(spec.gigabytes) + "gb.studyws");
}

std::uint64_t bytesIn(const fs::path& path, std::uint64_t* files = nullptr) {
    std::uint64_t total = 0;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        if (files != nullptr) {
            ++*files;
        }
        return fs::file_size(path, ec);
    }
    for (const auto& entry : fs::recursive_directory_iterator(path, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
            if (files != nullptr) {
                ++*files;
            }
        }
    }
    return total;
}

void writeRandomFile(const fs::path& file, std::uint64_t bytes, std::uint64_t seed,
                     std::string_view header = {}) {
    std::mt19937_64 random(seed);
    std::ofstream out(file, std::ios::binary);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::vector<std::uint64_t> chunk(1 << 16);
    std::uint64_t written = header.size();
    while (written < bytes) {
        for (auto& word : chunk) {
            word = random();
        }
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes - written, chunk.size() * sizeof(std::uint64_t)));
        out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n));
        written += n;
    }
}

/// Generates the workspace once; afterwards only checks that it is there.
void ensureFixture(const Spec& spec) {
    const fs::path root = fixtureRoot(spec);
    const fs::path done = root.string() + ".complete";
    if (fs::exists(done)) {
        return;
    }
    fs::remove_all(root);
    fs::create_directories(benchRoot());
    Services services;
    auto session = std::move(*application::WorkspaceSession::create(
        root, std::to_string(spec.gigabytes) + " GB benchmark", services.get()));
    application::WorkspaceStructure structure(*session, services.clock, services.ids);
    const auto created = *structure.createNotebook("Biology");
    const fs::path sources = benchRoot() / "sources";
    fs::create_directories(sources);
    const int imagesPerPage = spec.images / spec.pages;
    std::uint64_t seed = std::uint64_t{1000} * static_cast<std::uint64_t>(spec.gigabytes);
    for (int p = 0; p < spec.pages; ++p) {
        const core::PageId page = p == 0 ? created.page : *structure.createPage(created.section);
        const core::LayerId layer = session->workspace().layersOf(page).front();
        std::mt19937 random(static_cast<unsigned>(20260930 + p));
        std::uniform_real_distribution<double> position(0.0, 6000.0);
        document::Workspace scratch = session->workspace();
        document::Patch patch;
        const auto add = [&](document::ElementPayload payload, core::DVec2 at) {
            auto made = document::commands::createElement(
                scratch, layer, {.transform = {.position = at}, .payload = std::move(payload)},
                services.ids);
            (void)scratch.apply(made->command.patch);
            for (const auto& change : made->command.patch.changes()) {
                patch.add(change);
            }
        };
        for (int s = 0; s < spec.strokesPerPage; ++s) {
            add(document::Stroke{.baseWidth = 2.0F,
                                 .points = document::makeStrokePoints(handwritingStroke(random))},
                {position(random), position(random) * 0.6});
        }
        for (int i = 0; i < imagesPerPage; ++i) {
            const fs::path file = sources / "image.jpg";
            writeRandomFile(file, static_cast<std::uint64_t>(spec.imageMiB) << 20U, ++seed,
                            "\xFF\xD8\xFF\xE0");
            auto asset = session->importAsset(file, "image/jpeg");
            add(document::Image{.asset = *asset, .size = {1600, 1200}},
                {200.0 + 1700.0 * i, 4000.0});
        }
        (void)session->execute(document::Command{"benchmark content", std::move(patch)});
    }
    const std::vector<core::DVec2> a4(static_cast<std::size_t>(spec.pdfPages), {816.0, 1056.0});
    for (int d = 0; d < spec.pdfs; ++d) {
        const fs::path file = sources / "document.pdf";
        writeRandomFile(file, static_cast<std::uint64_t>(spec.pdfMiB) << 20U, ++seed, "%PDF-1.7\n");
        auto asset = session->importAsset(file, "application/pdf");
        (void)structure.importDocument(created.notebook, "Reading " + std::to_string(d + 1), *asset,
                                       a4);
    }
    (void)session->close(); // writes the first automatic backup, as a real session would
    fs::remove_all(sources);
    std::ofstream(done) << "complete\n";
}

/// Characteristics of the generated workspace, as counters (MB, counts).
void describe(benchmark::State& state, const Spec& spec) {
    const fs::path root = fixtureRoot(spec);
    std::uint64_t assetFiles = 0;
    std::uint64_t backupFiles = 0;
    state.counters["total_MB"] = static_cast<double>(bytesIn(root)) / 1e6;
    state.counters["db_MB"] = static_cast<double>(bytesIn(root / "workspace.db")) / 1e6;
    state.counters["assets_MB"] = static_cast<double>(bytesIn(root / "assets", &assetFiles)) / 1e6;
    state.counters["asset_files"] = static_cast<double>(assetFiles);
    state.counters["backups_MB"] =
        static_cast<double>(bytesIn(root / "backups", &backupFiles)) / 1e6;
    state.counters["backup_files"] = static_cast<double>(backupFiles);
}

std::unique_ptr<application::WorkspaceSession> openSession(const Spec& spec, Services& services) {
    return std::move(*application::WorkspaceSession::open(fixtureRoot(spec), {}, services.get()));
}

void finish(benchmark::State& state, double before) {
    state.counters["mem_before_MB"] = before;
    state.counters["mem_peak_MB"] = peakMemoryMB();
}

// ---------------------------------------------------------------------------- fixtures

/// Generates the workspace (once) and reports what it contains.
void BM_MaintenanceFixture(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        ensureFixture(spec);
    }
    Services services;
    auto session = openSession(spec, services);
    const document::Workspace& ws = session->workspace();
    std::size_t strokes = 0;
    std::size_t images = 0;
    std::size_t pdfPages = 0;
    for (const core::NotebookId notebook : ws.notebooks()) {
        for (const core::SectionId section : ws.sectionsOf(notebook)) {
            for (const core::PageId page : ws.pagesOf(section)) {
                pdfPages += ws.findPage(page)->document ? 1U : 0U;
                for (const core::LayerId layer : ws.layersOf(page)) {
                    for (const core::ElementId id : ws.elementsOf(layer)) {
                        const auto& payload = ws.findElement(id)->payload;
                        strokes += std::holds_alternative<document::Stroke>(payload) ? 1U : 0U;
                        images += std::holds_alternative<document::Image>(payload) ? 1U : 0U;
                    }
                }
            }
        }
    }
    state.counters["pages"] = static_cast<double>(ws.pageCount());
    state.counters["pdf_pages"] = static_cast<double>(pdfPages);
    state.counters["strokes"] = static_cast<double>(strokes);
    state.counters["images"] = static_cast<double>(images);
    (void)session->close();
    describe(state, spec);
}

// ---------------------------------------------------------------------------- Check Workspace

/// File ▸ Check Workspace: PRAGMA integrity_check, then every asset hashed.
void BM_CheckWorkspace(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto session = openSession(spec, services);
    const double before = workingSetMB();
    for (auto _ : state) {
        auto report = session->checkIntegrity();
        if (!report || !report->problems.empty()) {
            state.SkipWithError("the workspace is not intact");
            return;
        }
    }
    finish(state, before);
}

/// Its first part: SQLite's PRAGMA integrity_check of workspace.db.
void BM_DatabaseIntegrityCheck(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto file = persistence::WorkspaceFile::open(
        fixtureRoot(spec), persistence::AccessMode::ReadOnly, services.clock.now(), "bench");
    const double before = workingSetMB();
    for (auto _ : state) {
        auto check = file->database().queryText("PRAGMA integrity_check");
        if (!check || *check != "ok") {
            state.SkipWithError("integrity_check failed");
            return;
        }
    }
    finish(state, before);
}

/// Its second part: every asset file read and hashed (AssetStore::verify).
void BM_AssetVerify(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto file = persistence::WorkspaceFile::open(
        fixtureRoot(spec), persistence::AccessMode::ReadOnly, services.clock.now(), "bench");
    persistence::AssetStore assets(file->database(), fixtureRoot(spec));
    const double before = workingSetMB();
    for (auto _ : state) {
        auto problems = assets.verify();
        if (!problems || !problems->empty()) {
            state.SkipWithError("asset verification failed");
            return;
        }
    }
    finish(state, before);
}

/// The recovery check after an unclean shutdown: PRAGMA quick_check and foreign_key_check.
void BM_QuickAndForeignKeyCheck(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto file = persistence::WorkspaceFile::open(
        fixtureRoot(spec), persistence::AccessMode::ReadOnly, services.clock.now(), "bench");
    const double before = workingSetMB();
    for (auto _ : state) {
        if (!file->integrityCheck()) {
            state.SkipWithError("quick_check failed");
            return;
        }
    }
    finish(state, before);
}

// ---------------------------------------------------------------------------- backups

/// File ▸ Back Up Now: flush, VACUUM INTO backups/, rotation. Assets are not copied (D49).
void BM_BackUpNow(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto session = openSession(spec, services);
    const double before = workingSetMB();
    fs::path made;
    for (auto _ : state) {
        auto backup = session->backUpNow();
        if (!backup) {
            state.SkipWithError(backup.error().message.c_str());
            return;
        }
        made = *backup;
    }
    finish(state, before);
    state.counters["backup_MB"] = static_cast<double>(bytesIn(made)) / 1e6;
    (void)session->close();
}

/// Closing a session that changed the workspace when no backup is less than a day old:
/// the automatic backup (D49) is part of the close. Backups are removed (untimed) first
/// so that one is due on every iteration.
void BM_CloseWithDailyBackup(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    const double before = workingSetMB();
    for (auto _ : state) {
        state.PauseTiming();
        Services services;
        std::error_code ec;
        fs::remove_all(fixtureRoot(spec) / "backups", ec);
        auto session = openSession(spec, services);
        application::WorkspaceStructure structure(*session, services.clock, services.ids);
        const auto notebook = session->workspace().notebooks().front();
        (void)structure.rename(notebook, "Biology"); // a change, so the close backs up
        (void)structure.rename(notebook, "Biology notes");
        state.ResumeTiming();
        (void)session->close();
    }
    finish(state, before);
    state.counters["backup_files"] = static_cast<double>(
        persistence::listBackups(persistence::WorkspaceLayout{fixtureRoot(spec)},
                                 persistence::kAutoBackupLabel)
            ->size());
}

/// The same close when the newest backup is recent: no backup (the reference).
void BM_CloseWithoutBackup(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    for (auto _ : state) {
        state.PauseTiming();
        Services services;
        auto session = openSession(spec, services);
        if (persistence::listBackups(persistence::WorkspaceLayout{fixtureRoot(spec)},
                                     persistence::kAutoBackupLabel)
                ->empty()) {
            (void)session->backUpNow();
        }
        application::WorkspaceStructure structure(*session, services.clock, services.ids);
        (void)structure.rename(session->workspace().notebooks().front(), "Biology");
        state.ResumeTiming();
        (void)session->close();
    }
}

// ---------------------------------------------------------------------------- bundles

fs::path outputDir() {
    const fs::path out = benchRoot() / "out";
    fs::create_directories(out);
    return out;
}

/// The whole workspace as one bundle (stored zip; File ▸ Export Workspace).
fs::path bundleOf(const Spec& spec) {
    const fs::path bundle =
        outputDir() / ("ws-" + std::to_string(spec.gigabytes) + "gb.studybundle");
    if (!fs::exists(bundle)) {
        Services services;
        auto session = openSession(spec, services);
        (void)session->exportBundle(bundle, std::nullopt);
        (void)session->close();
    }
    return bundle;
}

/// File ▸ Export Workspace: database snapshot plus every asset into a stored zip.
void BM_ExportBundle(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto session = openSession(spec, services);
    const fs::path target = outputDir() / "export.studybundle";
    const double before = workingSetMB();
    for (auto _ : state) {
        // Bundles are limited to 4 GiB (no zip64, D46): a larger workspace fails, and the
        // time until it fails is what the user waits for — so it is measured, not skipped.
        const auto written = session->exportBundle(target, std::nullopt);
        state.PauseTiming();
        state.counters["failed"] = written ? 0.0 : 1.0;
        if (!written) {
            state.SetLabel(written.error().message);
        }
        std::error_code ec;
        state.counters["bundle_MB"] = static_cast<double>(bytesIn(target)) / 1e6;
        fs::remove(target, ec);
        state.ResumeTiming();
    }
    finish(state, before);
    (void)session->close();
}

/// File ▸ Open Bundle as Workspace: the bundle extracted into a new workspace directory.
void BM_ExtractBundle(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    const fs::path bundle = bundleOf(spec);
    const fs::path target = outputDir() / "extracted.studyws";
    const double before = workingSetMB();
    for (auto _ : state) {
        state.PauseTiming();
        fs::remove_all(target);
        state.ResumeTiming();
        if (auto extracted = application::WorkspaceSession::extractBundle(bundle, target);
            !extracted) {
            state.SkipWithError(extracted.error().message.c_str());
            return;
        }
    }
    finish(state, before);
    fs::remove_all(target);
}

/// File ▸ Import Notebook: the bundle's notebooks (here: the whole workspace) imported into
/// an empty workspace — extraction to a staging directory, every asset hashed and copied,
/// one command for the content.
void BM_ImportBundle(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    const fs::path bundle = bundleOf(spec);
    const fs::path target = outputDir() / "imported.studyws";
    const double before = workingSetMB();
    for (auto _ : state) {
        state.PauseTiming();
        fs::remove_all(target);
        Services services;
        auto session =
            std::move(*application::WorkspaceSession::create(target, "Import", services.get()));
        state.ResumeTiming();
        auto imported = session->importBundle(bundle);
        state.PauseTiming();
        if (!imported) {
            state.SkipWithError(imported.error().message.c_str());
            return;
        }
        (void)session->close();
        state.ResumeTiming();
    }
    finish(state, before);
    fs::remove_all(target);
}

// ---------------------------------------------------------------------------- Save a Copy

/// Save a Copy (from the unsaved-changes question): the in-memory workspace written into a
/// new workspace with every asset it uses.
void BM_SaveCopy(benchmark::State& state) {
    const Spec spec = specFor(static_cast<int>(state.range(0)));
    ensureFixture(spec);
    Services services;
    auto session = openSession(spec, services);
    const fs::path target = outputDir() / "copy.studyws";
    const double before = workingSetMB();
    for (auto _ : state) {
        state.PauseTiming();
        fs::remove_all(target);
        state.ResumeTiming();
        if (auto saved = session->saveCopy(target); !saved) {
            state.SkipWithError(saved.error().message.c_str());
            return;
        }
    }
    finish(state, before);
    fs::remove_all(target);
    (void)session->close();
}

// ---------------------------------------------------------------------------- hashing

/// What the operations above spend their time on: StudyBoard's own SHA-256 (asset identity,
/// verification) and the zip CRC-32, over 256 MiB in memory — no disk involved.
void BM_HashThroughput(benchmark::State& state) {
    std::vector<std::uint8_t> data(std::size_t{256} << 20U);
    std::mt19937_64 random(7);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(random());
    }
    const bool crc = state.range(0) != 0;
    for (auto _ : state) {
        if (crc) {
            benchmark::DoNotOptimize(persistence::crc32(data));
        } else {
            benchmark::DoNotOptimize(persistence::Sha256::of(data));
        }
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(data.size()));
    state.SetLabel(crc ? "CRC-32 (zip)" : "SHA-256 (assets)");
}
BENCHMARK(BM_HashThroughput)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

double minimum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
}
double maximum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

void maintenance(benchmark::internal::Benchmark* b) {
    b->Arg(1)->Arg(5)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
    b->ComputeStatistics("min", minimum)->ComputeStatistics("max", maximum);
}

BENCHMARK(BM_MaintenanceFixture)
    ->Arg(1)
    ->Arg(5)
    ->Iterations(1)
    ->UseRealTime()
    ->Unit(benchmark::kSecond);
BENCHMARK(BM_CheckWorkspace)->Apply(maintenance);
BENCHMARK(BM_DatabaseIntegrityCheck)->Apply(maintenance);
BENCHMARK(BM_AssetVerify)->Apply(maintenance);
BENCHMARK(BM_QuickAndForeignKeyCheck)->Apply(maintenance);
BENCHMARK(BM_BackUpNow)->Apply(maintenance);
BENCHMARK(BM_CloseWithDailyBackup)->Apply(maintenance);
BENCHMARK(BM_CloseWithoutBackup)->Apply(maintenance);
BENCHMARK(BM_ExportBundle)->Apply(maintenance);
BENCHMARK(BM_ExtractBundle)->Apply(maintenance);
BENCHMARK(BM_ImportBundle)->Apply(maintenance);
BENCHMARK(BM_SaveCopy)->Apply(maintenance);

} // namespace
} // namespace studyapp::bench
