# StudyBoard

StudyBoard is a desktop study workspace for Windows, Linux and macOS. Open your lecture PDFs
and images, write and draw on them with pen and highlighter, add typed notes, shapes and
arrows, and keep everything organised in notebooks, sections and pages — next to a planner
for your courses, projects and tasks.

It works offline and keeps your work on your own disk: a workspace is an ordinary folder,
saved continuously as you work, with automatic backups. It is a native C++ application
built with Qt, drawing with OpenGL.

## Download

**[Download StudyBoard 1.0.0 from the releases page](https://github.com/polaariiis/studying-app/releases/latest)**

| Platform | Package |
|---|---|
| Windows x64 | [Installer (`.exe`)](https://github.com/polaariiis/studying-app/releases/download/v1.0.0/StudyBoard-1.0.0-windows-AMD64.exe) · [Portable ZIP](https://github.com/polaariiis/studying-app/releases/download/v1.0.0/StudyBoard-1.0.0-windows-AMD64.zip) |
| Linux x86_64 | [AppImage](https://github.com/polaariiis/studying-app/releases/download/v1.0.0/StudyBoard-1.0.0-linux-x86_64.AppImage) · [`.tar.gz`](https://github.com/polaariiis/studying-app/releases/download/v1.0.0/StudyBoard-1.0.0-linux-x86_64.tar.gz) |
| macOS, Apple silicon | [Disk image (`.dmg`)](https://github.com/polaariiis/studying-app/releases/download/v1.0.0/StudyBoard-1.0.0-macos-arm64.dmg) |

Every package includes everything it needs; you do not have to install Qt or anything
else. Checksums (`SHA256SUMS.txt`) are attached to the release.

> **The packages are not code-signed.** On first start Windows shows "Windows protected
> your PC" (choose *More info* ▸ *Run anyway*) and macOS blocks the app (right-click it ▸
> *Open*). See [docs/RELEASE.md](docs/RELEASE.md) for details and checksum verification.

## Current release

| | |
|---|---|
| Version | 1.0.0 |
| Status | First stable release |
| Platforms | Windows x64 · Linux x86_64 · macOS arm64 |
| Signing | Unsigned; signing and notarisation are prepared for when certificates exist |

## Features

* **Organise** — workspaces with notebooks, sections and pages; drag to reorder; undo and
  redo for every change.
* **Write and draw** — pen and highlighter (with tablet pen pressure), partial and whole-stroke eraser,
  lines, arrows, rectangles, ellipses, text boxes, images and connectors.
* **Edit** — select, move and resize; cut, copy and paste; styles per tool.
* **PDFs** — import a PDF as pages and annotate on top of it; the original stays unchanged.
* **Plan** — courses, projects, tasks with subtasks, priorities, due dates, time blocks and
  tags; Today, Tasks and Week views; tasks linked to pages.
* **Search** — full-text search across page titles, typed notes and tasks.
* **Export and share** — pages to PDF, PNG and SVG, whole sections to PDF, printing; notebooks
  and workspaces as single-file bundles.
* **Safety** — continuous saving, automatic daily backups, Back Up Now, Check Workspace, and
  Save a Copy when a disk keeps failing.
* **Accessibility** — every control is named for screen readers; F6 moves between panes.

## System requirements

| | Requirement |
|---|---|
| Windows | Windows 10 22H2 or later, x64 |
| Linux | x86_64 distribution equivalent to Ubuntu 22.04 or later (built on glibc 2.35; tested under X11, Wayland untested) |
| macOS | macOS 13 or later on Apple silicon (arm64); no Intel build |
| Graphics | **OpenGL 3.3** (core profile). On a Windows PC without such a driver the canvas cannot show pages; `studyapp --self-test` reports it |
| Disk | About 110 MB installed on Windows; workspaces grow with your content (PDFs and images are stored once each) |
| Runtime | Included: Qt 6.8 and its plugins. No compiler, Qt or other runtime to install |

Memory: StudyBoard uses about 260 MB right after start on the reference laptop (8 GB RAM),
mostly the graphics driver; more with large pages ([docs/CAPACITY.md](docs/CAPACITY.md)).
No minimum beyond the operating system's has been established.

## Performance and tested hardware

Tested baseline — the machine all published performance figures come from:

| | |
|---|---|
| Machine | HP Victus 15 laptop |
| CPU | AMD Ryzen 5 8645HS (6 cores, 12 threads) |
| GPU | AMD Radeon 760M (integrated) |
| RAM | 8 GB |
| Display | 1920 × 1080, 144 Hz |
| OS / build | Windows 10 22H2, Release build of 1.0.0 |

On it, a workspace with a 10 000-stroke page opens in about 0.1 s, and that page pans
smoothly even with every stroke in view (under 2 ms of CPU work per frame; at least 60 fps
in the running application). Pages of up to about 25 000 strokes stay within the 60 Hz frame
budget; 100 000 strokes still work, more slowly. These are measurements on the listed hardware, not guarantees for other
systems. Linux and macOS are tested in CI on virtual machines with software OpenGL, not yet
on physical hardware.

Details: [performance](docs/PERFORMANCE.md) · [benchmarks](docs/BENCHMARKS.md) ·
[stress tests](docs/STRESS_TESTING.md) · [capacity](docs/CAPACITY.md) ·
[hardware matrix](docs/HARDWARE_MATRIX.md).

## Known limitations

* Packages are unsigned (see above).
* Linux and macOS have not been tested on physical machines; no pen/tablet (pressure) has
  been tested on any platform.
* Very dense pages (50 000+ strokes) pan below 60 fps when the whole page is in view.
* Backups and Check Workspace briefly block the window for very large workspaces.
* Not included in 1.0: shape rotation, a layers panel, recurring tasks and reminders,
  syncing between devices.

More in [docs/ROADMAP.md](docs/ROADMAP.md) (Phase 9, "Not in Phase 9").

## Documentation

| Topic | Document |
|---|---|
| Building from source | [docs/BUILDING.md](docs/BUILDING.md) |
| Releases, packages, signing | [docs/RELEASE.md](docs/RELEASE.md) |
| Architecture and decisions | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Data model | [docs/DATA_MODEL.md](docs/DATA_MODEL.md) |
| Database schema | [docs/DATABASE_SCHEMA.md](docs/DATABASE_SCHEMA.md) |
| Canvas | [docs/CANVAS.md](docs/CANVAS.md) |
| Rendering | [docs/RENDERING.md](docs/RENDERING.md) |
| PDF inspection worker (design, D53) | [docs/PDF_WORKER.md](docs/PDF_WORKER.md) |
| Testing | [docs/TESTING.md](docs/TESTING.md) |
| Performance, bottlenecks | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| Benchmarks | [docs/BENCHMARKS.md](docs/BENCHMARKS.md) |
| Stress testing | [docs/STRESS_TESTING.md](docs/STRESS_TESTING.md) |
| Capacity | [docs/CAPACITY.md](docs/CAPACITY.md) |
| Hardware matrix | [docs/HARDWARE_MATRIX.md](docs/HARDWARE_MATRIX.md) |
| Coding style | [docs/CODING_STYLE.md](docs/CODING_STYLE.md) |
| Roadmap and history | [docs/ROADMAP.md](docs/ROADMAP.md) |

## Building from source

C++20, CMake 3.25+ with Ninja, Qt 6.8 LTS (with the Qt PDF add-on). Tested toolchains: MSVC
2022, GCC 13, AppleClang 15.

```sh
cmake --preset release
cmake --build --preset release
```

Run the tests with `cmake --workflow --preset debug`. Prerequisites, presets and packaging
are in [docs/BUILDING.md](docs/BUILDING.md).

## License

StudyBoard is released under the [MIT License](LICENSE). Third-party components and their
licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
