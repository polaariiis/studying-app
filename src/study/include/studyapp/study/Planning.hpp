#pragma once

#include <studyapp/study/Records.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace studyapp::study {

// Pure planning logic (docs/DATA_MODEL.md §5): values in, values out; no clock, no I/O.
// Tasks are passed by pointer (they live in the Workspace; nothing is copied).
//
// Date semantics: due dates are floating calendar dates compared with the local date of
// "now"; time blocks are instants, placed on local days through a TimeZone.

/// Local time for instants. Implemented by `platform` with the system time zone; tests
/// use fixed offsets and zones with daylight-saving changes.
class TimeZone {
public:
    TimeZone() = default;
    virtual ~TimeZone() = default;
    TimeZone(const TimeZone&) = delete;
    TimeZone& operator=(const TimeZone&) = delete;
    TimeZone(TimeZone&&) = delete;
    TimeZone& operator=(TimeZone&&) = delete;

    /// Local time minus UTC at `instant` (e.g. +60 min for CET in winter).
    [[nodiscard]] virtual std::chrono::minutes utcOffset(core::Timestamp instant) const = 0;
};

/// A zone with a constant offset (UTC itself, tests).
class FixedOffsetZone final : public TimeZone {
public:
    explicit FixedOffsetZone(std::chrono::minutes offset) noexcept : offset_(offset) {}
    [[nodiscard]] std::chrono::minutes utcOffset(core::Timestamp /*instant*/) const override {
        return offset_;
    }

private:
    std::chrono::minutes offset_;
};

using LocalTime = std::chrono::local_time<std::chrono::milliseconds>;

[[nodiscard]] LocalTime toLocal(core::Timestamp instant, const TimeZone& zone);
/// The instant shown as `local`. A local time skipped by a daylight-saving change maps to
/// the instant with the offset in effect before it (the wall clock then shows it one
/// offset change later); a repeated local time maps to its first occurrence.
[[nodiscard]] core::Timestamp toInstant(LocalTime local, const TimeZone& zone);
[[nodiscard]] CalendarDate localDate(core::Timestamp instant, const TimeZone& zone);
/// Start (local midnight) of `date` as an instant.
[[nodiscard]] core::Timestamp startOfDay(CalendarDate date, const TimeZone& zone);
/// The Monday of the week that contains `date` (ISO 8601 weeks).
[[nodiscard]] CalendarDate weekStart(CalendarDate date) noexcept;

/// Index (into the given task span) lists of the agenda. Only open tasks (to do, in
/// progress) appear; each list is in display order.
struct Agenda {
    std::vector<std::size_t> overdue;     ///< due before today; oldest first
    std::vector<std::size_t> today;       ///< due today, or with a time block today
    std::vector<std::size_t> upcoming;    ///< due within the next `upcomingDays` days
    std::vector<std::size_t> unscheduled; ///< no due date and no time block

    [[nodiscard]] friend bool operator==(const Agenda&, const Agenda&) = default;
};

struct AgendaOptions {
    int upcomingDays = 7; ///< after today
};

/// The agenda at `now` in `zone`. O(n log n) in the number of tasks.
[[nodiscard]] Agenda buildAgenda(std::span<const Task* const> tasks, core::Timestamp now,
                                 const TimeZone& zone, AgendaOptions options = {});

/// One task on one day of the week view.
struct DayEntry {
    std::size_t task; ///< index into the given span
    /// A time block's part within this day in local minutes [start, end]; absent for a
    /// task that is only due that day (its due time, if any, is `Task::dueTime`).
    std::optional<MinuteOfDay> blockStart;
    std::optional<MinuteOfDay> blockEnd;

    [[nodiscard]] friend bool operator==(const DayEntry&, const DayEntry&) = default;
};

struct WeekDay {
    CalendarDate date{};
    std::vector<DayEntry> entries; ///< time blocks by start, then due tasks by due time

    [[nodiscard]] friend bool operator==(const WeekDay&, const WeekDay&) = default;
};

/// Seven days from `firstDay`: tasks due on each day and time blocks cut at local
/// midnights (a block over midnight appears on both days). Cancelled tasks are left out;
/// done tasks stay (the view shows them as done). O(n log n + blocks).
[[nodiscard]] std::array<WeekDay, 7> buildWeek(std::span<const Task* const> tasks,
                                               CalendarDate firstDay, const TimeZone& zone);

struct Progress {
    std::size_t done = 0;
    std::size_t total = 0; ///< not counting cancelled tasks

    [[nodiscard]] friend bool operator==(const Progress&, const Progress&) = default;
};

/// Done / total over `tasks` (e.g. a project's), cancelled tasks excluded.
[[nodiscard]] Progress progressOf(std::span<const Task* const> tasks) noexcept;

} // namespace studyapp::study
