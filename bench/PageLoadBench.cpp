// Page load (docs/ARCHITECTURE.md D25): time to open a workspace holding a 10 000-stroke
// page — SQLite read, blob decode, Workspace::apply + validate. This is the measurement
// that decides when the catalog / per-page split becomes necessary.

#include "SyntheticPage.hpp"

#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/persistence/WorkspaceStore.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <benchmark/benchmark.h>

namespace studyapp::bench {
namespace {

void BM_LoadWorkspace10kStrokes(benchmark::State& state) {
    const SyntheticPage page(static_cast<int>(state.range(0)));
    testing::TempDirectory dir;
    const auto root = dir / "bench.studyws";
    {
        auto file = persistence::WorkspaceFile::create(root, page.workspace.info(), "bench");
        if (!file) {
            state.SkipWithError(file.error().message.c_str());
            return;
        }
        persistence::WorkspaceStore store(file->database(), page.clock);
        // Hierarchy first, then all strokes as one patch, as the application writes them.
        document::Patch hierarchy;
        for (const core::NotebookId notebook : page.workspace.notebooks()) {
            hierarchy.add(document::created(*page.workspace.findNotebook(notebook)));
            for (const core::SectionId section : page.workspace.sectionsOf(notebook)) {
                hierarchy.add(document::created(*page.workspace.findSection(section)));
                for (const core::PageId p : page.workspace.pagesOf(section)) {
                    hierarchy.add(document::created(*page.workspace.findPage(p)));
                    for (const core::LayerId layer : page.workspace.layersOf(p)) {
                        hierarchy.add(document::created(*page.workspace.findLayer(layer)));
                    }
                }
            }
        }
        if (!store.write(hierarchy) || !store.write(page.contentPatch)) {
            state.SkipWithError("writing the benchmark workspace failed");
            return;
        }
    }
    for (auto _ : state) {
        auto file = persistence::WorkspaceFile::open(root, persistence::AccessMode::ReadOnly,
                                                     page.clock.now(), "bench");
        auto loaded = persistence::WorkspaceStore(file->database(), page.clock).load();
        benchmark::DoNotOptimize(loaded->elementCount());
    }
}
BENCHMARK(BM_LoadWorkspace10kStrokes)->Arg(10'000)->Unit(benchmark::kMillisecond);

} // namespace
} // namespace studyapp::bench
