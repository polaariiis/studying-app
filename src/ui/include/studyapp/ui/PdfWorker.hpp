#pragma once

#include <QString>

namespace studyapp::ui {

/// The argument that starts StudyBoard as a PDF inspection worker (docs/PDF_WORKER.md, D53):
/// `studyapp --pdf-worker <job-directory>`. Checked before any QApplication exists.
inline constexpr const char* kPdfWorkerArgument = "--pdf-worker";

/// Runs the PDF inspection worker in this process, which must have a QCoreApplication and
/// nothing else (no window, no settings, no workspace). It opens the job directory's
/// `request.ipc` and `response.ipc`, takes one request, inspects the staged PDF it names
/// (read only), sends one reply and returns the process exit code: 0 a reply was sent;
/// 1 bad arguments or the channels could not be used; 2 the request was malformed or
/// unsupported (an error reply was sent when possible); 3 no request within 10 s. The
/// process also ends itself with code 3 after 60 s, so it never outlives a dead parent.
[[nodiscard]] int runPdfWorker(const QString& jobDirectory);

} // namespace studyapp::ui
