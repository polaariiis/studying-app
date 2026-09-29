# Coding style

How StudyBoard's C++ is written. This describes the conventions the code base follows
(derived from it in Phase 9, not copied from a general guide); `.clang-format` and
`.clang-tidy` enforce the mechanical parts in CI. When code and this document disagree,
look at the surrounding code first and then fix whichever is wrong.

## 1. Language and formatting

* **C++20**, no compiler extensions. Use C++20 where it makes code clearer or safer:
  designated initializers for aggregates (`{.id = …, .title = …}`), `std::span`,
  `std::string_view`, `<=>`/defaulted `==`, `std::chrono` calendar types, concepts sparingly.
  Not available on every supported standard library, so not used: `std::jthread` (Apple
  libc++ 15), `std::format` of floating-point values in code that must run on macOS 13.0.
* **clang-format 19** with `.clang-format` (LLVM base, 4-space indent, 100 columns, left
  pointer alignment, include groups: own header, private headers, `studyapp/`, Qt,
  third-party, standard library). CI rejects unformatted code.
* Warnings are errors in CI on MSVC (`/W4`), GCC and Clang (`-Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion …`, cmake/CompilerWarnings.cmake). Fix the cause;
  suppressions are local, justified in a comment, and rare (two exist: GCC 13's
  designated-initializer false positive, Qt Test macros' narrowing).

## 2. Naming

| Kind | Style | Example |
|---|---|---|
| Namespace | `lower_case` | `studyapp::canvas` |
| Class, struct, enum, alias | `CamelCase` | `WorkspaceSession`, `PageExtent` |
| Enumerator | `CamelCase` | `ExportFormat::Pdf` |
| Function, method, variable, parameter | `camelBack` | `importAsset`, `pageSizes` |
| Private / protected member | `camelBack_` | `documentTiles_` |
| Constant (namespace-scope or `static constexpr`) | `kCamelCase` | `kDocumentTilePx` |
| Files | `CamelCase.hpp/.cpp`, one main type per file | `SearchIndex.hpp` |
| Tests | `<Unit>Test.cpp`, `TEST(UnitTest, SentenceDescribingBehaviour)` | `ImportingANotebookTwiceRemapsEveryId` |

Names say what something *is* in the domain (page, layer, asset, tile), not how it is stored.
Qt object names (`setObjectName`) are stable identifiers used by tests: `camelBack`, prefixed
by the area (`plannerTaskTitle`, `actionImportPdf`).

## 3. Architecture boundaries

The module graph (docs/ARCHITECTURE.md §4) is enforced by CMake target dependencies and
`tools/check_boundaries.py` (CI):

* `core`, `document`, `study`, `render`, `canvas`, `persistence`, `application` are
  **Qt-free**; they build and test without Qt (`core-only` preset).
* **SQLite** only in `persistence`; `application` links it privately and its public headers
  mirror persistence types when needed (`SearchHit`, `StagedAssetFile`).
* **OpenGL** only in `render_gl`, behind `render::Renderer`.
* `canvas` knows no persistence: it edits through `DocumentPort` and reads pixels through
  ports (`ImageSource`, `DocumentRasterizer`, `TextLayout`) implemented in `ui`/`platform`.
* `ui` may not include `platform`; the composition root (`app/main.cpp`) wires platform
  objects into the UI through interfaces.

## 4. Document mutations

Every change to a workspace is a **command** producing a data-only **patch**, applied by the
`Editor` through `WorkspaceSession::execute` (which checks, applies, records undo history and
persists the exact patch). There is no other mutation path:

* Commands are pure functions `(const Workspace&, …) → Result<Command>` (or
  `Result<Created<Id>>`) in `document::commands`; they validate, never mutate.
* One user gesture is **one** command and one undo step. Operations that create several
  records combine their changes in one patch (`createDocumentSection`, `importNotebooks`,
  `WorkspaceStructure`'s compound).
* Invariants live in `Workspace::apply`/`checkRecord`; never weaken one to make a feature
  easier. A failed command changes nothing and leaves no history entry.

## 5. Errors

* Expected failures return `core::Result<T>` (`tl::expected<T, core::Error>`) with an
  `ErrorCode` (`InvalidArgument`, `NotFound`, `AlreadyExists`, `Conflict`, `IoError`,
  `ParseError`, `Unsupported`, `Internal`) and a message for people. Propagate with `forward`
  / `tl::unexpected`; do not throw for expected conditions.
* Exceptions are not used for control flow. Code that can meet them from the standard
  library in a destructor or `noexcept` context uses the non-throwing overloads (e.g.
  `std::filesystem` with `std::error_code`, `directory_iterator::increment(ec)` — a range-for
  over a directory iterator can throw).
* The UI turns errors into a `ShellDialogs::showError` call with a short summary and the
  message as details; nothing fails silently, nothing opens a dialog in a loop.

## 6. Identity, values and ownership

* Records are identified by **typed UUIDv7 ids** (`core::PageId`, `core::AssetId`, …), never
  by pointers, indices or order. Ids come from an injected `core::IdGenerator` (deterministic
  `SequentialIds` in tests), timestamps from an injected `core::Clock` (`ManualClock`).
* Domain records are plain values (`struct` with defaulted `==`); the `Workspace` owns them.
  Stroke points are shared immutable data (`StrokePoints`) so undo history does not copy.
* Ownership is explicit: `std::unique_ptr` for owned objects, raw pointers/references for
  non-owning access (documented lifetime: e.g. a controller's image source must outlive it),
  `std::shared_ptr` only where lifetime is genuinely shared across threads (worker results).
  Qt widgets use Qt parent ownership (`new` with a parent).

## 7. Containers and performance

* Default to `std::vector` (and `std::span` in interfaces); use `std::unordered_map`/`set`
  where lookups by id dominate and the data is large (workspace indexes, caches), `std::map`
  only when order matters. Reserve when the size is known.
* No accidental O(n²): a change touches the records it changes (indexes make edits O(1) per
  reference); a frame does work proportional to what is visible or changed. New hot paths get
  a benchmark (`bench/`, docs/PERFORMANCE.md) before and after optimisation.
* Every cache has an explicit bound (bytes and/or entries), an eviction policy and a
  documented lifetime (docs/CANVAS.md §10, docs/PERFORMANCE.md).

## 8. Threads

* The document, the session and SQLite are used on the GUI thread only.
* Worker threads do pure computation or file I/O on data they own or that is immutable, and
  hand results back to the GUI thread (`QMetaObject::invokeMethod(…, Qt::QueuedConnection)`
  with a liveness check, or a join): tessellation (`canvas/src/ParallelFor.hpp`), image
  decoding, PDF tiles (one worker thread, PDFium is not reentrant), asset staging (copy +
  hash). Workers never touch widgets, the `Workspace` or the database.
* Anything a worker delivers later must tolerate the requester being gone: results carry a
  key (tile key, workspace token) and are dropped if stale.

## 9. Qt

* Qt lives in `ui`, `platform`, `render_gl` and `app`. `QT_NO_KEYWORDS`: use `Q_EMIT`,
  `Q_SLOTS`, `Q_SIGNALS`.
* Strings cross the boundary as UTF-8 `std::string` ↔ `QString::fromStdString` /
  `toStdString`; paths as `std::filesystem::path` ↔ `QString::fromStdU16String(path.u16string())`.
* User-visible text goes through `tr()`; accessible names for every control without visible
  text (tested through `QAccessible`).
* Colours come from `DesignTokens`, never literals in widgets.

## 10. Tests

* GoogleTest for Qt-free modules (`document_tests`, `canvas_tests`, `persistence_tests`,
  `application_tests`, …), Qt Test for widgets (`shell_tests`, `canvas_widget_tests`) on the
  offscreen platform; real SQLite in temporary directories, never mocked.
* Tests name behaviour, use injected clocks and ids, and check results through public APIs
  (patches, stored rows, rendered pixels). A bug fix comes with the test that would have
  caught it.
* Static analysis: `python tools/run_clang_tidy.py -p <build>` with the curated
  `.clang-tidy` (the reason for each disabled check is in the file); CI gates it for the
  Qt-free modules.
