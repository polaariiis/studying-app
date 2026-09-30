# Releases

How StudyBoard releases are built, tested, signed (or not) and published, and the record of
each release's packages. Building the packages locally is described in
[BUILDING.md](BUILDING.md) §8.

## Current release

| | |
|---|---|
| Version | **1.0.0** (tag `v1.0.0`) |
| Source | `main` at the tag |
| Platforms | Windows x64, Linux x86_64, macOS arm64 (Apple silicon) |
| Signing | **Unsigned / signing-ready** — see [Signing](#signing) |
| Downloads | [GitHub releases](https://github.com/polaariiis/studying-app/releases) |

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
   package, its `.sha256` file and `SHA256SUMS.txt`.
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

Recorded after the `v1.0.0` release workflow; until then see the release's
`SHA256SUMS.txt`.
