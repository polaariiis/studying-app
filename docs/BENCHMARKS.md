# Benchmarks

Measured performance of StudyBoard 1.0.0, how each number was obtained, and how to
reproduce it. How the application is built for performance, and its known bottlenecks, are
in [PERFORMANCE.md](PERFORMANCE.md); larger workloads are in
[STRESS_TESTING.md](STRESS_TESTING.md) and what they mean in practice in
[CAPACITY.md](CAPACITY.md).

## Environment

| | |
|---|---|
| Machine | Reference laptop: HP Victus 15-fb2xxx, AMD Ryzen 5 8645HS (6 cores / 12 threads), 8 GB RAM, AMD Radeon 760M, 1920 × 1080 at 144 Hz ([HARDWARE_MATRIX.md](HARDWARE_MATRIX.md)) |
| OS | Windows 10 Home 22H2 (build 19045), on mains power |
| Build | Release, `ci-full` preset (MSVC 19.44, `/O2`), Qt 6.8.3, Google Benchmark |
| Source | Commit `ad76998` — the product code of 1.0.0 (`v1.0.0` differs only in the version number and in benchmarks, documentation and CI) |
| Date | 2026-09-30 |
| Raw results | `bench/results/canvas.json`, `bench/results/app.json`, `bench/results/stress-canvas.json` (Google Benchmark JSON, including the CPU and cache description) |

These are measurements on one machine, for comparison over time and between versions; they
are not guarantees for other hardware. **Variance:** repeated runs on this laptop differ by
up to about 15 % for millisecond-scale results and up to about 35 % for multi-second export
runs (for example, exporting 10 000 strokes to PDF took 2.34 s alone and 3.15 s when run
right after the other benchmarks); percentiles are given where a benchmark records single
frames.

**What the canvas benchmarks measure:** the CPU side of drawing a frame — culling, level of
detail, tessellation, batching and draw lists — against a recording renderer. GPU time is
not part of them; the running application's GPU-side frame times are measured with its debug
HUD (F3) and `studyapp --bench-pan`.

## Canvas

Synthetic handwriting on one page (`bench/SyntheticPage.hpp`): strokes of 30–90 points,
2 units wide, over about 6000 × 3600 units; 10 000 strokes unless stated. Viewport
1920 × 1080.

| Benchmark | Workload | Result | Phase 9 result | Baseline (before Phase 9) |
|---|---|---|---|---|
| `BM_TessellateStroke` | One 30-point stroke into triangles | 6.2 µs | — | 5.2 µs |
| `BM_TessellatePageAllStrokes` | Every stroke, serially | 128 ms (1.70 M triangles) | — | 98.6 ms |
| `BM_SceneRebuild` | Spatial index of the page | 22.7 ms | — | 18.4 ms |
| `BM_VisibleQuery/400` / `100` / `25` | Strokes in view at zoom 4 / 1 / 0.25 | 0.04 / 0.41 / 1.45 ms (77 / 596 / 7 238 visible) | — | — |
| `BM_HitTestTopmost` | Topmost element under the pointer | 10.6 µs | — | — |
| `BM_StressFirstFrame/1000` … `/100000` | First frame, whole page, 1 000 / 10 000 / 25 000 / 50 000 / 100 000 strokes | 24 ms / 99 ms / 226 ms / 491 ms / 1.06 s | — | — |
| `BM_StressPanFrames/…/100/0` | Panning at zoom 1, p95, 1 000 / 10 000 / 25 000 / 50 000 / 100 000 strokes | 0.04 / 0.57 / 1.19 / 2.44 / 7.73 ms | — | — |
| `BM_StressPanFrames/…/15/0` | Panning with the whole page in view, p95, same sizes | 0.29 / 1.99 / 5.71 / 20.0 / 31.1 ms | — | — |
| `BM_FirstFrameWholePage` | First frame, whole page, 10 000 strokes (parallel mesh build) | 99.6 ms, 50 MB of meshes | 88 ms | 237 ms |
| `BM_BuildFramePanning/100` / `30` / `15` | Frame while panning at zoom 1 / 0.3 / whole page | 0.29 / 0.81 / 0.83 ms | — | 0.27 / 0.73 / 0.85 ms |
| `BM_BuildFramePanningAllSelected/100` / `15` | As above, all 10 000 selected | 0.29 / 0.75 ms | 0.2–0.5 ms | 8.5–10.5 ms |
| `BM_BuildFramePanningWithHighlighter/100` / `15` | Every 10th stroke a translucent highlighter | 0.28 / 0.85 ms | — | — |
| `BM_BuildFrameZoomingWholePage` | Wheel zoom ×1.2, 8 steps in and out, from the whole page | median 8.2 ms, worst 23.5 ms per step | — | — |
| `BM_InkGesture/0/240` | Drawing a 240-sample stroke on the 10 000-stroke page | per sample p50 0.40 ms, p99 0.73 ms; commit frame p50 4.5 ms, max 5.7 ms | commit p50 3.4 ms | 4.5 ms |
| `BM_InkGesture/1/2000` | A 2 000-sample stroke | per sample p50 0.58 ms, p99 1.06 ms; commit p50 5.0 ms | — | — |
| `BM_PartialEraseGesture/0/120` | Partial (vector) eraser across strokes in writing order | per sample p50 0.58 ms; commit p50 20.9 ms, max 24.0 ms | 20.8 ms | 31.7 ms |
| `BM_PartialEraseGesture/1/120` | Erasing across 300 long strokes | per sample p50 4.9 ms; commit p50 77.3 ms, max 99.6 ms | 64 ms | 98 ms |
| `BM_PartialEraseGesture/2/120` | Random order | commit p50 12.9 ms | — | — |
| `BM_CopyPaste/0` | Copy and paste 1 014 strokes | copy 1.9 ms, paste 3.2 ms, next frame 21.5 ms | — | — |
| `BM_CopyPaste/1` | Copy and paste all 10 000 strokes | copy 11.2 ms, paste 35.8 ms, next frame 94 ms | next frame 100 ms | paste 40 ms, next frame 188 ms |
| `BM_ToolAndSettingsChange` | Switching tool or colour | 1.8 ms, nothing rebuilt | — | — |
| `BM_LoadWorkspace10kStrokes` | Reading a 10 000-stroke workspace (persistence only) | 91.9 ms | — | 97 ms |

Every size and percentile of the stress series is in [STRESS_TESTING.md](STRESS_TESTING.md).
Not measured: GPU frame time at sizes other than 10 000 strokes; moving a *partial*
selection on a huge page (the drag preview); rendering with a physical pen.

## Workspace and application

Real workspaces on disk through the application's session, SQLite and Qt adapters
(`bench/AppBench.cpp`, offscreen Qt platform).

| Benchmark | Workload | Result | Phase 9 result | Baseline |
|---|---|---|---|---|
| `BM_OpenWorkspace/1000` / `10000` / `100000` | Open a workspace with N strokes | 14 ms / 105 ms / 1.22 s | 10 000: 77 ms | 10 000: 85 ms |
| `BM_MoveAllAndSave/1000` / `10000` | Move every stroke, write to SQLite | 10.5 ms / 123 ms (command 0.7 / 11.6 ms) | 10 000: 88–127 ms | — |
| `BM_PlannerCreateTask/100` / `1000` / `10000` | Create a task with N tasks present (incl. write) | 0.33 / 0.34 / 0.33 ms | 0.28–0.36 ms | 0.28–0.30 ms |
| `BM_Bundle/0` / `1` | Export / import a 10 000-stroke notebook bundle | 299 ms / 465 ms | — | 263 / 378 ms |

Not measured: start-up time and memory in this run. Measured earlier in Phase 9 with
`tools/measure_startup.ps1`: warm start to an idle window 150 ms, cold 297 ms, working set
260 MB (mostly the OpenGL driver).

## Text

| Benchmark | Workload | Result | Baseline |
|---|---|---|---|
| `BM_TextRaster/10` | Rasterise a text box at 1× | 0.13 ms | 0.12 ms |
| `BM_TextRaster/42` | The same at 4.2× zoom | 1.65 ms | 1.1 ms |

Not measured: editing (the text editor is a Qt widget over the canvas; typing costs are
Qt's), and zooming across many text boxes (refinement is limited to about 1 megapixel per
frame, so it is spread over frames by design).

## Images

| Benchmark | Workload | Result | Baseline |
|---|---|---|---|
| `BM_ImportImage/512` | Import a 512² PNG (0.8 MB): copy, hash, store | 18 ms | 17 ms |
| `BM_ImportImage/4096` | Import a 4096² PNG (50 MB) | 394 ms | 412 ms |
| `BM_ImportImageGuiPart/4096` | Of that, the part left on the GUI thread | 2.2 ms | 393 ms (all of it) |

Not measured: repeated drawing of the same image (textures are cached, 256 MB budget) and
decoding time for very large images.

## PDF

| Benchmark | Workload | Result | Baseline |
|---|---|---|---|
| `BM_PdfInspect/200` | Import a 200-page PDF: read page sizes | 55 ms | 47 ms |
| `BM_PdfTile/-1` | Render a preview tile (worker thread) | 6.4 ms | 5.5 ms |
| `BM_PdfTile/1` | Render a level-1 tile | 3.9 ms | 3.1 ms |

Not measured: page navigation through a long document, and documents over 200 pages.

## Search

| Benchmark | Workload | Result | Baseline |
|---|---|---|---|
| `BM_Search/1000` / `10000` / `100000` | FTS5 query, 50 results, 12.5 % of text boxes match | 0.30 / 2.2 / 23 ms | 0.24 / 1.6 / 19 ms |

## Export

A page with N strokes spread over about 6000 × 3600 units, and a typical A4 page with 1 000
strokes.

| Benchmark | Workload | PDF | PNG | SVG |
|---|---|---|---|---|
| `BM_ExportPage/100/…` | 100 strokes | 35 ms, 0.11 MB | 5.4 s, 1.5 MB | 68 ms, 1.5 MB |
| `BM_ExportPage/1000/…` | 1 000 strokes | 320 ms, 1.1 MB | 6.4 s, 5.5 MB | 569 ms, 15 MB |
| `BM_ExportPage/10000/…` | 10 000 strokes | 3.1 s, 11 MB | 11.9 s, 24 MB | 5.8 s, 153 MB |
| `BM_ExportA4Page/…` | A4, 1 000 strokes | 353 ms, 1.1 MB | 869 ms, 2.0 MB | 531 ms, 15 MB |

Before Phase 9 the 100-stroke page took 465 ms (1.7 MB) as PDF and 10 000 strokes 10.2 s
(32 MB); ink is now exported as stroked outlines instead of triangles.

## Regression thresholds

Changes that exceed these on the reference laptop are investigated before they are merged
(about 1.5× the Phase 9 results). The 1.0.0 measurements above are within all of them.

| Benchmark | Threshold | 1.0.0 |
|---|---|---|
| `BM_FirstFrameWholePage` | 130 ms | 99.6 ms |
| `BM_BuildFramePanning/15`, `BM_BuildFramePanningAllSelected/15` | 1 ms | 0.83 / 0.75 ms |
| `BM_InkGesture/0/240` commit frame p50 | 5 ms | 4.5 ms |
| `BM_PartialEraseGesture/0/120` commit frame p50 | 32 ms | 20.9 ms |
| `BM_OpenWorkspace/10000` | 130 ms | 105 ms |
| `BM_Search/10000` | 3 ms | 2.2 ms |
| `BM_PlannerCreateTask/10000` | 0.5 ms | 0.33 ms |
| `BM_ImportImageGuiPart/4096` | 5 ms | 2.2 ms |
| `BM_ExportA4Page/0` (PDF) | 450 ms | 353 ms |
| `BM_MoveAllAndSave/10000` | 190 ms | 123 ms |
| Warm start (`tools/measure_startup.ps1`) | 250 ms | not re-measured (150 ms in Phase 9) |

## Reproducing the measurements

The datasets are generated in code with fixed seeds (`bench/SyntheticPage.hpp`, the fixtures
in `bench/AppBench.cpp`); nothing is downloaded or committed. See [bench/README.md](../bench/README.md).

```sh
cmake --preset ci-full && cmake --build --preset ci-full    # Release + benchmarks
python tools/run_benchmarks.py --build build/ci-full        # every suite, JSON + machine record
```

or individual suites, e.g. `build/ci-full/bench/studyapp_benchmarks
--benchmark_filter=BM_FirstFrameWholePage`. On Linux and macOS set `QT_QPA_PLATFORM=offscreen`
for `studyapp_app_benchmarks`. Record the machine with the results (the script does); close
other applications, use mains power, and compare only runs from the same machine.
