#include <studyapp/persistence/StudyStore.hpp>

#include "RowDecoder.hpp"
#include "StoreSupport.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <utility>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::forward;
using detail::millis;
using detail::RowDecoder;

std::string formatDate(const study::CalendarDate& date) {
    char text[32]; // room for any int year, so no truncation warning
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    return text;
}

std::optional<study::CalendarDate> parseDate(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    const auto number = [&](std::size_t at, std::size_t length) -> std::optional<int> {
        int value = 0;
        const char* first = text.data() + at;
        const char* last = first + length;
        if (!std::all_of(first, last, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        std::from_chars(first, last, value);
        return value;
    };
    const auto y = number(0, 4);
    const auto m = number(5, 2);
    const auto d = number(8, 2);
    if (!y || !m || !d) {
        return std::nullopt;
    }
    const study::CalendarDate date{std::chrono::year{*y},
                                   std::chrono::month{static_cast<unsigned>(*m)},
                                   std::chrono::day{static_cast<unsigned>(*d)}};
    return study::isStorableDate(date) ? std::optional(date) : std::nullopt;
}

namespace {

/// Study-specific column decoding on top of RowDecoder.
class StudyDecoder {
public:
    StudyDecoder(const Statement& statement, std::string_view what) noexcept
        : statement_(&statement), decode_(statement, what) {}

    RowDecoder& operator*() noexcept { return decode_; }
    RowDecoder* operator->() noexcept { return &decode_; }

    std::optional<study::CalendarDate> date(int column) {
        if (statement_->columnIsNull(column)) {
            return std::nullopt;
        }
        const std::string text = decode_.text(column);
        if (!decode_.ok()) {
            return std::nullopt;
        }
        auto parsed = parseDate(text);
        if (!parsed) {
            decode_.failRow("date '" + text + "' is not YYYY-MM-DD");
        }
        return parsed;
    }

    std::optional<core::Timestamp> optionalTimestamp(int column) {
        return statement_->columnIsNull(column) ? std::nullopt
                                                : std::optional(decode_.timestamp(column));
    }

    std::optional<std::chrono::minutes> optionalMinutes(int column) {
        return statement_->columnIsNull(column)
                   ? std::nullopt
                   : std::optional(std::chrono::minutes{decode_.integer(column)});
    }

private:
    const Statement* statement_;
    RowDecoder decode_;
};

/// Runs `sql` and calls `row(statement)` for each result row.
template <class Row>
Result<void> forEachRow(Database& database, std::string_view sql, Row row) {
    auto statement = database.cached(sql);
    if (!statement) {
        return forward(statement);
    }
    while (true) {
        auto next = (*statement)->step();
        if (!next) {
            return forward(next);
        }
        if (!*next) {
            return {};
        }
        if (auto done = row(**statement); !done) {
            return done;
        }
    }
}

Result<void> deleteById(Database& database, std::string_view table, const core::Uuid& id) {
    auto statement = database.cached("DELETE FROM " + std::string(table) + " WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)->bindUuid(1, id);
    return detail::runOnOneRow(database, **statement, table, id.toString());
}

Statement& bindDate(Statement& s, int index, const std::optional<study::CalendarDate>& date) {
    return date ? s.bindText(index, formatDate(*date)) : s.bindNull(index);
}
Statement& bindColor(Statement& s, int index, const std::optional<core::Color>& color) {
    return color ? s.bindInt(index, static_cast<std::int64_t>(color->toArgb32()))
                 : s.bindNull(index);
}
Statement& bindTime(Statement& s, int index, const std::optional<core::Timestamp>& time) {
    return time ? s.bindInt(index, millis(*time)) : s.bindNull(index);
}
Statement& bindMinutes(Statement& s, int index, const std::optional<std::chrono::minutes>& value) {
    return value ? s.bindInt(index, value->count()) : s.bindNull(index);
}

/// Replaces the join rows `table (owner, other)` of `owner` from `before` to `after`
/// (both sorted): only the difference is written.
template <class OwnerId, class OtherId>
Result<void> syncLinks(Database& database, std::string_view table, std::string_view ownerColumn,
                       std::string_view otherColumn, OwnerId owner,
                       const std::vector<OtherId>& before, const std::vector<OtherId>& after) {
    std::vector<OtherId> removedIds;
    std::vector<OtherId> addedIds;
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::back_inserter(removedIds));
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(addedIds));
    const std::string where =
        std::string(ownerColumn) + " = ?1 AND " + std::string(otherColumn) + " = ?2";
    for (const OtherId& id : removedIds) {
        auto statement = database.cached("DELETE FROM " + std::string(table) + " WHERE " + where);
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, owner).bindId(2, id);
        if (auto done = detail::runOnOneRow(database, **statement, table, owner.toString());
            !done) {
            return done;
        }
    }
    for (const OtherId& id : addedIds) {
        auto statement =
            database.cached("INSERT INTO " + std::string(table) + " (" + std::string(ownerColumn) +
                            ", " + std::string(otherColumn) + ") VALUES (?1, ?2)");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, owner).bindId(2, id);
        if (auto done = (*statement)->run(); !done) {
            return done;
        }
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------- load

Result<StudyData> StudyStore::load() {
    StudyData data;
    Database& db = *database_;
    auto courses = forEachRow(
        db,
        "SELECT id, title, code, term, instructor, color, start_date, end_date, sort_key, "
        "archived_at, created_at, updated_at FROM course",
        [&](const Statement& s) -> Result<void> {
            StudyDecoder decode(s, "course");
            study::Course course{.id = decode->id<core::CourseId>(0),
                                 .title = decode->text(1),
                                 .code = decode->text(2),
                                 .term = decode->text(3),
                                 .instructor = decode->text(4),
                                 .color = decode->optionalColor(5),
                                 .start = decode.date(6),
                                 .end = decode.date(7),
                                 .archived = decode.optionalTimestamp(9),
                                 .order = decode->key(8),
                                 .created = decode->timestamp(10),
                                 .modified = decode->timestamp(11)};
            if (auto status = decode->status(); !status) {
                return status;
            }
            data.courses.push_back(std::move(course));
            return {};
        });
    if (!courses) {
        return forward(courses);
    }
    auto projects = forEachRow(
        db,
        "SELECT id, title, description, status, course_id, start_date, due_date, color, "
        "sort_key, created_at, updated_at, completed_at FROM project",
        [&](const Statement& s) -> Result<void> {
            StudyDecoder decode(s, "project");
            study::Project project{.id = decode->id<core::ProjectId>(0),
                                   .title = decode->text(1),
                                   .description = decode->text(2),
                                   .status = decode->enumeration<study::ProjectStatus>(3, 0, 4),
                                   .course = decode->optionalId<core::CourseId>(4),
                                   .start = decode.date(5),
                                   .due = decode.date(6),
                                   .color = decode->optionalColor(7),
                                   .order = decode->key(8),
                                   .created = decode->timestamp(9),
                                   .modified = decode->timestamp(10),
                                   .completed = decode.optionalTimestamp(11)};
            if (auto status = decode->status(); !status) {
                return status;
            }
            data.projects.push_back(std::move(project));
            return {};
        });
    if (!projects) {
        return forward(projects);
    }
    std::unordered_map<core::TaskId, std::size_t> taskIndex;
    auto tasks = forEachRow(
        db,
        "SELECT id, title, notes, status, priority, course_id, project_id, parent_id, due_date, "
        "due_time_minutes, scheduled_start, scheduled_end, estimate_minutes, recurrence, "
        "sort_key, created_at, updated_at, completed_at FROM task",
        [&](const Statement& s) -> Result<void> {
            if (!s.columnIsNull(13)) {
                return makeError(ErrorCode::Unsupported,
                                 "the workspace contains recurring tasks, which this version of "
                                 "StudyBoard does not support");
            }
            StudyDecoder decode(s, "task");
            study::Task task{.id = decode->id<core::TaskId>(0),
                             .title = decode->text(1),
                             .notes = decode->text(2),
                             .status = decode->enumeration<study::TaskStatus>(3, 0, 3),
                             .priority = decode->enumeration<study::Priority>(4, 0, 3),
                             .course = decode->optionalId<core::CourseId>(5),
                             .project = decode->optionalId<core::ProjectId>(6),
                             .parent = decode->optionalId<core::TaskId>(7),
                             .dueDate = decode.date(8),
                             .dueTime = decode.optionalMinutes(9),
                             .estimate = decode.optionalMinutes(12),
                             .order = decode->key(14),
                             .created = decode->timestamp(15),
                             .modified = decode->timestamp(16),
                             .completed = decode.optionalTimestamp(17)};
            const auto start = decode.optionalTimestamp(10);
            const auto end = decode.optionalTimestamp(11);
            if (start.has_value() != end.has_value()) {
                decode->failRow("a time block needs both a start and an end");
            } else if (start) {
                task.scheduled = study::TimeBlock{*start, *end};
            }
            if (auto status = decode->status(); !status) {
                return status;
            }
            taskIndex.emplace(task.id, data.tasks.size());
            data.tasks.push_back(std::move(task));
            return {};
        });
    if (!tasks) {
        return forward(tasks);
    }
    auto tags = forEachRow(db, "SELECT id, name, color, created_at FROM tag",
                           [&](const Statement& s) -> Result<void> {
                               StudyDecoder decode(s, "tag");
                               study::Tag tag{.id = decode->id<core::TagId>(0),
                                              .name = decode->text(1),
                                              .color = decode->optionalColor(2),
                                              .created = decode->timestamp(3)};
                               if (auto status = decode->status(); !status) {
                                   return status;
                               }
                               data.tags.push_back(std::move(tag));
                               return {};
                           });
    if (!tags) {
        return forward(tags);
    }
    // Join rows. A row whose task is unknown cannot exist (foreign keys); checked anyway.
    const auto taskOf = [&](RowDecoder& decode) -> study::Task* {
        const auto id = decode.id<core::TaskId>(0);
        const auto it = taskIndex.find(id);
        if (decode.ok() && it == taskIndex.end()) {
            decode.failRow("task " + id.toString() + " does not exist");
        }
        return decode.ok() ? &data.tasks[it->second] : nullptr;
    };
    auto taskTags = forEachRow(db, "SELECT task_id, tag_id FROM task_tag",
                               [&](const Statement& s) -> Result<void> {
                                   RowDecoder decode(s, "task_tag");
                                   study::Task* task = taskOf(decode);
                                   const auto tag = decode.id<core::TagId>(1);
                                   if (task != nullptr && decode.ok()) {
                                       task->tags.push_back(tag);
                                   }
                                   return decode.status();
                               });
    if (!taskTags) {
        return forward(taskTags);
    }
    auto taskPages = forEachRow(db, "SELECT task_id, page_id FROM task_page",
                                [&](const Statement& s) -> Result<void> {
                                    RowDecoder decode(s, "task_page");
                                    study::Task* task = taskOf(decode);
                                    const auto page = decode.id<core::PageId>(1);
                                    if (task != nullptr && decode.ok()) {
                                        task->linkedPages.push_back(page);
                                    }
                                    return decode.status();
                                });
    if (!taskPages) {
        return forward(taskPages);
    }
    auto pageTags = forEachRow(db, "SELECT page_id, tag_id FROM page_tag",
                               [&](const Statement& s) -> Result<void> {
                                   RowDecoder decode(s, "page_tag");
                                   const auto page = decode.id<core::PageId>(0);
                                   const auto tag = decode.id<core::TagId>(1);
                                   if (decode.ok()) {
                                       data.pageTags[page].push_back(tag);
                                   }
                                   return decode.status();
                               });
    if (!pageTags) {
        return forward(pageTags);
    }
    for (study::Task& task : data.tasks) {
        std::sort(task.tags.begin(), task.tags.end());
        std::sort(task.linkedPages.begin(), task.linkedPages.end());
    }
    for (auto& [page, ids] : data.pageTags) {
        std::sort(ids.begin(), ids.end());
    }
    return data;
}

// ---------------------------------------------------------------------------- apply

Result<void> StudyStore::apply(Transaction& transaction, const document::CourseChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        return deleteById(*database_, "course", change.before->id.value());
    }
    const study::Course& c = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO course (id, title, code, term, instructor, color, start_date, end_date, "
              "sort_key, archived_at, created_at, updated_at) "
              "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
            : "UPDATE course SET title = ?2, code = ?3, term = ?4, instructor = ?5, color = ?6, "
              "start_date = ?7, end_date = ?8, sort_key = ?9, archived_at = ?10, created_at = ?11, "
              "updated_at = ?12 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    Statement& s = **statement;
    s.bindId(1, c.id)
        .bindText(2, c.title)
        .bindText(3, c.code)
        .bindText(4, c.term)
        .bindText(5, c.instructor);
    bindColor(s, 6, c.color);
    bindDate(s, 7, c.start);
    bindDate(s, 8, c.end);
    s.bindText(9, c.order.value());
    bindTime(s, 10, c.archived);
    s.bindInt(11, millis(c.created)).bindInt(12, millis(c.modified));
    return detail::runOnOneRow(*database_, s, "course", c.id.toString());
}

Result<void> StudyStore::apply(Transaction& transaction, const document::ProjectChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        return deleteById(*database_, "project", change.before->id.value());
    }
    const study::Project& p = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO project (id, title, description, status, course_id, start_date, "
              "due_date, color, sort_key, created_at, updated_at, completed_at) "
              "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
            : "UPDATE project SET title = ?2, description = ?3, status = ?4, course_id = ?5, "
              "start_date = ?6, due_date = ?7, color = ?8, sort_key = ?9, created_at = ?10, "
              "updated_at = ?11, completed_at = ?12 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    Statement& s = **statement;
    s.bindId(1, p.id)
        .bindText(2, p.title)
        .bindText(3, p.description)
        .bindInt(4, static_cast<std::int64_t>(p.status))
        .bindId(5, p.course);
    bindDate(s, 6, p.start);
    bindDate(s, 7, p.due);
    bindColor(s, 8, p.color);
    s.bindText(9, p.order.value()).bindInt(10, millis(p.created)).bindInt(11, millis(p.modified));
    bindTime(s, 12, p.completed);
    return detail::runOnOneRow(*database_, s, "project", p.id.toString());
}

Result<void> StudyStore::apply(Transaction& transaction, const document::TaskChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        // Its task_tag / task_page rows go with it (ON DELETE CASCADE).
        return deleteById(*database_, "task", change.before->id.value());
    }
    const study::Task& t = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO task (id, title, notes, status, priority, course_id, project_id, "
              "parent_id, due_date, due_time_minutes, scheduled_start, scheduled_end, "
              "estimate_minutes, sort_key, created_at, updated_at, completed_at) "
              "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)"
            : "UPDATE task SET title = ?2, notes = ?3, status = ?4, priority = ?5, course_id = ?6, "
              "project_id = ?7, parent_id = ?8, due_date = ?9, due_time_minutes = ?10, "
              "scheduled_start = ?11, scheduled_end = ?12, estimate_minutes = ?13, sort_key = ?14, "
              "created_at = ?15, updated_at = ?16, completed_at = ?17 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    Statement& s = **statement;
    s.bindId(1, t.id)
        .bindText(2, t.title)
        .bindText(3, t.notes)
        .bindInt(4, static_cast<std::int64_t>(t.status))
        .bindInt(5, static_cast<std::int64_t>(t.priority))
        .bindId(6, t.course)
        .bindId(7, t.project)
        .bindId(8, t.parent);
    bindDate(s, 9, t.dueDate);
    bindMinutes(s, 10, t.dueTime);
    bindTime(s, 11, t.scheduled ? std::optional(t.scheduled->start) : std::nullopt);
    bindTime(s, 12, t.scheduled ? std::optional(t.scheduled->end) : std::nullopt);
    bindMinutes(s, 13, t.estimate);
    s.bindText(14, t.order.value()).bindInt(15, millis(t.created)).bindInt(16, millis(t.modified));
    bindTime(s, 17, t.completed);
    if (auto written = detail::runOnOneRow(*database_, s, "task", t.id.toString()); !written) {
        return written;
    }
    static const std::vector<core::TagId> kNoTags;
    static const std::vector<core::PageId> kNoPages;
    const bool update = change.isUpdate();
    if (auto linked = syncLinks(*database_, "task_tag", "task_id", "tag_id", t.id,
                                update ? change.before->tags : kNoTags, t.tags);
        !linked) {
        return linked;
    }
    return syncLinks(*database_, "task_page", "task_id", "page_id", t.id,
                     update ? change.before->linkedPages : kNoPages, t.linkedPages);
}

Result<void> StudyStore::apply(Transaction& transaction, const document::TagChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        return deleteById(*database_, "tag", change.before->id.value());
    }
    const study::Tag& t = *change.after;
    auto statement = database_->cached(
        change.isCreate() ? "INSERT INTO tag (id, name, color, created_at) VALUES (?1, ?2, ?3, ?4)"
                          : "UPDATE tag SET name = ?2, color = ?3, created_at = ?4 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    Statement& s = **statement;
    s.bindId(1, t.id).bindText(2, t.name);
    bindColor(s, 3, t.color);
    s.bindInt(4, millis(t.created));
    return detail::runOnOneRow(*database_, s, "tag", t.id.toString());
}

Result<void> StudyStore::applyPageTags(Transaction& transaction,
                                       const document::PageChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        return {};
    }
    static const std::vector<core::TagId> kNone;
    return syncLinks(*database_, "page_tag", "page_id", "tag_id", change.after->id,
                     change.isUpdate() ? change.before->tags : kNone, change.after->tags);
}

} // namespace studyapp::persistence
