# Testing Architecture

> Status: **Final baseline.** §0 lists what exists after Phase 5; the rest of this
> document is the plan that later phases follow.

## 0. Current state (Phase 5)

| Test target | Label | Framework | Covers |
|---|---|---|---|
| `core_tests` | `unit` | GoogleTest | `Vec2`/`DVec2`, `Rect`/`DRect`, `Color`, `Uuid`, `Id<T>`, `UuidV7Generator`, `SystemClock`, `Result`/`Error`, logging facade, `FractionalIndex` (incl. randomised insertion and key-growth tests), `FrameTimings` (nearest-rank percentiles, jitter, idle gaps start a new burst, window) |
| `document_tests` | `unit` | GoogleTest | Workspace hierarchy, lookup, ordering, uniqueness, invariants; patches (apply, inverse, conflicts, atomic rollback); commands (success, failure, determinism, cascades); undo/redo (empty history, multiple steps, redo-branch clearing, failed commands, capacity, exact state restoration, nested 10-step scenario); element local/world bounds; moving notebooks/sections/pages (reorder, move across parents, undo, repeated moves keep a total order) |
| `render_tests` | `unit` | GoogleTest | Tessellation: empty/degenerate/non-finite input, discs, width and topology of straight lines, pressure-varying width, round (spike-free) sharp turns, bounded miters, circle subdivision, fills, outlines, ellipses, mesh concatenation (linear: reallocations counted over 10 000 appends) |
| `canvas_tests` | `unit` | GoogleTest | Camera (round trips over ±10⁷ and all zoom levels, cursor-centred zoom incl. clamping, pan∘zoom, viewport/DPR changes, fit, invalid input); One-Euro and RDP (determinism, jitter, lag, tolerance guarantee, pressure, 10 000-point worst case); spatial grid vs brute force; scene draw order, incremental = rebuild, versions, hidden layers, removals; render cache invalidation and LOD; hit tests; `CanvasController` scenarios through an Editor-backed port and a recording renderer: pen (world alignment after zoom/pan/resize/DPR, pressure, width, live preview, cancel, read-only, bounded pages), select (click, Shift, rectangle intersect/contain, zoomed), move/delete/erase as one command each with undo/redo, pruning, navigation, culling, stale-cache freedom, backgrounds, context reset, batching (same triangles, rebuild only on change, preview fallback), hover requests no repaint (every tool), eraser ring is the cursor and never rendered content, level-of-detail refinement within a per-frame budget (frames requested until done, content never deferred), runs moving as a whole stay one draw |
| `persistence_tests` | `integration` | GoogleTest | Real SQLite in temp directories (Unicode paths). Linked SQLite requirements; `Database`/`Statement`/`Transaction` (pragmas, storage classes, error mapping, commit/rollback, failed commit); migrations (new schema = documented tables, WAL, application_id, integrity/FK check clean; no-op re-run; upgrade harness with backup; failed migration rolls back; newer schema refused read-write; foreign databases refused); stroke codec (bit-exact round trip, malformed blobs) and SHA-256 vectors; `WorkspaceFile` (layout, create/open/reopen, read-only, backup, integrity check, temp cleanup, moved directory); `WorkspaceStore` (every record and element kind round-trips field by field, UUID/parent/order preservation, renames, updates, kind change, layer move, deletes + cascades, undo/redo patches, move does not rewrite stroke blobs, content_version, failed patch leaves the database unchanged, repeated cycles, 20 corrupt-data cases); `AssetStore` (content addressing, dedupe, crash ordering → orphan file only, GC grace period and references, ON DELETE RESTRICT, verify) |
| `application_tests` | `integration` | GoogleTest | `componentVersions()`; shell workflows (`WorkspaceStructure`: default names, compound creation as one undo step, rename/delete/move, read-only; `PageNavigator`: neighbour on delete and undo, previous/next; `pageChangedBy`; create → edit → switch page → return → reopen); canvas + session end to end (draw/zoom/pan/move/delete/erase/undo → autosave → reopen: identical ink, ids, order), start page, patch listener; `WorkspaceSession`: create → commands → close → reopen equality for a complex workspace (all element kinds, attached connectors, assets, renames, deletes, undo/redo), repeated edit/reopen cycles, rejected commands change nothing, write failure keeps edits queued and retries (fault injection via a second connection), locking (active → read-only only, stale → explicit recovery with integrity check and temp cleanup) |
| `ui.CanvasWidgetTest` | `gui;gpu` | Qt Test | Real widget + OpenGL + session: scripted mouse/wheel drawing aligned with world coordinates, ink visible in the framebuffer, undo action, reopen with identical ink; blank/ruled/grid/dot backgrounds and bounded pages by pixel sampling, selection frame, debug HUD on/off; ink stays under the pointer after resizes and the framebuffer is logical size × DPR; using the navigation tree paints no canvas frame; the eraser ring platform cursor (centred hot spot, applied on tool change; runs without OpenGL too). The OpenGL cases skip without a 3.3 context (the offscreen platform in CTest/CI); run `canvas_widget_tests` directly on a desktop (`STUDYAPP_TEST_SCREENSHOTS=<dir>`, `STUDYAPP_TEST_THEME=dark`) |
| `platform.QtWorkspaceLockerTest` | `integration` | Qt Test | `QLockFile` adapter: free/active/exclusive, release, stale lock of a dead process, other host never stale, unreadable lock file |
| `architecture_tests` | `architecture` | GoogleTest | All Qt-free modules link into one plain C++ executable |
| `architecture.include_boundaries` | `architecture` | Python script | `tools/check_boundaries.py`: forbidden includes / GL calls per module |
| `ui.MainWindowTest` | `gui` | Qt Test (offscreen) | Main window structure, theme switching, theme actions, settings persistence, application icon resources, neutral palette (zero-saturation tokens), stylesheet template fully resolved |
| `ui.ShellTest` | `gui` | Qt Test (offscreen) | Navigation tree model (`QAbstractItemModelTester`; renames are `dataChanged` only, structure changes are row inserts/removals/moves, canvas edits emit nothing; active page; drag-and-drop validation); the window with real sessions, locks and scripted dialogs: new workspace starts on a page, navigation and New Page/Section/Notebook, undo shows the undone page, rename/delete (confirmation)/move, closing asks nothing, a workspace in use opens read-only, invalid workspace reported, stale lock (cancel / read-only / recover), a failing start-up workspace is forgotten, opening an empty workspace writes nothing, a collapsed section stays collapsed while drawing, theme toggle icon, navigation toggle, dropped page stays visible; every window workflow runs under `QAbstractItemModelTester` |

Tests use the header-only targets in `tests/support`: `studyapp_test_support`
(`testing::ManualClock` and `testing::SequentialIds` make every timestamp and id
deterministic; `EXPECT_OK`/`ASSERT_OK`; `testing::TempDirectory`, a self-deleting
directory with a non-ASCII name) and `studyapp_document_test_support` (adds the
`TestWorkspace` fixture, shared by document, persistence and application tests). Document
tests link only `studyapp_document` and `studyapp_core` — no Qt, GPU, SQLite, filesystem,
network or wall clock. Persistence and application tests use real SQLite and never mock it.

GoogleTest tests are registered with `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)`,
so each test case is its own CTest test. Qt Widgets tests use Qt Test because they need
a `QApplication`; CTest runs them with `QT_QPA_PLATFORM=offscreen`, and on Windows
prepends the Qt `bin` directory to `PATH`.

```sh
ctest --preset debug                # everything
ctest --preset core-only            # without Qt
ctest --preset debug -L unit        # by label
```


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
├── support/        # studyapp_test_support: ManualClock, SequentialIds (Phase 2); later
│                   #   builders, ImmediateExecutor, TempWorkspace, RecordingRenderer, FakeTextLayout
├── architecture/   → architecture_tests + include-boundary check (Phase 1)
├── core/           → core_tests
├── document/       → document_tests          (Phase 2)
├── study/          → study_tests
├── render/         → render_tests          (tessellation only; no GL)
├── canvas/         → canvas_tests
├── persistence/    → persistence_tests
├── application/    → application_tests
├── render_gl/      → render_gl_tests       (needs a GL context; labelled "gpu")
├── ui/             → ui_tests              (Qt Test; labelled "gui"; Phase 1)
└── fixtures/       # sample workspaces per schema version, PDFs, images, golden PNGs
```

One test executable per module, registered with CTest via `gtest_discover_tests`. CTest
labels (`unit`, `integration`, `architecture`, `gui`; later `gpu`, `slow`) let CI and developers select subsets:
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
* `FractionalIndex` (Phase 2): `between(a, b)` is strictly between; repeated insertion at the same
  spot stays valid for 10 000 iterations; keys sort bytewise.
* UUIDv7: format bits, monotonicity within a millisecond and across clock regressions,
  counter overflow, parse/print round-trip *(Phase 1)*.

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
