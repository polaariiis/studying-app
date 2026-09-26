# StudyBoard

StudyBoard is a local-first, native desktop application for studying. It is planned to
combine handwritten and typed notes on an infinite canvas, drawing, PDF annotation,
notebooks, and course/project/task planning, all usable without an internet connection.

> **Status: early development — Phases 1–5 are complete.**
> StudyBoard opens and creates workspaces (a folder with a SQLite database, saved
> continuously), shows their notebooks, sections and pages in a navigation tree (create,
> rename, reorder, move, delete — all undoable) and draws on an OpenGL canvas with pen,
> selection, stroke eraser, pan and zoom. Rich canvas content (shapes, text, images,
> highlighter), planning, search and PDF support are **not implemented yet**; see the
> [roadmap](docs/ROADMAP.md).

## Technology

| Area | Choice |
|---|---|
| Language | C++20 |
| UI | Qt 6.8 LTS, Qt Widgets |
| Canvas rendering | OpenGL 3.3 core, isolated behind a renderer abstraction |
| Persistence | SQLite (WAL, relational schema, patch-driven writes); large assets as content-addressed external files |
| Build | CMake 3.25+, Ninja, CMake presets |
| Tests | GoogleTest, Qt Test, CTest |
| CI | GitHub Actions on Windows, Linux and macOS |

## Architecture at a glance

The code is split into small static libraries with enforced one-way dependencies. The
domain, canvas engine, render API, persistence and application layers are plain C++
without Qt, so they can be tested headlessly. Qt, OpenGL and SQLite are used only at the
edges.

```
app (studyapp executable)
 └─ ui (Qt Widgets) ── platform (Qt/OS adapters)
     └─ application ── persistence (SQLite, private)
         └─ document · study ─┐
 canvas ── document + render  ├─ core
 render_gl (OpenGL) ── render ┘
```

| Module | Responsibility | State (end of Phase 5) |
|---|---|---|
| `core` | Geometry, UUIDv7 ids, ordering keys, colours, `Result`, clock, logging | Implemented |
| `document` | Notebooks, sections, pages, layers, elements, patches, commands, undo/redo | Implemented (headless) |
| `study` | Courses, projects, tasks, planning | Skeleton (Phase 7) |
| `render` / `render_gl` | Renderer API / OpenGL 3.3 backend | Implemented: tessellation, solid + pattern programs, batching |
| `canvas` | Camera, tools, hit testing, selection | Implemented: pen, select/move, stroke eraser, pan/zoom |
| `persistence` | SQLite schema, stores, assets, workspace directory | Implemented: schema v1 + migrations, patch-driven stores, asset store |
| `application` | Sessions and use cases | `WorkspaceSession` (create/open/edit/undo/close, persisted); structure edits and the active page |
| `platform` | Qt-backed adapters, OS-specific code | Qt log sink, workspace lock (`QLockFile`) |
| `ui` | Main window, design tokens, themes, app icon | Application shell: workspaces, navigation tree, page management, the OpenGL canvas |

Read more in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), plus the
[data model](docs/DATA_MODEL.md), [database schema](docs/DATABASE_SCHEMA.md),
[canvas](docs/CANVAS.md), [rendering](docs/RENDERING.md), [testing](docs/TESTING.md) and
[build](docs/BUILDING.md) documents.

## Supported platforms

* Windows 10 22H2 or later (x64). This is the primary development platform (Visual Studio 2022 / MSVC).
* macOS 13 or later
* Linux distributions equivalent to Ubuntu 22.04 or later

## Prerequisites

* CMake 3.25+ and Ninja
* A C++20 compiler: MSVC (Visual Studio 2022 17.8+), GCC 13+, Clang 17+ or AppleClang 15+
* Qt 6.8 (Core, Gui, Widgets, OpenGL, Test). This is not needed for the Qt-free `core-only` build.
* Python 3 (optional; runs the module-boundary check as a test)

Platform-specific setup is described in [docs/BUILDING.md](docs/BUILDING.md).

## Configure

Tell CMake where Qt is installed, without committing local paths:

```powershell
$env:QT_ROOT_DIR = "C:\Qt\6.8.3\msvc2022_64"   # or: export QT_ROOT_DIR=~/Qt/6.8.3/gcc_64
cmake --preset debug
```

Alternatively, copy `cmake/CMakeUserPresets.example.json` to `CMakeUserPresets.json`
(this file is git-ignored) and use its `my-debug` preset. On Windows, run the commands
from a *Developer PowerShell for VS 2022*.

## Build

```sh
cmake --build --preset debug
```

Other presets: `release`, `relwithdebinfo`, `core-only` (no Qt required) and `asan` (GCC/Clang).

## Run the tests

```sh
ctest --preset debug
# or configure + build + test in one step:
cmake --workflow --preset debug
```

## Run the application

* Windows: run `build\debug\app\studyapp.exe`. The build copies the Qt runtime next to
  the executable (`windeployqt` post-build step), so Qt does not need to be on `PATH`.
* macOS: `open build/debug/app/studyapp.app`
* Linux: `./build/debug/app/studyapp`

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| 0 | Architecture | ✅ |
| 1 | Foundation & build skeleton | ✅ |
| 2 | Document model, commands, undo/redo (headless); app icon; design foundation | ✅ |
| 3 | SQLite persistence, assets, workspace locking | ✅ |
| 4 | Canvas engine + OpenGL renderer (first drawing) | ✅ |
| 5 | Navigation: notebooks, sections, pages | ✅ |
| 6 | Text, shapes, images, connectors, layers | Planned |
| 7 | Study & planning | Planned |
| 8 | Search, PDF annotation, export | Planned |
| 9 | Hardening, packaging, release | Planned |

Details and exit criteria for each phase are in [docs/ROADMAP.md](docs/ROADMAP.md).

## License

StudyBoard is released under the [MIT License](LICENSE). It uses third-party components
under their own licenses (Qt under LGPL-3.0, SQLite in the public domain, tl::expected
under CC0-1.0, and GoogleTest under BSD-3-Clause for tests only); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
