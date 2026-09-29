# StudyBoard — Claude Code Project Memory

## 1. Project Identity

Project name: StudyBoard

Repository:
- GitHub: https://github.com/polaariiis/studying-app
- Local project directory is machine-dependent.

C++ namespace:
- `studyapp`

Executable:
- `studyapp`

Primary language:
- C++20

Build system:
- CMake 3.25+
- Ninja preferred

GUI:
- Qt 6.8 LTS
- Qt Widgets

Rendering:
- OpenGL 3.3 Core
- OpenGL must remain isolated behind the rendering abstraction.

Persistence:
- SQLite is the source of truth for structured application data.
- Large binary assets such as images/PDFs are stored externally using content-addressed storage.

Testing:
- GoogleTest
- GitHub Actions
- Cross-platform testing is required.

License:
- MIT

Target platforms:
- Windows 10 22H2+
- Linux equivalent to Ubuntu 22.04+
- macOS 13+

---

# 2. Product Vision

StudyBoard is a native desktop study/organization application combining:

- digital whiteboard functionality
- freehand drawing
- handwriting
- text
- shapes
- images
- PDF viewing/annotation
- pages
- notebooks
- sections
- study organization
- planning
- search
- export

The goal is NOT to build another Word-like document editor.

The intended workflow is:

1. Import/open external study material such as PDFs or images.
2. View/read the material inside StudyBoard.
3. Add annotations above it:
   - pen
   - highlighter
   - text
   - shapes
   - arrows/connectors
   - sticky notes
4. Save the workspace containing the source material and StudyBoard annotations.

The original external document remains the source material. StudyBoard annotations are separate structured objects/layers.

---

# 3. Current Development Status

Phase 1: COMPLETE

Phase 2: COMPLETE

Phase 3: COMPLETE (audited; B-list audit fixes P3-02..P3-08 applied, CI green on all platforms)

Phase 4: COMPLETE (canvas + OpenGL renderer; stabilization pass for pointer preview, frame pacing, resize and 10k-stroke performance; CI green on all platforms)

Phase 5: COMPLETE (application shell: navigation tree, page management, active page, per-page camera)

Phase 6: COMPLETE (final audit passed with non-blocking follow-ups)

Phase 7: COMPLETE (study/planning; audited, non-blocking follow-ups only)

Phase 8: IMPLEMENTED (search, PDF, export, bundles; uncommitted, awaiting review/commit)

Phase 8 established (details: docs/ROADMAP.md Phase 8, docs/ARCHITECTURE.md D43–D46,
docs/DATABASE_SCHEMA.md §6/§12, docs/CANVAS.md §10, docs/RENDERING.md §9):

- search: FTS5 index maintained in the write transaction (`persistence::SearchIndex`),
  `WorkspaceSession::search` + `application::search`, search field in the navigation panel
- PDF: `PageInfo::document` (schema v1 columns), `createDocumentSection`, `canvas::DocumentRasterizer`
  tiles, `ui::SessionDocumentRasterizer` (QtPdf, one worker thread), File ▸ Import PDF
- export/print: `ui::PageExport` (PDF/PNG/SVG, print) from the canvas meshes
- bundles: `persistence` Zip/Bundle (untrusted-input checks), `commands::importNotebooks`,
  `WorkspaceSession::exportBundle/importBundle/extractBundle`, File menu actions
- Qt modules now required: Pdf (add-on `qtpdf`), PrintSupport, Svg

Do NOT redo Phases 1–7 unless a concrete regression requires it.

Do NOT begin Phase 9+ work while implementing Phase 8 unless explicitly instructed.

Phase 7 established (details: docs/ROADMAP.md Phase 7, docs/DATA_MODEL.md §5,
docs/DATABASE_SCHEMA.md §11, docs/ARCHITECTURE.md D42):

- `study`: records `Course`, `Project`, `Task` (one level of subtasks, priority, floating due
  date/time, UTC time block, linked pages, tags), `Tag`; pure planning logic behind a
  `TimeZone` port (agenda, week, progress), unit-tested across time zones and DST
- D42: the document `Workspace` owns the study records (`document` depends on `study`);
  study edits are ordinary commands/patches — one undo history, one persistence path
- `persistence`: `StudyStore` over the existing schema v1 tables (no migration)
- `application::Planner`, `platform::QtTimeZone`, `ui::PlannerPanel` (View ▸ Planner,
  Ctrl+Shift+P: Today, Tasks by course/project, Week, Page tags + backlinks, task editor;
  built on first show; planner edits cost no canvas frame)
- not implemented: recurrence, reminders, study sessions, notebook ↔ course links, editing
  course details beyond the name, drag-and-drop reordering in the planner, multi-column week grid

Phase 6 established (details: docs/CANVAS.md, docs/RENDERING.md, docs/ARCHITECTURE.md D39–D41):

- tools: pen/highlighter styles, partial (vector) and whole-stroke eraser, shapes (line, arrow,
  rectangle, ellipse), text boxes (overlay editor), images (content-addressed assets, decoded off
  the GUI thread), connectors, selection handles/resize, cut/copy/paste (per-workspace clipboard)
- not implemented: rotation, handles on ink strokes, layers panel, cross-workspace/OS clipboard

Phase 5 established: `PageNavigator`, `WorkspaceStructure`, `NavigationPanel` +
`WorkspaceTreeModel`, `ShellDialogs`, welcome screen; failed writes are never closed over silently.

Phase 4 established (details: docs/CANVAS.md §13, docs/RENDERING.md §12, docs/ROADMAP.md
Phase 4, docs/ARCHITECTURE.md D30–D34):

- `canvas`: camera, input events, `DocumentPort` (D30), stroke pipeline (dedupe → One-Euro → RDP),
  spatial grid, scene, render cache (LOD refinement spread over frames), draw-call batching (D31),
  hit testing, selection, tools (pen, select/rectangle/move, stroke eraser, pan, zoom)
- `render` / `render_gl`: renderer API, CPU tessellation, OpenGL 3.3 core renderer (solid + pattern
  programs, GPU timer queries)
- `ui`: `CanvasWidget` (render on demand, vsync-paced; eraser ring is a platform cursor; debug HUD
  refreshed outside `paintGL`), default workspace (D34); `core::FrameTimings` for frame statistics
- continuous autosave through the Phase 3 session path; one gesture = one command = one undo step
- `bench/` (Google Benchmark, D32); the 10 000-stroke page pans at ≥ 60 fps with every stroke visible
  on the reference integrated-GPU laptop (measured at 144 Hz only)

Known limitations / deferred from Phase 4:

- not validated with a physical pen (pressure) or on a 60/120 Hz display; Linux/macOS rendering
  is covered by CI builds only (real-GL tests skip there)
- final-optimization candidates (Phase 9): first frame of a whole 10k-stroke page (160–250 ms),
  per-frame selection outlines for huge selections, move preview of partial selections on huge
  pages, autosave of 10k-element moves (applying the move itself is 2.8 ms since Phase 7)

Phase 3 established (details: docs/DATABASE_SCHEMA.md §11, docs/ARCHITECTURE.md D25–D29):

- SQLite schema v1 via embedded migration `0001_initial.sql`; migration runner with backup
- patch-driven persistence: `WorkspaceSession` writes the exact applied patch per edit/undo/redo
- load rebuilds the `Workspace` through `Workspace::apply` (invariants re-checked)
- content-addressed `AssetStore`, `WorkspaceFile` directory layout, `QLockFile` workspace lock
- single in-memory `Workspace` kept (catalog/page split deferred, D25)

Deferred from the Phase 3 audit:

- P3-01 (MEDIUM): a permanently failing write blocks the save queue. Closing is handled since
  Phase 5 (retry / close without saving / cancel); "save a copy" and recovery beyond closing
  remain open.
- P3-09..P3-12: asset durability/repair, asset checks in recovery, read-only media (Phases 8–9).
- P3-13: guard migration 0001 against edits before the first release.

The repository and existing documentation are the source of truth.

---

# 4. Completed Phase 1

Phase 1 established:

- repository/build structure
- CMake
- module targets
- C++20 baseline
- core foundation types
- Qt application shell
- basic theme infrastructure
- GoogleTest
- formatting/lint infrastructure
- GitHub Actions
- documentation
- licensing

Core foundation includes concepts such as:

- Vec2 / DVec2
- Rect / DRect
- Color
- UUID
- typed IDs
- Result / Error
- Timestamp / Clock
- deterministic test infrastructure
- FractionalIndex where required

---

# 5. Completed Phase 2

Phase 2 established the document model and undo architecture.

Hierarchy:

Workspace
  → Notebook
    → Section
      → Page
        → Layer
          → Element

Workspace owns the records.

Records are identified by stable typed IDs.

IDs use UUID-based identity and must not depend on:

- memory addresses
- vector position
- row order
- UI order

Document mutation occurs through patches/commands rather than arbitrary direct mutation.

Core model includes elements such as:

- Stroke
- TextBox
- Shape
- Image
- Connector

Elements use a common header plus variant-specific data.

Stroke point data can be shared between undo-history states where appropriate.

---

# 6. Patch / Command / Undo Architecture

Every meaningful document edit should follow the existing command → patch architecture.

Conceptually:

GUI
  ↓
Command
  ↓
Patch
  ↓
Workspace
  ↓
Undo/Redo

Commands produce data-only patches.

Patches contain complete before/after state where appropriate.

Patch application must preserve existing conflict/invariant behavior.

Failed commands must not pollute undo history.

New commands clear the redo stack.

Undo and redo must restore exact document state.

Do NOT introduce an alternate mutation mechanism without a strong architectural reason.

---

# 7. Important Domain Invariants

Preserve existing document invariants.

Among them:

- IDs must be unique and valid.
- Required parents must exist.
- Invalid parent relationships are rejected.
- Objects with required children cannot be deleted incorrectly.
- Every page must maintain at least one layer.
- Names must be valid/nonblank where required.
- Numeric values must be finite and within required ranges.
- Connectors must obey the existing page/element constraints.
- Patch conflicts must remain detectable.
- Patch application must remain atomic.

Never weaken an existing invariant just to make a feature easier to implement.

---

# 8. Current UI Philosophy

The visual design should be restrained and neutral.

Do NOT introduce:

- vibrant purple/violet/indigo branding
- blue/purple gradients
- neon effects
- glassmorphism
- glowing effects
- decorative gradients
- excessive rounded cards
- excessive pill-shaped controls
- excessive shadows

Current design uses a neutral monochrome-oriented palette with muted status colors where appropriate.

The UI should prioritize:

- clarity
- low visual noise
- usability
- functional density
- consistency

The existing DesignTokens system should remain the central location for visual constants.

Do not scatter arbitrary colors throughout the UI.

---

# 9. Architecture Principles

The project follows inward dependency direction.

The core/domain/engine layers should remain independent from framework-specific concerns where the architecture specifies that.

In particular, preserve these boundaries:

- document/core logic must not depend on Qt
- document/core logic must not depend on OpenGL
- canvas engine must not depend directly on persistence
- rendering abstraction must not depend directly on persistence
- persistence must not depend on UI
- UI must not bypass persistence architecture
- OpenGL-specific code belongs in `render_gl`

Qt should live at the UI/platform boundary.

OpenGL should remain behind the rendering abstraction.

SQLite should remain in persistence.

Do not casually move dependencies across these boundaries.

---

# 10. Known CMake / Module Structure

Current architectural targets include:

- `studyapp_core`
- `studyapp_document`
- `studyapp_study`
- `studyapp_render`
- `studyapp_canvas`
- `studyapp_persistence`
- `studyapp_application`
- `studyapp_render_gl`
- `studyapp_platform`
- `studyapp_ui`
- `studyapp`

Respect the existing module boundaries.

Before introducing dependencies, inspect the current CMake dependency graph.

---

# 11. Persistence Architecture

SQLite is the source of truth for structured application state.

Planned persistence model includes:

- relational entities
- workspace/document metadata
- ordered relationships
- structured records
- binary data where appropriate
- external content-addressed assets for large files

Large assets such as images/PDFs should not automatically become giant SQLite blobs.

Asset storage uses content-addressed storage based on hashes.

Existing database/persistence documentation must be read before implementing persistence.

Do not invent a second persistence system.

---

# 12. Workspace Concept

The eventual workspace is intended to be a directory containing things such as:

- `workspace.db`
- `assets/`
- `backups/`
- `temporary/`
- workspace lock information

Workspace locking/recovery/read-only behavior must follow the documented architecture.

Do not implement aggressive concurrent-write behavior unless explicitly required.

---

# 13. Phase Roadmap

Phase 1 — Foundation
COMPLETE

Phase 2 — Document model / undo
COMPLETE

Phase 3 — Persistence
COMPLETE

Focus:
- SQLite integration
- database schema implementation
- workspace save/load
- persistence of document state
- transactions
- patch-driven persistence
- loading/restoring document state
- persistence tests
- corruption/recovery considerations covered by the architecture
- proper asset handling foundations as specified

Phase 4 — Canvas / OpenGL MVP
COMPLETE

Focus:
- actual canvas
- world/screen coordinate handling
- OpenGL rendering
- basic interaction
- basic drawing
- camera/zoom/pan foundations

Phase 5 — Application shell
COMPLETE

Focus:
- notebooks
- sections
- page navigation
- application-level navigation
- workspace UI
- navigation structure
- shell refinement

Phase 6 — Rich canvas
COMPLETE (rotation, ink handles and a layers panel deferred)

Focus:
- pen
- eraser
- highlighter
- colors
- selection
- transforms
- shapes
- text
- layers
- images
- zoom/pan refinement
- better drawing/input behavior

Phase 7 — Study / planning
COMPLETE

Focus:
- study organization
- courses
- projects
- tasks
- planner
- related study workflows

Phase 8 — Search / PDF / export
IMPLEMENTED (uncommitted)

Focus:
- document search
- PDF viewing
- PDF-related study workflow
- annotations over source material
- import/export
- asset/document search
- additional productivity functionality

The built-in command interface is now planned as a POST-V1 update rather than a core Phase 8 requirement.

Phase 9 — Hardening / release

Focus:
- performance
- recovery
- packaging
- installer
- release artifacts
- clean-machine testing
- upgrade/uninstall testing
- cross-platform release verification

---

# 14. Terminal / Scripting — POST-V1 ONLY

The terminal/command interface should NOT become a Linux-like shell.

It is intended as a small power-user command layer / command palette.

The GUI remains responsible for visual operations:
- drawing
- handwriting
- selection
- moving objects
- page layout
- PDF annotation
- visual editing

The command layer handles:
- finding files
- searching material
- opening results
- simple workspace operations
- simple batch actions
- diagnostics
- text-oriented operations

Potential commands include:

- `find`
- `search`
- `open`
- `recent`
- `pages`
- `new`
- `info`
- `rename`
- `workspace`
- `backup`
- `help`

Do not expand this into a general-purpose shell unless explicitly requested.

Do not implement terminal/scripting before the main application is functionally complete.

When eventually implemented, command-based StudyBoard modifications MUST use the same existing command/patch/workspace/undo architecture:

GUI
  ┐
Terminal
  ├──> Command → Patch → Workspace → Undo/Redo
Scripts
  ┘

Arbitrary-PC file operations must have appropriate safety boundaries and confirmations where necessary.

Small scripting functionality can be added after the main v1 application is deployed and used.

---

# 15. Development Workflow

Before changing code:

1. Inspect the repository.
2. Read the relevant architecture/design documentation.
3. Check current Git state.
4. Understand existing implementation.
5. Identify the smallest correct change.
6. Implement.
7. Run relevant tests.
8. Run broader tests/builds when appropriate.
9. Inspect the diff.
10. Only then commit.

Do not blindly modify code based on assumptions.

---

# 16. Documentation First

Important documents include:

- `docs/Architecture.md`
- `docs/Roadmap.md`
- `docs/Building.md`
- `docs/DataModel.md`
- `docs/DatabaseSchema.md`
- `docs/CanvasArchitecture.md`
- `docs/RenderingArchitecture.md`
- `docs/TestingArchitecture.md`

Before implementing a major subsystem, read the relevant documents.

If implementation and documentation disagree:

1. Identify the discrepancy.
2. Determine whether the implementation or documentation represents the newer decision.
3. Do not silently rewrite architectural intent.
4. Ask for clarification when necessary.

---

# 17. Testing Expectations

Tests are part of implementation, not an afterthought.

Prefer logic that can be tested without GUI/GPU/display dependencies.

Existing testing principles include:

- deterministic clocks where required
- deterministic ID generation where required
- real SQLite for persistence tests rather than mocks where appropriate
- focused unit tests
- integration tests
- application/UI tests where required
- cross-platform CI

A change is not considered complete merely because it compiles.

---

# 18. Cross-Platform Requirements

The project must remain cross-platform.

Be particularly careful about:

- filesystem behavior
- path handling
- timestamp handling
- compiler differences
- libc++ vs libstdc++ vs MSVC STL
- warning-as-error behavior
- Qt deployment
- platform-specific APIs

Do not fix a Windows-only issue by introducing behavior that breaks Linux/macOS.

Existing CI coverage is important.

---

# 19. Git Rules

Git history is important.

DO NOT:

- rewrite history
- force-push
- reset away user work
- squash unrelated history
- rebase unnecessarily
- delete branches without reason

Normal workflow:

```text
git status
git diff
git log
implement
test
git diff
commit
push