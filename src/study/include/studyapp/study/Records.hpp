#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/FractionalIndex.hpp>
#include <studyapp/core/Ids.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::study {

// Study & planning records (docs/DATA_MODEL.md §5). Plain values that reference notes only
// by id (PageId), never by pointer. The document Workspace owns them next to the notes
// hierarchy, so they are edited by the same patches, undo history and persistence
// (docs/ARCHITECTURE.md D42).

/// A calendar date without a time zone ("that day wherever I am"): due dates are floating.
using CalendarDate = std::chrono::year_month_day;

/// Minutes after local midnight, [0, 1440).
using MinuteOfDay = std::chrono::minutes;

inline constexpr MinuteOfDay kMinutesPerDay{1440};

/// A scheduled time block: two instants (UTC), end >= start.
struct TimeBlock {
    core::Timestamp start{};
    core::Timestamp end{};

    [[nodiscard]] friend bool operator==(const TimeBlock&, const TimeBlock&) = default;
};

struct Course {
    core::CourseId id;
    std::string title; ///< must not be blank
    std::string code;  ///< e.g. "BIO 101"; may be empty
    std::string term;
    std::string instructor;
    std::optional<core::Color> color;
    std::optional<CalendarDate> start;
    std::optional<CalendarDate> end; ///< not before `start`
    std::optional<core::Timestamp> archived;
    core::FractionalIndex order = core::FractionalIndex::first();
    core::Timestamp created{};
    core::Timestamp modified{};

    [[nodiscard]] friend bool operator==(const Course&, const Course&) = default;
};

/// Stored values (docs/DATABASE_SCHEMA.md `project.status`).
enum class ProjectStatus : std::uint8_t {
    Planned = 0,
    Active = 1,
    OnHold = 2,
    Done = 3,
    Archived = 4
};

struct Project {
    core::ProjectId id;
    std::string title; ///< must not be blank
    std::string description;
    ProjectStatus status = ProjectStatus::Active;
    std::optional<core::CourseId> course;
    std::optional<CalendarDate> start;
    std::optional<CalendarDate> due;
    std::optional<core::Color> color;
    core::FractionalIndex order = core::FractionalIndex::first();
    core::Timestamp created{};
    core::Timestamp modified{};
    std::optional<core::Timestamp> completed;

    [[nodiscard]] friend bool operator==(const Project&, const Project&) = default;
};

/// Stored values (docs/DATABASE_SCHEMA.md `task.status`).
enum class TaskStatus : std::uint8_t {
    Todo = 0,
    InProgress = 1,
    Done = 2,
    Cancelled = 3
};
/// Stored values (`task.priority`).
enum class Priority : std::uint8_t {
    None = 0,
    Low = 1,
    Medium = 2,
    High = 3
};

[[nodiscard]] constexpr bool isOpen(TaskStatus status) noexcept {
    return status == TaskStatus::Todo || status == TaskStatus::InProgress;
}

/// A task. Top-level tasks and the subtasks of one task are each an ordered sibling list;
/// subtasks are one level deep (a subtask has no subtasks of its own).
struct Task {
    core::TaskId id;
    std::string title; ///< must not be blank
    std::string notes;
    TaskStatus status = TaskStatus::Todo;
    Priority priority = Priority::None;
    std::optional<core::CourseId> course;
    std::optional<core::ProjectId> project;
    std::optional<core::TaskId> parent;           ///< a subtask's task
    std::optional<CalendarDate> dueDate;          ///< floating
    std::optional<MinuteOfDay> dueTime;           ///< floating local time; only with a due date
    std::optional<TimeBlock> scheduled;           ///< time blocking (instants)
    std::optional<std::chrono::minutes> estimate; ///< >= 0
    std::vector<core::PageId> linkedPages;        ///< sorted, unique
    std::vector<core::TagId> tags;                ///< sorted, unique
    core::FractionalIndex order = core::FractionalIndex::first();
    core::Timestamp created{};
    core::Timestamp modified{};
    std::optional<core::Timestamp> completed; ///< when it was marked done

    [[nodiscard]] friend bool operator==(const Task&, const Task&) = default;
};

/// A workspace-wide label for pages and tasks. Names are unique ignoring ASCII case (as
/// the database's `COLLATE NOCASE`).
struct Tag {
    core::TagId id;
    std::string name; ///< must not be blank
    std::optional<core::Color> color;
    core::Timestamp created{};

    [[nodiscard]] friend bool operator==(const Tag&, const Tag&) = default;
};

/// `name` with ASCII letters lower-cased: the identity of a tag name.
[[nodiscard]] std::string foldTagName(std::string_view name);

/// Whether `date` is a real calendar date within years 1..9999 (storable as YYYY-MM-DD).
[[nodiscard]] bool isStorableDate(const CalendarDate& date) noexcept;

/// Value checks of each record (references to other records are the workspace's).
[[nodiscard]] core::Result<void> checkValues(const Course& course);
[[nodiscard]] core::Result<void> checkValues(const Project& project);
[[nodiscard]] core::Result<void> checkValues(const Task& task);
[[nodiscard]] core::Result<void> checkValues(const Tag& tag);

} // namespace studyapp::study
