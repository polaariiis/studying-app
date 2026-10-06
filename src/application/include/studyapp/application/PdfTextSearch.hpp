#pragma once

#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Workspace.hpp>

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace studyapp::application {

// Searching the text inside imported PDFs (1.2-CMD-01, docs/COMMAND_CONSOLE.md §6). The text
// is extracted by the PDF worker process (docs/PDF_WORKER.md §23); everything here is plain
// data and Qt-free: what to search (the PDFs a workspace's pages show), how text matches,
// and a bounded cache of extracted text.

/// The text of one PDF, one UTF-8 string per page (0-based), as the worker extracted it.
struct PdfDocumentText {
    std::vector<std::string> pages;
    bool truncated = false; ///< the worker cut text at its limits (docs/PDF_WORKER.md §23)

    /// Bytes of text held (for the cache's budget).
    [[nodiscard]] std::size_t byteSize() const noexcept;
};

/// One place where a query occurs in a PDF's text.
struct PdfTextMatch {
    std::size_t page = 0;   ///< 0-based page of the PDF
    std::size_t offset = 0; ///< byte offset of the match in that page's text
    std::string context;    ///< the text around it on one line, "…" where it was cut
};

/// Most matches findInPdfText reports per document unless told otherwise.
inline constexpr std::size_t kMaxPdfMatchesPerDocument = 50;

/// The query as it is matched: whitespace runs become one space, leading and trailing
/// whitespace is dropped. Empty when nothing is left.
[[nodiscard]] std::string normalizePdfQuery(std::string_view query);

/// Occurrences of `query` in `text`, in page order then position, at most `limit`, each
/// with about `contextBytes` of text on either side (cut at word boundaries where possible).
/// Matching ignores case (ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic letters),
/// treats every run of whitespace — including the line breaks PDFs put inside sentences —
/// as one space, and keeps punctuation as written. Overlapping occurrences are not
/// reported twice. An empty (normalized) query matches nothing. Text that is not valid
/// UTF-8 is matched byte by byte where it is broken.
[[nodiscard]] std::vector<PdfTextMatch> findInPdfText(const PdfDocumentText& text,
                                                      std::string_view query,
                                                      std::size_t limit = kMaxPdfMatchesPerDocument,
                                                      std::size_t contextBytes = 40);

/// A section of the workspace showing pages of an imported PDF, and which of its pages show
/// which PDF page.
struct PdfSource {
    core::AssetId asset;
    core::NotebookId notebook;
    core::SectionId section;
    std::string title; ///< "Notebook › Section"
    /// (PDF page index, page) for every page of the section that shows this PDF, ordered by
    /// PDF page index, then section order.
    std::vector<std::pair<std::int32_t, core::PageId>> pages;

    /// The section's page showing PDF page `index` (the first one, if several do).
    [[nodiscard]] std::optional<core::PageId> pageFor(std::size_t index) const;
};

/// Every (section, PDF) pair of `workspace`, in workspace order (notebook, section). A PDF
/// imported twice appears once per section; pages showing different PDFs within a section
/// give one entry per PDF.
[[nodiscard]] std::vector<PdfSource> pdfSourcesOf(const document::Workspace& workspace);

/// Extracted PDF text kept between searches, keyed by asset: assets are content-addressed
/// (SHA-256) and never change, so an entry never goes stale. Bounded by a byte budget and an
/// entry count; the least recently used entries are dropped first; a document larger than
/// the whole budget is not kept. Thread-safe (searches run on pool threads).
class PdfTextCache {
public:
    static constexpr std::size_t kDefaultByteBudget = std::size_t{32} * 1024U * 1024U;
    static constexpr std::size_t kDefaultMaxEntries = 64;

    explicit PdfTextCache(std::size_t byteBudget = kDefaultByteBudget,
                          std::size_t maxEntries = kDefaultMaxEntries);

    [[nodiscard]] std::shared_ptr<const PdfDocumentText> find(core::AssetId asset);
    void insert(core::AssetId asset, std::shared_ptr<const PdfDocumentText> text);
    void clear();

    struct Stats {
        std::size_t entries = 0;
        std::size_t bytes = 0;
        std::size_t byteBudget = 0;
        std::size_t maxEntries = 0;
        std::size_t hits = 0;
        std::size_t misses = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Entry {
        core::AssetId asset;
        std::shared_ptr<const PdfDocumentText> text;
        std::size_t bytes = 0;
    };
    void evict(); ///< with mutex_ held

    mutable std::mutex mutex_;
    std::size_t byteBudget_;
    std::size_t maxEntries_;
    std::size_t bytes_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::list<Entry> entries_; ///< most recently used first
    std::unordered_map<core::AssetId, std::list<Entry>::iterator> index_;
};

} // namespace studyapp::application
