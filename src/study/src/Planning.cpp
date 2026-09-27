#include <studyapp/study/Planning.hpp>

#include <algorithm>
#include <tuple>

namespace studyapp::study {

namespace {

using std::chrono::days;
using std::chrono::local_days;
using std::chrono::milliseconds;
using std::chrono::minutes;
using std::chrono::sys_days;

/// Sort key of a time of day; untimed entries sort after timed ones.
minutes timeKey(const std::optional<minutes>& time) noexcept {
    return time ? *time : kMinutesPerDay;
}

/// Minutes from the local midnight of `day` to `instant` (which lies within that day).
minutes minutesInto(core::Timestamp instant, CalendarDate day, const TimeZone& zone) {
    const LocalTime local = toLocal(instant, zone);
    return std::chrono::floor<minutes>(local - LocalTime(local_days(day)));
}

/// Tie-break shared by every list: higher priority first, then the user's order.
bool before(const Task& a, const Task& b) noexcept {
    const auto pa = static_cast<int>(a.priority);
    const auto pb = static_cast<int>(b.priority);
    if (pa != pb) {
        return pa > pb;
    }
    return a.order != b.order ? a.order < b.order : a.id < b.id;
}

} // namespace

LocalTime toLocal(core::Timestamp instant, const TimeZone& zone) {
    return LocalTime(instant.time_since_epoch() + zone.utcOffset(instant));
}

core::Timestamp toInstant(LocalTime local, const TimeZone& zone) {
    const auto asUtc = [&](minutes offset) {
        return core::Timestamp(local.time_since_epoch() - offset);
    };
    // Offset changes are far more than a day apart: the offsets a day before and a day after
    // are the only candidates (equal when no change is near).
    const core::Timestamp guess(local.time_since_epoch());
    const minutes earlier = zone.utcOffset(guess - days{1});
    const minutes later = zone.utcOffset(guess + days{1});
    const bool earlierValid = zone.utcOffset(asUtc(earlier)) == earlier;
    const bool laterValid = zone.utcOffset(asUtc(later)) == later;
    if (earlierValid && laterValid) {
        return asUtc(std::max(earlier, later)); // repeated: the first occurrence
    }
    if (earlierValid || laterValid) {
        return asUtc(earlierValid ? earlier : later);
    }
    return asUtc(earlier); // skipped: with the offset in effect before the change
}

CalendarDate localDate(core::Timestamp instant, const TimeZone& zone) {
    return CalendarDate(std::chrono::floor<days>(toLocal(instant, zone)));
}

core::Timestamp startOfDay(CalendarDate date, const TimeZone& zone) {
    return toInstant(LocalTime(local_days(date)), zone);
}

CalendarDate weekStart(CalendarDate date) noexcept {
    const sys_days day(date);
    const std::chrono::weekday weekday(day);
    const days sinceMonday((weekday.c_encoding() + 6) % 7);
    return CalendarDate(day - sinceMonday);
}

Agenda buildAgenda(std::span<const Task* const> tasks, core::Timestamp now, const TimeZone& zone,
                   AgendaOptions options) {
    const CalendarDate today = localDate(now, zone);
    const core::Timestamp dayStart = startOfDay(today, zone);
    const core::Timestamp dayEnd = startOfDay(CalendarDate(sys_days(today) + days{1}), zone);
    const CalendarDate horizon =
        CalendarDate(sys_days(today) + days{std::max(options.upcomingDays, 0)});

    // Each list sorts by (date, time of day, priority/order).
    struct Item {
        CalendarDate date;
        minutes time;
        std::size_t index;
    };
    std::vector<Item> overdue;
    std::vector<Item> todays;
    std::vector<Item> upcoming;
    Agenda agenda;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const Task& task = *tasks[i];
        if (!isOpen(task.status)) {
            continue;
        }
        const std::optional<TimeBlock>& block = task.scheduled;
        const bool blockToday =
            block && block->start < dayEnd && (block->end > dayStart || block->start >= dayStart);
        const std::optional<minutes> blockTime =
            blockToday
                ? std::optional(block->start <= dayStart ? minutes{0}
                                                         : minutesInto(block->start, today, zone))
                : std::nullopt;
        if (task.dueDate && *task.dueDate < today) {
            overdue.push_back({*task.dueDate, timeKey(task.dueTime), i});
        } else if ((task.dueDate && *task.dueDate == today) || blockToday) {
            const minutes time = std::min(
                timeKey(task.dueDate == today ? task.dueTime : std::nullopt), timeKey(blockTime));
            todays.push_back({today, time, i});
        } else if (task.dueDate) {
            if (*task.dueDate <= horizon) {
                upcoming.push_back({*task.dueDate, timeKey(task.dueTime), i});
            }
        } else if (block) {
            // Only a time block: its local day decides; a block already over counts as
            // overdue (the plan passed and the task is still open).
            if (block->end <= dayStart) {
                overdue.push_back({localDate(block->start, zone),
                                   minutesInto(block->start, localDate(block->start, zone), zone),
                                   i});
            } else {
                const CalendarDate date = localDate(block->start, zone);
                if (date <= horizon) {
                    upcoming.push_back({date, minutesInto(block->start, date, zone), i});
                }
            }
        } else {
            agenda.unscheduled.push_back(i);
        }
    }
    const auto byDateAndTime = [&](const Item& a, const Item& b) {
        if (a.date != b.date) {
            return a.date < b.date;
        }
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return before(*tasks[a.index], *tasks[b.index]);
    };
    const auto indices = [&](std::vector<Item>& items, std::vector<std::size_t>& out) {
        std::sort(items.begin(), items.end(), byDateAndTime);
        out.reserve(items.size());
        for (const Item& item : items) {
            out.push_back(item.index);
        }
    };
    indices(overdue, agenda.overdue);
    indices(todays, agenda.today);
    indices(upcoming, agenda.upcoming);
    std::sort(agenda.unscheduled.begin(), agenda.unscheduled.end(),
              [&](std::size_t a, std::size_t b) { return before(*tasks[a], *tasks[b]); });
    return agenda;
}

std::array<WeekDay, 7> buildWeek(std::span<const Task* const> tasks, CalendarDate firstDay,
                                 const TimeZone& zone) {
    std::array<WeekDay, 7> week;
    std::array<core::Timestamp, 8> bounds; // local midnights, as instants
    for (std::size_t d = 0; d < 8; ++d) {
        const CalendarDate date(sys_days(firstDay) + days{static_cast<int>(d)});
        bounds[d] = startOfDay(date, zone);
        if (d < 7) {
            week[d].date = date;
        }
    }
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const Task& task = *tasks[i];
        if (task.status == TaskStatus::Cancelled) {
            continue;
        }
        if (task.dueDate) {
            const auto offset = (sys_days(*task.dueDate) - sys_days(firstDay)).count();
            if (offset >= 0 && offset < 7) {
                week[static_cast<std::size_t>(offset)].entries.push_back({.task = i});
            }
        }
        if (!task.scheduled) {
            continue;
        }
        const TimeBlock& block = *task.scheduled;
        for (std::size_t d = 0; d < 7; ++d) {
            const bool overlaps =
                block.start < bounds[d + 1] && (block.end > bounds[d] || block.start >= bounds[d]);
            if (!overlaps) {
                continue;
            }
            const minutes start = block.start <= bounds[d]
                                      ? minutes{0}
                                      : minutesInto(block.start, week[d].date, zone);
            const minutes end = block.end >= bounds[d + 1]
                                    ? kMinutesPerDay
                                    : minutesInto(block.end, week[d].date, zone);
            week[d].entries.push_back({.task = i, .blockStart = start, .blockEnd = end});
        }
    }
    for (WeekDay& day : week) {
        std::sort(day.entries.begin(), day.entries.end(),
                  [&](const DayEntry& a, const DayEntry& b) {
                      const bool aBlock = a.blockStart.has_value();
                      const bool bBlock = b.blockStart.has_value();
                      if (aBlock != bBlock) {
                          return aBlock; // time blocks first
                      }
                      const minutes ta = aBlock ? *a.blockStart : timeKey(tasks[a.task]->dueTime);
                      const minutes tb = bBlock ? *b.blockStart : timeKey(tasks[b.task]->dueTime);
                      if (ta != tb) {
                          return ta < tb;
                      }
                      return before(*tasks[a.task], *tasks[b.task]);
                  });
    }
    return week;
}

Progress progressOf(std::span<const Task* const> tasks) noexcept {
    Progress progress;
    for (const Task* task : tasks) {
        if (task->status == TaskStatus::Cancelled) {
            continue;
        }
        ++progress.total;
        if (task->status == TaskStatus::Done) {
            ++progress.done;
        }
    }
    return progress;
}

} // namespace studyapp::study
