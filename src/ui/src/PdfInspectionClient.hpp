#pragma once

#include "SessionDocumentRasterizer.hpp"

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/ipc/PdfInspection.hpp>

#include <QString>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>

namespace studyapp::ui {

// The StudyBoard side of the PDF inspection worker (docs/PDF_WORKER.md, D53). One call
// runs one job to its end on the calling thread with blocking calls only — never call it
// on the GUI thread. It owns the job directory and the worker process; the staged PDF
// belongs to the caller (StagedImport).

struct PdfWorkerOptions {
    /// The executable started with `--pdf-worker <dir>`; empty: this application
    /// (QCoreApplication::applicationFilePath()).
    QString program;
    /// Arguments placed before `--pdf-worker` (test helpers choose a behaviour this way).
    QStringList arguments;
    /// Where job directories are created; empty: QDir::tempPath().
    std::filesystem::path tempRoot;
    std::chrono::milliseconds deadline{30'000};    ///< wall clock, from starting the worker
    std::chrono::milliseconds startTimeout{5'000}; ///< within the deadline
    std::chrono::milliseconds exitTimeout{2'000};  ///< after a reply
    std::chrono::milliseconds slice{200};          ///< one receive; bounds stop and deadline checks
};

/// The client's states (§9). Only used by the PDF client, for its control flow and logs.
enum class PdfJobState {
    Preparing,
    Starting,
    Waiting,
    Exiting,
    Succeeded, ///< the worker answered (Ok or a PDF error status)
    Failed,
    Cancelled,
};

enum class PdfJobFailure {
    None,
    Setup,    ///< job directory or channels
    Start,    ///< the worker could not be started
    Crashed,  ///< the worker crashed
    Exited,   ///< it exited non-zero, without a reply, or did not exit after replying
    Protocol, ///< malformed reply, another job's reply, or an IPC error
    Timeout,  ///< the deadline passed
};

struct PdfJobResult {
    PdfJobState state = PdfJobState::Failed;
    PdfJobFailure failure = PdfJobFailure::Setup;
    PdfJobState reached = PdfJobState::Preparing; ///< the last non-terminal state
    std::optional<ipc::pdf::InspectReply> reply;  ///< with Succeeded, already decoded
    std::filesystem::path directory;              ///< the job directory (removed by now)
};

/// `<tempRoot>/sb-<job as 32 hex digits>`.
[[nodiscard]] std::filesystem::path pdfJobDirectory(const std::filesystem::path& tempRoot,
                                                    core::JobId job);

/// Runs one inspection job for the staged PDF `staged` (absolute). `stop` ends it as
/// Cancelled within one slice (the worker is killed). The job directory is removed and the
/// worker is gone when this returns, on every path.
[[nodiscard]] PdfJobResult runPdfInspectionJob(const std::filesystem::path& staged, core::JobId job,
                                               const std::atomic<bool>& stop,
                                               const PdfWorkerOptions& options = {});

/// runPdfInspectionJob, and what it means for the import: pdfInfoFrom(reply) when the
/// worker answered; IoError "the PDF could not be read…" when the job failed; Conflict
/// when it was cancelled (no dialog: the workspace is closing).
[[nodiscard]] core::Result<PdfInfo> inspectPdfInWorker(const std::filesystem::path& staged,
                                                       core::JobId job,
                                                       const std::atomic<bool>& stop,
                                                       const PdfWorkerOptions& options = {});

} // namespace studyapp::ui
