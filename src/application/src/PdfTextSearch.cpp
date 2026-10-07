#include <studyapp/application/PdfTextSearch.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace studyapp::application {

namespace {

/// A page's text folded for matching: one entry per folded code point, with the byte range
/// of the source character it came from.
struct Folded {
    std::u32string chars;
    std::vector<std::uint32_t> begin; ///< byte offset of the source character
    std::vector<std::uint32_t> end;   ///< byte offset just after it
};

/// Bytes that are not valid UTF-8 become code points above U+10FFFF, so they match only
/// themselves.
constexpr char32_t kBrokenByte = 0x110000;

/// Decodes one character at `at`; returns its code point and advances `at`.
char32_t decode(std::string_view text, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80U) {
        ++at;
        return lead;
    }
    std::size_t length = 0;
    char32_t code = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        code = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        code = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        code = lead & 0x07U;
    }
    if (length == 0 || at + length > text.size()) {
        ++at;
        return kBrokenByte + lead;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[at + k]);
        if ((next & 0xC0U) != 0x80U) {
            ++at;
            return kBrokenByte + lead;
        }
        code = (code << 6U) | (next & 0x3FU);
    }
    const char32_t smallest = length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U;
    if (code < smallest || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
        ++at;
        return kBrokenByte + lead;
    }
    at += length;
    return code;
}

bool isWhitespace(char32_t c) noexcept {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f' || c == U'\v' ||
           c == 0x00A0U || (c >= 0x2000U && c <= 0x200AU) || c == 0x2028U || c == 0x2029U ||
           c == 0x202FU || c == 0x205FU || c == 0x3000U;
}

/// Simple case folding for the scripts study material is mostly written in, plus the
/// typographic punctuation PDFs use where people type ASCII.
char32_t foldOne(char32_t c) noexcept {
    if (c < 0x80U) {
        return c >= U'A' && c <= U'Z' ? c + 32 : c;
    }
    if (c >= 0x00C0U && c <= 0x00DEU && c != 0x00D7U) {
        return c + 32; // Latin-1 capitals
    }
    if (c >= 0x0100U && c <= 0x017FU) { // Latin Extended-A
        if (c == 0x0130U) {
            return U'i';
        }
        if (c == 0x0178U) {
            return 0x00FFU;
        }
        if (c == 0x017FU) {
            return U's';
        }
        const bool oddCapitals = (c >= 0x0139U && c <= 0x0148U) || (c >= 0x0179U && c <= 0x017EU);
        if (c == 0x0131U || c == 0x0138U || c == 0x0149U) {
            return c;
        }
        if (oddCapitals) {
            return (c % 2U == 1U) ? c + 1 : c;
        }
        return (c % 2U == 0U) ? c + 1 : c;
    }
    if (c >= 0x0391U && c <= 0x03A9U && c != 0x03A2U) {
        return c + 32; // Greek capitals
    }
    switch (c) {
    case 0x0386U:
        return 0x03ACU;
    case 0x0388U:
    case 0x0389U:
    case 0x038AU:
        return c + 37;
    case 0x038CU:
        return 0x03CCU;
    case 0x038EU:
    case 0x038FU:
        return c + 63;
    case 0x03C2U:
        return 0x03C3U; // final sigma
    case 0x2018U:
    case 0x2019U:
    case 0x201BU:
    case 0x2032U:
        return U'\'';
    case 0x201CU:
    case 0x201DU:
    case 0x201FU:
    case 0x2033U:
        return U'"';
    case 0x2010U:
    case 0x2011U:
    case 0x2012U:
    case 0x2013U:
    case 0x2014U:
    case 0x2212U:
        return U'-';
    default:
        break;
    }
    if (c >= 0x0410U && c <= 0x042FU) {
        return c + 32; // Cyrillic capitals
    }
    if (c >= 0x0400U && c <= 0x040FU) {
        return c + 80;
    }
    return c;
}

/// Ligatures PDFs keep in their text, spelled out.
std::u32string_view expandLigature(char32_t c) noexcept {
    switch (c) {
    case 0xFB00U:
        return U"ff";
    case 0xFB01U:
        return U"fi";
    case 0xFB02U:
        return U"fl";
    case 0xFB03U:
        return U"ffi";
    case 0xFB04U:
        return U"ffl";
    default:
        return {};
    }
}

/// Folds `text` (case, ligatures, whitespace runs → one space; no leading space).
Folded fold(std::string_view text) {
    Folded out;
    out.chars.reserve(text.size());
    out.begin.reserve(text.size());
    out.end.reserve(text.size());
    bool pendingSpace = false;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto begin = static_cast<std::uint32_t>(at);
        const char32_t c = decode(text, at);
        const auto end = static_cast<std::uint32_t>(at);
        if (c == 0x00ADU || c == 0x200BU || c == 0xFEFFU) {
            continue; // soft hyphen, zero-width space, BOM: invisible
        }
        if (isWhitespace(c)) {
            pendingSpace = !out.chars.empty();
            continue;
        }
        if (pendingSpace) {
            out.chars.push_back(U' ');
            out.begin.push_back(begin);
            out.end.push_back(begin);
            pendingSpace = false;
        }
        const std::u32string_view ligature = expandLigature(c);
        if (!ligature.empty()) {
            for (const char32_t part : ligature) {
                out.chars.push_back(part);
                out.begin.push_back(begin);
                out.end.push_back(end);
            }
            continue;
        }
        out.chars.push_back(foldOne(c));
        out.begin.push_back(begin);
        out.end.push_back(end);
    }
    return out;
}

bool isContinuation(char c) noexcept {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

/// `text[begin, end)` widened by `around` bytes on each side (cut at character boundaries),
/// on one line with whitespace runs collapsed, "…" where text was left out.
std::string contextOf(std::string_view text, std::size_t begin, std::size_t end,
                      std::size_t around) {
    std::size_t from = begin > around ? begin - around : 0;
    while (from > 0 && isContinuation(text[from])) {
        --from;
    }
    std::size_t to = std::min(text.size(), end + around);
    while (to < text.size() && isContinuation(text[to])) {
        ++to;
    }
    // Whole words at the edges: start after the first space, end before the last one, as
    // long as at least half of the context is left (a very long word is cut instead).
    if (from > 0) {
        const std::size_t space = text.find_first_of(" \t\r\n", from);
        if (space != std::string_view::npos && space < begin && begin - (space + 1) >= around / 2) {
            from = space + 1;
        }
    }
    if (to < text.size()) {
        const std::size_t space = text.find_last_of(" \t\r\n", to - 1);
        if (space != std::string_view::npos && space >= end && space - end >= around / 2) {
            to = space;
        }
    }
    std::string out;
    out.reserve(to - from + 8);
    if (from > 0) {
        out += "…";
    }
    bool space = false;
    for (std::size_t i = from; i < to; ++i) {
        const char c = text[i];
        const bool blank = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
                           c == '\v' || static_cast<unsigned char>(c) < 0x20U;
        if (blank) {
            space = true;
            continue;
        }
        if (space && !out.empty() && out != "…") {
            out += ' ';
        }
        space = false;
        out += c;
    }
    if (to < text.size()) {
        out += "…";
    }
    return out;
}

} // namespace

std::size_t PdfDocumentText::byteSize() const noexcept {
    std::size_t bytes = 0;
    for (const std::string& page : pages) {
        bytes += page.size() + sizeof(std::string);
    }
    return bytes;
}

std::string normalizePdfQuery(std::string_view query) {
    std::string out;
    bool space = false;
    for (const char c : query) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        out += c;
    }
    return out;
}

std::vector<PdfTextMatch> findInPdfText(const PdfDocumentText& text, std::string_view query,
                                        std::size_t limit, std::size_t contextBytes) {
    std::vector<PdfTextMatch> matches;
    const Folded needle = fold(normalizePdfQuery(query));
    if (needle.chars.empty() || limit == 0) {
        return matches;
    }
    for (std::size_t page = 0; page < text.pages.size(); ++page) {
        const std::string& source = text.pages[page];
        if (source.empty()) {
            continue;
        }
        const Folded haystack = fold(source);
        std::size_t from = 0;
        for (;;) {
            const std::size_t found = haystack.chars.find(needle.chars, from);
            if (found == std::u32string::npos) {
                break;
            }
            const std::size_t last = found + needle.chars.size() - 1;
            const std::size_t begin = haystack.begin[found];
            const std::size_t end = haystack.end[last];
            matches.push_back({.page = page,
                               .offset = begin,
                               .context = contextOf(source, begin, end, contextBytes)});
            if (matches.size() >= limit) {
                return matches;
            }
            from = found + needle.chars.size();
        }
    }
    return matches;
}

std::optional<core::PageId> PdfSource::pageFor(std::size_t index) const {
    const auto found =
        std::lower_bound(pages.begin(), pages.end(), static_cast<std::int64_t>(index),
                         [](const auto& entry, std::int64_t value) { return entry.first < value; });
    if (found == pages.end() || static_cast<std::size_t>(found->first) != index) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<PdfSource> pdfSourcesOf(const document::Workspace& workspace) {
    std::vector<PdfSource> sources;
    for (const core::NotebookId notebook : workspace.notebooks()) {
        const document::NotebookInfo* notebookInfo = workspace.findNotebook(notebook);
        for (const core::SectionId section : workspace.sectionsOf(notebook)) {
            const document::SectionInfo* sectionInfo = workspace.findSection(section);
            const std::size_t first = sources.size();
            for (const core::PageId page : workspace.pagesOf(section)) {
                const document::PageInfo* info = workspace.findPage(page);
                if (info == nullptr || !info->document) {
                    continue;
                }
                auto source = std::find_if(
                    sources.begin() + static_cast<std::ptrdiff_t>(first), sources.end(),
                    [&](const PdfSource& s) { return s.asset == info->document->asset; });
                if (source == sources.end()) {
                    sources.push_back({.asset = info->document->asset,
                                       .notebook = notebook,
                                       .section = section,
                                       .title = notebookInfo->title + " › " + sectionInfo->title,
                                       .pages = {}});
                    source = sources.end() - 1;
                }
                source->pages.emplace_back(info->document->index, page);
            }
            for (auto it = sources.begin() + static_cast<std::ptrdiff_t>(first);
                 it != sources.end(); ++it) {
                std::stable_sort(it->pages.begin(), it->pages.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
            }
        }
    }
    return sources;
}

// ---------------------------------------------------------------------------- cache

PdfTextCache::PdfTextCache(std::size_t byteBudget, std::size_t maxEntries)
    : byteBudget_(byteBudget), maxEntries_(std::max<std::size_t>(maxEntries, 1)) {}

std::shared_ptr<const PdfDocumentText> PdfTextCache::find(core::AssetId asset) {
    const std::lock_guard lock(mutex_);
    const auto found = index_.find(asset);
    if (found == index_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found->second); // most recently used
    return found->second->text;
}

void PdfTextCache::insert(core::AssetId asset, std::shared_ptr<const PdfDocumentText> text) {
    if (!text) {
        return;
    }
    const std::size_t bytes = text->byteSize();
    const std::lock_guard lock(mutex_);
    if (const auto found = index_.find(asset); found != index_.end()) {
        bytes_ -= found->second->bytes;
        entries_.erase(found->second);
        index_.erase(found);
    }
    if (bytes > byteBudget_) {
        return; // would push out everything else and still not fit
    }
    entries_.push_front({.asset = asset, .text = std::move(text), .bytes = bytes});
    index_[asset] = entries_.begin();
    bytes_ += bytes;
    evict();
}

void PdfTextCache::clear() {
    const std::lock_guard lock(mutex_);
    entries_.clear();
    index_.clear();
    bytes_ = 0;
}

PdfTextCache::Stats PdfTextCache::stats() const {
    const std::lock_guard lock(mutex_);
    return {.entries = entries_.size(),
            .bytes = bytes_,
            .byteBudget = byteBudget_,
            .maxEntries = maxEntries_,
            .hits = hits_,
            .misses = misses_};
}

void PdfTextCache::evict() {
    while (!entries_.empty() && (bytes_ > byteBudget_ || entries_.size() > maxEntries_)) {
        const Entry& oldest = entries_.back();
        bytes_ -= oldest.bytes;
        index_.erase(oldest.asset);
        entries_.pop_back();
    }
}

} // namespace studyapp::application
