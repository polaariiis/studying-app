# Stress testing

How StudyBoard behaves as workloads grow well past everyday use, measured with the stress
benchmarks. What the results mean for users is summarised in [CAPACITY.md](CAPACITY.md);
the everyday benchmarks are in [BENCHMARKS.md](BENCHMARKS.md).

## Method

* **Machine:** the reference laptop ([HARDWARE_MATRIX.md](HARDWARE_MATRIX.md): Ryzen 5
  8645HS, 8 GB, Windows 10 22H2), Release build (`ci-full` preset, MSVC 19.44, Qt 6.8.3),
  StudyBoard 1.0.0 source (product code identical to `ad76998`), 2026-09-30.
* **Workload:** synthetic handwriting (`bench/SyntheticPage.hpp`): smooth random strokes of
  30–90 points, 2 units wide, spread over one page of about 6000 × 3600 world units — so a
  larger stroke count is also a *denser* page. Deterministic seeds; nothing is loaded from
  files.
* **Sizes:** 1 000, 5 000, 10 000, 25 000, 50 000 and 100 000 strokes (fewer sizes where a
  run takes long).
* **Canvas** (`bench/StressBench.cpp`, `studyapp_benchmarks --benchmark_filter=BM_Stress`):
  the CPU side of the renderer — culling, tessellation, batching, draw lists — against a
  recording renderer. GPU time is not included; the frame rate the GPU sustains was measured
  in the running application only for the 10 000-stroke page (≥ 60 fps with the whole page
  in view, Phase 4, [PERFORMANCE.md](PERFORMANCE.md)).
* **Frames:** the camera pans continuously (12 × 4 px per frame, reversing every 120
  frames) with warm caches; every frame is timed, then the median (p50), 95th and 99th
  percentiles and the worst frame are taken. Two views: zoom 1 (normal writing; about
  1/40 of the page visible) and zoom 0.15 (the whole page visible, every stroke drawn).
* **Memory:** the process working set after the first frame. It includes the document, the
  CPU meshes *and* the recording renderer's copy of every mesh (a real GPU keeps that copy in
  video memory instead), so it overstates the application's RAM use by about the mesh size.
* **Workspace operations** (`bench/AppBench.cpp`): a real workspace on disk, through the
  same session, SQLite and Qt adapters as the application.
* Results: `bench/results/stress-canvas.json` and `bench/results/app.json` (Google
  Benchmark JSON, with the machine's CPU and cache description).

Labels used below: **normal** — everyday sizes; **stress** — far beyond typical notes but
still working; **limit** — a failure or unusable degradation. No limit was reached: every
size up to 100 000 strokes completed without errors; costs grow roughly linearly with the
number of strokes in view.

## Canvas: first frame of the whole page

Time until the whole page is drawn for the first time after opening it (every stroke
tessellated and batched, using all cores).

| Strokes | Time | Mesh memory | Working set | Label |
|---:|---:|---:|---:|---|
| 1 000 | 24 ms | 3.5 MB | 20 MB | normal |
| 5 000 | 58 ms | 25 MB | 70 MB | normal |
| 10 000 | 99 ms | 50 MB | 128 MB | normal |
| 25 000 | 226 ms | 125 MB | 301 MB | stress |
| 50 000 | 491 ms | 250 MB | 588 MB | stress |
| 100 000 | 1 055 ms | 501 MB | 1 148 MB | stress |

Opening a page at normal zoom builds only the visible strokes, so the first frame of a
page is much cheaper than this whole-page case.

## Canvas: panning

CPU time per frame while panning, in milliseconds. Budgets on the reference display:
6.9 ms per frame at 144 Hz, 16.7 ms at 60 Hz.

| Strokes | Zoom 1: p50 / p95 / p99 / worst | Whole page: p50 / p95 / p99 / worst |
|---:|---|---|
| 1 000 | 0.02 / 0.04 / 0.05 / 0.26 | 0.09 / 0.29 / 0.42 / 1.28 |
| 5 000 | 0.11 / 0.25 / 0.38 / 1.14 | 0.31 / 0.93 / 1.16 / 3.17 |
| 10 000 | 0.26 / 0.57 / 0.73 / 2.90 | 0.75 / 1.99 / 2.36 / 2.50 |
| 25 000 | 0.57 / 1.19 / 1.43 / 3.37 | 2.08 / 5.71 / 5.89 / 7.02 |
| 50 000 | 1.12 / 2.44 / 3.00 / 3.49 | 12.2 / 20.0 / 21.6 / 21.6 |
| 100 000 | 2.07 / 7.73 / 8.24 / 8.82 | 29.4 / 31.1 / 35.5 / 35.5 |

With every stroke selected (Select All), the selection outline is part of each frame:

| Strokes | Zoom 1: p50 / p95 / p99 / worst | Whole page: p50 / p95 / p99 / worst |
|---:|---|---|
| 1 000 | 0.04 / 0.06 / 0.07 / 0.23 | 0.08 / 0.28 / 0.43 / 1.07 |
| 5 000 | 0.12 / 0.28 / 0.39 / 1.09 | 0.31 / 0.81 / 0.91 / 2.34 |
| 10 000 | 0.26 / 0.58 / 0.85 / 2.82 | 0.79 / 2.02 / 2.35 / 3.01 |
| 25 000 | 0.52 / 1.29 / 1.86 / 2.56 | 1.54 / 8.83 / 9.66 / 9.67 |
| 50 000 | 1.19 / 2.74 / 3.53 / 4.42 | 11.6 / 15.3 / 17.4 / 17.4 |
| 100 000 | 2.82 / 7.69 / 8.66 / 9.89 | 29.2 / 37.5 / 38.4 / 38.4 |

**Visible degradation.** At zoom 1 panning stays inside the 144 Hz budget up to 50 000
strokes and inside 60 Hz at 100 000. With the whole page in view, 50 000 strokes exceed the
60 Hz budget at the 95th percentile (the median still fits), and 100 000 strokes run at
about 30 frames per second on the CPU side alone. The cost is the number of strokes in
view (culling, draw lists and batch bookkeeping), which is why zooming in restores smooth
panning.

## Workspace: opening

Opening a workspace (reading the database and rebuilding the document through the patch
path, with every invariant checked):

| Strokes | Time | Label |
|---:|---:|---|
| 1 000 | 14 ms | normal |
| 5 000 | 51 ms | normal |
| 10 000 | 105 ms | normal |
| 25 000 | 250 ms | stress |
| 50 000 | 552 ms | stress |
| 100 000 | 1 220 ms | stress |

## Workspace: moving everything and saving

Select All, then drag: building the move command, applying it and writing every changed
element to SQLite in one transaction, on the GUI thread, once when the drag ends.

| Strokes | Total | Of which the command | Label |
|---:|---:|---:|---|
| 1 000 | 10.5 ms | 0.7 ms | normal |
| 10 000 | 123 ms | 11.6 ms | normal |
| 50 000 | 807 ms | 73 ms | stress |
| 100 000 | 1 475 ms | 157 ms | stress |

The remainder is SQLite writing one row per element. A pause of this length happens once,
when the gesture ends; the drag itself is previewed without writing.

## Search

Full-text search (SQLite FTS5 with bm25 ranking, first 50 results) over text boxes, of
which 12.5 % contain the query word:

| Text boxes | Time | Label |
|---:|---:|---|
| 1 000 | 0.30 ms | normal |
| 10 000 | 2.2 ms | normal |
| 100 000 | 23 ms | stress |

Typing in the search field is debounced by 150 ms, so even the 100 000 case does not delay
typing.

## Export

One page exported with every stroke on it (the strokes spread over 6000 × 3600 units):

| Strokes | PDF | PNG | SVG |
|---:|---:|---:|---:|
| 100 | 35 ms, 0.11 MB | 5.4 s, 1.5 MB | 68 ms, 1.5 MB |
| 1 000 | 320 ms, 1.1 MB | 6.4 s, 5.5 MB | 569 ms, 15 MB |
| 10 000 | 3.1 s, 11 MB | 11.9 s, 24 MB | 5.8 s, 153 MB |

PNG time is dominated by the page area (the image is capped at 64 megapixels), not by the
stroke count. Export shows a progress dialog and can be cancelled.

## Not stress-tested

Numbers of images per page, PDF documents with more than 200 pages, text boxes on the
canvas (as opposed to in search), planner task counts above 10 000, bundles larger than one
10 000-stroke notebook, and long sessions (hours of editing, memory growth over time).
These have no measurements; see [CAPACITY.md](CAPACITY.md) for what is known.

## Reproducing

```sh
cmake --preset ci-full && cmake --build --preset ci-full
build/ci-full/bench/studyapp_benchmarks --benchmark_filter=BM_Stress \
    --benchmark_out=stress-canvas.json --benchmark_out_format=json
QT_QPA_PLATFORM=offscreen build/ci-full/bench/studyapp_app_benchmarks \
    --benchmark_out=app.json --benchmark_out_format=json
```

More in [BENCHMARKS.md](BENCHMARKS.md#reproducing-the-measurements).
