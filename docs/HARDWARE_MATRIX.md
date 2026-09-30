# Hardware and platform matrix

Where StudyBoard 1.0.0 has been built, tested and run, and how. Three kinds of evidence are
kept apart:

* **Physically tested** — run by a person on real hardware with a real display and GPU.
* **CI tested** — built and tested automatically on GitHub-hosted virtual machines. These
  have no physical GPU: OpenGL comes from a software renderer, and there is no real
  display or input device. CI proves that the code builds, the tests pass and the packages
  start and render offscreen; it is *not* end-user hardware validation.
* **Not tested** — everything else. A platform that is not listed here has not been run.

## Physically tested

| | Reference laptop |
|---|---|
| Machine | HP Victus 15-fb2xxx |
| OS | Windows 10 Home 22H2 (build 19045), x64 |
| CPU | AMD Ryzen 5 8645HS, 6 cores / 12 threads |
| RAM | 8 GB installed (7.3 GB usable) |
| GPU used by StudyBoard | AMD Radeon 760M (integrated), driver 32.0.31041.1004, OpenGL 3.3 core context |
| Second GPU | NVIDIA GeForce RTX 4050 Laptop GPU, driver 32.0.15.9282 (not used by StudyBoard's window) |
| Display | 1920 × 1080 at 144 Hz |
| Toolchain | MSVC 19.44 (Visual Studio 2022), Qt 6.8.3, CMake 3.31, Ninja |
| Builds | Release (`release`, `ci-full` presets) and Debug; MSVC AddressSanitizer for the Qt-free modules |
| Package | The Windows install layout (`cmake --install` of the release build: the ZIP's contents), started with only `C:\Windows\System32` on `PATH` |
| Tests performed | All test suites (492 tests); the real-OpenGL canvas widget tests (12/12, none skipped); every benchmark in [BENCHMARKS.md](BENCHMARKS.md) and [STRESS_TESTING.md](STRESS_TESTING.md); `--self-test` (OpenGL rendering on the Radeon, and Qt's software OpenGL, which offers only 3.0 and is correctly reported); a manual end-to-end session driving the packaged application: new workspace, pen, text, image, PDF import with highlighting, search, a planner task, PDF export, close and reopen |
| Result | Pass |

Not covered on this machine: a pen or tablet with pressure (only mouse input), a 60 Hz or
120 Hz display, a high-DPI (scaled) display, and the NSIS installer itself (its install and
uninstall are exercised in CI).

## CI tested (GitHub Actions)

Every push to `main` runs `ci.yml` and `package.yml`; release tags run `release.yml`.

| Runner | Role | Compiler / environment | Graphics | What runs | Result (1.0.0) |
|---|---|---|---|---|---|
| `windows-2022` | Build and test | MSVC 2022, Qt 6.8.3 | — (offscreen Qt platform) | Qt-free and full builds with warnings as errors; every test | Pass |
| `ubuntu-24.04` | Build and test | GCC 13, Qt 6.8.3; Clang for sanitizers and clang-tidy | — (offscreen) | Full build, every test; ASan + UBSan; clang-tidy | Pass |
| `macos-14` (arm64) | Build and test | AppleClang (Xcode), Qt 6.8.3 | — (offscreen) | Full build, every test | Pass |
| `windows-2022` | Package, clean-machine smoke test | NSIS + ZIP | No GPU; Qt's software OpenGL is 3.0, so the OpenGL check is reported, not required (`--no-opengl-check`) | Silent install, `--self-test` of the installed copy and of the ZIP, silent uninstall (must remove the application) | Pass |
| `ubuntu-22.04` | Package, clean-machine smoke test | Built on 22.04 (glibc 2.35) with GCC 13 | Xvfb + Mesa 23.2.1 llvmpipe (LLVM 15), OpenGL 4.5 core | AppImage and `.tar.gz` through the launcher, from a path with a space; the launcher chooses the **bundled** C++ runtime (system `GLIBCXX_3.4.30` < needed `3.4.31`); OpenGL rendering required; exit status passed on | Pass |
| `ubuntu-24.04` | Clean-machine smoke test | — | Xvfb + Mesa 25.2.8 llvmpipe (LLVM 20), OpenGL 4.5 core | As above; the launcher chooses the **system** C++ runtime (`GLIBCXX_3.4.33`) | Pass |
| `macos-14` (arm64) | Package, clean-machine smoke test | DragNDrop DMG | Apple Software Renderer, OpenGL 4.1 | DMG mounted, `--self-test` including OpenGL rendering | Pass |

All smoke tests run on fresh runners without Qt or a compiler: each package must bring
everything it needs (Qt libraries and plugins, Qt PDF, Svg and Print Support).

## Not tested

* Any physical Linux or macOS machine, and therefore any real Linux or macOS GPU driver.
  Rendering there is covered only by the software renderers above.
* Windows 11, and Windows on ARM (no ARM64 Windows package exists).
* macOS on Intel (x86_64): only an arm64 DMG is built.
* Linux distributions other than Ubuntu 22.04 and 24.04, Wayland sessions, and ARM64
  Linux (only x86_64 packages exist).
* NVIDIA or Intel GPUs driving StudyBoard's window (the reference laptop renders on the
  AMD integrated GPU).
* Pens and tablets (pressure input), touch input, high-DPI scaling, 60/120 Hz displays.
* Screen readers in use (accessible names are audited by a test, not with a screen reader).

Reports from these configurations are welcome; see the
[issue tracker](https://github.com/polaariiis/studying-app/issues).
