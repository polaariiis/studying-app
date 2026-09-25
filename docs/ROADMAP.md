# Roadmap

> Status: **Proposed — awaiting review.** Phases are ordered by dependency; each ends
> with explicit exit criteria. Durations are intentionally not given until Phase 1 has
> calibrated velocity.

## Phase 0 — Architecture ✅ (this change)

* Architecture, data model, schema, canvas, rendering, testing, build docs.
* **Exit:** documents reviewed; open questions (below) answered or explicitly deferred.

## Phase 1 — Foundation & build skeleton

* Top-level CMake, `cmake/` helpers, `CMakePresets.json`, dependency pinning.
* Empty module targets with the final dependency graph; forbidden-include CI check.
* `core`: geometry, `Uuid`/`Id<T>`, `Color`, `FractionalIndex`, `Result`, `Signal`,
  logging facade, `Clock`/`IdGenerator`.
* `app` + `ui`: a `MainWindow` shell that starts, with light/dark theme switching.
* GitHub Actions `ci.yml` (format, core, full on 3 OSes), `.clang-format`, `.clang-tidy`,
  `.gitignore`, `.gitattributes`, `README`, licence.
* **Exit:** green CI on Windows/Linux/macOS; `core` tests ≥ 90 % coverage;
  `cmake --workflow --preset debug` works from a clean checkout on all three OSes.

## Phase 2 — Document model & undo (headless)

* Element model with all five payload kinds (data only), `PageDocument`,
  `WorkspaceCatalog`, `Patch`/`CatalogPatch`, invariant checks.
* Command functions for create/delete/move/transform/style/reorder/layers/pages.
* `Editor`, `UndoStack` with merging and budgets.
* Test builders and round-trip undo property tests.
* **Exit:** every command has unit tests and passes the undo round-trip property.

## Phase 3 — Persistence

* SQLite wrapper, migrations infrastructure, schema v1 (`0001_initial.sql`).
* `CatalogStore`, `PageStore` (all element kinds), stroke codec v1, rich-text JSON codec.
* `AssetStore` (import, dedupe, GC), `WorkspaceFile` (create/open/lock/backup).
* Persistence writer thread + `PersistOp` queue; `WorkspaceSession`/`PageSession`
  in `application` with `ImmediateExecutor` tests.
* **Exit:** create workspace → edit via commands → close → reopen yields identical
  model; crash-ordering tests for assets pass; migration test harness in place.

## Phase 4 — Canvas & OpenGL MVP (first "real" drawing)

* `Camera`, `CanvasScene` (spatial grid, draw order, render cache), `CanvasController`.
* Stroke tessellation, `render::Renderer`, `OpenGLRenderer` (solid + pattern programs),
  `CanvasWidget`.
* Tools: Pan/Zoom, Pen (pressure, smoothing, simplification), Stroke eraser,
  Select (click/rect) + move, Delete, Undo/Redo.
* Infinite and bounded pages with ruled/grid/dot backgrounds. Continuous autosave.
* Debug HUD, profiling macros, first benchmarks (tessellation, grid, page load).
* **Exit:** draw → close app → reopen: ink is identical; 10k-stroke page pans at 60 fps
  on a reference integrated-GPU laptop; no GL calls outside `render_gl`.

## Phase 5 — Application shell & navigation

* Notebook/section/page tree (create, rename, reorder by drag, move, trash/restore).
* Page templates (size, background), page thumbnails (cache dir), recent pages.
* Workspace create/open/switch, app settings (QSettings), themes finalised.
* **Exit:** a user can organise notes across notebooks and sections without touching
  the file system; all structure edits are undoable.

## Phase 6 — Rich canvas content

* Shapes (rect, ellipse, line, polygon), connectors (attach/detach, arrowheads),
  images (import, crop, resize), text boxes (overlay editing, rich text model v1),
  highlighter (stencil no-overlap), partial eraser, lasso selection, rotate/scale handles,
  layers panel, z-order commands, copy/paste (internal format + images/text from OS
  clipboard).
* **Exit:** all element kinds are creatable, editable, undoable, persisted and
  hit-testable with tests in each layer.

## Phase 7 — Study & planning

* Courses, projects, tasks (subtasks, priorities, due dates, time blocks), tags on pages
  and tasks, task ↔ page links and backlinks.
* Views: Today/agenda, list by course/project, simple week calendar.
* **Exit:** agenda logic fully unit-tested across timezones; planner usable end-to-end.

## Phase 8 — Search, PDF, export

* FTS5 search (page titles, text boxes, tasks) with jump-to-result.
* PDF import (one bounded page per PDF page, `DocumentRasterizer` tiles) and annotation.
* Export page/section to PDF, PNG, SVG via `QtPageExporter`; print.
* Workspace/notebook export bundles and import.
* **Exit:** annotate a 200-page PDF smoothly; exported PDF matches on-screen content.

## Phase 9 — Hardening & release

* Profiling-driven optimisation (batching/arenas, LOD, texture budgets, page-load
  streaming), backup rotation and integrity check UI, crash-safe startup checks.
* Packaging (installers/DMG/AppImage), signing, release workflow, auto-backup defaults.
* Accessibility pass (keyboard navigation, screen-reader labels in UI chrome).
* **Exit:** 1.0 release candidate on all three platforms.

## Later / candidate features

Recurrence & reminders · study session timer & statistics · handwriting recognition and
OCR for search · shape recognition (snap hand-drawn shapes) · audio recording synced to
ink · LaTeX/math · QRhi backend (Metal/Vulkan/D3D) · sync between devices (op log) ·
Qt Quick tablet UI · spaced-repetition flashcards from notes.

---

## Open questions for review

1. **Product name / namespace.** Docs use the placeholder namespace `studyapp` and
   executable `studyapp`. Rename before Phase 1 if a name is chosen.
2. **Licence** of the project itself — affects Qt (LGPLv3 obligations: dynamic linking,
   relinkability) and the choice of PDF library.
3. **PDF library**: QtPdf (PDFium-based, ships with Qt) is the default proposal;
   MuPDF (AGPL/commercial) and Poppler (GPL) are alternatives with licence implications.
4. **Minimum OS versions**: proposed Windows 10 22H2+, macOS 13+, Ubuntu 22.04+/equivalent.
5. **Workspace location in cloud-synced folders**: proposed "unsupported for the live
   workspace, supported for backups/exports". Acceptable?
6. **Rich-text editing scope for v1**: overlay editor with basic run styles — enough?
7. **Pen hardware** to validate on (Wacom, Surface/Windows Ink, iPad-as-tablet via
   Sidecar, XP-Pen): defines the Phase 4 test matrix.
