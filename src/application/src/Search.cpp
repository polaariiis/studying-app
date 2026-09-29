#include <studyapp/application/Search.hpp>

#include <studyapp/application/WorkspaceStructure.hpp>

#include <algorithm>

namespace studyapp::application {

namespace {

bool isContinuation(char c) noexcept {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), lowerAscii);
    return out;
}

/// "Notebook › Section" of a page ("" if the page is unknown).
std::string placeOf(const document::Workspace& ws, core::PageId page) {
    const document::PageInfo* info = ws.findPage(page);
    const document::SectionInfo* section =
        info != nullptr ? ws.findSection(info->section) : nullptr;
    const document::NotebookInfo* notebook =
        section != nullptr ? ws.findNotebook(section->notebook) : nullptr;
    if (notebook == nullptr) {
        return {};
    }
    return notebook->title + " › " + section->title;
}

std::string firstLine(std::string_view text, std::size_t limit = 60) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    std::size_t cut = std::min(end, limit);
    while (cut > 0 && cut < text.size() && isContinuation(text[cut])) {
        --cut;
    }
    std::string line(text.substr(0, cut));
    if (cut < end) {
        line += "…";
    }
    return line;
}

} // namespace

std::string snippetOf(std::string_view text, std::string_view query, std::size_t width) {
    if (text.empty()) {
        return {};
    }
    const std::string haystack = lowered(text);
    std::size_t found = std::string::npos;
    std::size_t at = 0;
    while (at < query.size()) {
        while (at < query.size() && query[at] == ' ') {
            ++at;
        }
        const std::size_t start = at;
        while (at < query.size() && query[at] != ' ') {
            ++at;
        }
        std::string word = lowered(query.substr(start, at - start));
        std::erase_if(word, [](char c) { return c == '"' || c == '*'; });
        if (!word.empty()) {
            found = std::min(found, haystack.find(word));
        }
    }
    std::size_t begin = found == std::string::npos || found < width / 3 ? 0 : found - width / 3;
    while (begin > 0 && isContinuation(text[begin])) {
        --begin;
    }
    std::size_t end = std::min(text.size(), begin + width);
    while (end < text.size() && isContinuation(text[end])) {
        --end;
    }
    std::string snippet(text.substr(begin, end - begin));
    std::replace(snippet.begin(), snippet.end(), '\n', ' ');
    std::replace(snippet.begin(), snippet.end(), '\r', ' ');
    if (begin > 0) {
        snippet.insert(0, "…");
    }
    if (end < text.size()) {
        snippet += "…";
    }
    return snippet;
}

core::Result<std::vector<SearchResult>> search(WorkspaceSession& session, std::string_view text,
                                               std::size_t limit) {
    auto hits = session.search(text, limit);
    if (!hits) {
        return tl::unexpected(hits.error());
    }
    const document::Workspace& ws = session.workspace();
    std::vector<SearchResult> results;
    results.reserve(hits->size());
    for (const SearchHit& hit : *hits) {
        switch (hit.kind) {
        case SearchHit::Kind::PageTitle: {
            const core::PageId page{hit.owner};
            if (ws.findPage(page) == nullptr) {
                break; // not in the workspace (cannot happen with a consistent index)
            }
            results.push_back({.kind = SearchResult::Kind::Page,
                               .page = page,
                               .title = WorkspaceStructure::displayTitle(ws, page),
                               .context = placeOf(ws, page)});
            break;
        }
        case SearchHit::Kind::TextBox: {
            const core::ElementId element{hit.owner};
            const document::Element* record = ws.findElement(element);
            const auto* box =
                record != nullptr ? std::get_if<document::TextBox>(&record->payload) : nullptr;
            const auto page = ws.pageOf(element);
            if (box == nullptr || !page) {
                break;
            }
            results.push_back({.kind = SearchResult::Kind::TextBox,
                               .page = page,
                               .element = element,
                               .title = firstLine(box->text),
                               .context = placeOf(ws, *page) + " › " +
                                          WorkspaceStructure::displayTitle(ws, *page),
                               .snippet = snippetOf(box->text, text)});
            break;
        }
        case SearchHit::Kind::Task: {
            const core::TaskId id{hit.owner};
            const study::Task* task = ws.findTask(id);
            if (task == nullptr) {
                break;
            }
            std::string context = "Task";
            if (task->project) {
                context += " · " + ws.findProject(*task->project)->title;
            } else if (task->course) {
                context += " · " + ws.findCourse(*task->course)->title;
            }
            results.push_back({.kind = SearchResult::Kind::Task,
                               .task = id,
                               .title = task->title,
                               .context = std::move(context),
                               .snippet = snippetOf(task->notes, text)});
            break;
        }
        }
    }
    return results;
}

} // namespace studyapp::application
