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

/// Blocks after the plan's last day (rule 4).
struct PlanOverflow {
    std::vector<std::size_t> blocks;     ///< indices into the preview's blocks
    int daysNeeded = 0;                  ///< the last day used
    std::chrono::minutes minutesOver{0}; ///< the overflow blocks' minutes
    /// Resolution (a) is offered: daysNeeded ≤ kMaxPlanDays and its date is storable.
    bool canExtend = false;

    [[nodiscard]] friend bool operator==(const PlanOverflow&, const PlanOverflow&) = default;
};

/// A page linked by a block of the preview.
struct PlanReference {
    std::size_t block = 0;
    core::PageId page;

    [[nodiscard]] friend bool operator==(const PlanReference&, const PlanReference&) = default;
};

/// Why a preview cannot be accepted.
enum class PlanHold : std::uint8_t {
    Overflow,          ///< blocks after the last day (rule 4)
    InvalidReferences, ///< linked pages that no longer exist (rule 7)
    ChangedReferences, ///< linked pages moved out of the scope and not kept (rule 7)
    InvalidDraft       ///< any other validation error
};

/// The preview checked against the workspace as it is now (rule 7).
struct PlanReview {
    PlanValidation validation;
    std::vector<PlanReference> invalid; ///< by block, in page order
    std::vector<PlanReference> changed;
    std::vector<PlanReference> kept; ///< outside the scope, kept by the user
    /// "Excluded pages" that still exist: those of removed blocks, then the others.
    std::vector<core::PageId> excluded;
    /// In the scope now but in no block and not excluded (pages added to a chosen section,
    /// or brought back by undo), in tree order. Shown; they do not hold Accept.
    std::vector<core::PageId> notInPlan;
    std::vector<PlanHold> holds; ///< in PlanHold order; empty when Accept may save

    [[nodiscard]] bool canAccept() const noexcept { return holds.empty(); }
};

/// The editable preview of a plan (rules 4, 6 and 7): the draft, the request it was built
/// from, and the pages the user took out of it ("Excluded pages"). Every page of the
/// draft as built is, after any edits, in exactly one block or under "Excluded pages";
/// only a page that no longer exists is dropped when its reference is removed. Rebuilding
/// (resolution (b)) is a new preview from draftByTreeOrder.
///
/// The blocks are kept in saving order: by day, unscheduled blocks last; within a day in
/// placement order, a block moved to a day after that day's blocks, a restored block in
/// its old place. Block indices change when a block is moved, removed or restored.
///
/// Holds ids only, like PlanDraft; operations that look at pages take the workspace as it
/// is now. Edits refused with an error change nothing.
class PlanPreview {
public:
    PlanPreview(PlanRequest request, PlanDraft draft);

    /// The request, with the goal, start date and days as edited in the preview.
    [[nodiscard]] const PlanRequest& request() const noexcept { return request_; }
    [[nodiscard]] const PlanDraft& draft() const noexcept { return draft_; }
    /// Removed blocks, in the order they were removed; restoreBlock takes an index.
    [[nodiscard]] const std::vector<PlanBlock>& removedBlocks() const noexcept { return removed_; }
    /// Pages excluded one at a time, not with their block.
    [[nodiscard]] const std::vector<core::PageId>& excludedPages() const noexcept {
        return excluded_;
    }

    /// nullopt when no block is after the last day.
    [[nodiscard]] std::optional<PlanOverflow> overflow() const;
    /// O(blocks + pages + pages in scope).
    [[nodiscard]] PlanReview review(const document::Workspace& workspace) const;

    // ---- the plan: the goal and start date change titles and dates only.
    /// The goal becomes the description; trimmed, the title.
    void setGoal(std::string goal);
    /// Errors: InvalidArgument if the start or the last day is not storable.
    [[nodiscard]] core::Result<void> setStart(study::CalendarDate start);

    // ---- overflow resolutions (a), (c), (d) of rule 4, each for every overflow block.
    // Errors: Conflict if there is no overflow, or for (a) if it is not offered.
    /// (a) The plan's days become the days needed; the placement is kept.
    [[nodiscard]] core::Result<void> extendToNeededDays();
    /// (c) The overflow blocks become unscheduled.
    [[nodiscard]] core::Result<void> keepOverflowUnscheduled();
    /// (d) The overflow blocks are removed, as by removeBlock.
    [[nodiscard]] core::Result<void> removeOverflow();

    // ---- block edits (rule 6). Errors: NotFound for an index out of range.
    /// Errors: InvalidArgument unless 5 … 480 in steps of 5.
    [[nodiscard]] core::Result<void> setBlockMinutes(std::size_t block, std::chrono::minutes value);
    /// Stored as entered; review reports a blank title, validation cuts a long one.
    [[nodiscard]] core::Result<void> setBlockTitle(std::size_t block, std::string title);
    /// Puts the block on `day` (1 … days); an unscheduled block becomes scheduled.
    /// Errors: InvalidArgument for a day outside the plan; Conflict if that day already
    /// has kMaxBlocksPerDay blocks.
    [[nodiscard]] core::Result<void> moveBlock(std::size_t block, int day);
    /// Its pages go to "Excluded pages", where restoreBlock brings the block back.
    [[nodiscard]] core::Result<void> removeBlock(std::size_t block);
    /// Errors: NotFound for an index out of range; Conflict if its day has
    /// kMaxBlocksPerDay blocks.
    [[nodiscard]] core::Result<void> restoreBlock(std::size_t removed);
    /// Adds an excluded page (of a removed block or excluded alone) at the end of the
    /// block's pages; minutes are unchanged. Errors: NotFound if `page` is not excluded.
    [[nodiscard]] core::Result<void> addPage(std::size_t block, core::PageId page);
    /// Puts `page` under "Excluded pages": taken out of its block (minutes unchanged), or a
    /// page in the scope but not in the plan. A page that no longer exists is taken out of
    /// its block and dropped. Errors: Conflict if it is excluded already; NotFound if no
    /// block links it and it is not in the scope.
    [[nodiscard]] core::Result<void> excludePage(const document::Workspace& workspace,
                                                 core::PageId page);
    /// Keeps a linked page that moved out of the scope (rule 7). Errors: NotFound if no
    /// block links it; InvalidArgument if it no longer exists or is in the scope.
    [[nodiscard]] core::Result<void> keepPage(const document::Workspace& workspace,
                                              core::PageId page);

private:
    void sortBlocks();
    [[nodiscard]] std::size_t blocksOnDay(int day) const;

    PlanRequest request_;
    PlanDraft draft_;
    std::vector<std::size_t> sequence_; ///< per block: its place in placement order
    std::vector<PlanBlock> removed_;
    std::vector<std::size_t> removedSequence_;
    std::vector<core::PageId> excluded_;
    std::vector<core::PageId> kept_;
    std::size_t nextSequence_ = 0;
};

} // namespace studyapp::application
