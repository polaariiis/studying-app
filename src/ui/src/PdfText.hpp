#pragma once

#include <studyapp/application/PdfTextSearch.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/ipc/PdfInspection.hpp>

#include <filesystem>

namespace studyapp::ui {

// The text inside PDFs (1.2-CMD-01, docs/PDF_WORKER.md §23): extracted with QtPdf in the PDF
// worker process only — StudyBoard itself never parses a PDF to search it — and checked
// again when the reply arrives.

/// Opens `file` with QtPdf and reads the text of every page, bounded by the protocol's
/// limits (ipc::pdf::kMaxPageTextBytes per page, kMaxTextBytes in all; text is cut at a
/// character boundary and the reply marked truncated). Control characters other than line
/// breaks and tabs become spaces. The worker runs this for an ExtractText request; the
/// reply's job is left nil. Never throws; only reads the file.
[[nodiscard]] ipc::pdf::TextReply extractPdfTextLocally(const std::filesystem::path& file);

/// What a text reply means: the document's text, or the error the user sees (the same
/// wording as for imports: IoError unreadable or not a PDF, Unsupported protected,
/// InvalidArgument no pages or too many).
[[nodiscard]] core::Result<application::PdfDocumentText>
pdfTextFrom(const ipc::pdf::TextReply& reply);

} // namespace studyapp::ui
