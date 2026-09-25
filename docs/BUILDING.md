# Building

> Status: **Proposed — the build system is designed here and implemented in Phase 1.**
> Commands below describe the intended workflow.

## 1. Requirements

| Tool | Minimum | Notes |
|---|---|---|
| CMake | **3.25** | Presets schema v6 (workflow presets), `FetchContent_Declare(... SYSTEM)`, `block()` |
| Ninja | 1.11 | Default generator on all platforms (Visual Studio generator also works) |
| C++ compiler | MSVC 19.38 (VS 2022 17.8), GCC 13, Clang 17, AppleClang 15 | C++20 incl. `std::format`, `std::span`, concepts |
| Qt | **6.8 LTS** | Modules: Core, Gui, Widgets, OpenGL, OpenGLWidgets, (Pdf), Test |
| Git | any recent | |

Fetched automatically (pinned versions + hashes, via FetchContent):
GoogleTest, SQLite amalgamation, nlohmann/json, tl::expected; later Google Benchmark and
optionally Tracy.

Why these minimums: CMake 3.25 gives presets v6 and `SYSTEM` includes for third-party
code (no warnings leaking from deps). Qt 6.8 is an LTS release, provides
`QStyleHints::colorScheme` and `QRhiWidget` (future backend). GCC 13 / Clang 17 are the
first versions with a usable `std::format`.

### Platform setup (summary)

* **Windows**: Visual Studio 2022 (Desktop C++), Qt 6.8 MSVC 2022 64-bit via the Qt
  online installer or `aqtinstall`; build from a *Developer PowerShell*.
* **macOS**: Xcode 15+ command line tools, Qt 6.8 (installer, `aqtinstall`, or Homebrew
  `qt`), `brew install cmake ninja`.
* **Linux**: GCC 13+ or Clang 17+, `cmake`, `ninja`, OpenGL/X11/Wayland dev packages
  (`libgl1-mesa-dev libxkbcommon-dev` etc.), Qt 6.8 via `aqtinstall` (distro Qt is often
  older than 6.8).

Point CMake at Qt through `CMAKE_PREFIX_PATH`, preferably in a personal, git-ignored
`CMakeUserPresets.json` that inherits the shared presets:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "my-debug", "inherits": "debug",
      "cacheVariables": { "CMAKE_PREFIX_PATH": "C:/Qt/6.8.3/msvc2022_64" } }
  ]
}
```

## 2. Everyday commands

```sh
cmake --preset debug                 # configure  → build/debug
cmake --build --preset debug         # build
ctest --preset debug                 # run tests
cmake --workflow --preset debug      # configure + build + test in one go

cmake --preset core-only             # Qt-free modules + tests only (no Qt needed)
cmake --preset asan                  # Clang/GCC with Address+UB sanitizers
cmake --install build/release --prefix dist   # install + deploy Qt runtime
```

## 3. CMake architecture

### 3.1 Top-level layout

```
CMakeLists.txt                 # project(), policies, options, dependency setup, subdirs
CMakePresets.json
cmake/
├── StudyAppModule.cmake       # studyapp_add_module(), studyapp_add_test()
├── CompilerWarnings.cmake     # per-compiler warning sets; STUDYAPP_WARNINGS_AS_ERRORS
├── Sanitizers.cmake           # STUDYAPP_SANITIZERS="address;undefined" etc.
├── Dependencies.cmake         # FetchContent declarations (pinned) + find_package(Qt6)
├── EmbedFiles.cmake           # turns migrations/*.sql into a generated .cpp
└── Deploy.cmake               # install rules, qt_generate_deploy_app_script, CPack
src/<module>/CMakeLists.txt    # one target per module
tests/<module>/CMakeLists.txt
```

### 3.2 Targets

| Target (alias) | Type | Links (PUBLIC unless noted) |
|---|---|---|
| `studyapp_core` (`studyapp::core`) | static | `tl::expected` |
| `studyapp_document` | static | core |
| `studyapp_study` | static | core |
| `studyapp_render` | static | core |
| `studyapp_canvas` | static | document, render |
| `studyapp_persistence` | static | document, study; PRIVATE sqlite3, nlohmann_json |
| `studyapp_application` | static | persistence, document, study |
| `studyapp_render_gl` | static | render; PRIVATE Qt6::Gui, Qt6::OpenGL |
| `studyapp_platform` | static | application, canvas; PRIVATE Qt6::Core, Qt6::Gui, (Qt6::Pdf) |
| `studyapp_ui` | static | application, canvas, render_gl; Qt6::Widgets, Qt6::OpenGLWidgets |
| `studyapp` | executable | ui, platform |

Notes:

* Static libraries: simple deployment, whole-program optimisation possible, no symbol
  export macros. Can switch individual modules to shared later if build times demand.
* SQLite and JSON are **PRIVATE** to persistence — no other module can accidentally use
  them. Qt is PRIVATE to `render_gl` and `platform`; only `ui` exposes Qt publicly.
* Dependency rules from ARCHITECTURE.md are therefore enforced by the linker and the
  include paths, not just by convention.

### 3.3 Module helper

```cmake
# cmake/StudyAppModule.cmake (sketch)
function(studyapp_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(studyapp_${name} STATIC ${ARG_SOURCES})
  add_library(studyapp::${name} ALIAS studyapp_${name})
  target_compile_features(studyapp_${name} PUBLIC cxx_std_20)
  set_target_properties(studyapp_${name} PROPERTIES CXX_EXTENSIONS OFF)
  target_include_directories(studyapp_${name}
      PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(studyapp_${name}
      PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS} studyapp_warnings)
endfunction()
```

Globbing is not used for sources; files are listed explicitly per module.

### 3.4 Options

| Option | Default | Purpose |
|---|---|---|
| `STUDYAPP_BUILD_APP` | `ON` | Build Qt-dependent targets. `OFF` → Qt-free modules only (no Qt required) |
| `STUDYAPP_BUILD_TESTS` | `ON` (top-level project) | Tests and test support |
| `STUDYAPP_BUILD_BENCHMARKS` | `OFF` | Google Benchmark targets |
| `STUDYAPP_WARNINGS_AS_ERRORS` | `OFF` (CI: `ON`) | |
| `STUDYAPP_SANITIZERS` | empty | e.g. `address;undefined`, `thread` |
| `STUDYAPP_USE_SYSTEM_SQLITE` | `OFF` | Use `find_package(SQLite3)` instead of the vendored amalgamation (for distro packaging; must have FTS5) |
| `STUDYAPP_ENABLE_PDF` | `ON` if Qt6::Pdf found | Build the QtPdf-based rasteriser |
| `STUDYAPP_ENABLE_TRACY` | `OFF` | Profiler zones |

### 3.5 Warnings and compiler flags

* MSVC: `/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor`, `/WX` when
  warnings-as-errors.
* GCC/Clang: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wnull-dereference`.
* Third-party targets are included as `SYSTEM` so their warnings don't surface.
* Qt: `QT_NO_KEYWORDS` (use `Q_SIGNALS`/`Q_SLOTS`), `QT_NO_CAST_FROM_ASCII`,
  `QT_NO_NARROWING_CONVERSIONS_IN_CONNECT`; `CMAKE_AUTOMOC` only on Qt targets.

### 3.6 Build configurations

`Debug`, `Release`, `RelWithDebInfo` (profiling), plus sanitizer presets built as Debug
with sanitizer flags. Multi-config generators are supported, but presets use Ninja
single-config for consistency.

### 3.7 Presets (planned `CMakePresets.json` structure)

```
configurePresets
  base (hidden)            generator Ninja, binaryDir build/${presetName},
                           CMAKE_EXPORT_COMPILE_COMMANDS=ON
  debug / release / relwithdebinfo    inherit base
  core-only                STUDYAPP_BUILD_APP=OFF (no Qt)
  asan / tsan              Clang/GCC sanitizer builds (not on MSVC)
  ci-windows / ci-linux / ci-macos    warnings-as-errors, CI-specific cache vars
buildPresets     one per configure preset
testPresets      one per configure preset; output-on-failure; label filters (e.g. exclude "gpu")
workflowPresets  debug, core-only, ci-* (configure → build → test)
```

### 3.8 Resources

* Shaders, icons and themes: `qt_add_resources()` on the Qt targets that use them.
* SQL migrations: `EmbedFiles.cmake` generates `migrations_generated.cpp` containing
  `constexpr std::string_view` entries, so persistence stays Qt-free and migrations are
  compiled into the binary.

## 4. Install, deploy and packaging

* `install(TARGETS studyapp BUNDLE DESTINATION . RUNTIME DESTINATION bin)`.
* `qt_generate_deploy_app_script()` (Qt ≥ 6.3) runs `windeployqt`/`macdeployqt` (and
  the Linux equivalent) at install time, so `cmake --install` yields a runnable tree.
  On Windows it includes `opengl32sw.dll` as the software-GL fallback.
* CPack (Phase 9): WiX/NSIS installer (Windows), DMG (macOS, signed + notarised when
  certificates exist), AppImage and/or `.deb`/Flatpak (Linux, to be decided).
* Version: `project(studyapp VERSION x.y.z)` → generated `Version.hpp`; git hash embedded
  for diagnostics.

## 5. Continuous integration (GitHub Actions)

```
.github/workflows/
├── ci.yml          # on push / pull_request
├── nightly.yml     # fuzzing, benchmarks, TSan, coverage
└── release.yml     # on tag: build packages, attach to GitHub Release
```

### `ci.yml` jobs

| Job | Runner | What |
|---|---|---|
| `format` | ubuntu-24.04 | `clang-format --dry-run -Werror` on changed files; forbidden-include check (GL/SQLite/Qt headers outside allowed modules) |
| `core` | ubuntu-24.04, windows-2022, macos-14 | `core-only` workflow preset: Qt-free build + unit/integration tests. Fast gate, no Qt download |
| `sanitize` | ubuntu-24.04 (Clang) | `asan` preset, all non-GPU tests |
| `full` | ubuntu-24.04 (GCC), windows-2022 (MSVC), macos-14 (AppleClang arm64) | Install Qt 6.8 via `jurplel/install-qt-action` (cached), build everything, run tests with `QT_QPA_PLATFORM=offscreen`; `gpu` label only on Linux with Mesa llvmpipe |
| `clang-tidy` | ubuntu-24.04 | on changed files (from Phase 2; non-blocking at first) |

Practices: `concurrency` groups to cancel superseded runs; ccache/sccache keyed by
preset + compiler; FetchContent sources cached; test results uploaded as JUnit XML;
failing-test logs uploaded as artifacts. Branch protection requires `format`, `core` and
`full` to pass.

## 6. Cross-platform build notes

* No platform `#ifdef`s outside `src/platform/os/*` and the `app` target's
  manifest/plist/icon glue; CMake selects `os/<platform>` sources with generator
  expressions / `if(WIN32)`, `if(APPLE)`, `if(UNIX AND NOT APPLE)`.
* Windows: `WIN32_EXECUTABLE`, application manifest (DPI awareness per-monitor v2, UTF-8
  code page), `/utf-8` for source encoding.
* macOS: `MACOSX_BUNDLE`, `Info.plist` template, deployment target 13.0, universal
  binaries optional (arm64 first).
* Linux: `.desktop` file and icons installed to standard locations; both X11 and Wayland
  tested.
* Paths in code use `std::filesystem::path`; never build paths by string concatenation.
