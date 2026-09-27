#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Transaction.hpp>
#include <studyapp/study/Records.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace studyapp::persistence {

/// The study records of a workspace (Phase 7), unordered.
struct StudyData {
    std::vector<study::Course> courses;
    std::vector<study::Project> projects;
    std::vector<study::Task> tasks; ///< with linked pages and tags
    std::vector<study::Tag> tags;
    /// Tags of each page (`page_tag`), sorted per page.
    std::unordered_map<core::PageId, std::vector<core::TagId>> pageTags;
};

/// Reads and writes `course`, `project`, `task` (+ `task_tag`, `task_page`), `tag` and
/// `page_tag` (docs/DATABASE_SCHEMA.md §5, §11). Writes are driven by the same record
/// changes the Workspace applied, like the other stores. Columns the model does not have
/// (`task.recurrence`) keep their defaults; a row that uses them fails the load with
/// Unsupported. Dates are stored as `YYYY-MM-DD`, times of day as minutes, instants as UTC
/// milliseconds.
class StudyStore {
public:
    explicit StudyStore(Database& database) noexcept : database_(&database) {}

    [[nodiscard]] core::Result<StudyData> load();

    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::CourseChange& change);
    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::ProjectChange& change);
    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::TaskChange& change);
    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::TagChange& change);
    /// The `page_tag` rows of a created or updated page (the page row is written by the
    /// CatalogStore first; a removed page's rows go with it by `ON DELETE CASCADE`).
    [[nodiscard]] core::Result<void> applyPageTags(Transaction& transaction,
                                                   const document::PageChange& change);

private:
    Database* database_;
};

/// `YYYY-MM-DD` (the schema's floating dates).
[[nodiscard]] std::string formatDate(const study::CalendarDate& date);
[[nodiscard]] std::optional<study::CalendarDate> parseDate(std::string_view text);

} // namespace studyapp::persistence
