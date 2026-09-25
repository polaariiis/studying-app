# Building StudyBoard

> Status: **Implemented in Phase 1.** Everything on this page describes the current build.

## 1. Requirements

| Tool | Minimum | Notes |
|---|---|---|
| CMake | **3.25** | Presets schema v6 (workflow presets), `FetchContent_Declare(... SYSTEM)` |
| Ninja | 1.11 | Generator used by all presets (Visual Studio ships one) |
| C++ compiler | MSVC 19.38 (VS 2022 17.8), GCC 13, Clang 17, AppleClang 15 | C++20, no extensions |
| Qt | **6.8 LTS** | Core, Gui, Widgets, OpenGL, Test. Not needed for the `core-only` preset |
| Python | 3.8+ | Optional locally: runs the include-boundary check as a CTest test |
| Git | any recent | |

Fetched automatically at configure time (pinned by version and SHA-256 in
[`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake); licenses in
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)):

| Dependency | Version | Used by |
|---|---|---|
| tl::expected | 1.1.0 | `core` (`Result<T>`) |
| SQLite amalgamation | 3.50.4 | `persistence` (private) |
| GoogleTest | 1.17.0 | tests |

Why these minimums: CMake 3.25 gives presets v6 and `SYSTEM` includes for third-party
code. Qt 6.8 is the current LTS line and provides `QStyleHints::colorScheme` /
`setColorScheme`, used for light/dark theming. GCC 13 / Clang 17 / MSVC 17.8 are the
toolchains CI tests; older compilers may work but are not supported.

### Supported platforms

| Platform | Minimum | CI runner | Compiler in CI |
|---|---|---|---|
| Windows | 10 22H2, x64 | `windows-2022` | MSVC (VS 2022) |
| Linux | Ubuntu 22.04 or equivalent, x86_64 | `ubuntu-24.04` | GCC 13 (+ Clang for sanitizers) |
| macOS | 13 (Ventura) | `macos-14` (arm64) | AppleClang 15 |

The primary development environment is **Windows + Visual Studio 2022**. Ubuntu 22.04's
default compiler is GCC 11, so building *on* 22.04 needs GCC 13 (e.g. from the
`ubuntu-toolchain-r/test` PPA); running there is the packaging concern of Phase 9.

## 2. Installing prerequisites

**Windows**
1. Visual Studio 2022 with the *Desktop development with C++* workload (includes CMake and Ninja).
2. Qt 6.8 for `msvc2022_64`, either with the Qt Online Installer or with
   [aqtinstall](https://github.com/miurahr/aqtinstall):
   `pip install aqtinstall` then `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:\Qt`.
3. Build from a **Developer PowerShell / Developer Command Prompt for VS 2022** (so `cl.exe`
   and `ninja` are on `PATH`), or open the folder in Visual Studio, which uses the presets
   directly.

**macOS**
1. Xcode 15+ command-line tools, `brew install cmake ninja`.
2. Qt 6.8 (Qt Online Installer or `aqt install-qt mac desktop 6.8.3 clang_64 -O ~/Qt`).

**Linux (Ubuntu/Debian)**
1. `sudo apt install build-essential g++-13 cmake ninja-build libgl1-mesa-dev libxkbcommon-dev libxcb-cursor0 libfontconfig1`
2. Qt 6.8 via `aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O ~/Qt` (distribution Qt
   packages are usually older than 6.8).

## 3. Telling CMake where Qt is

The repository never contains a machine-specific Qt path. Use **one** of:

1. **Environment variable `QT_ROOT_DIR`** (the shared presets pass it as `CMAKE_PREFIX_PATH`;
   the CI action sets it too):
   ```powershell
   $env:QT_ROOT_DIR = "C:\Qt\6.8.3\msvc2022_64"      # PowerShell
   ```
   ```sh
   export QT_ROOT_DIR=~/Qt/6.8.3/gcc_64              # bash/zsh
   ```
2. **A personal `CMakeUserPresets.json`** (git-ignored). Copy
   [`cmake/CMakeUserPresets.example.json`](../cmake/CMakeUserPresets.example.json) to the
   repository root, adjust the path, and use its presets (`my-debug`, `my-release`).
3. `-DCMAKE_PREFIX_PATH=<qt-dir>` on the configure command line.

If Qt cannot be found, configuration stops with a message listing these options.

## 4. Configure, build, test, run

```sh
cmake --preset debug                 # configure  → build/debug
cmake --build --preset debug         # build
ctest --preset debug                 # run all tests
cmake --workflow --preset debug      # configure + build + test in one step
```

| Preset | Configuration | Qt needed | Purpose |
|---|---|---|---|
| `debug` | Debug | yes | Everyday development |
| `release` | Release | yes | Optimised build |
| `relwithdebinfo` | RelWithDebInfo | yes | Profiling |
| `core-only` | Debug, `STUDYAPP_BUILD_APP=OFF` | **no** | Qt-free modules + their tests |
| `asan` | Debug, core-only + ASan/UBSan | no | GCC/Clang only (not offered on Windows) |
| `ci-core` / `ci-full` | Debug / Release, warnings as errors | no / yes | Used by CI |

Build trees go to `build/<preset>`, install trees to `install/<preset>`; both are
git-ignored. In-source builds are rejected.

**Running the application**

* Windows: run `build/debug/app/studyapp.exe` directly. Windows only finds DLLs next to
  the executable or on `PATH`, so a post-build step runs `windeployqt` (the
  `Qt6::windeployqt` target from the Qt package) on the executable **in the build tree**,
  copying the matching debug/release Qt DLLs and plugins into `build/<preset>/app/`
  (build output, git-ignored; translations and the MSVC runtime are skipped). Controlled
  by `STUDYAPP_DEPLOY_QT_TO_BUILD_TREE` (default `ON` on Windows, unused elsewhere:
  Linux/macOS executables find Qt through RPATH).
* macOS: `open build/debug/app/studyapp.app`
* Linux: `build/debug/app/studyapp`

**Installing** (runs `windeployqt` / `macdeployqt` / Qt's Linux deployment via
`qt_generate_deploy_app_script`, producing a self-contained tree):

```sh
cmake --install build/release                    # → install/release
cmake --install build/release --prefix <dir>     # custom location; must be an absolute
                                                 # path (Qt's deploy step rejects relative)
```

On Windows the deployed tree includes `opengl32sw.dll`, Qt's software OpenGL fallback.

**Selecting tests** (CTest labels: `unit`, `integration`, `architecture`, `gui`;
`integration` = real SQLite databases and lock files in temporary directories):

```sh
ctest --preset debug -L unit
ctest --preset debug -L gui          # Qt Widgets tests (offscreen platform)
ctest --preset debug -R Uuid         # by name
```

## 5. CMake architecture

### 5.1 Layout

```
CMakeLists.txt                 project(), options, global settings, subdirectories
CMakePresets.json              shared presets (above)
cmake/
├── StudyAppModule.cmake       studyapp_add_module(), studyapp_add_gtest(), studyapp_enable_qt()
├── CompilerWarnings.cmake     studyapp_project_options + studyapp_warnings (INTERFACE targets)
├── Sanitizers.cmake           STUDYAPP_SANITIZERS (GCC/Clang)
├── Dependencies.cmake         FetchContent (pinned) + find_package(Qt6)
├── Deploy.cmake               studyapp_install_app(): install + Qt deployment script
├── EmbedFiles.cmake           studyapp_embed_files(): files → byte arrays in a generated .cpp
├── EmbedFilesScript.cmake     script-mode helper of EmbedFiles.cmake (runs at build time)
└── CMakeUserPresets.example.json
src/<module>/CMakeLists.txt    one static library per module
app/CMakeLists.txt             the `studyapp` executable
tests/<area>/CMakeLists.txt    one test executable per area
```

Source files are listed explicitly; there is no globbing.

### 5.2 Targets and dependency enforcement

| Target (alias) | Links PUBLIC | Links PRIVATE | Qt |
|---|---|---|---|
| `studyapp_core` (`studyapp::core`) | `tl::expected` | — | no |
| `studyapp_document` | core | — | no |
| `studyapp_study` | core | — | no |
| `studyapp_render` | core | — | no |
| `studyapp_canvas` | document, render | — | no |
| `studyapp_persistence` | document, study | SQLite | no |
| `studyapp_application` | document, study | **persistence** | no |
| `studyapp_render_gl` | render | Qt6::Gui, Qt6::OpenGL | yes |
| `studyapp_platform` | application, canvas | Qt6::Core | yes |
| `studyapp_ui` | Qt6::Widgets | application, canvas, render_gl | yes |
| `studyapp` (executable) | — | ui, platform, core, Qt6::Widgets | yes |

Enforcement:

* **Include roots.** Each module exposes only `src/<module>/include`; private sources live
  in `src/<module>/src`. A target can include only headers of modules it links.
* **PRIVATE linkage.** SQLite is private to `persistence`, and `persistence` is private to
  `application`, so `ui`, `platform` and `app` cannot see either. Qt is private to
  `render_gl` and `platform`.
* **Qt-free by construction.** The `core-only` preset builds all Qt-free modules on a
  machine without Qt; `architecture_tests` links all of them into one plain executable.
* **Include scanner.** `tools/check_boundaries.py` checks every `#include` and every
  `gl*()` call against the rules in docs/ARCHITECTURE.md §2 (CTest
  `architecture.include_boundaries` + CI `format` job).

### 5.3 Options

| Option | Default | Purpose |
|---|---|---|
| `STUDYAPP_BUILD_APP` | `ON` | Build Qt-dependent targets. `OFF` = Qt-free modules only |
| `STUDYAPP_BUILD_TESTS` | `ON` when top-level | Tests |
| `STUDYAPP_WARNINGS_AS_ERRORS` | `OFF` (CI presets: `ON`) | `/WX` / `-Werror` for first-party targets |
| `STUDYAPP_SANITIZERS` | empty | e.g. `address;undefined` or `thread` (GCC/Clang) |
| `STUDYAPP_DEPLOY_QT_TO_BUILD_TREE` | `ON` on Windows | Run `windeployqt` after each build so the build-tree executable starts without Qt on `PATH` |
| `STUDYAPP_USE_SYSTEM_SQLITE` | `OFF` | `find_package(SQLite3 3.43)` instead of the amalgamation; must include FTS5 (verified by `persistence_tests`) |

### 5.4 Compiler settings

* C++20 via `target_compile_features(... cxx_std_20)`, `CXX_EXTENSIONS OFF`; C++20 module
  scanning is disabled (the project does not use modules).
* MSVC: `/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline`, external
  headers at `/external:W0`; `NOMINMAX` and `WIN32_LEAN_AND_MEAN` on Windows.
* GCC/Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wcast-align -Wformat=2
  -Wimplicit-fallthrough -Wmissing-declarations` (+ `-Wduplicated-cond -Wlogical-op` on GCC).
* Warning flags apply to first-party targets only; third-party code is `SYSTEM`. No
  warnings are globally suppressed.
* Qt targets: `AUTOMOC`, `QT_NO_KEYWORDS`, `QT_NO_CAST_FROM_ASCII`, `QT_NO_CAST_TO_ASCII`,
  `QT_NO_URL_CAST_FROM_STRING`, `QT_NO_NARROWING_CONVERSIONS_IN_CONNECT`,
  `QT_DISABLE_DEPRECATED_UP_TO=0x060800`.
* Generated header `studyapp/core/BuildInfo.hpp` carries product name and version from
  `project()`.

### 5.5 Resources

* `resources/themes/studyboard.qss` — stylesheet template (placeholders filled from design
  tokens at runtime), compiled into `studyapp_ui` with `qt_add_resources` under `:/themes`.
* `resources/icons/app/studyboard-{16..256}.png` — application icon, compiled into
  `studyapp_ui` under `:/icons/app`.
* `resources/icons/app/studyboard.ico` — Windows only: `app/studyapp.rc.in` is configured
  with its absolute path and added to the `studyapp` executable when `WIN32`.
* Icon assets are generated from `resources/icons/app/studyboard-master.png` by
  `python tools/generate_app_icons.py` (requires Pillow; developers only — the build uses
  the committed files). To replace the artwork:
  `python tools/generate_app_icons.py --import-source <image>`.
* SQL migrations (`src/persistence/migrations/*.sql`) are embedded into
  `studyapp_persistence` at build time by `studyapp_embed_files()`
  (`cmake/EmbedFiles.cmake`): a custom command regenerates
  `build/<preset>/src/persistence/generated/…migrationFiles.cpp` (byte arrays) whenever a
  migration changes, so `persistence` stays Qt-free and needs no files at run time. See
  `src/persistence/migrations/README.md`.

## 6. Code quality tooling

* **clang-format** — `.clang-format` (clang-format 19; CI pins 19.1.1 via pip):
  ```sh
  git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
  ```
  Visual Studio 2022 ships clang-format under `VC/Tools/Llvm/x64/bin`.
* **clang-tidy** — `.clang-tidy`; run with a compile database from any preset
  (`CMAKE_EXPORT_COMPILE_COMMANDS` is on):
  ```sh
  clang-tidy -p build/debug src/core/src/*.cpp
  ```
  Findings are advisory in Phase 1 and not yet gated in CI.
* **Boundary check** — `python tools/check_boundaries.py`.

## 7. Continuous integration

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on pushes to `main`, on
pull requests and manually:

| Job | Runners | What it does |
|---|---|---|
| `format` | ubuntu-24.04 | clang-format 19.1.1 `--dry-run --Werror` on all tracked C++ files; `tools/check_boundaries.py` |
| `core` | windows-2022, ubuntu-24.04, macos-14 | `cmake --workflow --preset ci-core`: Qt-free modules + tests, warnings as errors, **no Qt installed** |
| `sanitizers` | ubuntu-24.04 (Clang) | `cmake --workflow --preset asan` |
| `full` | windows-2022, ubuntu-24.04, macos-14 | Installs Qt 6.8.3, runs `cmake --workflow --preset ci-full` (Release, warnings as errors, all tests incl. `gui` on the offscreen platform), then `cmake --install` to exercise Qt deployment |

External actions and why:

* `lukka/get-cmake` — installs recent CMake + Ninja identically on all three OSes.
* `ilammy/msvc-dev-cmd` — sets up the MSVC environment on Windows so Ninja finds `cl.exe`.
* `jurplel/install-qt-action` — installs the official Qt binaries (via aqtinstall) with
  caching and exports `QT_ROOT_DIR`, which the presets consume. Chosen over distribution
  packages because Ubuntu's Qt is older than 6.8 and it works the same on all runners.

Nothing in CI depends on a developer's machine: all paths come from the actions or from
the presets.

## 8. Packaging (Phase 9)

CPack installers (Windows), signed/notarised DMG (macOS) and AppImage/Flatpak (Linux)
are planned for Phase 9. Until then, `cmake --install` produces a runnable, self-contained
directory.
