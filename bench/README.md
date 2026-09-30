# Benchmarks

Google Benchmark suites for StudyBoard. They are not run by CTest; build them with the
`ci-full` preset (Release, `STUDYAPP_BUILD_BENCHMARKS=ON`). Results and methodology are in
[docs/BENCHMARKS.md](../docs/BENCHMARKS.md), [docs/STRESS_TESTING.md](../docs/STRESS_TESTING.md)
and [docs/CAPACITY.md](../docs/CAPACITY.md).

| Executable | Sources | Measures |
|---|---|---|
| `studyapp_benchmarks` | `CanvasBench.cpp`, `PageLoadBench.cpp`, `StressBench.cpp` | Canvas CPU work (tessellation, frames, gestures), loading a workspace, and the stress series (`BM_Stress*`, 1 000–100 000 strokes). Qt-free |
| `studyapp_app_benchmarks` | `AppBench.cpp` | The application's operations through the real session, SQLite and Qt adapters: open, move and save, search, planner, image import, text, PDF, export, bundles. Needs Qt; run on the offscreen platform |

## Datasets

All data is generated in code with fixed random seeds, so every run uses the same content
and nothing large is stored in the repository:

* `SyntheticPage.hpp` — a page of handwriting-like strokes (30–90 points, optionally every
  n-th a highlighter stroke), built through the document commands as the application
  stores them.
* `AppBench.cpp` fixtures — workspaces on disk with N strokes or text boxes, generated
  PNG images (512² and 4096²) and PDFs (1 and 200 pages).

## Running

```sh
cmake --preset ci-full && cmake --build --preset ci-full
python tools/run_benchmarks.py --build build/ci-full
```

`run_benchmarks.py` runs every suite and writes `canvas.json`, `stress-canvas.json`,
`app.json` and `machine.json` (OS, CPU, commit; add the GPU and display by hand) to
`bench/results/<date>-<label>/`. `--label` names the machine; the computer's host name is
not recorded, so results can be published. `--filter <regex>` runs a subset.

## Results

`results/` holds the JSON behind the published numbers (the 1.0.0 measurements on the
reference laptop: `canvas.json`, `app.json`, `stress-canvas.json`). Keep new result sets in
their own dated directory; compare only results from the same machine.
