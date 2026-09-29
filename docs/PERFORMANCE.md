# Performance

Measured performance of StudyBoard and the benchmarks that track it (Phase 9). Numbers are
Release builds on the reference machine unless noted; they are for comparison over time on
the same hardware, not guarantees.

**Reference machine:** laptop, AMD Ryzen 5 8645HS (6 cores / 12 threads), 7.3 GB RAM usable,
AMD Radeon 760M (integrated, used by StudyBoard) + NVIDIA RTX 4050 Laptop, Windows 10 22H2,
MSVC 19.4x (VS 2022), Qt 6.8.3. Display 144 Hz.

## How to measure

| What | Command | Notes |
|---|---|---|
| Canvas, page load | `build/ci-full/bench/studyapp_benchmarks` | Google Benchmark; Qt-free (`bench/CanvasBench.cpp`, `PageLoadBench.cpp`) |
| Application operations | `build/ci-full/bench/studyapp_app_benchmarks` | Real session, SQLite and Qt adapters on the offscreen platform (`bench/AppBench.cpp`) |
| Start-up and memory | `tools/measure_startup.ps1 -Exe build/release/app/studyapp.exe` | Windows; time until the main window is idle, working set 3 s later; private settings |
| Frame timing in the app | View ▸ Debug HUD (F3) | p50/p95/p99, worst, CPU per frame |

`ci-full` is the Release configuration with warnings as errors and benchmarks. Use
`--benchmark_filter=<regex>` to run a subset and `--benchmark_out=<file>
--benchmark_out_format=json` to keep results.

## Baseline (Phase 8 checkpoint, 4726655)

### Start-up and memory (release app, empty workspace)

| Measure | Value |
|---|---|
| Cold start to idle main window | 297 ms |
| Warm start (median of 4) | 154 ms |
| Working set / private bytes after start | 259 MB / 309 MB |

The largest loaded module is the AMD OpenGL driver (`atio6axx.dll`, 62 MB image); document
caches (meshes, text/image/PDF textures) are empty until content is shown.

### Canvas (`studyapp_benchmarks`, 10 000 handwriting strokes unless noted)

| Benchmark | Time | Notes |
|---|---|---|
| Tessellate one 30-point stroke | 5.2 µs | |
| Tessellate all strokes of the page | 98.6 ms | 1.70 M triangles |
| Scene rebuild | 18.4 ms | |
| Frame while panning, zoom 1 / 0.3 / 0.15 | 0.27 / 0.73 / 0.85 ms | build + submission, recording renderer |
| Frame while panning, all 10 000 selected | 8.5–10.5 ms | selection outlines rebuilt per frame |
| First frame of the whole page | **237 ms** | 49.8 MB of meshes |
| Stroke commit frame (p50) | 4.5 ms | 4.3 ms of it `prepare content` (batch re-hash) |
| Partial erase commit frame (p50), writing order / 300 long strokes | 31.7 / 98 ms | |
| Paste 10 000 strokes: paste / next frame | 40 / 188 ms | |
| Open a workspace with 10 000 strokes (persistence only) | 97 ms | |

### Application operations (`studyapp_app_benchmarks`)

| Benchmark | Time | Notes |
|---|---|---|
| Open workspace, 10 000 strokes (session) | 85 ms | |
| Search, 1 000 / 10 000 / 100 000 text boxes | 0.24 / 1.6 / 19 ms | "photo", 50 results, 12.5 % of documents match |
| Create a task with 100 / 10 000 tasks | 0.28 / 0.30 ms | includes the write |
| Import image, 512² / 4096² PNG | 17 / 412 ms | 0.8 / 50 MB, hashed and copied on the calling (GUI) thread |
| Rasterize a text box at 1× / 4.2× | 0.12 / 1.1 ms | |
| Inspect a 200-page PDF | 47 ms | |
| PDF tile, preview / level 1 | 5.5 / 3.1 ms | on the worker thread |
| Export 100-stroke page: PDF / PNG / SVG | 465 / 3 882 / 153 ms | 1.7 / 1.5 / 2.7 MB; strokes spread over 6 000 units |
| Export 10 000-stroke page: PDF / PNG / SVG | 10.2 / 8.7 / 8.6 s | 32 / 24 / 142 MB |
| Notebook bundle of 10 000 strokes: export / import | 263 / 378 ms | |

## Phase 9 results

(Filled in as each optimisation lands; see ROADMAP.md Phase 9.)
