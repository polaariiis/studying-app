#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Workspace.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::application {

// Batch export from the command console (1.2-CMD-03, docs/COMMAND_CONSOLE.md §10): what a
// batch writes (one file per section of a notebook, with file names no platform refuses for
// their characters or length, distinct within the batch) and a sequential runner that
// honours a stop flag and keeps going after a failed item. Everything here is plain data and
// Qt-free; the caller supplies the function that writes one item.

/// Most Unicode code points exportFileName keeps of a title (the "_" of a device name, any
/// " (2)" and the extension come on top).
inline constexpr std::size_t kMaxExportNameLength = 100;
/// Most UTF-8 bytes it keeps of a title. Linux and macOS file systems hold 255 bytes per
/// name: this leaves 55 for the "_" of a device name, a " (n)" (at most 23 bytes for a
/// std::size_t) and an extension of up to 31 bytes.
inline constexpr std::size_t kMaxExportNameBytes = 200;

/// A file name (without extension) from a title: `\ / : * ? " < > |`, control characters
/// and bytes that are not valid UTF-8 become "_"; leading and trailing spaces and trailing
/// dots are removed; at most kMaxExportNameLength code points and kMaxExportNameBytes bytes
/// are kept (a UTF-8 sequence is never split, and what the cut leaves is trimmed again); a
/// name whose part before the first dot is a Windows device name (CON, PRN, AUX, NUL,
/// COM1–COM9, LPT1–LPT9, any case) gets "_" after that part; an empty result is `fallback`.
/// The result is valid UTF-8 of at most kMaxExportNameBytes + 1 bytes (a `fallback` that
/// long at most) without a character Windows, Linux or macOS refuses in a file name.
/// Whether a folder accepts it is not checked: the length of the whole path is not known
/// here.
[[nodiscard]] std::string exportFileName(std::string_view title,
                                         std::string_view fallback = "Section");

/// One section of a batch export.
struct BatchExportItem {
    core::SectionId section;
    std::string title;
    std::vector<core::PageId> pages; ///< in section order; empty: the section is skipped
    std::string fileName;            ///< with the extension; empty for a skipped section

    [[nodiscard]] bool skipped() const noexcept { return pages.empty(); }
};

/// Every section of `notebook`, in notebook order, with its pages and the file it is
/// written to: exportFileName(title) plus `extension`. Names that are equal ignoring ASCII
/// case get " (2)", " (3)", … in notebook order, so the file names of a plan are distinct
/// ignoring ASCII case. Names that differ only in the case of other letters, or in Unicode
/// normalisation, stay as they are and can be one file on a case-insensitive file system.
/// A file name is at most 255 bytes with an `extension` of up to 31 bytes. A section
/// without pages is listed as skipped and takes no file name. Errors: NotFound (the
/// notebook does not exist).
[[nodiscard]] core::Result<std::vector<BatchExportItem>>
planNotebookExport(const document::Workspace& workspace, core::NotebookId notebook,
                   std::string_view extension = ".pdf");

/// What became of one item of a batch.
struct BatchExportOutcome {
    enum class State : std::uint8_t {
        NotStarted, ///< the batch was cancelled before or while it was written
        Done,
        Skipped, ///< nothing to write (a section without pages)
        Failed
    };
    State state = State::NotStarted;
    std::string message; ///< why it failed

    [[nodiscard]] friend bool operator==(const BatchExportOutcome&,
                                         const BatchExportOutcome&) = default;
};

struct BatchExportResult {
    std::vector<BatchExportOutcome> outcomes; ///< one per item, in order
    bool cancelled = false;

    [[nodiscard]] std::size_t count(BatchExportOutcome::State state) const noexcept;
};

/// Writes one item. Conflict means cancelled; any other error is that item's failure.
using BatchExportWriter = std::function<core::Result<void>(const BatchExportItem&)>;
/// Called after each item that was written, skipped or failed, with its index.
using BatchExportProgress = std::function<void(std::size_t, const BatchExportOutcome&)>;

/// Runs `write` for each item in order on the calling thread. `stop` is read before each
/// item: once it is set, or once `write` returns Conflict, the batch ends as cancelled and
/// the remaining items (and the one that returned Conflict) stay NotStarted. A failed item,
/// including one whose `write` throws, does not end the batch. Skipped items are reported
/// without calling `write`. `progress` is optional; an exception it throws is not caught
/// here and ends the batch.
[[nodiscard]] BatchExportResult runBatchExport(std::span<const BatchExportItem> items,
                                               const BatchExportWriter& write,
                                               const std::atomic<bool>& stop,
                                               const BatchExportProgress& progress = {});

} // namespace studyapp::application
