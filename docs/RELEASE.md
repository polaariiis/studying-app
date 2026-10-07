# Releases

How StudyBoard releases are built, tested, signed (or not) and published, and the record of
each release's packages. Building the packages locally is described in
[BUILDING.md](BUILDING.md) §8.

## Current release

| | |
|---|---|
| Version | **1.0.0** (tag `v1.0.0`) |
| Source | tag `v1.0.0` (commit `5d477e6`, tagged on `main`); release line `release/1.0` |
| Platforms | Windows x64, Linux x86_64, macOS arm64 (Apple silicon) |
| Signing | **Unsigned / signing-ready** — see [Signing](#signing) |
| Downloads | [GitHub releases](https://github.com/polaariiis/studying-app/releases) |

The next version, 1.2, is in development and **not released**: no `v1.2.0` tag or package
exists yet. Builds of `main` and `dev/1.2` still report 1.0.0 until step 2 below sets the
1.2 version.

## Branches and tags

| Reference | Role | Rules |
|---|---|---|
| `v<version>` tags (`v1.0.0`) | The released source of each version | Immutable: never moved, deleted or recreated; the GitHub release and its packages belong to the tag |
| `release/<major>.<minor>` (`release/1.0`) | Maintenance line of a released version, created at its tag | Fixes for that version only; a fix release is tagged here |
| `main` | Integration line, the default branch | Receives the state of the development branch as one "Integrate …" commit (squash merge), so its history stays short; never force-pushed |
| `dev/<major>.<minor>` (`dev/1.2`) | Development of the next version with the full history | Every `feature/…` branch is merged here (`--no-ff`) after CI and package runs pass; then the feature branch is deleted |

When 1.2 is released, its tag is made on `main`, `release/1.2` is created at it, and the
next version's development continues on `dev/1.3`.

## Packages

| Platform | File | What it is |
|---|---|---|
| Windows 10 22H2+ x64 | `StudyBoard-<version>-windows-AMD64.exe` | Installer (NSIS): Start menu and desktop shortcuts, uninstaller; installing a newer version replaces the previous one |
| | `StudyBoard-<version>-windows-AMD64.zip` | Portable: unpack anywhere, run `bin\studyapp.exe` |
| Linux x86_64 (Ubuntu 22.04 or equivalent, and later) | `StudyBoard-<version>-linux-x86_64.AppImage` | One file: make it executable and run it |
| | `StudyBoard-<version>-linux-x86_64.tar.gz` | Unpack anywhere, run `bin/studyboard` |
| macOS 13+ (Apple silicon) | `StudyBoard-<version>-macos-arm64.dmg` | Drag `studyapp.app` to Applications |

Every package carries its runtime: the Qt libraries and plugins (including Qt PDF, Svg and
Print Support), on Windows the Microsoft C++ runtime, on Linux a C++ runtime that the
launcher uses only when the system's is too old ([BUILDING.md](BUILDING.md) §8). No Qt, compiler or other
download is needed. Each release also has `SHA256SUMS.txt` and a `.sha256` file per
package.

Workspaces are folders in the user's own documents; installing, upgrading or uninstalling
never touches them.

## How a release is made

1. The release candidate is on `main` with CI (`ci.yml`) and the package workflow
   (`package.yml`) green on all three platforms: every package is built and smoke-tested on
   a clean runner without Qt or a compiler (`studyapp --self-test`: Qt plugins, OpenGL 3.3
   rendering, fonts, workspaces, search, Qt PDF, export, printing; on Windows the installer
   is installed and uninstalled; on Linux the launcher's runtime choice is checked).
2. The version is set in the top-level `CMakeLists.txt` (`project(VERSION …)`); the
   packages, the application's About box and the workspace metadata take it from there.
3. Pushing a tag `v<version>` on `main` runs `release.yml`: the same package and
   smoke-test jobs, the optional signing jobs, then a **draft** GitHub release with every
   package, its `.sha256` file and `SHA256SUMS.txt`. Every package's size and SHA-256, and
   the version each packaged application reports, are also published as notices on the
   workflow run (readable without signing in).
4. A maintainer reviews the draft (files, checksums, notes) and publishes it. Nothing is
   published automatically.
5. The published packages are recorded in [Release records](#release-records) below.

## Signing

The packages of 1.0.0 are **not signed**: the repository has no code-signing certificates.
Windows SmartScreen shows "Windows protected your PC" on first start (More info ▸ Run
anyway); macOS Gatekeeper blocks the first start (right-click the application ▸ Open, or
System Settings ▸ Privacy & Security ▸ Open Anyway). Linux packages are not signed on any
distribution channel.

Signing is prepared and activates only when certificates are provided as repository
secrets:

| Platform | Secrets | Where it happens |
|---|---|---|
| Windows (Authenticode) | `WINDOWS_SIGN_PFX_BASE64`, `WINDOWS_SIGN_PASSWORD` | `release.yml` job *Signed Windows packages*: CPack runs `cmake/SignPackage.cmake` before packaging (signs `studyapp.exe`) and after (signs the installer) with `signtool` |
| macOS (Developer ID + notarisation) | `MACOS_CODESIGN_IDENTITY`, `MACOS_CERT_P12_BASE64`, `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | `release.yml` job *Signed and notarised macOS package*: `codesign` with the hardened runtime via `cmake/SignPackage.cmake`, then `notarytool` and `stapler` |

A signed build replaces the unsigned package in the draft only after it passes the same
`--self-test` as the tested packages (the signed ZIP on Windows, the signed DMG on macOS).
On macOS the DMG's checksum is computed **after** notarisation and stapling (stapling
changes the file), and `SHA256SUMS.txt` is always generated from the final files.

Not covered: the Windows uninstaller itself is not signed, and `signtool` receives the
certificate password on its command line inside the CI job.

## Verifying a download

```sh
sha256sum -c StudyBoard-1.0.0-linux-x86_64.AppImage.sha256     # Linux
shasum -a 256 -c StudyBoard-1.0.0-macos-arm64.dmg.sha256         # macOS
```

On Windows: `Get-FileHash StudyBoard-1.0.0-windows-AMD64.exe -Algorithm SHA256` and compare
with `SHA256SUMS.txt`. Any installation can check itself with `studyapp --self-test` (on
Windows run it as `studyapp --self-test | more` to see the output).

## Release records

The packages published for each release, with their checksums. A tagged build is not
bit-for-bit reproducible, so these values belong to the files attached to the GitHub
release; they are recorded here after the release workflow has produced and verified them.

### 1.0.0

Tag `v1.0.0` on commit `5d477e6`, built by the
[release workflow run](https://github.com/polaariiis/studying-app/actions/runs/36695155624)
on 2026-09-30. All packages **unsigned** (no certificates configured; the signing jobs
skipped their steps).

| File | Platform | Architecture | Size (bytes) | SHA-256 | Validation |
|---|---|---|---:|---|---|
| `StudyBoard-1.0.0-windows-AMD64.exe` | Windows 10 22H2+ | x64 | 50 422 309 | `6c5b3734bb2e6e93abcd281375561f3fb71d94aaf129c07cf58bb069822491c7` | Silent install, `--self-test` of the installed copy, uninstall — clean `windows-2022` runner |
| `StudyBoard-1.0.0-windows-AMD64.zip` | Windows 10 22H2+ | x64 | 60 342 426 | `09d8dd9af3feed1b627df87bec7c7824222abe45102bb045ade7623224d816ca` | `--self-test`, version 1.0.0 — clean `windows-2022` runner |
| `StudyBoard-1.0.0-linux-x86_64.AppImage` | Ubuntu 22.04+ | x86_64 | 41 634 296 | `6e68ee05f6173540240ccbc1bb925565a13fadddfa37f5f1582a9a50354527be` | `--self-test` with OpenGL rendering (Mesa) through the launcher — clean Ubuntu 22.04 and 24.04 runners |
| `StudyBoard-1.0.0-linux-x86_64.tar.gz` | Ubuntu 22.04+ | x86_64 | 45 726 237 | `495b3991ad31cd7045069cdd9b314025ab99ef154efeb132ed1dae4734c2c2be` | As the AppImage, from a path with a space; version 1.0.0; the launcher chose the bundled C++ runtime on 22.04 and the system's on 24.04 |
| `StudyBoard-1.0.0-macos-arm64.dmg` | macOS 13+ | arm64 | 33 278 150 | `c93aafcbdeb0161ccc2c8b6a5e45ee53016e425214dd4862110b612322f86c77` | Mounted, `--self-test` with OpenGL rendering (Apple software renderer), version 1.0.0 — clean `macos-14` runner |

The Windows OpenGL check on the GPU-less runner is reported, not required (Qt's software
OpenGL offers only 3.0). The same source was also built locally on the reference laptop:
its ZIP unpacked from a path with a space, with no Qt or compiler on `PATH`, reported
1.0.0 and passed `--self-test` including OpenGL rendering on the Radeon 760M
([HARDWARE_MATRIX.md](HARDWARE_MATRIX.md)).
