// Study domain: record value checks, local time in fixed and daylight-saving zones, and the
// agenda / week / progress logic across time zones (ROADMAP.md Phase 7 exit criterion);
// estimate totals (2.0-PLAN-02).

#include <studyapp/study/Planning.hpp>

#include <studyapp/study/Records.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace studyapp::study {
namespace {

using namespace std::chrono_literals;
using std::chrono::days;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::sys_days;

constexpr CalendarDate date(int y, unsigned m, unsigned d) {
    return CalendarDate{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
}

/// A distinct, non-nil test UUID.
core::Uuid uuid(std::uint64_t n) {
    core::Uuid::Bytes bytes{};
    bytes[0] = 1;
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[15 - i] = static_cast<std::uint8_t>(n >> (8 * i));
    }
    return core::Uuid{bytes};
}

core::Timestamp utc(CalendarDate day, int hour, int minute = 0) {
    return core::Timestamp(sys_days(day)) + hours{hour} + minutes{minute};
}

/// A zone that switches from `before` to `after` offset at `change` and back at `back`
/// (Europe- or US-style daylight saving time in one year).
class DstZone final : public TimeZone {
public:
    DstZone(minutes standard, minutes summer, core::Timestamp begin, core::Timestamp end)
        : standard_(standard), summer_(summer), begin_(begin), end_(end) {}
    [[nodiscard]] minutes utcOffset(core::Timestamp instant) const override {
        return instant >= begin_ && instant < end_ ? summer_ : standard_;
    }

private:
    minutes standard_;
    minutes summer_;
    core::Timestamp begin_;
    core::Timestamp end_;
};

/// Central Europe 2026: CET (+1) / CEST (+2), changes at 01:00 UTC on 29 Mar and 25 Oct.
DstZone centralEurope() {
    return {60min, 120min, utc(date(2026, 3, 29), 1), utc(date(2026, 10, 25), 1)};
}
/// US Pacific 2026: PST (−8) / PDT (−7), changes at 10:00 UTC on 8 Mar and 09:00 UTC 1 Nov.
DstZone usPacific() {
    return {-480min, -420min, utc(date(2026, 3, 8), 10), utc(date(2026, 11, 1), 9)};
}

Task task(std::uint64_t n, std::string title) {
    Task t;
    t.id = core::TaskId{uuid(n)};
    t.title = std::move(title);
    t.order = core::FractionalIndex::first();
    return t;
}

std::vector<const Task*> pointers(const std::vector<Task>& tasks) {
    std::vector<const Task*> out;
    for (const Task& t : tasks) {
        out.push_back(&t);
    }
    return out;
}

std::vector<std::string> titles(const std::vector<Task>& tasks,
                                const std::vector<std::size_t>& at) {
    std::vector<std::string> out;
    for (const std::size_t i : at) {
        out.push_back(tasks[i].title);
    }
    return out;
}

// ---------------------------------------------------------------------------- records

TEST(StudyRecordsTest, ValueChecks) {
    Task t = task(1, "Read chapter 3");
    EXPECT_TRUE(checkValues(t).has_value());
    t.title = "  \t";
    EXPECT_FALSE(checkValues(t).has_value());
    t.title = "ok";
    t.dueTime = 9h;
    EXPECT_FALSE(checkValues(t).has_value()); // a time needs a date
    t.dueDate = date(2026, 2, 30);
    EXPECT_FALSE(checkValues(t).has_value()); // not a date
    t.dueDate = date(2026, 2, 28);
    EXPECT_TRUE(checkValues(t).has_value());
    t.dueTime = 24h;
    EXPECT_FALSE(checkValues(t).has_value());
    t.dueTime = 23h + 59min;
    t.scheduled = TimeBlock{utc(date(2026, 1, 1), 10), utc(date(2026, 1, 1), 9)};
    EXPECT_FALSE(checkValues(t).has_value()); // ends before it starts
    t.scheduled = TimeBlock{utc(date(2026, 1, 1), 10), utc(date(2026, 1, 1), 10)};
    EXPECT_TRUE(checkValues(t).has_value()); // an instant is a valid block
    t.estimate = -1min;
    EXPECT_FALSE(checkValues(t).has_value());
    t.estimate = 0min;
    t.parent = t.id;
    EXPECT_FALSE(checkValues(t).has_value());
    t.parent.reset();
    const core::TagId a{uuid(1)};
    const core::TagId b{uuid(2)};
    t.tags = {b, a};
    EXPECT_FALSE(checkValues(t).has_value()); // unsorted
    t.tags = {a, a};
    EXPECT_FALSE(checkValues(t).has_value()); // repeated
    t.tags = {a, b};
    EXPECT_TRUE(checkValues(t).has_value());
    t.status = static_cast<TaskStatus>(9);
    EXPECT_FALSE(checkValues(t).has_value());

    Course course{.title = "Biology"};
    course.start = date(2026, 9, 1);
    course.end = date(2026, 8, 1);
    EXPECT_FALSE(checkValues(course).has_value());
    course.end = date(2026, 12, 20);
    EXPECT_TRUE(checkValues(course).has_value());
    EXPECT_FALSE(checkValues(Project{.title = ""}).has_value());
    EXPECT_FALSE(checkValues(Tag{.name = " "}).has_value());

    EXPECT_EQ(foldTagName("Exam PREP ä"), "exam prep ä"); // ASCII only, like COLLATE NOCASE
    EXPECT_FALSE(isStorableDate(date(0, 1, 1)));
    EXPECT_FALSE(isStorableDate(date(10000, 1, 1)));
    EXPECT_TRUE(isStorableDate(date(2024, 2, 29)));
}

// ---------------------------------------------------------------------------- local time

TEST(StudyTimeTest, FixedOffsetsPlaceInstantsOnLocalDays) {
    const core::Timestamp instant = utc(date(2026, 9, 27), 11, 30);
    EXPECT_EQ(localDate(instant, FixedOffsetZone{0min}), date(2026, 9, 27));
    EXPECT_EQ(localDate(instant, FixedOffsetZone{825min}), date(2026, 9, 28));  // +13:45
    EXPECT_EQ(localDate(instant, FixedOffsetZone{-720min}), date(2026, 9, 26)); // −12:00
    for (const minutes offset : {-720min, -210min, 0min, 330min, 825min}) {
        const FixedOffsetZone zone{offset};
        EXPECT_EQ(toInstant(toLocal(instant, zone), zone), instant) << offset.count();
        EXPECT_EQ(localDate(startOfDay(date(2026, 9, 27), zone), zone), date(2026, 9, 27));
    }
}

TEST(StudyTimeTest, DaylightSavingChangesAreResolved) {
    const DstZone zone = centralEurope();
    // 29 Mar: 02:00–03:00 local does not exist; the day is 23 h long.
    const LocalTime skipped(std::chrono::local_days(date(2026, 3, 29)) + 2h + 30min);
    EXPECT_EQ(toInstant(skipped, zone), utc(date(2026, 3, 29), 1, 30)); // shown as 03:30
    EXPECT_EQ(startOfDay(date(2026, 3, 30), zone) - startOfDay(date(2026, 3, 29), zone), 23h);
    // 25 Oct: 02:00–03:00 local happens twice; the first occurrence is taken.
    const LocalTime repeated(std::chrono::local_days(date(2026, 10, 25)) + 2h + 30min);
    EXPECT_EQ(toInstant(repeated, zone), utc(date(2026, 10, 25), 0, 30));
    EXPECT_EQ(startOfDay(date(2026, 10, 26), zone) - startOfDay(date(2026, 10, 25), zone), 25h);
    // Round trips away from the changes, in summer and winter, both zones.
    const DstZone pacific = usPacific();
    for (const core::Timestamp t : {utc(date(2026, 1, 15), 8), utc(date(2026, 7, 1), 23, 59),
                                    utc(date(2026, 3, 8), 11), utc(date(2026, 11, 1), 12)}) {
        EXPECT_EQ(toInstant(toLocal(t, zone), zone), t);
        EXPECT_EQ(toInstant(toLocal(t, pacific), pacific), t);
    }
    EXPECT_EQ(localDate(utc(date(2026, 7, 1), 22, 30), zone), date(2026, 7, 2));   // 00:30 CEST
    EXPECT_EQ(localDate(utc(date(2026, 1, 1), 22, 30), zone), date(2026, 1, 1));   // 23:30 CET
    EXPECT_EQ(localDate(utc(date(2026, 7, 2), 6, 30), pacific), date(2026, 7, 1)); // 23:30 PDT
}

TEST(StudyTimeTest, WeeksStartOnMonday) {
    EXPECT_EQ(weekStart(date(2026, 9, 27)), date(2026, 9, 21)); // Sunday
    EXPECT_EQ(weekStart(date(2026, 9, 21)), date(2026, 9, 21)); // Monday
    EXPECT_EQ(weekStart(date(2027, 1, 2)), date(2026, 12, 28)); // across a year
}

// ---------------------------------------------------------------------------- agenda

TEST(StudyAgendaTest, BucketsFollowTheLocalDateOfNow) {
    std::vector<Task> tasks{task(1, "due 26"),    task(2, "due 27"),    task(3, "due 28"),
                            task(4, "due 4 Oct"), task(5, "due 5 Oct"), task(6, "someday"),
                            task(7, "done"),      task(8, "cancelled")};
    tasks[0].dueDate = date(2026, 9, 26);
    tasks[1].dueDate = date(2026, 9, 27);
    tasks[2].dueDate = date(2026, 9, 28);
    tasks[3].dueDate = date(2026, 10, 4); // the 7th day after the 27th
    tasks[4].dueDate = date(2026, 10, 5);
    tasks[6].dueDate = date(2026, 9, 27);
    tasks[6].status = TaskStatus::Done;
    tasks[7].status = TaskStatus::Cancelled;
    const auto all = pointers(tasks);
    // 23:30 UTC on the 27th: still the 27th in UTC and west of it, already the 28th east.
    const core::Timestamp now = utc(date(2026, 9, 27), 23, 30);

    const Agenda inUtc = buildAgenda(all, now, FixedOffsetZone{0min});
    EXPECT_EQ(titles(tasks, inUtc.overdue), (std::vector<std::string>{"due 26"}));
    EXPECT_EQ(titles(tasks, inUtc.today), (std::vector<std::string>{"due 27"}));
    EXPECT_EQ(titles(tasks, inUtc.upcoming), (std::vector<std::string>{"due 28", "due 4 Oct"}));
    EXPECT_EQ(titles(tasks, inUtc.unscheduled), (std::vector<std::string>{"someday"}));

    const Agenda inEurope = buildAgenda(all, now, centralEurope()); // 01:30 CEST on the 28th
    EXPECT_EQ(titles(tasks, inEurope.overdue), (std::vector<std::string>{"due 26", "due 27"}));
    EXPECT_EQ(titles(tasks, inEurope.today), (std::vector<std::string>{"due 28"}));
    EXPECT_EQ(titles(tasks, inEurope.upcoming),
              (std::vector<std::string>{"due 4 Oct", "due 5 Oct"}));

    const Agenda inPacific = buildAgenda(all, now, usPacific()); // 16:30 PDT on the 27th
    EXPECT_EQ(inPacific, inUtc);
    const Agenda farEast = buildAgenda(all, now, FixedOffsetZone{825min}); // 13:15 on the 28th
    EXPECT_EQ(farEast, inEurope);

    const Agenda shortHorizon = buildAgenda(all, now, FixedOffsetZone{0min}, {.upcomingDays = 1});
    EXPECT_EQ(titles(tasks, shortHorizon.upcoming), (std::vector<std::string>{"due 28"}));
}

TEST(StudyAgendaTest, TimeBlocksAndOrdering) {
    const DstZone zone = centralEurope();
    const core::Timestamp now = utc(date(2026, 9, 28), 8); // 10:00 CEST, Monday
    std::vector<Task> tasks{task(1, "block 14:00"),        task(2, "due 09:00"),
                            task(3, "due untimed low"),    task(4, "due untimed high"),
                            task(5, "block last night"),   task(6, "block tomorrow"),
                            task(7, "block over midnight")};
    tasks[0].scheduled = TimeBlock{utc(date(2026, 9, 28), 12), utc(date(2026, 9, 28), 13)};
    tasks[1].dueDate = date(2026, 9, 28);
    tasks[1].dueTime = 9h;
    tasks[2].dueDate = date(2026, 9, 28);
    tasks[2].priority = Priority::Low;
    tasks[3].dueDate = date(2026, 9, 28);
    tasks[3].priority = Priority::High;
    tasks[4].scheduled = TimeBlock{utc(date(2026, 9, 27), 18), utc(date(2026, 9, 27), 19)};
    tasks[5].scheduled = TimeBlock{utc(date(2026, 9, 29), 7), utc(date(2026, 9, 29), 8)};
    // 23:00–01:00 local from the 27th into the 28th: it belongs to today too.
    tasks[6].scheduled = TimeBlock{utc(date(2026, 9, 27), 21), utc(date(2026, 9, 27), 23)};
    const Agenda agenda = buildAgenda(pointers(tasks), now, zone);
    EXPECT_EQ(titles(tasks, agenda.today),
              (std::vector<std::string>{"block over midnight", "due 09:00", "block 14:00",
                                        "due untimed high", "due untimed low"}));
    EXPECT_EQ(titles(tasks, agenda.overdue), (std::vector<std::string>{"block last night"}));
    EXPECT_EQ(titles(tasks, agenda.upcoming), (std::vector<std::string>{"block tomorrow"}));
    EXPECT_TRUE(agenda.unscheduled.empty());
}

TEST(StudyAgendaTest, FloatingDueDatesDoNotMoveWithTheZone) {
    // A task due on the 28th is due on the 28th in every zone: only "today" moves.
    std::vector<Task> tasks{task(1, "exam")};
    tasks[0].dueDate = date(2026, 9, 28);
    tasks[0].dueTime = 9h;
    const auto all = pointers(tasks);
    for (const minutes offset : {-720min, -300min, 0min, 60min, 330min, 825min}) {
        const FixedOffsetZone zone{offset};
        const core::Timestamp localNoon =
            toInstant(LocalTime(std::chrono::local_days(date(2026, 9, 28)) + 12h), zone);
        EXPECT_EQ(buildAgenda(all, localNoon, zone).today, (std::vector<std::size_t>{0}))
            << offset.count();
        const core::Timestamp dayBefore = localNoon - days{1};
        EXPECT_EQ(buildAgenda(all, dayBefore, zone).upcoming, (std::vector<std::size_t>{0}));
        const core::Timestamp dayAfter = localNoon + days{1};
        EXPECT_EQ(buildAgenda(all, dayAfter, zone).overdue, (std::vector<std::size_t>{0}));
    }
}

// ---------------------------------------------------------------------------- week

TEST(StudyWeekTest, DaysHoldDueTasksAndBlocksCutAtLocalMidnight) {
    const DstZone zone = centralEurope();
    const CalendarDate monday = date(2026, 10, 19);
    std::vector<Task> tasks{task(1, "due Wed 15:00"), task(2, "due Wed"),   task(3, "overnight"),
                            task(4, "next week"),     task(5, "cancelled"), task(6, "done Mon")};
    tasks[0].dueDate = date(2026, 10, 21);
    tasks[0].dueTime = 15h;
    tasks[1].dueDate = date(2026, 10, 21);
    // Sat 22:00 local → Sun 03:00 local, across the change back to CET on Sunday 25 Oct.
    tasks[2].scheduled = TimeBlock{utc(date(2026, 10, 24), 20), utc(date(2026, 10, 25), 2)};
    tasks[3].dueDate = date(2026, 10, 26);
    tasks[4].dueDate = date(2026, 10, 20);
    tasks[4].status = TaskStatus::Cancelled;
    tasks[5].dueDate = monday;
    tasks[5].status = TaskStatus::Done;
    const auto week = buildWeek(pointers(tasks), monday, zone);
    EXPECT_EQ(week[0].date, monday);
    EXPECT_EQ(week[6].date, date(2026, 10, 25));
    EXPECT_EQ(week[0].entries, (std::vector<DayEntry>{{.task = 5}})); // done tasks stay
    EXPECT_TRUE(week[1].entries.empty());                             // cancelled left out
    EXPECT_EQ(week[2].entries, (std::vector<DayEntry>{{.task = 0}, {.task = 1}}));
    EXPECT_EQ(week[5].entries,
              (std::vector<DayEntry>{{.task = 2, .blockStart = 22h, .blockEnd = 24h}}));
    // Sunday: 00:00 local to 03:00 CET (02:00 UTC); the repeated hour makes it 4 h long.
    EXPECT_EQ(week[6].entries,
              (std::vector<DayEntry>{{.task = 2, .blockStart = 0h, .blockEnd = 3h}}));
    for (const WeekDay& day : week) {
        for (const DayEntry& entry : day.entries) {
            EXPECT_NE(entry.task, 3U); // next week
        }
    }
}

TEST(StudyWeekTest, ProgressCountsDoneAmongNotCancelled) {
    std::vector<Task> tasks{task(1, "a"), task(2, "b"), task(3, "c"), task(4, "d")};
    tasks[0].status = TaskStatus::Done;
    tasks[1].status = TaskStatus::InProgress;
    tasks[2].status = TaskStatus::Cancelled;
    EXPECT_EQ(progressOf(pointers(tasks)), (Progress{.done = 1, .total = 3}));
    EXPECT_EQ(progressOf({}), (Progress{}));
}

// ---------------------------------------------------------------------------- estimates

TEST(StudyEstimateTest, TotalsPerDueDateAndOverall) {
    const CalendarDate monday = date(2026, 10, 12);
    const CalendarDate tuesday = date(2026, 10, 13);
    const CalendarDate friday = date(2026, 10, 16);
    std::vector<Task> tasks{task(1, "read"),    task(2, "notes"),   task(3, "exercises"),
                            task(4, "review"),  task(5, "summary"), task(6, "done"),
                            task(7, "dropped"), task(8, "zero"),    task(9, "blocked")};
    tasks[0].dueDate = tuesday;
    tasks[0].estimate = 30min;
    tasks[1].dueDate = tuesday; // no estimate
    tasks[2].dueDate = friday;
    tasks[2].estimate = 45min;
    tasks[3].estimate = 20min; // no due date
    // tasks[4]: no due date, no estimate
    tasks[5].dueDate = friday; // done tasks count
    tasks[5].estimate = 15min;
    tasks[5].status = TaskStatus::Done;
    tasks[6].dueDate = tuesday; // cancelled tasks do not
    tasks[6].estimate = 100min;
    tasks[6].status = TaskStatus::Cancelled;
    tasks[7].dueDate = monday; // 0 is an estimate, not a missing one
    tasks[7].estimate = 0min;
    tasks[8].scheduled = TimeBlock{.start = utc(friday, 9), .end = utc(friday, 10)};
    tasks[8].estimate = 60min; // a time block alone is not a due date
    tasks[8].status = TaskStatus::InProgress;

    const EstimateTotals totals = estimateTotals(pointers(tasks));
    EXPECT_EQ(totals.days, (std::vector<DayEstimate>{
                               {.date = monday, .sum = {.minutes = 0min, .withoutEstimate = 0}},
                               {.date = tuesday, .sum = {.minutes = 30min, .withoutEstimate = 1}},
                               {.date = friday, .sum = {.minutes = 60min, .withoutEstimate = 0}},
                           }));
    EXPECT_EQ(totals.noDueDate, (EstimateSum{.minutes = 80min, .withoutEstimate = 1}));
    EXPECT_EQ(totals.total, (EstimateSum{.minutes = 170min, .withoutEstimate = 2}));

    // The order of the tasks does not matter.
    std::vector<const Task*> reversed = pointers(tasks);
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_EQ(estimateTotals(reversed), totals);
}

TEST(StudyEstimateTest, OnlyTheGivenTasksCount) {
    EXPECT_EQ(estimateTotals({}), EstimateTotals{});

    // A subtask is not added to its task: whatever the caller passes is counted once.
    std::vector<Task> tasks{task(1, "task"), task(2, "subtask")};
    tasks[0].estimate = 90min;
    tasks[1].estimate = 30min;
    tasks[1].parent = tasks[0].id;
    const std::vector<const Task*> all = pointers(tasks);
    EXPECT_EQ(estimateTotals(std::span(all).first(1)).total,
              (EstimateSum{.minutes = 90min, .withoutEstimate = 0}));
    EXPECT_EQ(estimateTotals(all).total, (EstimateSum{.minutes = 120min, .withoutEstimate = 0}));

    // Only cancelled tasks: nothing counted, no day listed.
    tasks[0].status = TaskStatus::Cancelled;
    tasks[0].dueDate = date(2026, 10, 14);
    EXPECT_TRUE(estimateTotals(std::span(all).first(1)).days.empty());
    EXPECT_EQ(estimateTotals(std::span(all).first(1)), EstimateTotals{});
}

TEST(StudyEstimateTest, LargeSumsAreExact) {
    std::vector<Task> tasks;
    for (std::uint64_t i = 0; i < 2000; ++i) {
        tasks.push_back(task(i + 1, "t"));
        tasks.back().estimate = 9999min;
        tasks.back().dueDate =
            CalendarDate{sys_days(date(2026, 1, 1)) + days{static_cast<int>(i % 366)}};
    }
    const EstimateTotals totals = estimateTotals(pointers(tasks));
    EXPECT_EQ(totals.total.minutes, 2000 * 9999min);
    EXPECT_EQ(totals.days.size(), 366U);
    EXPECT_TRUE(
        std::is_sorted(totals.days.begin(), totals.days.end(),
                       [](const DayEstimate& a, const DayEstimate& b) { return a.date < b.date; }));
}

} // namespace
} // namespace studyapp::study
