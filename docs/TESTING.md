# Testing Architecture

> Status: **Proposed — awaiting review.**

## 1. Principles

* **Most logic is testable without a GUI, a GPU or a display.** That is the payoff of the
  Qt-free `core`/`document`/`study`/`render`/`canvas`/`persistence`/`application` modules.
* **Real SQLite, not mocks.** Persistence and application tests use `:memory:` or
  temp-directory databases; they are fast and catch real SQL errors.
* **Determinism.** Time (`Clock`), ids (`IdGenerator`) and threading (`Executor`) are
  injected; tests use fixed clocks, sequential ids and synchronous executors.
* **Every bug fix adds a test** reproducing it first.

## 2. Test pyramid

| Level | Scope | Framework | Runs in |
|---|---|---|---|
| Unit | Single functions/classes in Qt-free modules | GoogleTest (+ GMock sparingly) | Every CI job, milliseconds |
| Integration | Stores against SQLite, application sessions end-to-end without UI, migrations | GoogleTest | Every CI job, seconds |
| Canvas scenario | Synthetic input sequences → patches and render frames | GoogleTest + `RecordingRenderer` | Every CI job |
| Rendering | Shader compile, golden-image comparisons via offscreen GL | GoogleTest + Qt offscreen surface | Linux CI with Mesa llvmpipe; optional locally |
| UI smoke | Main window opens, actions wired, models behave | Qt Test (`QTest`) with `QT_QPA_PLATFORM=offscreen` | Full CI jobs |
| Fuzz | Codecs and parsers (stroke blob, rich-text JSON, migrations input) | libFuzzer (Clang) | Nightly / manual |
| Benchmarks | Tessellation, spatial index, page load, persistence throughput | Google Benchmark | Manual + nightly trend (not gating) |

## 3. Layout

```
tests/
├── support/        # studyapp_test_support: builders, FixedClock, SequentialIds,
│                   #   ImmediateExecutor, TempWorkspace, RecordingRenderer, FakeTextLayout
├── core/           → core_tests
├── document/       → document_tests
├── study/          → study_tests
├── render/         → render_tests          (tessellation only; no GL)
├── canvas/         → canvas_tests
├── persistence/    → persistence_tests
├── application/    → application_tests
├── render_gl/      → render_gl_tests       (needs a GL context; labelled "gpu")
├── ui/             → ui_tests              (Qt Test; labelled "gui")
└── fixtures/       # sample workspaces per schema version, PDFs, images, golden PNGs
```

One test executable per module, registered with CTest via `gtest_discover_tests`. CTest
labels (`unit`, `integration`, `gpu`, `gui`, `slow`) let CI and developers select subsets:
`ctest --preset debug -L unit`.

Test builders keep tests readable:

```cpp
auto page = PageBuilder{}
    .layer("Ink")
    .stroke({{0,0},{10,0},{10,10}}).color(Color::black()).at({100, 50})
    .text("Hello").at({0, 200})
    .build(ids);
```

## 4. Test plan by area

### Geometry (`core`)
* Vec/Rect arithmetic, `Affine2` compose/invert round-trips, degenerate inputs.
* Point–segment distance, segment intersection, polygon containment, AABB of transformed
  rects — with property-style randomized tests against brute-force references.
* `FractionalIndex`: `between(a, b)` is strictly between; repeated insertion at the same
  spot stays valid for 10 000 iterations; keys sort bytewise.
* UUIDv7: format bits, monotonicity within a millisecond, parse/print round-trip.

### Document model (`document`)
* `PageDocument::apply` for create/update/delete of each element kind; layer operations.
* Invariants: rejected/asserted patches (element in missing layer, duplicate z, …).
* `worldBounds` for each payload under rotation/scale.
* `WorkspaceCatalog` tree operations: move page across sections, nested sections, trash.

### Commands
* Each command function: given a document, returns the expected patch (golden patches
  for representative cases). E.g. `deleteElements` detaches connectors;
  `eraseStrokeSegments` produces correct pieces with new ids; `reorder` keys are correct.
* Commands are pure: the input document is unchanged.

### Undo/redo
* For every command: `apply(p); apply(p.inverted())` restores a document equal to the
  original (**round-trip property**, run across randomized documents).
* Redo after undo reproduces the post-state; new commit clears redo.
* Merging: consecutive same-key commits compose correctly; different keys don't merge.
* Budget eviction by count and bytes; payload sharing verified (use counts, no copies).
* Undo across page eviction/reload yields the same state.

### Database & persistence
* Schema creation from scratch; `PRAGMA foreign_key_check` and `integrity_check` clean.
* Every store: write via patch → reload → equal to in-memory model (for each element kind
  and each catalog/study entity).
* Move-only patch does not rewrite the stroke blob (verified via update counting).
* Cascades and restrictions: deleting a page removes elements/kind rows; deleting a
  referenced asset fails.
* Search index: updated with text changes, deletes, rebuild equals incremental result;
  diacritics/case-insensitive matching.
* Asset store: dedupe by hash, crash-simulation orderings (orphan files, never dangling
  rows), GC with grace period.
* Migrations: for each version N, open `fixtures/workspaces/vN` → migrate → verify
  content; refuse newer versions; backup created before migrating.
* Writer thread: ordering preserved; coalescing; error surfaces and retries (fault
  injection via a failing VFS or read-only file).

### Serialization
* Stroke codec: encode/decode round-trip (bit-exact floats), all versions decodable,
  truncated/corrupt input rejected without crashing (and fuzzed).
* Rich-text JSON: round-trip, unknown fields preserved or ignored per policy, version
  handling, invalid JSON rejected.
* Export bundle: export → import into empty workspace → equal content.

### Study system
* Agenda: overdue/today/upcoming boundaries around midnight, floating dates independent
  of the test's timezone (run tests under several `TZ` values in CI).
* Progress roll-ups with subtasks, cancelled tasks excluded.
* Task ↔ page links and tag filtering through the store.
* Recurrence expansion (when introduced): table-driven against known RRULE results.

### Canvas
* Camera: `viewToWorld(worldToView(p)) ≈ p` over a zoom/position grid, incl. far from
  origin; `zoomAt` keeps anchor fixed.
* Spatial grid: query results equal brute-force results on random scenes (10k elements).
* Hit testing per element kind, with rotation/scale, tolerance scaling with zoom, hidden
  and locked layers.
* Tool scenarios: pen down/move/up → exactly one create patch with simplified points;
  select-drag → one transform patch; eraser across N strokes → one patch; cancel leaves
  the document unchanged.
* Render frame building: culled items are excluded; order matches `(layer, z)`.

### Rendering
* Tessellation: vertex/index validity, no degenerate NaNs, bounds contain the input,
  width follows pressure, join/cap geometry for sharp angles and single-point strokes.
* GL backend (label `gpu`): shaders compile on the CI GL implementation; golden-image
  tests for a small set of scenes with per-pixel tolerance (anti-aliasing varies between
  drivers, so goldens are generated with Mesa llvmpipe on Linux and checked only there).

### UI (few, focused)
* Main window constructs with a temp workspace; notebook tree model reflects catalog
  patches (`QAbstractItemModelTester`); key actions (new page, undo) update state.

## 5. Tooling

* **Sanitizers**: ASan + UBSan preset on Linux/macOS (Clang/GCC) running all non-GPU
  tests; TSan job for `persistence`/`application` threading tests.
* **Coverage**: llvm-cov/gcovr report on Linux, uploaded as a CI artifact (informational,
  no hard threshold initially; aim ≥ 80 % on Qt-free modules).
* **Static analysis**: clang-tidy on changed files in CI (Phase 1 config, grown gradually).
* **Leak/handle checks**: SQLite `sqlite3_close` must return `SQLITE_OK` in tests
  (unfinalised statements fail the test).
