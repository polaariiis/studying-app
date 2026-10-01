# StudyBoard — Isolated PDF Inspection Worker (D53)

> **Status: design (1.2, task 1.2-PDF-01, backlog V-05/V-06, roadmap S6). Not implemented.**
> Implementation is roadmap step S7. Until then StudyBoard inspects PDFs in-process as
> described in §1. Decision record: [ARCHITECTURE.md §18, D53](ARCHITECTURE.md#18-decision-log).

This document specifies the first production use of the `ipc` module (D52): reading the
page count and page sizes of an imported PDF in a separate process, so that a PDF that
crashes or hangs PDFium during import ends that process instead of StudyBoard.

---

## 1. Problem

Today `File ▸ Import PDF` runs `ui::inspectPdf` (`src/ui/src/SessionDocumentRasterizer.cpp`)
on a pool thread of the window (`MainWindow::runInBackground`, D48), in the StudyBoard
process:

```
importPdf (GUI)                          pool thread                       GUI (finish step)
  chosen file ─► runInBackground ─► inspectPdf(source)  ──────►  validate result (already in
                                    │ QPdfDocument::load            inspectPdf), StagedImport
                                    │ pageCount, pagePointSize      (S5) ─► finishAssetImport
                                    └ stage(): copy + hash into       ─► importDocument(...)
                                      <workspace>/temporary/          ─► open or reveal (S5)
```

`inspectPdf` opens the **user's source file** with QtPdf (PDFium), reads the page count
(≤ `kMaxPdfPages` = 5000) and every page's size (finite, > 0, ≤ `kMaxPdfPagePoints` =
14 400 pt), and maps load errors to `core::ErrorCode` (`IncorrectPassword` and
`UnsupportedSecurityScheme` → `Unsupported`, `FileNotFound` → `IoError`, anything else →
`IoError` "the file is not a readable PDF document"; no pages, too many pages or a page
without a supported size → `InvalidArgument`). Only when it succeeds is the file staged.

PDFium parses untrusted input. A PDF that makes it crash (or corrupt memory) terminates
StudyBoard, with the import's staged file in `temporary/`; a PDF that makes it loop forever
blocks the pool thread, and closing the workspace waits for that thread (D48), so the
window cannot close.

## 2. Goals

1. A crash, abnormal exit or hang while **inspecting** an imported PDF fails that import
   with a normal error; StudyBoard keeps running and its workspace stays consistent.
2. Every terminal path — success, failure, crash, timeout, cancellation, workspace close —
   removes the job's files promptly (no scanner, no polling of directories).
3. The GUI thread never waits for the worker or for IPC.
4. One identity per job (`core::JobId`, UUIDv7) correlates request and response.
5. No new executable, package file or signing step; the same tests run on every platform.

## 3. Non-goals

* Rendering tiles out of process. `SessionDocumentRasterizer` keeps rendering imported PDFs
  in-process (§16, §17); its end-to-end cost out of process is not measured.
* A sandbox. The worker runs as the same user with the same rights (§15).
* A job framework, process manager, worker pool, queue or retry mechanism.
* User-visible cancellation (a Cancel button). The cancellation *semantics* are defined
  (§10) because workspace close needs them; a UI for it is backlog V-11.
* Changes to IPCFileLab, the `ipc::FileChannel` API, the database schema, the document
  model, persistence or rendering.

## 4. Scope of the worker

| Worker (`studyapp --pdf-worker`) | StudyBoard (the main process) |
|---|---|
| Open the staged copy read-only with QtPdf | Choose the file; stage it (copy + hash into `temporary/`, `AssetStore::stage`) |
| Read the page count and each page's size in points | Generate the `JobId`, create the job directory and channels, start and supervise the worker |
| Classify failures (unreadable, unsupported, invalid) | Validate the reply again (§12) — the authority on what the document model accepts |
| Reply once, then exit | Store the asset, create the section and pages (`WorkspaceStructure::importDocument`), navigation (S5) |
| — | Render tiles (`SessionDocumentRasterizer`), export, persistence, everything else |

The worker never sees the workspace database, never commits or deletes assets, never
writes anything except its one reply into the response channel.

**Order change (S7):** the file is staged **before** inspection and the worker inspects
the staged copy, not the user's source file. The source is then read once (by the copy),
the inspected bytes are exactly the bytes that will be stored, and a failed inspection
leaves only the staged file, which the S5 owner (`StagedImport`) removes.

## 5. Process model

**Same executable, worker mode.** The worker is `studyapp` started with
`--pdf-worker <job-directory>`. `app/main.cpp` checks for `--pdf-worker` **before** it
constructs `QApplication` and then calls `ui::runPdfWorker(jobDirectory)` inside a
`QCoreApplication` — no window, no settings, no theme, no Qt log sink to the user's log, no
platform plugin. (Windows packages deploy only `qwindows`, so the worker must not need a GUI
platform; S7 verifies that `QPdfDocument::load`, `pageCount` and `pagePointSize` work under
`QCoreApplication`. If they need `QGuiApplication`, the worker uses it with the platform
plugin the package already deploys.)

Why the same executable: the worker needs exactly the libraries the application already
loads (Qt Core/Gui, QtPdf/PDFium) and the same build of `inspectPdf`; a second binary would
need its own deployment (windeployqt, macdeployqt, linuxdeploy), its own signature and
notarisation, and could drift from the application's version.

**Start.** The client (§9) starts `QProcess` with:

* program: `QCoreApplication::applicationFilePath()` — the real `studyapp` (on Linux the
  launcher `bin/studyboard` already `exec`s it with `LD_LIBRARY_PATH` set where the bundled
  C++ runtime is needed, and the child inherits that environment; in the AppImage the path
  points into the mounted image, which stays mounted while StudyBoard runs; on macOS it is
  `studyapp.app/Contents/MacOS/studyapp`);
* arguments: `--pdf-worker <job-directory>` only (the PDF path travels in the request, so
  it is never parsed from a command line);
* environment: inherited unchanged;
* working directory: the job directory;
* channels: stdin closed, stdout discarded, stderr captured (kept to the first 4 KiB, for
  the log only — §18).

**Lifetime.** One worker per job; it handles one request and exits. It never outlives its
job: the client kills it on every path that does not end in a normal exit (§8–§11). It
also limits itself (§11): if no request arrives within 10 s, or its own work takes longer
than 60 s, it exits with code 3 — so a worker whose parent died does not linger.

**Exit codes** (diagnostics only; the reply is the contract): 0 a reply was sent; 1 bad
arguments or channels could not be opened; 2 the request was malformed or unsupported (an
error reply was sent when possible); 3 its own time limit. A crash is whatever the OS
reports (`QProcess::CrashExit`).

## 6. Job directory and channels

**One directory, two channels, one process per job.** Nothing is shared between jobs, so a
reply can never reach another job, a stale state left by a killed worker can never affect
the next job (IPCFileLab recovers stale `WRITING`/`READING` states, but a fresh file has
none), and cleanup is "remove the directory".

```
<system temp>/sb-<JobId as 32 hex digits>/      created by StudyBoard (mode 0700 on POSIX)
    request.ipc     StudyBoard → worker   one message: the request
    response.ipc    worker → StudyBoard   one message: the reply
    (POSIX only: request.ipc.lock, .data.fifo, .space.fifo and the same for response.ipc —
     IPCFileLab's lock and notification files; Windows uses named kernel objects whose
     names hash the full path, so they are unique per job as well)
```

* `<system temp>` is `QDir::tempPath()`. Not the workspace's `temporary/`: IPCFileLab
  creates its temporary files with `GetTempFileNameW`, which limits the channel's directory
  to `MAX_PATH` (260) on Windows, and workspace paths are chosen by the user. The system
  temp path plus `sb-` and 32 hex digits is about 80 characters on a typical Windows
  profile; the client fails the job (`IoError`, logged) if the directory path is longer
  than 200 characters instead of risking IPCFileLab errors.
* The staged PDF stays in the workspace's `temporary/` (D48); only its path travels.
* Creation order: the client creates the directory, opens `request.ipc` and `response.ipc`
  (`ipc::FileChannel::open` creates them empty), **sends the request**, then starts the
  worker. A single-slot channel keeps a `READY` message until it is read, so the worker
  finds the request whenever it starts — there is no start-up race.
* The worker opens both channels (they exist), receives the request, sends one reply.
* Each side uses its own handles from one thread only (IPCFileLab: one handle per thread).
* Removal: the client closes its handles and removes the directory recursively on every
  terminal path (§13), after the worker has exited or been killed.

Largest messages: request 32 + path ≤ 32 KiB; reply 40 + 16 × 5000 bytes ≈ 80 KiB — far
below IPCFileLab's 16 MiB limit.

## 7. Job identity

```cpp
// src/core/include/studyapp/core/Ids.hpp
using JobId = Id<struct JobTag>; // a PDF worker job (D53); never persisted
```

* **Generation:** `core::JobId::generate(ids)` with the window's `UuidV7Generator`
  (`ShellServices::ids`) **on the GUI thread** when the import starts (the generator is not
  thread-safe, as for `prepareAssetImport`). UUIDv7 makes collisions practically
  impossible; uniqueness, not secrecy, is what matters (IPC peers are the same user).
* **Ownership and lifetime:** the job's client owns it from start to its terminal state;
  it is never stored in the workspace, the database, settings or the undo history.
* **Serialisation:** the 16 bytes of `Uuid::bytes()`, in order, in every request and every
  reply; `toString()` (canonical text) in log lines and in the job directory name (without
  dashes).
* **Comparison:** `operator==` on `JobId`.
* **Mismatch:** a reply whose JobId is not the job's is a protocol error: the job fails
  (§11), the worker is killed, nothing from the reply is used. With one directory per job a
  mismatch means a broken or foreign writer, so it is not waited out.
* It is a StudyBoard value carried *in* messages; IPCFileLab knows nothing about it.

## 8. Protocol

Binary, little-endian, fixed layout; no general serialisation framework. Encoding and
decoding are pure functions (Qt-free, in `ipc`, unit-tested in every CI configuration).

### 8.1 Request (StudyBoard → worker)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `SBPQ` |
| 4 | 2 | version | `1` |
| 6 | 2 | kind | `1` = inspect PDF |
| 8 | 16 | job | JobId bytes; not nil |
| 24 | 4 | path length *n* | 1 ≤ *n* ≤ 32 768 |
| 28 | 4 | reserved | `0` |
| 32 | *n* | path | UTF-8, absolute, no NUL; the staged copy |

Total size must be exactly 32 + *n*. The worker opens the path read-only (`QPdfDocument::
load`); it never writes, renames or deletes it. A request that breaks any rule is
malformed: the worker replies `InvalidRequest` (with the JobId if the header was readable,
else nil) and exits with code 2. An unknown version or kind gets `UnsupportedRequest`.

### 8.2 Reply (worker → StudyBoard)

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `SBPR` |
| 4 | 2 | version | `1` |
| 6 | 2 | status | see below |
| 8 | 16 | job | the request's JobId |
| 24 | 4 | detail | status-specific number (below), else `0` |
| 28 | 4 | page count *n* | `0` unless status is `Ok`; 1 ≤ *n* ≤ 5000 when `Ok` |
| 32 | 16 × *n* | pages | per page: width, height as IEEE-754 `double`, PDF points |

Total size must be exactly 32 + 16 × *n*.

| Status | Value | Meaning | `detail` | Becomes in StudyBoard |
|---|---:|---|---|---|
| `Ok` | 0 | inspected | 0 | the page sizes (after §12) |
| `InvalidRequest` | 1 | the request was malformed | 0 | `IoError` "the PDF could not be read" |
| `UnsupportedRequest` | 2 | unknown version or kind | 0 | `IoError` "the PDF could not be read" |
| `Unreadable` | 3 | missing or not a readable PDF | 0 | `IoError` "the file is not a readable PDF document" |
| `Protected` | 4 | password or unsupported security scheme | 0 | `Unsupported` "password-protected PDFs are not supported" |
| `NoPages` | 5 | zero pages | 0 | `InvalidArgument` "the PDF has no pages" |
| `TooManyPages` | 6 | more than 5000 pages | the count | `InvalidArgument` "the PDF has *N* pages; at most 5000 are supported" |
| `BadPageSize` | 7 | a page without a supported size | page number (1-based) | `InvalidArgument` "page *N* of the PDF has no supported size" |
| `Internal` | 8 | the worker failed otherwise | 0 | `IoError` "the PDF could not be read" |

There is no free text in the reply: StudyBoard builds every message itself from the status
and the number, using today's wording, so the dialog ("The PDF could not be imported." +
details) reads as it does now and a misbehaving worker cannot put text into it. Unknown
status values are protocol errors.

## 9. State machine (inside the PDF client only)

The client is one function call on the window's job pool thread (§14); its states are an
enum used for its control flow, its log lines and its tests — not a framework.

```
Preparing ──► Starting ──► Waiting ──► Exiting ──► Succeeded
    │             │           │           │
    └─────────────┴───────────┴───────────┴──► Failed(reason)   (any non-terminal state)
                  └───────────┴───────────┴──► Cancelled         (stop requested)
```

| State | Entry | Leaves when | Next |
|---|---|---|---|
| Preparing | job starts | directory created, both channels open, request sent | Starting; any error → Failed(`Setup`) |
| Starting | `QProcess::start` | `waitForStarted` succeeds | Waiting; start failure → Failed(`Start`) |
| Waiting | worker running | a reply arrives | Exiting (after decode + JobId check); bad reply → Failed(`Protocol`); worker gone without reply → Failed(`Crashed`/`Exited`); deadline → Failed(`Timeout`); stop → Cancelled |
| Exiting | valid reply in hand | worker exits normally with code 0 within 2 s | Succeeded; otherwise → Failed(`Exited`/`Crashed`) |
| Succeeded / Failed / Cancelled | terminal | — | cleanup (§13), result to the GUI thread |

`Succeeded` means *the worker answered*: its reply is either `Ok` with page sizes or a PDF
error status (`Unreadable`, `Protected`, `NoPages`, `TooManyPages`, `BadPageSize`), which
becomes the import's error message (§8.2). `Failed` and `Cancelled` mean the job itself did
not complete. Every terminal state runs the same cleanup (§13) exactly once (an RAII guard
in the client). Only an `Ok` reply carries data, and it is validated again (§12) before use.

## 10. Timeouts and cancellation

IPCFileLab's timeouts bound the wait for the channel's *state*, not the wait for its
*lock* (a process that hangs while holding the lock blocks the other side). The worker
holds the lock only for a few file writes, but StudyBoard does not rely on that: the job
has a **wall-clock deadline** owned by the client.

* **Deadline:** 30 s, measured with `std::chrono::steady_clock` from entering Starting.
  (In-process, a 200-page PDF imports in 55 ms, docs/PERFORMANCE.md §3.7; the deadline only
  separates "slow" from "stuck" and is a named constant for S7 to confirm.)
* **Start:** `waitForStarted(5000)`, within the deadline.
* **Waiting:** `receive(200 ms)` in a loop; between slices the client checks the deadline,
  the stop token and `waitForFinished(0)` (the worker's state). The 200 ms slice bounds how
  late a stop or deadline is noticed.
* **Exiting:** `waitForFinished(2000)`.
* **Expiry:** `QProcess::kill()` (no polite terminate — the worker has nothing to save),
  `waitForFinished(1000)`, cleanup, Failed(`Timeout`) → `IoError` "the PDF could not be read
  in time".

**Cancellation** uses one `std::stop_source` per open workspace in `MainWindow`; each PDF
job gets its `std::stop_token`. It is requested by:

| Trigger | Effect |
|---|---|
| Workspace close / switch / window close (`releaseWorkspace`) | `request_stop()` **before** `jobs_->waitForDone()`; every running client kills its worker within one 200 ms slice, cleans up and ends `Cancelled`; the finish step is then dropped as in S5 (the staged file is removed by `StagedImport`) |
| Application shutdown | the window closes the workspace first (same path) |
| A future Cancel command (V-11) | the same token; no other mechanism |

A cancelled job produces no dialog. Closing therefore waits at most one slice plus the
kill, instead of up to the deadline. A stop requested while the job is still staging (the
copy into `temporary/`, before Preparing) is noticed when the copy ends — the copy itself is
not interrupted, as today (D48) — and the job then ends `Cancelled` without starting a
worker.

## 11. Failures

| Situation | Detection | Result |
|---|---|---|
| Directory or channel cannot be created, path too long | Preparing | Failed(`Setup`): `IoError` "the PDF could not be read" |
| `studyapp` cannot be started | `waitForStarted` false / `QProcess::FailedToStart` | Failed(`Start`), same message |
| Worker crashes (any time) | `exitStatus() == CrashExit` | Failed(`Crashed`): `IoError` "the PDF could not be read (the reader stopped)" |
| Worker exits non-zero, or exits before replying | `NormalExit` with code ≠ 0, or finished without a reply | Failed(`Exited`), same message |
| Malformed reply (size, magic, version, status, layout) | decode | Failed(`Protocol`), worker killed, same message |
| Reply with another JobId | decode | Failed(`Protocol`), worker killed |
| Worker hangs | deadline | Failed(`Timeout`), worker killed |
| IPC error on a channel (`ParseError`, `IoError`, `Conflict`) | `receive` | Failed(`Protocol`), worker killed |
| Reply valid but worker then crashes or exits ≠ 0 within 2 s | Exiting | Failed(`Exited`/`Crashed`): the reply is discarded |
| Stop requested | stop token | Cancelled |

**No automatic retry.** IPCFileLab delivers at most once; a lost message, a crash or a
timeout means the job failed. Repeating it would re-run a PDF that just crashed or hung
the parser, doubling the wait for the user without new information. The user can import
again.

## 12. Result validation in StudyBoard

The worker's reply is input, not truth. Before the result reaches `importDocument`, the
client checks — with the same constants and in the same function `inspectPdf` uses today
(S7 extracts it as `validatePdfPageSizes`):

* exact message size for the declared page count; magic, version, status known;
* JobId equals the job's;
* `Ok` ⇒ 1 ≤ *n* ≤ `kMaxPdfPages` (5000);
* every width and height finite, > 0 and ≤ `kMaxPdfPagePoints` (14 400 pt);
* conversion to world units (`document::kUnitsPerPoint`) as today; the document model then
  applies its own invariants when the pages are created (`WorkspaceStructure::
  importDocument`, one undo step "Import PDF", D44).

A reply that fails any check is a protocol failure (§11), never partially used.

## 13. Files and cleanup

| Owner | Files | Rules |
|---|---|---|
| StudyBoard (S5 `StagedImport`) | the staged PDF in `<workspace>/temporary/` | created by `stage()` before the job; stored by `finishAssetImport` on success; removed by the owner on every other path (failed inspection, dropped finish step, workspace closed) |
| StudyBoard (PDF client) | the job directory with both channels | created in Preparing; removed when the client reaches a terminal state, after the worker has exited or been killed |
| Worker | nothing | reads the staged copy; writes only its reply into `response.ipc` |

| Terminal path | Job directory | Staged PDF |
|---|---|---|
| Succeeded | removed by the client | stored (`finishAssetImport`) |
| Failed: worker error reply | removed | removed by `StagedImport` (finish step reports the error) |
| Failed: crash / non-zero exit / protocol | removed after kill | removed by `StagedImport` |
| Failed: timeout | removed after kill | removed by `StagedImport` |
| Failed: setup or start | removed (if created) | removed by `StagedImport` |
| Cancelled (stop, workspace close) | removed after kill | removed by `StagedImport` when the finish step is dropped |
| StudyBoard itself crashes | left in system temp (a few KiB of channel files) | left in `temporary/`; emptied at the next writable open (existing) |

No scanner and no polling: every removal is done by the owner of a known path. The one
leftover case (StudyBoard crashing mid-job) leaves only small files in the system temp
directory, which the OS manages.

## 14. Threading

* **GUI thread:** generates the JobId, prepares the staging function, captures the stop
  token, posts the job with `runInBackground` (D48), and later runs the finish step:
  `finishAssetImport`, `importDocument`, navigation (S5). It never touches `QProcess`,
  never calls `FileChannel`, never waits.
* **Pool thread (the job):** `stage()` (copy + hash), then the whole PDF client: creates the
  `QProcess` and the two `FileChannel`s *on this thread* and uses only blocking calls
  (`waitForStarted`, `waitForFinished(0…)`, `receive(200 ms)`), so no event loop and no
  signal connections are needed. It returns the finish step to the GUI thread exactly as
  imports do today; the workspace-lifetime and window-lifetime checks of `runInBackground`
  stay as they are.
* No permanent IPC thread and no new pool: the job occupies one thread of the window's
  existing `QThreadPool` for its duration.
* Shutdown: `releaseWorkspace` requests stop (§10) and then waits for the pool as today, so
  no client and no worker survive the workspace.

## 15. Security boundary

**Protects against** (during import): a PDF that crashes PDFium, corrupts memory inside
the parser, loops forever or exhausts memory while being opened and measured — the worker
dies or is killed, StudyBoard shows an error and keeps its workspace and unsaved state.
Such a PDF is not stored, so it cannot be opened again from the workspace.

**Does not protect against:**

* PDFs that inspect cleanly but crash PDFium while **rendering** tiles: rendering stays in
  StudyBoard (`SessionDocumentRasterizer`, one thread) — a crash there still ends
  StudyBoard. Only inspection is isolated.
* An attacker who can run code in the worker: the worker runs as the same user with the
  same file-system access; it is process isolation, **not a sandbox** (no reduced
  privileges, no seccomp/AppContainer/App Sandbox).
* Other same-user processes: IPCFileLab channels are files and named objects of the
  current user with no peer authentication (D52); the job directory is created with
  owner-only permissions on POSIX, which limits other users, not the same user.
* Denial of service by size: copying a huge file still costs time and disk space before
  the worker runs (as today).

## 16. Packaging

* No second binary, no second signing or notarisation step: the worker is the signed
  `studyapp` itself, already in every package (Windows installer and ZIP, macOS DMG, Linux
  AppImage and archive).
* Invocation is the same on every platform: `applicationFilePath()` + `--pdf-worker`.
  On Linux the launcher's environment (`LD_LIBRARY_PATH` for the bundled C++ runtime when
  needed) is inherited, which is what the worker needs; the worker must not be started via
  the launcher script.
* S7 extends `studyapp --self-test` with "inspect a PDF in the worker process", so the
  existing package smoke tests (Windows, Ubuntu 22.04 and 24.04, macOS) verify the worker
  in every package without new workflow steps.

## 17. Performance

Measured inputs:

* IPCFileLab round trip on the reference laptop (Windows, NVMe): ≈ 5 ms for a small
  message, ≈ 14 ms for 1 MiB (IPCFileLab README). Request and reply here are ≤ 80 KiB.
* In-process PDF import of a 200-page document: 55 ms (docs/PERFORMANCE.md §3.7); a PDF
  tile: 3.9–6.4 ms.

Not measured (S7 measures, before claiming anything): the start-up time of `studyapp
--pdf-worker` (process creation, Qt and QtPdf initialisation), hence the end-to-end cost
per import. It is paid once per import, on a pool thread, while the copy also runs, so it
does not affect the GUI thread.

**Tiles stay in-process:** each tile would need a request and a reply of up to ~1 MiB of
pixels (≈ 14 ms per round trip measured, against 4–6 ms to render the tile), plus a
long-lived worker or a start per document. Without an end-to-end measurement showing an
acceptable cost, D53 does not move rendering.

## 18. Diagnostics

Through the existing `core::log*` functions (category `pdf-worker`), no new framework:

| Event | Level | Content |
|---|---|---|
| job start | info | JobId, staged file name (not the full path), size in bytes |
| worker started / exited | info | JobId, exit status and code, elapsed ms |
| reply `Ok` | info | JobId, page count |
| worker error reply | warning | JobId, status name and number |
| crash, non-zero exit, protocol error, timeout | warning | JobId, state reached, exit status/code, first 4 KiB of the worker's stderr |
| cancelled | info | JobId |

No PDF content and no page data are logged. The staged file's name is an asset id, so the
log carries no user path for the job itself.

## 19. Tests (S7)

Protocol (Qt-free, `ipc` tests; run in ci-core, ci-full, sanitizers):
* request and reply round trips (encode → decode equal); every status value;
* malformed: short or long buffers, wrong magic, nil JobId, path length 0 / too long /
  mismatching the size, embedded NUL, page count 0 or > 5000 with `Ok`, size mismatch;
* unsupported version and kind;
* page sizes: NaN, infinities, 0, negative, > 14 400 rejected by the validation function.

Client and process lifecycle (`ui` tests with a small test helper executable that plays a
worker according to its arguments — the pattern of IPCFileLab's `IPC.TestHelper` — and the
client's worker program as a parameter):
* worker starts, replies `Ok`, exits 0 → Succeeded with the page sizes;
* program missing → Failed(`Start`); helper crashes (abort) → Failed(`Crashed`); exits 1 →
  Failed(`Exited`); exits 0 without replying → Failed(`Exited`);
* replies with another JobId, with garbage, with an unknown status → Failed(`Protocol`);
* hangs (sleeps past a shortened deadline) → Failed(`Timeout`) and the helper is gone;
* stop requested while waiting → Cancelled within one slice, helper gone;
* after every case: the job directory no longer exists.

Real worker (`studyapp --pdf-worker` built in the tree):
* a valid PDF (the test PDFs already written by `ShellTest`), an invalid file, a
  password-protected PDF, a PDF with more than 5000 pages, a page larger than 14 400 pt —
  the same results and messages as today's `inspectPdf` tests;
* self-test check "inspect a PDF in the worker process".

Shell (`ShellTest`, extending the S5 tests):
* PDF import through the worker; page switch during the job keeps the page (S5 rule);
* close the workspace while a (helper) worker hangs: close returns promptly, no worker
  process left, job directory and staged file gone;
* cleanup after success, worker error, crash and timeout: the workspace's `temporary/`
  and the system temp directory contain nothing of the job.

Concurrency:
* two PDF imports at once: distinct JobIds and job directories, each gets its own reply.

## 20. Rejected alternatives

| Alternative | Why not (now) |
|---|---|
| A second worker executable | Needs its own deployment, signing and notarisation in every package and can drift from the application; the same executable already carries QtPdf |
| A persistent worker or worker pool | One import at a time is the common case; a long-lived worker would need health checks and restart logic, and a crash would affect queued jobs |
| One shared channel pair for all jobs | Replies could reach the wrong job; a killed worker could leave a stale state for the next one; cleanup could not be "remove the directory" |
| One bidirectional channel | The single slot would hold either direction's message; each side would have to distinguish its own message from the peer's. Two one-way channels are simpler and match IPCFileLab's model |
| Sending the PDF bytes over IPC | Files can be hundreds of MB (IPCFileLab's limit is 16 MiB) and every message is written three times; the worker reads the staged copy instead |
| Automatic retries | At-most-once delivery and a crashing or hanging parser: a retry repeats the failure (§11) |
| A generic job framework | One consumer; the existing pool, finish-step and stop-token mechanisms cover it (ARCHITECTURE §10.2) |
| Persisting JobIds | Jobs end with the process; nothing refers to them later |
| Out-of-process tile rendering | Not measured end to end; per-tile IPC costs more than rendering a tile (§17) |
| Changing IPCFileLab (e.g. lock timeouts) | The client's wall-clock deadline and kill already bound every wait; D52 keeps the library unchanged |

## 21. S7 implementation checklist

Modules and build:

1. `core/Ids.hpp`: `using JobId = Id<struct JobTag>;` with the comment "a PDF worker job
   (D53); never persisted".
2. `ipc`: new `include/studyapp/ipc/PdfInspection.hpp` / `src/PdfInspection.cpp` — Qt-free:
   `struct InspectRequest { core::JobId job; std::string path; }`,
   `enum class InspectStatus : std::uint16_t`, `struct InspectReply { core::JobId job;
   InspectStatus status; std::uint32_t detail; std::vector<std::pair<double,double>>
   pageSizesPt; }`, `encode`/`decode` for both (decode returns `core::Result`, errors
   `ParseError`), constants (`kMagicRequest`, `kMagicReply`, `kVersion = 1`, `kMaxPathBytes`,
   `kMaxPages = 5000`).
3. `tools/check_boundaries.py` and `src/ui/CMakeLists.txt`: allow `ui` → `ipc` (a Qt-free
   leaf; no other boundary changes).
4. `ui`: extract from `inspectPdf` a pure `validatePdfPageSizes` (count and size limits,
   point → world units) and an `inspectPdfLocally` that returns a status + detail; keep the
   user-facing message builder in one place.
5. `ui`: `PdfWorker.hpp/.cpp` — `int runPdfWorker(const QString& jobDirectory)` (open both
   channels, receive with a 10 s limit, inspect, reply, exit codes of §5, 60 s self-limit).
6. `ui`: `PdfInspectionClient.hpp/.cpp` — `core::Result<PdfInfo> inspectPdfInWorker(const
   std::filesystem::path& staged, core::JobId job, std::stop_token stop, const
   PdfClientOptions& options)` with `options.program` (default `applicationFilePath()`),
   `options.deadline` (30 s), `options.slice` (200 ms); the state enum of §9 for logging and
   tests; an RAII guard for kill + directory removal.
7. `app/main.cpp`: if the first argument is `--pdf-worker`, run the worker under
   `QCoreApplication` **before** `QApplication` is constructed; verify on all three
   platforms that QtPdf inspection works there.
8. `MainWindow::importPdf`: generate the JobId on the GUI thread; in the job, stage first,
   then `inspectPdfInWorker(staged, job, stop)`; keep `StagedImport`, the S5 navigation rule
   and the existing messages; `std::stop_source` per open workspace, `request_stop()` in
   `releaseWorkspace` before `jobs_->waitForDone()`, a fresh source for the next workspace.
9. `SelfTest`: add "inspect a PDF in the worker process" (packages' smoke tests then cover it).

Tests: §19, including a small helper executable under `tests/ui/` registered like the other
test programs.

Documentation: mark this document "implemented" with the measured worker start-up and
import times (docs/PERFORMANCE.md §3.6–3.7, BENCHMARKS.md), update ARCHITECTURE §10.1
(threads/processes in use) and D52 ("first consumer: D53"), TESTING.md (new tests).

Not in S7: tile rendering, a Cancel button (V-11), any change to IPCFileLab or to the
document model, schema or persistence.
