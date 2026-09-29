#pragma once

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::application {

/// A search result ready to show and to jump to (Phase 8).
struct SearchResult {
    enum class Kind : std::uint8_t {
        Page,
        TextBox,
        Task
    };
    Kind kind = Kind::Page;
    std::optional<core::PageId> page;       ///< the page to open (page, text box; not tasks)
    std::optional<core::ElementId> element; ///< the text box
    std::optional<core::TaskId> task;
    std::string title;   ///< page title, task title, or the text box's first line
    std::string context; ///< where: "Notebook › Section › Page", or the task's course
    std::string snippet; ///< text around the first matching word ("" if none)
};

/// Up to `limit` results for `text`, best first. Hits are resolved against the session's
/// Workspace (the source of truth): a hit whose record no longer exists is dropped rather
/// than shown. O(hits + text of the hits).
[[nodiscard]] core::Result<std::vector<SearchResult>>
search(WorkspaceSession& session, std::string_view text, std::size_t limit = 50);

/// ~`width` bytes of `text` around the first word of `query` it contains (ASCII case is
/// ignored), cut at UTF-8 character boundaries, with "…" where it was cut; the start of the
/// text if no word is found.
[[nodiscard]] std::string snippetOf(std::string_view text, std::string_view query,
                                    std::size_t width = 90);

} // namespace studyapp::application
