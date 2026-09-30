# Performance

How StudyBoard is built to stay responsive, what is measured, and where the known
bottlenecks are. The numbers are in [BENCHMARKS.md](BENCHMARKS.md) (everyday workloads,
regression thresholds), [STRESS_TESTING.md](STRESS_TESTING.md) (1 000–100 000 strokes) and
[CAPACITY.md](CAPACITY.md) (what that means in practice); the machines are in
[HARDWARE_MATRIX.md](HARDWARE_MATRIX.md).

Labels used in this document: **measured** — a number from a benchmark or the running
application on the reference laptop; **design** — how the code works (see the linked
architecture documents); **limitation** — known and accepted in 1.0; **future** — possible
optimisation, not scheduled.

## 1. Performance design

### Frames: render on demand, paced by the display

* **Design:** no render loop or timer. A frame is requested only when something visible
  changes — a gesture step, wheel or pinch, a document change, a view or tool change, or a
  pending level-of-detail refinement. Hovering requests nothing. Qt coalesces requests into
  one paint of the latest state ([RENDERING.md](RENDERING.md) §12).
* **Design:** frames are paced by the display's vertical sync (swap interval 1); nothing
  assumes 60 Hz.
* **Measured:** during interaction on the 144 Hz reference display, the median frame
  interval is one refresh period; an idle window draws no frames.

### Caching and level of detail

* **Design:** each element's triangle mesh is cached by content version and a power-of-two
  zoom bucket; it is rebuilt only when the element changes or is drawn at a finer bucket.
  Refinement for a finer bucket is spread over frames (at most 256 meshes per frame, the
  coarser mesh drawn meanwhile); new or changed content is built at once
  ([CANVAS.md](CANVAS.md) §13).
* **Design:** above 1 024 visible elements, runs of up to 256 consecutive elements are drawn
  as one merged mesh (batching, decision D31), keeping draw calls low on dense pages.
* **Design:** text boxes and images are textures refined when the zoom crosses a step, with
  at most 1 megapixel (text) or 2 megapixels (images) of re-rasterisation per frame; the
  previous texture is stretched meanwhile.
* **Design:** the selection outline of a large selection is built once per zoom level and
  translated while panning.
* **Design:** only the page shown is kept in the mesh cache; switching pages releases it.

### Work off the GUI thread

| Work | Where | Since |
|---|---|---|
| Building missing meshes and merged batch meshes | Up to 8 threads (at least 128 elements each), then uploaded on the GUI thread | Phase 9 (D47) |
| Copying and hashing an imported image or PDF | A thread pool; the GUI thread only stores the result (≈ 2 ms) | Phase 9 (D48) |
| Decoding images | Up to two worker threads | Phase 6 |
| Rendering PDF tiles (PDFium) | One worker thread | Phase 8 |
| Export (PDF, PNG, SVG) | GUI thread with a progress dialog and Cancel | Phase 8–9 |

Writing to SQLite happens on the GUI thread, one transaction per edit, as part of the edit
(continuous autosave; [DATABASE_SCHEMA.md](DATABASE_SCHEMA.md)).

### Memory budgets

| Cache | Limit | Eviction |
|---|---|---|
| Triangle meshes (render cache) | The page shown (released on page switch) | Per element on change; whole page on switch |
| Image textures | 256 MB | Least recently drawn |
| Image decodes not yet collected | 256 MB | Oldest first |
| Text rasters | 1 Mpx of refinement per frame; released on page switch | Per text box on change |
| PDF tile textures | 192 MB; 512 entries without a texture | Least recently drawn; out-of-view pending entries |
| PDF tiles rendered, not yet collected | 32 MB / 256 tiles | Oldest first; queued requests dropped on page switch |
| Open PDF documents on the worker | 4 | Least recently used |
| Automatic backups | 7 daily + 4 weekly | Rotation on each backup |

## 2. What is measured, and what is not

**Measured** (CPU side unless stated; [BENCHMARKS.md](BENCHMARKS.md)):
tessellation, scene queries, hit testing, frame building while panning and zooming, first
frames, drawing, erasing, copy and paste, opening workspaces, moving and saving, search,
planner edits, image import, text rasterisation, PDF import and tiles, export in three
formats, bundles; start-up time and memory (Phase 9); the GPU side in the running
application for the 10 000-stroke page (≥ 60 fps panning with every stroke visible).

**Not measured:** GPU frame times for other page sizes; physical pens (pressure and
latency); 60/120 Hz and high-DPI displays; Linux and macOS on real hardware (CI runs there
use software renderers — [HARDWARE_MATRIX.md](HARDWARE_MATRIX.md)); long sessions (memory
over hours of editing); PDF documents over 200 pages; pages with many images or text boxes
on the canvas; text editing latency.

## 3. Bottlenecks

Each entry: workload → observed cost → cause → what was done → what remains → possible next
step. Costs are measured on the reference laptop.

### 3.1 First frame of a large page seen whole

* **Workload:** opening or zooming out to a whole page of 10 000 strokes.
* **Observed (baseline):** 237 ms before Phase 9.
* **Cause:** tessellating every visible stroke and merging the batches, serially.
* **Done:** parallel mesh building and batch merging (D47); word-wise batch signatures.
* **Now (measured):** 88–100 ms at 10 000 strokes; 491 ms at 50 000; 1.06 s at 100 000.
* **Future:** persistent meshes across page switches, or coarser meshes for strokes that
  are a few pixels long at whole-page zoom.

### 3.2 Panning a very dense page with everything in view

* **Workload:** 50 000–100 000 strokes, whole page visible.
* **Observed (measured):** 12 ms (p50) per frame at 50 000 and 29 ms at 100 000 (CPU side),
  below 144 Hz and at 100 000 below 60 Hz; up to 25 000 strokes stays within 6 ms.
* **Cause:** culling and batch bookkeeping per visible element every frame.
* **Done:** batching, cached selection outlines (8.5 → 0.2–0.5 ms per frame with 10 000
  selected).
* **Now:** zooming in restores smooth panning (≤ 2.4 ms p95 at 50 000 strokes, zoom 1).
* **Future:** a cached whole-page layer (render once, pan the texture) at low zoom.

### 3.3 Large selections: moving and saving

* **Workload:** Select All on 10 000 strokes and drag.
* **Observed (measured):** 123 ms once when the drag ends (command 12 ms, applying 3 ms,
  about 100 ms writing 10 000 rows); 807 ms at 50 000; 1.48 s at 100 000. The drag itself is
  previewed without writing.
* **Cause:** one SQLite `UPDATE` per moved element, on the GUI thread (the autosave is part
  of the edit).
* **Done:** applying the move was made cheap (2.8 ms) in Phase 7; the write stays one
  transaction.
* **Limitation / future:** a bulk-update path or deferred writing for very large moves
  (not worth the complexity for a rare gesture at typical sizes).

### 3.4 Partial erasing across many long strokes

* **Workload:** the partial (vector) eraser across 300 long strokes on a 10 000-stroke page.
* **Observed (measured):** 77 ms p50 when the gesture ends (was 98 ms); 21 ms in normal
  writing.
* **Cause:** each cut stroke becomes new pieces whose meshes and batches are rebuilt.
* **Done:** the cut pieces are rebuilt in parallel (D47).
* **Future:** incremental batch updates for the pieces.

### 3.5 Text rasterisation

* **Workload:** zooming across pages with text.
* **Observed (measured):** 0.13 ms per text box at 1×, 1.65 ms at 4.2×.
* **Cause:** Qt text layout and rasterisation per box.
* **Done:** refinement is limited to about 1 megapixel per frame (≈ 4 ms) and spread over
  frames; kept on the GUI thread because no measurement showed a stall.
* **Future:** a worker thread if profiling ever shows a frame stall.

### 3.6 Importing large images and PDFs

* **Workload:** inserting a 50 MB image.
* **Observed (baseline):** 393 ms frozen on the GUI thread.
* **Cause:** copying and hashing the file for content-addressed storage.
* **Done:** staged on a thread pool (D48).
* **Now (measured):** 2.2 ms on the GUI thread, 394 ms in the background.

### 3.7 PDF tiles

* **Workload:** reading an imported PDF.
* **Observed (measured):** 3.9–6.4 ms per tile on a worker thread; 55 ms to import a
  200-page PDF.
* **Cause:** PDFium rendering.
* **Done:** one worker thread, tile textures cached (192 MB), queued requests dropped when
  the page changes.
* **Limitation:** tiles appear progressively on fast scrolling through long documents
  (page navigation through long documents is not measured).

### 3.8 Export

* **Workload:** exporting a page.
* **Observed (measured):** PDF 35 ms per 100 strokes (3.1 s for 10 000); PNG 5–12 s for a
  page 6000 units wide (bounded by the 64-megapixel cap); SVG 153 MB for 10 000 strokes.
* **Cause:** QPainter's PDF/SVG backends and page area for PNG.
* **Done:** ink as stroked outlines instead of triangles (PDF 4× faster, 3× smaller, D51);
  progress dialog and cancellation.
* **Limitation:** SVG size is Qt's per-element output; export runs on the GUI thread
  behind a modal progress dialog.

### 3.9 Search over very many text boxes

* **Observed (measured):** 23 ms for 100 000 text boxes (bm25 ranking over the matches).
* **Done:** FTS5 index maintained in the edit's transaction; typing debounced by 150 ms.
* **Now:** no noticeable cost while typing.

### 3.10 Planner with many tasks

* **Observed (measured):** creating a task costs 0.33 ms with 100 to 10 000 tasks.
* **Limitation:** planner views with very large lists were not measured (the planner is
  built when first shown and its edits cost no canvas frame).

### 3.11 Backups and Check Workspace on the GUI thread

* **Workload:** Back Up Now, the daily backup when closing, and Check Workspace.
* **Observed:** a backup is one SQLite `VACUUM INTO` (proportional to the database size);
  Check Workspace hashes every asset (proportional to the assets' total size). Not
  benchmarked; seconds for gigabytes of assets.
* **Limitation (accepted for 1.0):** they run on the GUI thread without progress.
* **Future:** run them in the background with progress.

## 4. How to measure

| What | How |
|---|---|
| Benchmarks | `python tools/run_benchmarks.py --build build/ci-full` ([BENCHMARKS.md](BENCHMARKS.md)) |
| Start-up and memory (Windows) | `tools/measure_startup.ps1 -Exe build/release/app/studyapp.exe` |
| Frame timing in the application | View ▸ Debug HUD (F3): p50/p95/p99, worst, CPU per frame |
| GPU panning benchmark | `studyapp --bench-pan` (see `studyapp --help`) |
