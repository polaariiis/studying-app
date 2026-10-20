#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Workspace.hpp>
#include <studyapp/study/Records.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace studyapp::application {

// A study plan made from a chosen scope, without AI (2.0-PLAN-03; docs/VERSION_2_0.md §5,
// limits in §7.5). Pure values and functions: they read a Workspace and never change it.
// Drafts hold ids only, never pointers into the workspace, so that they stay valid while
// the workspace changes; validatePlanDraft checks them against the workspace as it is.

inline constexpr int kMaxPlanDays = 60;
inline constexpr std::chrono::minutes kMinPlanMinutes{5};   ///< per day and per block
inline constexpr std::chrono::minutes kMaxPlanMinutes{480}; ///< per day and per block
inline constexpr std::chrono::minutes kPlanMinuteStep{5};   ///< minutes per day
inline constexpr std::size_t kMaxBlocksPerDay = 12;
inline constexpr std::size_t kMaxPlanBlocks = 100;
inline constexpr std::size_t kMaxPlanTitleLength = 120; ///< characters (code points)
inline constexpr std::size_t kMaxPlanObjectiveLength = 500;

/// The notebooks and sections chosen for a plan (independent choices: checking a notebook
/// does not check its sections, but every page of a chosen notebook is in scope). A page is
/// in scope when its section or that section's notebook is chosen. Ids that no longer exist
/// choose nothing.
struct PlanScope {
    std::vector<core::NotebookId> notebooks;
    std::vector<core::SectionId> sections;

    [[nodiscard]] friend bool operator==(const PlanScope&, const PlanScope&) = default;
};

struct PlanRequest {
    std::string goal; ///< the project's description; trimmed, its title
    study::CalendarDate start{};
    int days = 7;                           ///< 1 … kMaxPlanDays
    std::chrono::minutes minutesPerDay{60}; ///< 5 … 480, a multiple of 5
    PlanScope scope;

    [[nodiscard]] friend bool operator==(const PlanRequest&, const PlanRequest&) = default;
};

/// One block of study: pages of one section on one day, saved as one task.
struct PlanBlock {
    /// 1 … days; a day after the plan's last day is overflow. Absent: unscheduled, saved
    /// without a due date.
    std::optional<int> day;
    std::string title;
    std::string objective; ///< the task's notes; empty from draftByTreeOrder
    std::chrono::minutes minutes{0};
    std::vector<core::PageId> pages; ///< in page order

    [[nodiscard]] friend bool operator==(const PlanBlock&, const PlanBlock&) = default;
};

struct PlanDraft {
    std::string title; ///< the project's title
    study::CalendarDate start{};
    int days = 0;
    std::chrono::minutes minutesPerDay{0};
    std::vector<PlanBlock> blocks; ///< in placement order

    [[nodiscard]] friend bool operator==(const PlanDraft&, const PlanDraft&) = default;
};

/// A chosen section that has pages, with its page count.
struct ScopeSection {
    core::SectionId section;
    std::size_t pages = 0;

    [[nodiscard]] friend bool operator==(const ScopeSection&, const ScopeSection&) = default;
};

enum class PlanBuildOutcome : std::uint8_t {
    Draft,        ///< `draft` holds the plan
    NoPages,      ///< "No pages in the chosen scope"
    TooManyBlocks ///< more than kMaxPlanBlocks blocks needed; no partial plan
};

struct PlanBuild {
    PlanBuildOutcome outcome = PlanBuildOutcome::NoPages;
    std::optional<PlanDraft> draft;
    std::size_t blocksNeeded = 0;       ///< overflow blocks included
    std::vector<ScopeSection> sections; ///< the scope in tree order

    [[nodiscard]] friend bool operator==(const PlanBuild&, const PlanBuild&) = default;
};

/// The pages in scope in tree order (notebook, section, page), each once. O(sections +
/// pages in scope).
[[nodiscard]] std::vector<core::PageId> pagesInScope(const document::Workspace& workspace,
                                                     const PlanScope& scope);

/// The date of plan day `day` (1-based): start + day − 1, with no time zone. Requires
/// start.ok().
[[nodiscard]] study::CalendarDate planDate(study::CalendarDate start, int day);

/// Spreads the pages in scope over the days in tree order (distribution rules 1–5 of
/// docs/VERSION_2_0.md §5): e = days × minutes per day ÷ pages, rounded down to a multiple
/// of 5 and kept within 5 … minutes per day, for every page; a section that fits in a day
/// is one block, a larger one is split into "Section (part i of n)"; a page is never split;
/// a day takes at most kMaxBlocksPerDay blocks; time left on a day that no further page
/// fits in stays unused. Work beyond the last day continues on the following days
/// (overflow blocks); no page is dropped. The draft's title is the trimmed goal, uncut
/// (validatePlanDraft cuts it). Deterministic. O(sections + pages in scope).
///
/// Errors: InvalidArgument if days, minutes per day, the start date or the last day
/// (start + days − 1) is out of range or not storable.
[[nodiscard]] core::Result<PlanBuild> draftByTreeOrder(const document::Workspace& workspace,
                                                       const PlanRequest& request);

enum class PlanIssueKind : std::uint8_t {
    // Errors: the draft cannot be saved.
    DaysOutOfRange,          ///< the plan's days are not 1 … kMaxPlanDays
    MinutesPerDayOutOfRange, ///< not 5 … 480 or not a multiple of 5
    DatesNotStorable,        ///< the start or the last day is not a storable date
    BlankTitle,              ///< the plan's title
    BlankBlockTitle,
    BlockDayOutOfRange,     ///< a day before day 1
    BlockAfterLastDay,      ///< overflow: a day after the plan's last day
    BlockMinutesOutOfRange, ///< not 5 … 480
    TooManyBlocksOnDay,     ///< more than kMaxBlocksPerDay
    TooManyBlocks,          ///< more than kMaxPlanBlocks
    MissingPage,            ///< the page no longer exists
    RepeatedPage,           ///< the page is in more than one block
    // Warnings: shown, the draft can still be saved.
    TitleCut, ///< the plan's title was cut to kMaxPlanTitleLength
    BlockTitleCut,
    ObjectiveCut,  ///< cut to kMaxPlanObjectiveLength
    DayOverMinutes ///< a day's blocks add up to more than minutes per day
};

struct PlanIssue {
    PlanIssueKind kind{};
    std::optional<std::size_t> block; ///< index into the draft's blocks
    std::optional<int> day;
    std::optional<core::PageId> page;
    std::string message;

    [[nodiscard]] friend bool operator==(const PlanIssue&, const PlanIssue&) = default;
};

struct PlanValidation {
    PlanDraft draft; ///< the validated draft, its titles and objectives cut to their limits
    std::vector<PlanIssue> errors;
    std::vector<PlanIssue> warnings;

    [[nodiscard]] bool canSave() const noexcept { return errors.empty(); }
};

/// Checks `draft` against the limits of docs/VERSION_2_0.md §7.5 and the workspace as it is
/// now: days 1 … 60 and storable dates; minutes per day 5 … 480 in steps of 5; every block
/// titled, on a day from 1 to the last day (or unscheduled), 5 … 480 minutes; at most 12
/// blocks a day and 100 in all; every page exists and is in one block only. Titles longer
/// than 120 characters and objectives longer than 500 are cut, with a warning; a day over
/// its minutes is a warning. Issues are listed for the plan, then by block, then by day.
/// O(blocks + pages).
[[nodiscard]] PlanValidation validatePlanDraft(const document::Workspace& workspace,
                                               PlanDraft draft);

} // namespace studyapp::application
