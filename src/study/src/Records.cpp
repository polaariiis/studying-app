#include <studyapp/study/Records.hpp>

#include <algorithm>
#include <utility>

namespace studyapp::study {

namespace {

using core::ErrorCode;
using core::makeError;
using core::Result;

Result<void> invalid(std::string message) {
    return makeError(ErrorCode::InvalidArgument, std::move(message));
}

bool isBlank(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    });
}

Result<void> checkTitle(std::string_view title, std::string_view what) {
    return isBlank(title) ? invalid(std::string(what) + " must not be blank") : Result<void>{};
}

Result<void> checkDate(const std::optional<CalendarDate>& date, std::string_view what) {
    return date && !isStorableDate(*date) ? invalid(std::string(what) + " is not a valid date")
                                          : Result<void>{};
}

template <class Id>
bool sortedUnique(const std::vector<Id>& ids) {
    return std::adjacent_find(ids.begin(), ids.end(),
                              [](const Id& a, const Id& b) { return !(a < b); }) == ids.end() &&
           std::none_of(ids.begin(), ids.end(), [](const Id& id) { return id.isNull(); });
}

} // namespace

std::string foldTagName(std::string_view name) {
    std::string folded(name);
    for (char& c : folded) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return folded;
}

bool isStorableDate(const CalendarDate& date) noexcept {
    return date.ok() && date.year() >= std::chrono::year{1} &&
           date.year() <= std::chrono::year{9999};
}

Result<void> checkValues(const Course& course) {
    if (auto title = checkTitle(course.title, "course title"); !title) {
        return title;
    }
    if (auto start = checkDate(course.start, "course start"); !start) {
        return start;
    }
    if (auto end = checkDate(course.end, "course end"); !end) {
        return end;
    }
    if (course.start && course.end && *course.end < *course.start) {
        return invalid("course end is before its start");
    }
    return {};
}

Result<void> checkValues(const Project& project) {
    if (auto title = checkTitle(project.title, "project title"); !title) {
        return title;
    }
    if (static_cast<int>(project.status) > static_cast<int>(ProjectStatus::Archived)) {
        return invalid("unknown project status");
    }
    if (auto start = checkDate(project.start, "project start"); !start) {
        return start;
    }
    return checkDate(project.due, "project due date");
}

Result<void> checkValues(const Task& task) {
    if (auto title = checkTitle(task.title, "task title"); !title) {
        return title;
    }
    if (static_cast<int>(task.status) > static_cast<int>(TaskStatus::Cancelled)) {
        return invalid("unknown task status");
    }
    if (static_cast<int>(task.priority) > static_cast<int>(Priority::High)) {
        return invalid("unknown task priority");
    }
    if (auto due = checkDate(task.dueDate, "task due date"); !due) {
        return due;
    }
    if (task.dueTime && !task.dueDate) {
        return invalid("a due time needs a due date");
    }
    if (task.dueTime && (*task.dueTime < MinuteOfDay{0} || *task.dueTime >= kMinutesPerDay)) {
        return invalid("due time must be within the day");
    }
    if (task.scheduled && task.scheduled->end < task.scheduled->start) {
        return invalid("a time block cannot end before it starts");
    }
    if (task.estimate && *task.estimate < std::chrono::minutes{0}) {
        return invalid("estimate must not be negative");
    }
    if (task.parent && *task.parent == task.id) {
        return invalid("a task cannot be its own subtask");
    }
    if (!sortedUnique(task.linkedPages) || !sortedUnique(task.tags)) {
        return invalid("linked pages and tags must be sorted, unique and non-null");
    }
    return {};
}

Result<void> checkValues(const Tag& tag) {
    return checkTitle(tag.name, "tag name");
}

} // namespace studyapp::study
