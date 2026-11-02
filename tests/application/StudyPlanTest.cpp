// 2.0-PLAN-03: a study plan from a chosen scope, without AI (docs/VERSION_2_0.md §5,
// §7.5). draftByTreeOrder against an in-memory workspace: the scope in tree order, the
// minutes per page, whole and split sections, unused time, the 12-blocks-a-day limit,
// overflow after the last day, the 100-block limit; every page of the scope is in exactly
// one block, in tree order. validatePlanDraft: each rule with a passing and a failing case.
// PlanPreview: the overflow summary and resolutions, block edits, "Excluded pages", and
// the review against a workspace that changes after the build (rules 4, 6, 7).

#include <studyapp/application/StudyPlan.hpp>

#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace studyapp::application {
namespace {

namespace commands = document::commands;

using std::chrono::minutes;
using study::CalendarDate;

constexpr CalendarDate date(int y, unsigned m, unsigned d) {
    return CalendarDate{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
}

constexpr CalendarDate kStart = date(2026, 10, 19);

/// A block as (day, 0 if unscheduled; minutes; pages; title).
using Placed = std::tuple<int, int, std::size_t, std::string>;

std::vector<Placed> layout(const PlanDraft& draft) {
    std::vector<Placed> out;
    for (const PlanBlock& block : draft.blocks) {
        out.emplace_back(block.day.value_or(0), static_cast<int>(block.minutes.count()),
                         block.pages.size(), block.title);
    }
    return out;
}

std::vector<PlanIssueKind> kinds(const std::vector<PlanIssue>& issues) {
    std::vector<PlanIssueKind> out;
    for (const PlanIssue& issue : issues) {
        out.push_back(issue.kind);
    }
    return out;
}

/// A section `title` of `pages` pages ("<title> 1", …) appended to `notebook`.
core::SectionId addPages(document::test::TestWorkspace& t, core::NotebookId notebook,
                         const std::string& title, int pages) {
    const core::SectionId section = t.addSection(notebook, title);
    for (int i = 1; i <= pages; ++i) {
        t.addPage(section, title + " " + std::to_string(i));
    }
    return section;
}

struct StudyPlanTest : ::testing::Test {
    document::test::TestWorkspace t;
    core::NotebookId biology = t.addNotebook("Biology");

    core::SectionId addSection(core::NotebookId notebook, const std::string& title, int pages) {
        return addPages(t, notebook, title, pages);
    }
    core::SectionId addSection(const std::string& title, int pages) {
        return addSection(biology, title, pages);
    }

    PlanRequest request(int days, int minutesPerDay, PlanScope scope) const {
        return {.goal = "Biology exam",
                .start = kStart,
                .days = days,
                .minutesPerDay = minutes{minutesPerDay},
                .scope = std::move(scope)};
    }
    PlanRequest request(int days, int minutesPerDay) const {
        return request(days, minutesPerDay, PlanScope{.notebooks = {biology}});
    }

    PlanBuild build(const PlanRequest& planRequest) const {
        auto built = draftByTreeOrder(t.workspace, planRequest);
        EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().message);
        return built ? *built : PlanBuild{};
    }

    /// The draft for `planRequest`, checking that its blocks hold every page of the scope
    /// exactly once, in tree order (acceptance 5).
    PlanDraft draft(const PlanRequest& planRequest) const {
        const PlanBuild built = build(planRequest);
        EXPECT_EQ(built.outcome, PlanBuildOutcome::Draft);
        if (!built.draft) {
            return {};
        }
        std::vector<core::PageId> placed;
        for (const PlanBlock& block : built.draft->blocks) {
            placed.insert(placed.end(), block.pages.begin(), block.pages.end());
        }
        EXPECT_EQ(placed, pagesInScope(t.workspace, planRequest.scope));
        EXPECT_EQ(built.blocksNeeded, built.draft->blocks.size());
        return *built.draft;
    }

    core::ErrorCode refusal(const PlanRequest& planRequest) const {
        auto built = draftByTreeOrder(t.workspace, planRequest);
        EXPECT_FALSE(built.has_value());
        return built ? core::ErrorCode::Internal : built.error().code;
    }
};

// ---------------------------------------------------------------------- scope and dates

TEST_F(StudyPlanTest, EmptyScopeHasNoDraft) {
    const core::SectionId empty = t.addSection(biology, "Empty");
    for (const PlanScope& scope : {PlanScope{}, PlanScope{.sections = {empty}},
                                   PlanScope{.notebooks = {core::NotebookId{t.ids.next()}},
                                             .sections = {core::SectionId{t.ids.next()}}}}) {
        const PlanBuild built = build(request(7, 60, scope));
        EXPECT_EQ(built.outcome, PlanBuildOutcome::NoPages);
        EXPECT_FALSE(built.draft.has_value());
        EXPECT_EQ(built.blocksNeeded, 0U);
        EXPECT_TRUE(built.sections.empty());
        EXPECT_TRUE(pagesInScope(t.workspace, scope).empty());
    }
}

TEST_F(StudyPlanTest, MixedScopeInTreeOrder) {
    const core::SectionId cells = addSection("Cells", 1);
    addSection("Empty", 0);
    const core::SectionId genes = addSection("Genes", 2);
    const core::NotebookId chemistry = t.addNotebook("Chemistry");
    addSection(chemistry, "Atoms", 1);
    const core::SectionId bonds = addSection(chemistry, "Bonds", 1);

    // Chosen in reverse order: the plan follows the tree, not the order of the choices.
    const PlanScope scope{.notebooks = {biology}, .sections = {bonds}};
    const PlanBuild built = build(request(1, 60, scope));
    EXPECT_EQ(built.sections, (std::vector<ScopeSection>{{cells, 1}, {genes, 2}, {bonds, 1}}));

    // N = 4, e = 60 / 4 = 15.
    const PlanDraft plan = draft(request(1, 60, scope));
    EXPECT_EQ(layout(plan),
              (std::vector<Placed>{{1, 15, 1, "Cells"}, {1, 30, 2, "Genes"}, {1, 15, 1, "Bonds"}}));
    EXPECT_EQ(plan.title, "Biology exam");
    EXPECT_EQ(plan.start, kStart);
    EXPECT_EQ(plan.days, 1);
    EXPECT_EQ(plan.minutesPerDay, minutes{60});
    for (const PlanBlock& block : plan.blocks) {
        EXPECT_TRUE(block.objective.empty());
    }
}

TEST_F(StudyPlanTest, PageChosenTwiceCountsOnce) {
    const core::SectionId cells = addSection("Cells", 3);
    const PlanScope scope{.notebooks = {biology}, .sections = {cells, cells}};
    EXPECT_EQ(pagesInScope(t.workspace, scope).size(), 3U);
    const PlanBuild built = build(request(1, 60, scope));
    EXPECT_EQ(built.sections, (std::vector<ScopeSection>{{cells, 3}}));
    EXPECT_EQ(layout(draft(request(1, 60, scope))), (std::vector<Placed>{{1, 60, 3, "Cells"}}));
}

TEST_F(StudyPlanTest, PdfPagesCountLikeAnyPage) {
    addSection("Notes", 1);
    const std::vector<core::DVec2> sizes{{612.0, 792.0}, {612.0, 792.0}};
    const core::SectionId lecture = t.run(commands::createDocumentSection(
        t.workspace, biology, "Lecture", core::AssetId{t.ids.next()}, sizes, t.clock, t.ids));
    ASSERT_TRUE(t.workspace.findPage(t.workspace.pagesOf(lecture).front())->document);

    // N = 3, e = 60 / 3 = 20.
    EXPECT_EQ(layout(draft(request(1, 60))),
              (std::vector<Placed>{{1, 20, 1, "Notes"}, {1, 40, 2, "Lecture"}}));
}

TEST_F(StudyPlanTest, GoalIsTrimmedIntoTheTitle) {
    addSection("Cells", 1);
    PlanRequest planRequest = request(1, 60);
    planRequest.goal = "  Biology exam \t";
    EXPECT_EQ(draft(planRequest).title, "Biology exam");
    planRequest.goal = "\f\vBiology exam\v\f";
    EXPECT_EQ(draft(planRequest).title, "Biology exam");
}

TEST_F(StudyPlanTest, DaysBecomeDatesWithoutATimeZone) {
    EXPECT_EQ(planDate(kStart, 1), kStart);
    EXPECT_EQ(planDate(kStart, 13), date(2026, 10, 31));
    EXPECT_EQ(planDate(kStart, 14), date(2026, 11, 1));           // month end
    EXPECT_EQ(planDate(date(2026, 12, 31), 2), date(2027, 1, 1)); // year end
    EXPECT_EQ(planDate(date(2028, 2, 28), 2), date(2028, 2, 29)); // leap day
    EXPECT_EQ(planDate(date(2028, 2, 28), 3), date(2028, 3, 1));
    EXPECT_EQ(planDate(date(2027, 2, 28), 2), date(2027, 3, 1));
    EXPECT_EQ(planDate(kStart, 60), date(2026, 12, 17));
    // The limits of storable dates (years 1 … 9999): the day after 9999-12-31 is a valid
    // calendar date that is not storable, and year 1 counts forward normally.
    EXPECT_EQ(planDate(date(9999, 12, 31), 2), date(10000, 1, 1));
    EXPECT_TRUE(planDate(date(9999, 12, 31), 2).ok());
    EXPECT_FALSE(study::isStorableDate(planDate(date(9999, 12, 31), 2)));
    EXPECT_EQ(planDate(date(1, 1, 1), 60), date(1, 3, 1));
}

TEST_F(StudyPlanTest, RequestOutOfRangeIsRefused) {
    addSection("Cells", 1);
    const auto with = [&](int days, int perDay, CalendarDate start) {
        PlanRequest planRequest = request(days, perDay);
        planRequest.start = start;
        return planRequest;
    };
    for (const PlanRequest& refused :
         {with(0, 60, kStart), with(61, 60, kStart), with(7, 0, kStart), with(7, 4, kStart),
          with(7, 62, kStart), with(7, 485, kStart), with(7, 60, date(2026, 2, 30)),
          with(7, 60, date(0, 12, 31)),
          // The last day must be storable too.
          with(2, 60, date(9999, 12, 31)), with(60, 60, date(9999, 11, 3))}) {
        EXPECT_EQ(refusal(refused), core::ErrorCode::InvalidArgument);
    }
    for (const PlanRequest& accepted :
         {with(1, 5, kStart), with(60, 480, kStart), with(1, 60, date(9999, 12, 31)),
          with(60, 60, date(9999, 11, 2)), with(60, 60, date(1, 1, 1))}) {
        EXPECT_EQ(build(accepted).outcome, PlanBuildOutcome::Draft);
    }
}

// ---------------------------------------------------------------------- placement

TEST_F(StudyPlanTest, OnePage) {
    addSection("Cells", 1);
    // e = 7 × 60 / 1, kept within the 60 minutes of a day: one block, the section's title.
    EXPECT_EQ(layout(draft(request(7, 60))), (std::vector<Placed>{{1, 60, 1, "Cells"}}));
}

TEST_F(StudyPlanTest, MorePagesThanDays) {
    addSection("Cells", 10);
    // e = 120 / 10 = 12, rounded down to 10; 100 minutes do not fit in a day: split.
    EXPECT_EQ(layout(draft(request(2, 60))),
              (std::vector<Placed>{{1, 60, 6, "Cells (part 1 of 2)"},
                                   {2, 40, 4, "Cells (part 2 of 2)"}}));
}

TEST_F(StudyPlanTest, MoreDaysThanPages) {
    addSection("Cells", 2);
    // e = 420 / 2 = 210, kept to the 60 minutes of a day: one page a day, days 3 … 7 empty.
    EXPECT_EQ(layout(draft(request(7, 60))),
              (std::vector<Placed>{{1, 60, 1, "Cells (part 1 of 2)"},
                                   {2, 60, 1, "Cells (part 2 of 2)"}}));
}

TEST_F(StudyPlanTest, SectionExactlyFillsTheRest) {
    addSection("A", 1);
    addSection("B", 2);
    addSection("C", 3);
    // e = 120 / 6 = 20: B's 40 minutes are exactly the 40 left on day 1.
    EXPECT_EQ(layout(draft(request(2, 60))),
              (std::vector<Placed>{{1, 20, 1, "A"}, {1, 40, 2, "B"}, {2, 60, 3, "C"}}));
}

TEST_F(StudyPlanTest, WholeSectionMovesToTheNextDay) {
    addSection("A", 2);
    addSection("B", 2);
    addSection("C", 1);
    // e = 120 / 5 = 24, rounded down to 20. B (40) fits in a day but not in the 20 left on
    // day 1: it moves to day 2 whole, and day 1's 20 minutes stay unused.
    EXPECT_EQ(layout(draft(request(2, 60))),
              (std::vector<Placed>{{1, 40, 2, "A"}, {2, 40, 2, "B"}, {2, 20, 1, "C"}}));
}

TEST_F(StudyPlanTest, SplitSectionStartsMidDay) {
    addSection("A", 1);
    addSection("B", 4);
    // e = 120 / 5 = 24 → 20. B (80) is larger than a day: its first part uses day 1's rest.
    EXPECT_EQ(layout(draft(request(2, 60))),
              (std::vector<Placed>{
                  {1, 20, 1, "A"}, {1, 40, 2, "B (part 1 of 2)"}, {2, 40, 2, "B (part 2 of 2)"}}));
}

TEST_F(StudyPlanTest, SplitSectionStartsOnAnEmptyDay) {
    addSection("B", 4);
    addSection("A", 1);
    EXPECT_EQ(layout(draft(request(2, 60))),
              (std::vector<Placed>{
                  {1, 60, 3, "B (part 1 of 2)"}, {2, 20, 1, "B (part 2 of 2)"}, {2, 20, 1, "A"}}));
}

TEST_F(StudyPlanTest, TimeSmallerThanAPageStaysUnused) {
    addSection("A", 2);
    addSection("B", 10);
    // e = 300 / 12 = 25. After A, 10 minutes are left on day 1: less than a page, so they
    // stay unused and B starts on day 2; each later day leaves 10 minutes too, and the last
    // part needs day 6 of 5.
    EXPECT_EQ(layout(draft(request(5, 60))), (std::vector<Placed>{{1, 50, 2, "A"},
                                                                  {2, 50, 2, "B (part 1 of 5)"},
                                                                  {3, 50, 2, "B (part 2 of 5)"},
                                                                  {4, 50, 2, "B (part 3 of 5)"},
                                                                  {5, 50, 2, "B (part 4 of 5)"},
                                                                  {6, 50, 2, "B (part 5 of 5)"}}));
}

TEST_F(StudyPlanTest, ThirteenthWholeBlockGoesToTheNextDay) {
    for (int i = 1; i <= 13; ++i) {
        addSection("S" + std::to_string(i), 1);
    }
    addSection("Z", 12);
    // N = 25, e = 960 / 25 = 38 → 35. Day 1 holds 12 blocks (420 minutes); the 13th still
    // fits by minutes (60 left) but goes to day 2, and Z (420) follows it there.
    const PlanDraft plan = draft(request(2, 480));
    ASSERT_EQ(plan.blocks.size(), 14U);
    for (std::size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(plan.blocks[i].day, 1);
    }
    EXPECT_EQ(layout(plan)[12], (Placed{2, 35, 1, "S13"}));
    EXPECT_EQ(layout(plan)[13], (Placed{2, 420, 12, "Z"}));
}

TEST_F(StudyPlanTest, ThirteenthSplitBlockGoesToTheNextDay) {
    for (int i = 1; i <= 12; ++i) {
        addSection("S" + std::to_string(i), 1);
    }
    addSection("Z", 14);
    // N = 26, e = 960 / 26 = 36 → 35. After 12 blocks day 1 has 60 minutes left, room for
    // one page of Z (490 minutes, split), but Z's first part starts on day 2.
    const PlanDraft plan = draft(request(2, 480));
    ASSERT_EQ(plan.blocks.size(), 14U);
    EXPECT_EQ(plan.blocks[11].day, 1);
    EXPECT_EQ(layout(plan)[12], (Placed{2, 455, 13, "Z (part 1 of 2)"}));
    EXPECT_EQ(layout(plan)[13], (Placed{3, 35, 1, "Z (part 2 of 2)"}));
}

// ---------------------------------------------------------------------- overflow and limits

TEST_F(StudyPlanTest, OverflowByOneBlock) {
    addSection("A", 2);
    addSection("B", 2);
    addSection("C", 2);
    // e = 120 / 6 = 20; each day leaves 20 unused, so C needs day 3 of a 2-day plan. It is
    // not squeezed onto day 2 and the plan keeps its 2 days.
    const PlanDraft plan = draft(request(2, 60));
    EXPECT_EQ(layout(plan),
              (std::vector<Placed>{{1, 40, 2, "A"}, {2, 40, 2, "B"}, {3, 40, 2, "C"}}));
    EXPECT_EQ(plan.days, 2);
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.errors), std::vector{PlanIssueKind::BlockAfterLastDay});
    EXPECT_EQ(checked.errors.front().block, 2U);
    EXPECT_FALSE(checked.canSave());
}

TEST_F(StudyPlanTest, OverflowBySeveralDays) {
    addSection("A", 4);
    // N × 5 > B: e = 5 / 4 = 1 → 0, raised to 5; one page a day on days 1 … 4 of 1.
    const PlanDraft plan = draft(request(1, 5));
    EXPECT_EQ(layout(plan), (std::vector<Placed>{{1, 5, 1, "A (part 1 of 4)"},
                                                 {2, 5, 1, "A (part 2 of 4)"},
                                                 {3, 5, 1, "A (part 3 of 4)"},
                                                 {4, 5, 1, "A (part 4 of 4)"}}));
    EXPECT_EQ(kinds(validatePlanDraft(t.workspace, plan).errors),
              (std::vector<PlanIssueKind>(3, PlanIssueKind::BlockAfterLastDay)));
}

TEST_F(StudyPlanTest, MinimumFiveMinutesPerPage) {
    for (const char* title : {"A", "B", "C", "D", "E"}) {
        addSection(title, 1);
    }
    // e = 20 / 5 = 4 → 0, raised to 5: two pages a day.
    EXPECT_EQ(layout(draft(request(2, 10))),
              (std::vector<Placed>{
                  {1, 5, 1, "A"}, {1, 5, 1, "B"}, {2, 5, 1, "C"}, {2, 5, 1, "D"}, {3, 5, 1, "E"}}));
}

TEST_F(StudyPlanTest, MoreThan100BlocksKeepsEverything) {
    std::vector<ScopeSection> sections;
    for (int i = 1; i <= 101; ++i) {
        sections.push_back({addSection("S" + std::to_string(i), 1), 1});
    }
    // e = 28800 / 101 = 285: one section a day, 101 blocks.
    const PlanRequest planRequest = request(60, 480);
    const PlanRequest before = planRequest;
    const PlanBuild built = build(planRequest);
    EXPECT_EQ(built.outcome, PlanBuildOutcome::TooManyBlocks);
    EXPECT_FALSE(built.draft.has_value());
    EXPECT_EQ(built.blocksNeeded, 101U);
    EXPECT_EQ(built.sections, sections);
    EXPECT_EQ(planRequest, before);
}

TEST_F(StudyPlanTest, OneHundredBlocksAreADraft) {
    for (int i = 1; i <= 100; ++i) {
        addSection("S" + std::to_string(i), 1);
    }
    // e = 28800 / 100 = 288 → 285: one section a day, overflow included in the count.
    const PlanDraft plan = draft(request(60, 480));
    ASSERT_EQ(plan.blocks.size(), 100U);
    EXPECT_EQ(plan.blocks.back().day, 100);
}

TEST_F(StudyPlanTest, SameInputSameDraft) {
    const auto fill = [](document::test::TestWorkspace& workspace, core::NotebookId notebook) {
        addPages(workspace, notebook, "Cells", 7);
        addPages(workspace, notebook, "Genes", 3);
        const core::NotebookId chemistry = workspace.addNotebook("Chemistry");
        addPages(workspace, chemistry, "Atoms", 20);
        return PlanScope{.notebooks = {notebook, chemistry}};
    };
    const PlanScope scope = fill(t, biology);
    const PlanBuild first = build(request(5, 90, scope));
    EXPECT_EQ(first.outcome, PlanBuildOutcome::Draft);
    EXPECT_EQ(build(request(5, 90, scope)), first);

    // A second workspace built the same way, with the same deterministic ids.
    document::test::TestWorkspace other;
    const PlanScope otherScope = fill(other, other.addNotebook("Biology"));
    EXPECT_EQ(otherScope, scope);
    auto again = draftByTreeOrder(other.workspace, request(5, 90, otherScope));
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, first);
}

TEST_F(StudyPlanTest, NoPageInTwoBlocks) {
    addSection("Cells", 9);
    addSection("Genes", 1);
    addSection("Proteins", 17);
    const core::NotebookId chemistry = t.addNotebook("Chemistry");
    addSection(chemistry, "Atoms", 4);
    const core::SectionId bonds = addSection(chemistry, "Bonds", 6);
    // 37 pages in 5 sections never need more than 100 blocks.
    for (const int days : {1, 3, 7, 60}) {
        for (const int perDay : {5, 30, 45, 480}) {
            const PlanScope scope{.notebooks = {biology}, .sections = {bonds}};
            // draft() checks that every page is in one block, in tree order.
            const PlanValidation checked =
                validatePlanDraft(t.workspace, draft(request(days, perDay, scope)));
            EXPECT_EQ(kinds(checked.warnings), std::vector<PlanIssueKind>{})
                << days << " " << perDay;
            for (const PlanIssue& error : checked.errors) {
                EXPECT_EQ(error.kind, PlanIssueKind::BlockAfterLastDay) << error.message;
            }
        }
    }
}

// ---------------------------------------------------------------------- validatePlanDraft

struct StudyPlanValidationTest : StudyPlanTest {
    core::SectionId cells = addSection("Cells", 3);
    std::vector<core::PageId> pages{t.workspace.pagesOf(cells).begin(),
                                    t.workspace.pagesOf(cells).end()};

    PlanDraft valid() const {
        return {
            .title = "Biology exam",
            .start = kStart,
            .days = 2,
            .minutesPerDay = minutes{60},
            .blocks = {{.day = 1, .title = "Cells 1", .minutes = minutes{30}, .pages = {pages[0]}},
                       {.day = 2,
                        .title = "Cells 2",
                        .minutes = minutes{60},
                        .pages = {pages[1], pages[2]}}}};
    }

    std::vector<PlanIssueKind> errors(const PlanDraft& plan) const {
        return kinds(validatePlanDraft(t.workspace, plan).errors);
    }
    std::vector<PlanIssueKind> warnings(const PlanDraft& plan) const {
        return kinds(validatePlanDraft(t.workspace, plan).warnings);
    }
};

using Kinds = std::vector<PlanIssueKind>;

TEST_F(StudyPlanValidationTest, AValidDraftHasNoIssues) {
    const PlanValidation checked = validatePlanDraft(t.workspace, valid());
    EXPECT_TRUE(checked.canSave());
    EXPECT_TRUE(checked.errors.empty());
    EXPECT_TRUE(checked.warnings.empty());
    EXPECT_EQ(checked.draft, valid());

    // A draft built by draftByTreeOrder is valid as built.
    const PlanValidation built = validatePlanDraft(t.workspace, draft(request(7, 60)));
    EXPECT_TRUE(built.errors.empty());
    EXPECT_TRUE(built.warnings.empty());
}

TEST_F(StudyPlanValidationTest, DaysAreOneToSixty) {
    for (const int days : {1, 60}) {
        PlanDraft plan = valid();
        plan.days = days;
        plan.blocks.resize(1);
        EXPECT_EQ(errors(plan), Kinds{}) << days;
    }
    for (const int days : {0, 61}) {
        PlanDraft plan = valid();
        plan.days = days;
        plan.blocks.resize(1);
        EXPECT_EQ(errors(plan), Kinds{PlanIssueKind::DaysOutOfRange}) << days;
    }
}

TEST_F(StudyPlanValidationTest, DatesMustBeStorable) {
    PlanDraft plan = valid();
    plan.start = date(9999, 12, 30);
    EXPECT_EQ(errors(plan), Kinds{});
    plan.start = date(9999, 12, 31); // the second day would be in year 10000
    EXPECT_EQ(errors(plan), Kinds{PlanIssueKind::DatesNotStorable});
    plan.start = date(2026, 2, 29);
    EXPECT_EQ(errors(plan), Kinds{PlanIssueKind::DatesNotStorable});
}

TEST_F(StudyPlanValidationTest, MinutesPerDayAreFiveTo480InStepsOfFive) {
    for (const int perDay : {60, 480}) {
        PlanDraft plan = valid();
        plan.minutesPerDay = minutes{perDay};
        EXPECT_EQ(errors(plan), Kinds{}) << perDay;
    }
    for (const int perDay : {0, 4, 62, 485}) {
        PlanDraft plan = valid();
        plan.minutesPerDay = minutes{perDay};
        EXPECT_EQ(errors(plan), Kinds{PlanIssueKind::MinutesPerDayOutOfRange}) << perDay;
    }
}

TEST_F(StudyPlanValidationTest, TitlesAreNotBlankAndAtMost120Characters) {
    PlanDraft plan = valid();
    plan.title = " \t";
    plan.blocks[1].title = "";
    EXPECT_EQ(errors(plan), (Kinds{PlanIssueKind::BlankTitle, PlanIssueKind::BlankBlockTitle}));
    plan = valid();
    plan.title = "\v\f";
    plan.blocks[1].title = "\f";
    EXPECT_EQ(errors(plan), (Kinds{PlanIssueKind::BlankTitle, PlanIssueKind::BlankBlockTitle}));

    plan = valid();
    plan.title = std::string(120, 'x');
    plan.blocks[0].title = std::string(120, 'y');
    EXPECT_EQ(warnings(plan), Kinds{});
    EXPECT_EQ(validatePlanDraft(t.workspace, plan).draft, plan);

    // Cut by characters, never inside one: "é" is two bytes in UTF-8.
    std::string accented;
    for (int i = 0; i < 121; ++i) {
        accented += "\xC3\xA9";
    }
    plan.title = std::string(121, 'x');
    plan.blocks[0].title = accented;
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.warnings),
              (Kinds{PlanIssueKind::TitleCut, PlanIssueKind::BlockTitleCut}));
    EXPECT_EQ(checked.warnings[1].block, 0U);
    EXPECT_TRUE(checked.canSave());
    EXPECT_EQ(checked.draft.title, std::string(120, 'x'));
    EXPECT_EQ(checked.draft.blocks[0].title, accented.substr(0, 240));
    EXPECT_EQ(checked.draft.blocks[1].title, "Cells 2");
}

TEST_F(StudyPlanValidationTest, ObjectivesAreAtMost500Characters) {
    PlanDraft plan = valid();
    plan.blocks[0].objective = std::string(500, 'o');
    EXPECT_EQ(warnings(plan), Kinds{});
    plan.blocks[1].objective = std::string(501, 'o');
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.warnings), Kinds{PlanIssueKind::ObjectiveCut});
    EXPECT_EQ(checked.warnings[0].block, 1U);
    EXPECT_TRUE(checked.canSave());
    EXPECT_EQ(checked.draft.blocks[1].objective, std::string(500, 'o'));
}

TEST_F(StudyPlanValidationTest, BlockMinutesAreFiveTo480) {
    PlanDraft plan = valid();
    plan.minutesPerDay = minutes{480};
    plan.blocks[0].minutes = minutes{5};
    plan.blocks[1].minutes = minutes{480};
    EXPECT_EQ(errors(plan), Kinds{});
    plan.blocks[0].minutes = minutes{4};
    plan.blocks[1].minutes = minutes{481};
    EXPECT_EQ(errors(plan), (Kinds{PlanIssueKind::BlockMinutesOutOfRange,
                                   PlanIssueKind::BlockMinutesOutOfRange}));
}

TEST_F(StudyPlanValidationTest, BlockDaysAreWithinThePlanOrUnscheduled) {
    PlanDraft plan = valid();
    plan.blocks[1].day.reset(); // unscheduled
    EXPECT_EQ(errors(plan), Kinds{});
    plan.blocks[0].day = 0;
    plan.blocks[1].day = 3;
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.errors),
              (Kinds{PlanIssueKind::BlockDayOutOfRange, PlanIssueKind::BlockAfterLastDay}));
    EXPECT_EQ(checked.errors[0].block, 0U);
    EXPECT_EQ(checked.errors[1].block, 1U);
}

TEST_F(StudyPlanValidationTest, AtMostTwelveBlocksADay) {
    PlanDraft plan = valid();
    plan.minutesPerDay = minutes{480};
    plan.blocks.assign(12, PlanBlock{.day = 1, .title = "Review", .minutes = minutes{5}});
    plan.blocks.push_back(PlanBlock{.day = 2, .title = "Review", .minutes = minutes{5}});
    EXPECT_EQ(errors(plan), Kinds{});
    plan.blocks.back().day = 1;
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.errors), Kinds{PlanIssueKind::TooManyBlocksOnDay});
    EXPECT_EQ(checked.errors[0].day, 1);
}

TEST_F(StudyPlanValidationTest, AtMost100Blocks) {
    PlanDraft plan = valid();
    plan.days = 60;
    plan.blocks.clear();
    for (int i = 0; i < 100; ++i) {
        plan.blocks.push_back({.day = i / 12 + 1, .title = "Review", .minutes = minutes{5}});
    }
    EXPECT_EQ(errors(plan), Kinds{});
    plan.blocks.push_back({.day = 60, .title = "Review", .minutes = minutes{5}});
    EXPECT_EQ(errors(plan), Kinds{PlanIssueKind::TooManyBlocks});
}

TEST_F(StudyPlanValidationTest, EveryPageExists) {
    PlanDraft plan = valid();
    EXPECT_EQ(errors(plan), Kinds{});
    t.run(commands::deletePage(t.workspace, pages[1]));
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.errors), Kinds{PlanIssueKind::MissingPage});
    EXPECT_EQ(checked.errors[0].block, 1U);
    EXPECT_EQ(checked.errors[0].page, pages[1]);
    EXPECT_EQ(checked.draft.blocks[1].pages, plan.blocks[1].pages); // reported, not dropped
}

TEST_F(StudyPlanValidationTest, NoPageInTwoBlocks) {
    PlanDraft plan = valid();
    plan.blocks[1].pages.push_back(pages[0]);
    PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.errors), Kinds{PlanIssueKind::RepeatedPage});
    EXPECT_EQ(checked.errors[0].block, 1U);
    EXPECT_EQ(checked.errors[0].page, pages[0]);

    plan = valid();
    plan.blocks[0].pages.push_back(pages[0]); // twice in one block
    EXPECT_EQ(errors(plan), Kinds{PlanIssueKind::RepeatedPage});
}

TEST_F(StudyPlanValidationTest, ADayOverItsMinutesIsAWarning) {
    PlanDraft plan = valid();
    plan.blocks[0].minutes = minutes{60};
    EXPECT_EQ(warnings(plan), Kinds{});
    plan.blocks[0].minutes = minutes{65};
    const PlanValidation checked = validatePlanDraft(t.workspace, plan);
    EXPECT_EQ(kinds(checked.warnings), Kinds{PlanIssueKind::DayOverMinutes});
    EXPECT_EQ(checked.warnings[0].day, 1);
    EXPECT_TRUE(checked.canSave());
    EXPECT_EQ(checked.draft.blocks[0].minutes, minutes{65}); // kept as entered
}

// ---------------------------------------------------------------------- PlanPreview

/// The error code of a refused edit; Internal if it was not refused.
core::ErrorCode refused(const core::Result<void>& result) {
    return result ? core::ErrorCode::Internal : result.error().code;
}

struct StudyPlanPreviewTest : StudyPlanTest {
    PlanPreview preview(const PlanRequest& planRequest) const {
        return {planRequest, draft(planRequest)};
    }

    /// Acceptance 5 while the workspace is unchanged: every page of the scope is in exactly
    /// one block or under "Excluded pages".
    void expectEveryPageOnce(const PlanPreview& plan) const {
        std::vector<core::PageId> pages = plan.excludedPages();
        for (const auto* blocks : {&plan.draft().blocks, &plan.removedBlocks()}) {
            for (const PlanBlock& block : *blocks) {
                pages.insert(pages.end(), block.pages.begin(), block.pages.end());
            }
        }
        std::vector<core::PageId> scope = pagesInScope(t.workspace, plan.request().scope);
        std::ranges::sort(pages);
        std::ranges::sort(scope);
        EXPECT_EQ(pages, scope);
    }

    std::vector<PlanHold> holds(const PlanPreview& plan) const {
        return plan.review(t.workspace).holds;
    }

    core::PageId page(core::SectionId section, std::size_t index) const {
        return t.workspace.pagesOf(section)[index];
    }
};

using Holds = std::vector<PlanHold>;
using References = std::vector<PlanReference>;
using Pages = std::vector<core::PageId>;

TEST_F(StudyPlanPreviewTest, OverflowIsSummarised) {
    addSection("A", 4);
    // One 5-minute page a day on days 1 … 4 of a 1-day plan (OverflowBySeveralDays).
    PlanPreview plan = preview(request(1, 5));
    EXPECT_EQ(
        plan.overflow(),
        (PlanOverflow{
            .blocks = {1, 2, 3}, .daysNeeded = 4, .minutesOver = minutes{15}, .canExtend = true}));
    const PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.holds, Holds{PlanHold::Overflow});
    EXPECT_FALSE(review.canAccept());
    // The overflow blocks keep the days they need; nothing is moved onto day 1.
    EXPECT_EQ(layout(plan.draft()), layout(draft(request(1, 5))));

    PlanPreview fits = preview(request(4, 5));
    EXPECT_FALSE(fits.overflow().has_value());
    EXPECT_TRUE(fits.review(t.workspace).canAccept());
}

TEST_F(StudyPlanPreviewTest, ExtendToNeededDays) {
    addSection("A", 2);
    addSection("B", 2);
    addSection("C", 2);
    // C needs day 3 of a 2-day plan (OverflowByOneBlock). (a) keeps the placement.
    PlanPreview plan = preview(request(2, 60));
    const std::vector<Placed> placed = layout(plan.draft());
    ASSERT_OK(plan.extendToNeededDays());
    EXPECT_EQ(plan.draft().days, 3);
    EXPECT_EQ(plan.request().days, 3);
    EXPECT_EQ(planDate(plan.draft().start, plan.draft().days), date(2026, 10, 21));
    EXPECT_EQ(layout(plan.draft()), placed);
    EXPECT_FALSE(plan.overflow().has_value());
    EXPECT_EQ(holds(plan), Holds{});
    expectEveryPageOnce(plan);

    const auto again = plan.extendToNeededDays();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, core::ErrorCode::Conflict);
    EXPECT_EQ(plan.draft().days, 3);
}

TEST_F(StudyPlanPreviewTest, ExtendNotOfferedBeyond60) {
    for (int i = 1; i <= 61; ++i) {
        addSection("S" + std::to_string(i), 1);
    }
    // e = 5: one section a day. 60 days requested, 61 needed: (a) is not offered.
    PlanPreview plan = preview(request(60, 5));
    ASSERT_TRUE(plan.overflow().has_value());
    EXPECT_EQ(plan.overflow()->daysNeeded, 61);
    EXPECT_FALSE(plan.overflow()->canExtend);
    const PlanDraft before = plan.draft();
    const auto outcome = plan.extendToNeededDays();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, core::ErrorCode::Conflict);
    EXPECT_EQ(plan.draft(), before);
    EXPECT_EQ(plan.request().days, 60);

    // 59 days requested, 60 needed: offered.
    PlanScope first60;
    for (std::size_t i = 0; i < 60; ++i) {
        first60.sections.push_back(t.workspace.sectionsOf(biology)[i]);
    }
    PlanPreview sixty = preview(request(59, 5, first60));
    ASSERT_TRUE(sixty.overflow().has_value());
    EXPECT_TRUE(sixty.overflow()->canExtend);
    ASSERT_OK(sixty.extendToNeededDays());
    EXPECT_EQ(sixty.draft().days, 60);
}

TEST_F(StudyPlanPreviewTest, ExtendNotOfferedPastTheLastStorableDate) {
    addSection("A", 2);
    addSection("B", 2);
    addSection("C", 2);
    PlanRequest planRequest = request(2, 60);
    planRequest.start = date(9999, 12, 30);
    // Day 3 would be 10000-01-01.
    PlanPreview plan = preview(planRequest);
    ASSERT_TRUE(plan.overflow().has_value());
    EXPECT_EQ(plan.overflow()->daysNeeded, 3);
    EXPECT_FALSE(plan.overflow()->canExtend);
    EXPECT_FALSE(plan.extendToNeededDays().has_value());
    EXPECT_EQ(plan.draft().days, 2);
}

TEST_F(StudyPlanPreviewTest, KeepOverflowUnscheduled) {
    addSection("A", 2);
    addSection("B", 2);
    addSection("C", 2);
    PlanPreview plan = preview(request(2, 60));
    ASSERT_OK(plan.keepOverflowUnscheduled());
    // (c): C has no day; A and B keep theirs, and the plan keeps its 2 days.
    EXPECT_EQ(layout(plan.draft()),
              (std::vector<Placed>{{1, 40, 2, "A"}, {2, 40, 2, "B"}, {0, 40, 2, "C"}}));
    EXPECT_EQ(plan.draft().days, 2);
    EXPECT_FALSE(plan.overflow().has_value());
    const PlanReview review = plan.review(t.workspace);
    EXPECT_TRUE(review.canAccept());
    EXPECT_TRUE(review.validation.errors.empty());
    expectEveryPageOnce(plan);
    EXPECT_FALSE(plan.keepOverflowUnscheduled().has_value());

    // Undone by giving the block a day: an ordinary block, here over day 1's minutes.
    ASSERT_OK(plan.moveBlock(2, 1));
    EXPECT_EQ(layout(plan.draft()),
              (std::vector<Placed>{{1, 40, 2, "A"}, {1, 40, 2, "C"}, {2, 40, 2, "B"}}));
    EXPECT_EQ(kinds(plan.review(t.workspace).validation.warnings),
              std::vector{PlanIssueKind::DayOverMinutes});
    EXPECT_EQ(holds(plan), Holds{});
}

TEST_F(StudyPlanPreviewTest, RemoveOverflowToExcluded) {
    const core::SectionId a = addSection("A", 4);
    PlanPreview plan = preview(request(1, 5));
    ASSERT_OK(plan.removeOverflow());
    // (d): the overflow blocks are removed, in placement order; their pages are excluded.
    EXPECT_EQ(layout(plan.draft()), (std::vector<Placed>{{1, 5, 1, "A (part 1 of 4)"}}));
    ASSERT_EQ(plan.removedBlocks().size(), 3U);
    EXPECT_EQ(plan.removedBlocks()[0].title, "A (part 2 of 4)");
    EXPECT_EQ(plan.removedBlocks()[2].title, "A (part 4 of 4)");
    const PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.excluded, (Pages{page(a, 1), page(a, 2), page(a, 3)}));
    EXPECT_TRUE(review.notInPlan.empty());
    EXPECT_TRUE(review.canAccept());
    expectEveryPageOnce(plan);
    EXPECT_FALSE(plan.removeOverflow().has_value());

    // Restoring one brings the overflow back.
    ASSERT_OK(plan.restoreBlock(0));
    EXPECT_EQ(layout(plan.draft())[1], (Placed{2, 5, 1, "A (part 2 of 4)"}));
    EXPECT_EQ(holds(plan), Holds{PlanHold::Overflow});
}

TEST_F(StudyPlanPreviewTest, RemoveAndRestoreBlock) {
    const core::SectionId a = addSection("A", 1);
    const core::SectionId b = addSection("B", 2);
    addSection("C", 3);
    // {1, 20, A}, {1, 40, B}, {2, 60, C} (SectionExactlyFillsTheRest).
    PlanPreview plan = preview(request(2, 60));
    const PlanDraft built = plan.draft();

    ASSERT_OK(plan.removeBlock(1));
    EXPECT_EQ(layout(plan.draft()), (std::vector<Placed>{{1, 20, 1, "A"}, {2, 60, 3, "C"}}));
    ASSERT_EQ(plan.removedBlocks().size(), 1U);
    EXPECT_EQ(plan.review(t.workspace).excluded, (Pages{page(b, 0), page(b, 1)}));
    EXPECT_EQ(holds(plan), Holds{});
    expectEveryPageOnce(plan);

    // Restored in its old place, also when removed blocks come back in another order.
    ASSERT_OK(plan.restoreBlock(0));
    EXPECT_EQ(plan.draft(), built);
    ASSERT_OK(plan.removeBlock(0));
    ASSERT_OK(plan.removeBlock(0));
    EXPECT_EQ(plan.review(t.workspace).excluded, (Pages{page(a, 0), page(b, 0), page(b, 1)}));
    ASSERT_OK(plan.restoreBlock(1));
    ASSERT_OK(plan.restoreBlock(0));
    EXPECT_EQ(plan.draft(), built);
    EXPECT_TRUE(plan.removedBlocks().empty());
    EXPECT_TRUE(plan.review(t.workspace).excluded.empty());

    EXPECT_EQ(refused(plan.removeBlock(3)), core::ErrorCode::NotFound);
    EXPECT_EQ(refused(plan.restoreBlock(0)), core::ErrorCode::NotFound);
    EXPECT_EQ(plan.draft(), built);
}

TEST_F(StudyPlanPreviewTest, DayOverMinutesIsAWarning) {
    addSection("A", 1);
    addSection("B", 2);
    addSection("C", 3);
    PlanPreview plan = preview(request(2, 60));
    ASSERT_OK(plan.moveBlock(2, 1));
    // Day 1: 20 + 40 + 60 minutes of 60. A warning; Accept may save it as entered.
    const PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(kinds(review.validation.warnings), std::vector{PlanIssueKind::DayOverMinutes});
    EXPECT_EQ(review.validation.warnings.front().day, 1);
    EXPECT_TRUE(review.canAccept());
    EXPECT_EQ(review.validation.draft.blocks[2].minutes, minutes{60});
}

TEST_F(StudyPlanPreviewTest, MoveOntoAFullDayIsRefused) {
    for (int i = 1; i <= 13; ++i) {
        addSection("S" + std::to_string(i), 1);
    }
    addSection("Z", 12);
    // Day 1 holds S1 … S12; S13 and Z are on day 2 (ThirteenthWholeBlockGoesToTheNextDay).
    PlanPreview plan = preview(request(2, 480));
    const PlanDraft built = plan.draft();
    const auto outcome = plan.moveBlock(12, 1);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, core::ErrorCode::Conflict);
    EXPECT_EQ(outcome.error().message, "Day 1 already has 12 blocks.");
    EXPECT_EQ(plan.draft(), built);
    for (const int day : {0, 3}) {
        EXPECT_EQ(refused(plan.moveBlock(12, day)), core::ErrorCode::InvalidArgument) << day;
    }
    EXPECT_EQ(refused(plan.moveBlock(14, 2)), core::ErrorCode::NotFound);
    ASSERT_OK(plan.moveBlock(0, 1)); // its own day: nothing changes
    EXPECT_EQ(plan.draft(), built);

    // A block moved to a day goes after that day's blocks (Q15).
    ASSERT_OK(plan.moveBlock(0, 2));
    EXPECT_EQ(layout(plan.draft()).back(), (Placed{2, 35, 1, "S1"}));
    // Restoring onto a full day is refused too.
    ASSERT_OK(plan.removeBlock(0)); // S2, day 1: 10 blocks left there
    ASSERT_OK(plan.moveBlock(plan.draft().blocks.size() - 1, 1));
    ASSERT_OK(plan.moveBlock(plan.draft().blocks.size() - 2, 1)); // S13
    EXPECT_EQ(refused(plan.restoreBlock(0)), core::ErrorCode::Conflict);
    EXPECT_EQ(plan.removedBlocks().size(), 1U);
    expectEveryPageOnce(plan);
}

TEST_F(StudyPlanPreviewTest, BlocksAreInSavingOrder) {
    addSection("A", 4);
    // Days 1 … 4 of 1; (c) keeps parts 2 … 4 unscheduled, after the scheduled blocks.
    PlanPreview plan = preview(request(1, 5));
    ASSERT_OK(plan.keepOverflowUnscheduled());
    ASSERT_OK(plan.moveBlock(2, 1)); // part 3
    EXPECT_EQ(layout(plan.draft()), (std::vector<Placed>{{1, 5, 1, "A (part 1 of 4)"},
                                                         {1, 5, 1, "A (part 3 of 4)"},
                                                         {0, 5, 1, "A (part 2 of 4)"},
                                                         {0, 5, 1, "A (part 4 of 4)"}}));

    // A draft given in any order is put in saving order.
    PlanDraft unordered = plan.draft();
    std::ranges::reverse(unordered.blocks);
    EXPECT_EQ(layout(PlanPreview(plan.request(), unordered).draft()),
              (std::vector<Placed>{{1, 5, 1, "A (part 3 of 4)"},
                                   {1, 5, 1, "A (part 1 of 4)"},
                                   {0, 5, 1, "A (part 4 of 4)"},
                                   {0, 5, 1, "A (part 2 of 4)"}}));
}

TEST_F(StudyPlanPreviewTest, BlockMinutesAndTitles) {
    addSection("A", 1);
    addSection("B", 2);
    addSection("C", 3);
    PlanPreview plan = preview(request(2, 60));
    for (const int value : {5, 480, 45}) {
        ASSERT_OK(plan.setBlockMinutes(0, minutes{value}));
        EXPECT_EQ(plan.draft().blocks[0].minutes, minutes{value});
    }
    for (const int value : {0, 4, 47, 485}) {
        EXPECT_EQ(refused(plan.setBlockMinutes(0, minutes{value})),
                  core::ErrorCode::InvalidArgument)
            << value;
    }
    EXPECT_EQ(plan.draft().blocks[0].minutes, minutes{45});

    ASSERT_OK(plan.setBlockTitle(1, " "));
    const PlanReview blank = plan.review(t.workspace);
    EXPECT_EQ(kinds(blank.validation.errors), std::vector{PlanIssueKind::BlankBlockTitle});
    EXPECT_EQ(blank.holds, Holds{PlanHold::InvalidDraft});
    ASSERT_OK(plan.setBlockTitle(1, "Genes"));
    EXPECT_EQ(plan.draft().blocks[1].title, "Genes");
    EXPECT_EQ(holds(plan), Holds{});

    EXPECT_EQ(refused(plan.setBlockMinutes(3, minutes{30})), core::ErrorCode::NotFound);
    EXPECT_EQ(refused(plan.setBlockTitle(3, "X")), core::ErrorCode::NotFound);
}

TEST_F(StudyPlanPreviewTest, PagesComeOnlyFromExcludedPages) {
    const core::SectionId a = addSection("A", 1);
    const core::SectionId b = addSection("B", 2);
    const core::SectionId c = addSection("C", 3);
    PlanPreview plan = preview(request(2, 60));

    // Taken out of B, minutes unchanged; then added to C, at the end.
    ASSERT_OK(plan.excludePage(t.workspace, page(b, 0)));
    EXPECT_EQ(plan.draft().blocks[1].pages, Pages{page(b, 1)});
    EXPECT_EQ(plan.draft().blocks[1].minutes, minutes{40});
    EXPECT_EQ(plan.excludedPages(), Pages{page(b, 0)});
    expectEveryPageOnce(plan);
    EXPECT_EQ(refused(plan.excludePage(t.workspace, page(b, 0))), core::ErrorCode::Conflict);
    ASSERT_OK(plan.addPage(2, page(b, 0)));
    EXPECT_EQ(plan.draft().blocks[2].pages,
              (Pages{page(c, 0), page(c, 1), page(c, 2), page(b, 0)}));
    EXPECT_TRUE(plan.excludedPages().empty());
    EXPECT_EQ(refused(plan.addPage(0, page(b, 0))), core::ErrorCode::NotFound);

    // A page of a removed block can be added to another block.
    ASSERT_OK(plan.removeBlock(0)); // A
    ASSERT_OK(plan.addPage(0, page(a, 0)));
    EXPECT_EQ(plan.draft().blocks[0].pages, (Pages{page(b, 1), page(a, 0)}));
    EXPECT_TRUE(plan.removedBlocks()[0].pages.empty());
    expectEveryPageOnce(plan);

    // A block without pages is allowed.
    ASSERT_OK(plan.excludePage(t.workspace, page(b, 1)));
    ASSERT_OK(plan.excludePage(t.workspace, page(a, 0)));
    EXPECT_TRUE(plan.draft().blocks[0].pages.empty());
    EXPECT_EQ(holds(plan), Holds{});
    expectEveryPageOnce(plan);

    // Nothing from outside the scope.
    const core::SectionId other = addPages(t, t.addNotebook("Chemistry"), "Atoms", 1);
    EXPECT_EQ(refused(plan.excludePage(t.workspace, page(other, 0))), core::ErrorCode::NotFound);
    EXPECT_EQ(refused(plan.addPage(0, page(other, 0))), core::ErrorCode::NotFound);
    EXPECT_EQ(refused(plan.addPage(5, page(b, 1))), core::ErrorCode::NotFound);
}

TEST_F(StudyPlanPreviewTest, GoalAndStartChangeTitlesAndDates) {
    addSection("A", 2);
    PlanPreview plan = preview(request(2, 60));
    const std::vector<Placed> placed = layout(plan.draft());
    plan.setGoal(" Physics\t");
    EXPECT_EQ(plan.draft().title, "Physics");
    EXPECT_EQ(plan.request().goal, " Physics\t");
    ASSERT_OK(plan.setStart(date(2026, 12, 31)));
    EXPECT_EQ(plan.draft().start, date(2026, 12, 31));
    EXPECT_EQ(plan.request().start, date(2026, 12, 31));
    EXPECT_EQ(planDate(plan.draft().start, 2), date(2027, 1, 1));
    // The last day, 10000-01-01, would not be storable.
    EXPECT_EQ(refused(plan.setStart(date(9999, 12, 31))), core::ErrorCode::InvalidArgument);
    EXPECT_EQ(plan.draft().start, date(2026, 12, 31));
    EXPECT_EQ(layout(plan.draft()), placed);

    plan.setGoal("  ");
    EXPECT_EQ(holds(plan), Holds{PlanHold::InvalidDraft});
}

// ---- the workspace changes after the build (rule 7)

TEST_F(StudyPlanPreviewTest, DeletedPageIsInvalid) {
    addSection("A", 1);
    const core::SectionId b = addSection("B", 2);
    addSection("C", 3);
    PlanPreview plan = preview(request(2, 60));
    const core::PageId deleted = page(b, 0);
    t.run(commands::deletePage(t.workspace, deleted));

    const PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.invalid, (References{{1, deleted}}));
    EXPECT_EQ(review.holds, Holds{PlanHold::InvalidReferences});
    EXPECT_EQ(kinds(review.validation.errors), std::vector{PlanIssueKind::MissingPage});

    // Removing the reference drops the page: it no longer exists to be excluded.
    ASSERT_OK(plan.excludePage(t.workspace, deleted));
    EXPECT_EQ(plan.draft().blocks[1].pages, Pages{page(b, 0)});
    EXPECT_TRUE(plan.excludedPages().empty());
    EXPECT_EQ(holds(plan), Holds{});

    // Undo brings the page back, in the scope but not in the plan (Q17).
    ASSERT_OK(t.editor.undo());
    const PlanReview back = plan.review(t.workspace);
    EXPECT_EQ(back.notInPlan, Pages{deleted});
    EXPECT_TRUE(back.canAccept());
    ASSERT_OK(plan.excludePage(t.workspace, deleted));
    EXPECT_EQ(plan.excludedPages(), Pages{deleted});
    EXPECT_TRUE(plan.review(t.workspace).notInPlan.empty());
    expectEveryPageOnce(plan);
}

TEST_F(StudyPlanPreviewTest, RestoredPageIsValidAgain) {
    const core::SectionId a = addSection("A", 1);
    PlanPreview plan = preview(request(2, 60));
    const PlanDraft built = plan.draft();
    t.run(commands::deletePage(t.workspace, page(a, 0)));
    EXPECT_EQ(holds(plan), Holds{PlanHold::InvalidReferences});
    ASSERT_OK(t.editor.undo());
    const PlanReview review = plan.review(t.workspace);
    EXPECT_TRUE(review.invalid.empty());
    EXPECT_TRUE(review.notInPlan.empty());
    EXPECT_TRUE(review.canAccept());
    EXPECT_EQ(plan.draft(), built);
}

TEST_F(StudyPlanPreviewTest, MovedOutOfScopeIsChanged) {
    addSection("A", 1);
    const core::SectionId b = addSection("B", 2);
    addSection("C", 3);
    const core::SectionId atoms = addPages(t, t.addNotebook("Chemistry"), "Atoms", 0);
    PlanPreview plan = preview(request(2, 60));
    const core::PageId moved = page(b, 1);
    t.run(commands::movePage(t.workspace, moved, atoms, 0, t.clock));

    PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.changed, (References{{1, moved}}));
    EXPECT_TRUE(review.invalid.empty());
    EXPECT_EQ(review.holds, Holds{PlanHold::ChangedReferences});

    // Kept by the user's choice: named as kept, no longer a hold (Q16).
    ASSERT_OK(plan.keepPage(t.workspace, moved));
    review = plan.review(t.workspace);
    EXPECT_TRUE(review.changed.empty());
    EXPECT_EQ(review.kept, (References{{1, moved}}));
    EXPECT_TRUE(review.canAccept());

    // Back in the scope, it is an ordinary reference again.
    ASSERT_OK(t.editor.undo());
    review = plan.review(t.workspace);
    EXPECT_TRUE(review.changed.empty());
    EXPECT_TRUE(review.kept.empty());

    EXPECT_EQ(refused(plan.keepPage(t.workspace, moved)), core::ErrorCode::InvalidArgument);
    EXPECT_EQ(refused(plan.keepPage(t.workspace, core::PageId{t.ids.next()})),
              core::ErrorCode::NotFound);
}

TEST_F(StudyPlanPreviewTest, ResolutionsChangeOnlyNamedReferences) {
    addSection("A", 1);
    const core::SectionId b = addSection("B", 2);
    const core::SectionId c = addSection("C", 3);
    const core::SectionId atoms = addPages(t, t.addNotebook("Chemistry"), "Atoms", 0);
    PlanPreview plan = preview(request(2, 60));
    const core::PageId keep = page(b, 0);
    const core::PageId exclude = page(c, 0);
    const core::PageId deleted = page(c, 1);
    t.run(commands::movePage(t.workspace, keep, atoms, 0, t.clock));
    t.run(commands::movePage(t.workspace, exclude, atoms, 1, t.clock));
    t.run(commands::deletePage(t.workspace, deleted));

    PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.changed, (References{{1, keep}, {2, exclude}}));
    EXPECT_EQ(review.invalid, (References{{2, deleted}}));
    EXPECT_EQ(review.holds, (Holds{PlanHold::InvalidReferences, PlanHold::ChangedReferences}));

    ASSERT_OK(plan.keepPage(t.workspace, keep));
    review = plan.review(t.workspace);
    EXPECT_EQ(review.kept, (References{{1, keep}}));
    EXPECT_EQ(review.changed, (References{{2, exclude}}));
    EXPECT_EQ(review.invalid, (References{{2, deleted}}));
    EXPECT_EQ(plan.draft().blocks[2].pages, (Pages{exclude, deleted, page(c, 0)}));

    ASSERT_OK(plan.excludePage(t.workspace, exclude));
    review = plan.review(t.workspace);
    EXPECT_EQ(review.excluded, Pages{exclude}); // it still exists, outside the scope
    EXPECT_EQ(review.kept, (References{{1, keep}}));
    EXPECT_EQ(review.invalid, (References{{2, deleted}}));
    EXPECT_EQ(review.holds, Holds{PlanHold::InvalidReferences});

    ASSERT_OK(plan.excludePage(t.workspace, deleted));
    review = plan.review(t.workspace);
    EXPECT_TRUE(review.canAccept());
    EXPECT_EQ(plan.draft().blocks[0].pages.size(), 1U);
    EXPECT_EQ(plan.draft().blocks[1].pages, (Pages{keep, page(b, 0)}));
    EXPECT_EQ(plan.draft().blocks[2].pages, Pages{page(c, 0)});
}

TEST_F(StudyPlanPreviewTest, NewPagesInScopeDoNotHoldAccept) {
    const core::SectionId a = addSection("A", 1);
    PlanPreview plan = preview(request(2, 60));
    const core::PageId added = t.addPage(a, "A 2");
    const PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.notInPlan, Pages{added});
    EXPECT_TRUE(review.canAccept());
    ASSERT_OK(plan.excludePage(t.workspace, added));
    EXPECT_TRUE(plan.review(t.workspace).notInPlan.empty());
    EXPECT_EQ(plan.review(t.workspace).excluded, Pages{added});
}

TEST_F(StudyPlanPreviewTest, NoBlocksHoldsAccept) {
    addSection("A", 1);
    addSection("B", 1);
    PlanPreview plan = preview(request(2, 60)); // A on day 1, B on day 2
    ASSERT_OK(plan.removeBlock(0));
    EXPECT_EQ(holds(plan), Holds{});
    ASSERT_OK(plan.removeBlock(0));
    // Every block removed: "The plan has no study blocks" (the user's decision, 2026-10-20).
    EXPECT_TRUE(plan.draft().blocks.empty());
    const PlanReview review = plan.review(t.workspace);
    EXPECT_EQ(review.holds, Holds{PlanHold::NoBlocks});
    EXPECT_FALSE(review.canAccept());
    expectEveryPageOnce(plan);

    ASSERT_OK(plan.restoreBlock(1));
    EXPECT_EQ(holds(plan), Holds{});
}

} // namespace
} // namespace studyapp::application
