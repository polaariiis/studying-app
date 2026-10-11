#include <studyapp/application/BatchExport.hpp>

#include <algorithm>
#include <exception>
#include <unordered_set>
#include <utility>

namespace studyapp::application {

namespace {

/// Bytes of the well-formed UTF-8 character at `at` and its code point; 0 if the bytes
/// there are not one (truncated, overlong, a surrogate, above U+10FFFF, a stray byte).
std::size_t decode(std::string_view text, std::size_t at, char32_t& code) {
    const auto lead = static_cast<unsigned char>(text[at]);
    if (lead < 0x80U) {
        code = lead;
        return 1;
    }
    std::size_t length = 0;
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
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[at + k]);
        if ((next & 0xC0U) != 0x80U) {
            return 0;
        }
        code = (code << 6U) | (next & 0x3FU);
    }
    const char32_t smallest = length == 2 ? 0x80U : length == 3 ? 0x800U : 0x10000U;
    if (code < smallest || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
        return 0;
    }
    return length;
}

bool isForbidden(char32_t c) noexcept {
    constexpr std::u32string_view kReserved = U"\\/:*?\"<>|";
    return c < 0x20U || (c >= 0x7FU && c <= 0x9FU) || kReserved.find(c) != kReserved.npos;
}

/// Removes trailing spaces and dots (Windows drops them, so two names would be one file).
void trimEnd(std::string& name) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
}

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string lowered(std::string_view text) {
    std::string folded(text);
    std::transform(folded.begin(), folded.end(), folded.begin(), lower);
    return folded;
}

/// CON, PRN, AUX, NUL, COM1–COM9, LPT1–LPT9, ignoring ASCII case.
bool isDeviceName(std::string_view stem) {
    const std::string name = lowered(stem);
    if (name == "con" || name == "prn" || name == "aux" || name == "nul") {
        return true;
    }
    return name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) &&
           name[3] >= '1' && name[3] <= '9';
}

} // namespace

std::string exportFileName(std::string_view title, std::string_view fallback) {
    std::string name;
    name.reserve(title.size());
    std::size_t length = 0; // code points kept
    for (std::size_t at = 0; at < title.size() && length < kMaxExportNameLength;) {
        char32_t code = 0;
        const std::size_t bytes = decode(title, at, code);
        if (bytes == 0 || isForbidden(code)) {
            if (name.size() + 1 > kMaxExportNameBytes) {
                break;
            }
            name.push_back('_');
            at += std::max<std::size_t>(bytes, 1);
        } else if (code == U' ' && name.empty()) {
            at += bytes; // leading spaces
            continue;
        } else {
            if (name.size() + bytes > kMaxExportNameBytes) {
                break; // a whole character or nothing
            }
            name.append(title.substr(at, bytes));
            at += bytes;
        }
        ++length;
    }
    trimEnd(name);
    // Windows treats the part before the first dot as the device, whatever follows it.
    if (const std::size_t stem = std::min(name.find('.'), name.size());
        isDeviceName(std::string_view(name).substr(0, stem))) {
        name.insert(stem, 1, '_');
    }
    return name.empty() ? std::string(fallback) : name;
}

core::Result<std::vector<BatchExportItem>> planNotebookExport(const document::Workspace& workspace,
                                                              core::NotebookId notebook,
                                                              std::string_view extension) {
    if (workspace.findNotebook(notebook) == nullptr) {
        return core::makeError(core::ErrorCode::NotFound, "the notebook does not exist");
    }
    std::vector<BatchExportItem> items;
    std::unordered_set<std::string> used; // names taken, ASCII case folded
    for (const core::SectionId section : workspace.sectionsOf(notebook)) {
        const auto pages = workspace.pagesOf(section);
        BatchExportItem item{.section = section,
                             .title = workspace.findSection(section)->title,
                             .pages = {pages.begin(), pages.end()},
                             .fileName = {}};
        if (!item.skipped()) {
            const std::string base = exportFileName(item.title);
            std::string name = base;
            // The numbered name can itself be another section's title: count on until free.
            for (std::size_t n = 2; !used.insert(lowered(name)).second; ++n) {
                name = base + " (" + std::to_string(n) + ")";
            }
            item.fileName = name + std::string(extension);
        }
        items.push_back(std::move(item));
    }
    return items;
}

std::size_t BatchExportResult::count(BatchExportOutcome::State state) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(outcomes.begin(), outcomes.end(),
                      [state](const BatchExportOutcome& o) { return o.state == state; }));
}

BatchExportResult runBatchExport(std::span<const BatchExportItem> items,
                                 const BatchExportWriter& write, const std::atomic<bool>& stop,
                                 const BatchExportProgress& progress) {
    using State = BatchExportOutcome::State;
    BatchExportResult result;
    result.outcomes.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (stop.load()) {
            result.cancelled = true;
            break;
        }
        BatchExportOutcome& outcome = result.outcomes[i];
        if (items[i].skipped()) {
            outcome.state = State::Skipped;
        } else {
            try {
                if (const auto written = write(items[i])) {
                    outcome.state = State::Done;
                } else if (written.error().code == core::ErrorCode::Conflict) {
                    result.cancelled = true;
                    break;
                } else {
                    outcome = {.state = State::Failed, .message = written.error().message};
                }
            } catch (const std::exception& error) {
                outcome = {.state = State::Failed, .message = error.what()};
            } catch (...) {
                outcome = {.state = State::Failed, .message = "unknown error"};
            }
        }
        if (progress) {
            progress(i, outcome);
        }
    }
    return result;
}

} // namespace studyapp::application
