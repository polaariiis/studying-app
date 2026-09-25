# Roadmap

> Status: **Phase 0 and Phase 1 complete.** Phases are ordered by dependency; each ends
> with explicit exit criteria. Durations are intentionally not given yet.
>
> **Rule:** functionality is implemented only in its phase, even when the architecture
> documents already describe it. A phase adds the smallest foundations it needs and no
> speculative code for later phases.

| Phase | Status |
|---|---|
| 0 — Architecture | ✅ Done |
| 1 — Foundation & build skeleton | ✅ Done |
| 2 — Document model & undo | Next |
| 3–9 | Planned |

## Phase 0 — Architecture ✅

* Architecture, data model, schema, canvas, rendering, testing, build docs; final
  decisions recorded in ARCHITECTURE.md §18.

## Phase 1 — Foundation & build skeleton ✅

Delivered:

* Top-level CMake (3.25+), `cmake/` helpers, `CMakePresets.json` (debug, release,
  relwithdebinfo, core-only, asan, ci-core, ci-full + workflow presets), pinned
  dependencies (tl::expected, SQLite amalgamation, GoogleTest), install + Qt deployment.
* All eleven targets with the final dependency graph. Modules without Phase 1 content
  contain only a module anchor (ARCHITECTURE.md §3.2). Boundaries enforced by include
  roots, PRIVATE linkage, a Qt-free link test and `tools/check_boundaries.py`.
* `core`: `Vec2`/`DVec2`, `Rect`/`DRect`, `Color`, `Uuid` + `UuidV7Generator`, `Id<T>` +
  entity id aliases, `Error`/`Result`, `Clock`/`SystemClock`, `IdGenerator`, logging facade.
* `persistence`: SQLite dependency wiring and `sqliteLibraryInfo()`; `application`:
  `componentVersions()`; `platform`: Qt log sink.
* `ui` + `app`: `MainWindow` shell (menus, toolbar, placeholder, status bar, About dialog)
  with System/Light/Dark theme switching persisted in `QSettings`.
* Tests: GoogleTest for core/persistence/application/architecture, Qt Test for the UI;
  CTest integration with labels.
* GitHub Actions `ci.yml`: format + boundaries, core (3 OSes, no Qt), sanitizers,
  full build with Qt (3 OSes). `.clang-format`, `.clang-tidy`, `.gitignore`,
  `.gitattributes`, MIT `LICENSE`, `THIRD_PARTY_NOTICES.md`, README.

Deliberately deferred to their first consumer: `FractionalIndex`, `Signal`, `Affine2`
(Phase 2); nlohmann/json (Phase 3); renderer API and OpenGL code (Phase 4).

* **Exit:** clean checkout builds with the presets; all tests pass in Debug and Release;
  CI configured for Windows, Linux and macOS.

## Phase 2 — Document model & undo (headless)

* `core` additions: `FractionalIndex`, `Signal`, `Affine2`.
* Element model with all five payload kinds (data only), `PageDocument`,
  `WorkspaceCatalog`, `Patch`/`CatalogPatch`, invariant checks.
* Command functions for create/delete/move/transform/style/reorder/layers/pages.
* `Editor`, `UndoStack` with merging and budgets.
* Test builders and round-trip undo property tests.
* **Exit:** every command has unit tests and passes the undo round-trip property.

## Phase 3 — Persistence

* SQLite wrapper, migrations infrastructure, schema v1 (`0001_initial.sql`), nlohmann/json.
* Workspace directory format and **workspace locking** (ARCHITECTURE.md §11.1: OS lock +
  owner metadata, stale-lock detection, read-only / recover / cancel).
* `CatalogStore`, `PageStore` (all element kinds), stroke codec v1, rich-text JSON codec.
* `AssetStore` (import, dedupe, GC), `WorkspaceFile` (create/open/lock/backup).
* `WorkspaceSession`/`PageSession` in `application`, persisting patches **synchronously**
  on the GUI thread. The persistence writer thread + `PersistOp` queue are introduced
  when measurements require it (ARCHITECTURE.md §10, expected Phase 4).
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

## Resolved decisions (formerly open questions)

| Question | Decision |
|---|---|
| Product name / namespace | **StudyBoard**; namespace and executable `studyapp`; repository `studying-app` |
| Project license | **MIT**; third-party licenses in `THIRD_PARTY_NOTICES.md`; Qt linked dynamically (LGPLv3) |
| Minimum OS versions | **Windows 10 22H2+, macOS 13+, Linux equivalent to Ubuntu 22.04+** |
| Renderer | **OpenGL 3.3 core** only; QRhi/Vulkan/Metal/D3D are a future extension point, not planned |
| Threading | Introduced in stages; single-threaded through Phase 2 |
| Workspace locking | Exclusive writer with stale-lock detection and read-only fallback (Phase 3) |

## Still open

1. **PDF library** (Phase 8): QtPdf (PDFium-based) is the default proposal; MuPDF
   (AGPL/commercial) and Poppler (GPL) have licence implications for an MIT project.
2. **Workspaces in cloud-synced folders**: proposed "unsupported for the live workspace,
   supported for backups/exports" — to confirm before Phase 3.
3. **Rich-text editing scope for v1** (Phase 6): overlay editor with basic run styles.
4. **Pen hardware test matrix** (Phase 4): Wacom, Surface/Windows Ink, XP-Pen, …
