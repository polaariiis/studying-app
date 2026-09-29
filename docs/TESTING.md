# Testing Architecture

> Status: **Final baseline.** §0 lists what exists after Phase 5; the rest of this
> document is the plan that later phases follow.

## 0. Current state (Phase 5)

| Test target | Label | Framework | Covers |
|---|---|---|---|
| `core_tests` | `unit` | GoogleTest | `Vec2`/`DVec2`, `Rect`/`DRect`, `Color`, `Uuid`, `Id<T>`, `UuidV7Generator`, `SystemClock`, `Result`/`Error`, logging facade, `FractionalIndex` (incl. randomised insertion and key-growth tests), `FrameTimings` (nearest-rank percentiles, jitter, idle gaps start a new burst, window) |
| `study_tests` | `unit` | GoogleTest | Study records' value checks, tag-name folding, storable dates; local dates, instants and midnights in fixed zones (−12:00…+13:45) and daylight-saving zones (skipped and repeated hours, 23/25-hour days); ISO weeks; the agenda across zones (floating due dates, time blocks over midnight, ordering by time/priority/order, horizon); the week view (blocks cut at midnight across a daylight-saving change, cancelled left out); progress |
| `document_tests` | `unit` | GoogleTest | Workspace hierarchy, lookup, ordering, uniqueness, invariants; patches (apply, inverse, conflicts, atomic rollback); commands (connector index and `setConnectorEnds`; `resizeElement` maps attached connector ends; `pasteElements` (new ids, order on top of the target layer, offset, connectors re-attached among copies and detached otherwise, also a connector drawn below its target, shared asset, one undo step, undo/redo, errors); `editText`; shape kinds validated; `splitStrokes`: style, ids and draw order of pieces, connectors, undo, errors; study commands (courses, projects, tasks, subtasks one level, move, complete/reopen labels, tags unique ignoring case, page tags, task–page links and backlinks, deleting pages/sections/notebooks unlinks tasks, deletes clear references, invalid references rejected, exact undo/redo); success, failure, determinism, cascades); undo/redo (empty history, multiple steps, redo-branch clearing, failed commands, capacity, exact state restoration, nested 10-step scenario); element local/world bounds; moving notebooks/sections/pages (reorder, move across parents, undo, repeated moves keep a total order) |
| `render_tests` | `unit` | GoogleTest | Tessellation: empty/degenerate/non-finite input, discs, width and topology of straight lines, pressure-varying width, round (spike-free) sharp turns, bounded miters, circle subdivision, fills, outlines, ellipses, mesh concatenation (linear: reallocations counted over 10 000 appends) |
| `canvas_tests` | `unit` | GoogleTest | Camera (round trips over ±10⁷ and all zoom levels, cursor-centred zoom incl. clamping, pan∘zoom, viewport/DPR changes, fit, invalid input); One-Euro and RDP (determinism, jitter, lag, tolerance guarantee, pressure, 10 000-point worst case); spatial grid vs brute force; scene draw order, incremental = rebuild, versions, hidden layers, removals; render cache invalidation and LOD; hit tests; `CanvasController` scenarios through an Editor-backed port and a recording renderer: pen (world alignment after zoom/pan/resize/DPR, pressure, width, live preview, cancel, read-only, bounded pages), select (click, Shift, rectangle intersect/contain, zoomed), move/delete/erase as one command each with undo/redo, pruning, navigation, culling, stale-cache freedom, backgrounds, context reset, batching (same triangles, rebuild only on change, preview fallback), tool settings (decide new strokes only, sanitized, a stroke keeps its starting style), highlighter (own style and cursor, pen style kept, even width at any pressure, sanitized, undo/redo of mixed ink, tool and style changes rebuild no mesh or batch), batch meshes number their parts, partial eraser (one command, ids and draw order kept, gap at the eraser, preview drawn from pieces and released, cancel/miss change nothing, highlighter style kept, only nearby strokes touched), mid-page insertions rebuild only nearby runs; shape tool (each kind one command with a live preview, clicks create nothing, select/move/delete, sanitized style that decides new shapes only); text (the tool starts an edit and writes nothing until it is finished, one command, UTF-8 kept, blank boxes not created, cancel, editing in place, clearing removes, undo ends an edit, one raster per content change, √2 zoom steps within a pixel budget, textures released, text boxes keep painter's order in batches); connectors (attach at element edges with an arrowhead, free ends, clicks make nothing, attached ends follow in the move preview and the command, endpoint drag detaches and re-attaches, delete detaches, a move touches only the moved element's connectors); clipboard (copy/paste of a mixed selection as one command, copies selected and offset per paste, cut is one step labelled Cut and pastes in place, other pages keep the position when entirely in view and centre it otherwise, read-only); selection handles (`handlesFor`/`dragHandle`: kinds per element, fixed opposite corner, flips, aspect on image corners and Shift, text width with laid-out height, 45° line snap, minimum size; a handle drag previews without writing and commits one command, also on a batched page, connector and line ends move, also of lines at an angle, no handles when read-only); images (one texture per asset at the resolution the view needs, zooming out keeps it, page switch releases, decoded elsewhere and shown when ready, missing assets drawn as a frame and not retried, textures within the memory budget, insertImage fits the view as one selected command); `ShapeBuilder` (boxes from any drag direction, Shift squares, lines/arrows along their rotation, 45° snap, no-extent drags, arrow head mesh and hit test, visual bounds); `StrokeEraser` geometry (capsule intervals, exact cuts in long segments, beginning/end/several regions, repeats and misses change nothing and copy nothing, tiny fragments, self-intersections, highlighter ink radius, long strokes), page switches release the previous page's meshes, planner edits (tasks, tags, page tags) cost no canvas frame while the page format does, hover requests no repaint (every tool), eraser ring is the cursor and never rendered content, level-of-detail refinement within a per-frame budget (frames requested until done, content never deferred), runs moving as a whole stay one draw |
| `persistence_tests` | `integration` | GoogleTest | Real SQLite in temp directories (Unicode paths). Linked SQLite requirements; `Database`/`Statement`/`Transaction` (pragmas, storage classes, error mapping, commit/rollback, failed commit); migrations (new schema = documented tables, WAL, application_id, integrity/FK check clean; no-op re-run; upgrade harness with backup; failed migration rolls back; newer schema refused read-write; foreign databases refused); stroke codec (bit-exact round trip, malformed blobs) and SHA-256 vectors; `WorkspaceFile` (layout, create/open/reopen, read-only, backup, integrity check, temp cleanup, moved directory); `WorkspaceStore` (every record and element kind round-trips field by field, UUID/parent/order preservation, renames, updates, kind change, layer move, deletes + cascades, undo/redo patches, move does not rewrite stroke blobs, content_version, failed patch leaves the database unchanged, repeated cycles, 20 corrupt-data cases; *(Phase 7)* ISO dates, every study field and link round-trips, join-table diffs, cascades and undo/redo writes, task order and subtasks, 7 corrupt study-data cases); `AssetStore` (content addressing, dedupe, crash ordering → orphan file only, GC grace period and references, ON DELETE RESTRICT, verify) |
| `application_tests` | `integration` | GoogleTest | `componentVersions()`; partially erased ink survives reopen; every shape kind (arrow, fill) survives reopen; Unicode multi-line text survives reopen; images reference one deduplicated stored asset and survive reopen; connectors keep their attachments across reopen; a handle-resized shape survives reopen; a pasted mixed selection (stroke, shape, text, image, connector) survives reopen with new ids, re-attached connector and shared asset; every Phase 6 edit on one page (pen, highlighter, shapes, connector, text, image, re-attach, partial erase, resize, move, paste, cut) undone step by step to each exact earlier state, redone, reopened, edited and reopened again; pen and highlighter styles persisted with each stroke (brush, colour with alpha, width, bit-exact points, drawn in stored colours after reopen); shell workflows (`WorkspaceStructure`: default names, compound creation as one undo step, rename/delete/move, read-only; `PageNavigator`: neighbour on delete and undo, previous/next; `pageChangedBy`; create → edit → switch page → return → reopen); canvas + session end to end (draw/zoom/pan/move/delete/erase/undo → autosave → reopen: identical ink, ids, order), start page, patch listener; `WorkspaceSession`: create → commands → close → reopen equality for a complex workspace (all element kinds, attached connectors, assets, renames, deletes, undo/redo), repeated edit/reopen cycles, rejected commands change nothing, write failure keeps edits queued and retries (fault injection via a second connection), `Planner` (one step per operation, default names, tags by name created in one step, scopes by course/project, links across structure deletes, reopen), locking (active → read-only only, stale → explicit recovery with integrity check and temp cleanup) |
| `ui.CanvasWidgetTest` | `gui;gpu` | Qt Test | Real widget + OpenGL + session: scripted mouse/wheel drawing aligned with world coordinates, ink visible in the framebuffer, undo action, reopen with identical ink; blank/ruled/grid/dot backgrounds and bounded pages by pixel sampling, selection frame, debug HUD on/off; ink stays under the pointer after resizes and the framebuffer is logical size × DPR; using the navigation tree paints no canvas frame; ink is drawn in the chosen colour and width; text boxes are drawn as text from a texture, only inside their box; images are drawn from their decoded asset; one mesh handle updated between plain and part-numbered data (the part buffer is created, dropped and re-created; pixels show one or two blends); the highlighter is translucent, even where a stroke doubles back over itself, 14 px wide, keeps pen ink visible and builds up across strokes, drawn per element and batched; the eraser ring platform cursor (centred hot spot, applied on tool change; runs without OpenGL too). The OpenGL cases skip without a 3.3 context (the offscreen platform in CTest/CI); run `canvas_widget_tests` directly on a desktop (`STUDYAPP_TEST_SCREENSHOTS=<dir>`, `STUDYAPP_TEST_THEME=dark`) |
| `platform.QtTimeZoneTest` | `integration` | Qt Test | `QtTimeZone`: Europe/Berlin and America/Los_Angeles offsets in winter/summer and at the change instant, a 23-hour spring day through the study functions, unknown names fall back to UTC, the system zone |
| `platform.QtWorkspaceLockerTest` | `integration` | Qt Test | `QLockFile` adapter: free/active/exclusive, release, stale lock of a dead process, other host never stale, unreadable lock file |
| `architecture_tests` | `architecture` | GoogleTest | All Qt-free modules link into one plain C++ executable |
| `architecture.include_boundaries` | `architecture` | Python script | `tools/check_boundaries.py`: forbidden includes / GL calls per module |
| `ui.MainWindowTest` | `gui` | Qt Test (offscreen) | Main window structure, theme switching, theme actions, settings persistence, application icon resources, neutral palette (zero-saturation tokens), stylesheet template fully resolved |
| `ui.ShellTest` | `gui` | Qt Test (offscreen) | Navigation tree model (`QAbstractItemModelTester`; renames are `dataChanged` only, structure changes are row inserts/removals/moves, canvas edits emit nothing; active page; drag-and-drop validation); the window with real sessions, locks and scripted dialogs: new workspace starts on a page, navigation and New Page/Section/Notebook, undo shows the undone page, rename/delete (confirmation)/move, closing asks nothing, a workspace in use opens read-only, invalid workspace reported, stale lock (cancel / read-only / recover), a failing start-up workspace is forgotten, opening an empty workspace writes nothing, a collapsed section stays collapsed while drawing, theme toggle icon, navigation toggle, dropped page stays visible, failed writes are never closed over silently (cancel / retry / close without saving), the pen style decides new strokes and is remembered, the highlighter tool with its own remembered style (style button follows the ink tool, choosing is not an edit, undo), tool letters do not fire while the tree has focus; eraser modes through the shell (partial default, whole strokes, remembered); shapes through the shell (kind, ink, width, fill, one undo step each, remembered); Insert Image through the shell (one stored copy per content, selected element, non-images refused, undo); text through the shell (editor overlay, letters and Ctrl+Z stay in the editor, Escape and focus-out finish, re-editing, undo; typed text is written when the page changes or the workspace closes from the keyboard, a popup keeps the edit); uncollected image decodes are bounded and decoded again when asked for; cut/copy/paste through the shell (Ctrl+A/C/V on the canvas, one undo step, Select tool chosen, Cut action, the same keys in the text editor edit its text); the planner through the shell (off by default, a course from the New menu, quick-added task in the scope and selected, title and due date edits, Today shows it, ticking completes and Undo reopens, page tags by name and a page-linked backlink, letters in its lists are not tool shortcuts, a canvas stroke rebuilds nothing, tags typed before a page switch stay with their page, a title typed before closing is saved, everything after reopening); every window workflow runs under `QAbstractItemModelTester` |

**Phase 8 additions.** `document_tests`: `createDocumentSection` (one bounded page per PDF
page, titles, sizes, one undo step; invalid input and null assets rejected atomically) and
`importNotebooks` (ids kept when free, every id remapped when one is taken, connectors
re-attached to the copies, tags reused by name or created, assets mapped, one undo step,
invalid requests). `persistence_tests`: document pages round-trip (`bg_asset_id`,
`bg_page_index`) through undo/redo, with annotations; the asset is protected by its foreign
key. `canvas_tests`: document pages draw a preview then only the tiles in view (bounded,
nothing re-rendered on unchanged frames), tiles rendered elsewhere appear when ready,
unreadable pages are not retried, uploads spread over frames then idle, leaving a PDF page
drops its queued tiles, textureless tile entries stay bounded, level selection.
`application_tests` (`SearchTest`, `BundleTest`): the index follows edit/undo/redo/move/
rename/delete, tasks, Unicode and diacritics, reopen, re-indexing of an older workspace
(read-only sessions report it), limits and snippets; a workspace bundle opens as an equal
workspace; notebook bundles import as one step and twice as a copy, survive reopen;
targets inside the workspace are refused however they are spelled; hostile bundles (path
traversal, absolute and backslash names, an injected trigger, flipped bytes, truncation,
foreign manifest, newer schema, tampered asset, non-empty target) are refused and leave
nothing behind. `ui.ShellTest`: search through the shell (debounce, jump to page, text box
and task, results follow edits but not ink), Import PDF (inspection, section, tiles from a
generated PDF, rendered pixels, out-of-range tiles, dropped requests, the source and the
stored copy byte-identical, annotation survives reopen), invalid PDFs (text, empty, bare
header, missing, truncated), export and print (PNG, PDF and SVG compared pixel by pixel,
highlighter overlap not darker, text laid out as the canvas raster, section export of an
imported PDF with its pages and sizes, print ranges, cancel, refused targets, nothing
changed), bundles through the shell (export notebook/workspace, import, a damaged bundle,
open as a workspace).

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
