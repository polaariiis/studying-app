#include <studyapp/application/StudyPlan.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace studyapp::application {

using core::ErrorCode;
using core::makeError;
using core::Result;
using std::chrono::minutes;

namespace {

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool isContinuation(char c) noexcept {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

/// Cuts `text` after `limit` characters (UTF-8 code points), never inside one. Returns
/// whether it cut.
bool cutToCharacters(std::string& text, std::size_t limit) {
    std::size_t characters = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!isContinuation(text[i]) && characters++ == limit) {
            text.resize(i);
            return true;
        }
    }
    return false;
}

bool minutesPerDayInRange(minutes perDay) noexcept {
    return perDay >= kMinPlanMinutes && perDay <= kMaxPlanMinutes &&
           perDay % kPlanMinuteStep == minutes{0};
}

/// Whether the plan's start and last day are storable; `days` must be in range.
bool datesStorable(study::CalendarDate start, int days) {
    return study::isStorableDate(start) && study::isStorableDate(planDate(start, days));
}

struct ChosenSection {
    core::SectionId id;
    std::span<const core::PageId> pages;
};

/// The chosen sections with pages, in tree order.
std::vector<ChosenSection> chosenSections(const document::Workspace& workspace,
                                          const PlanScope& scope) {
    const std::unordered_set<core::NotebookId> notebooks(scope.notebooks.begin(),
                                                         scope.notebooks.end());
    const std::unordered_set<core::SectionId> sections(scope.sections.begin(),
                                                       scope.sections.end());
    std::vector<ChosenSection> chosen;
    for (const core::NotebookId notebook : workspace.notebooks()) {
        const bool wholeNotebook = notebooks.contains(notebook);
        for (const core::SectionId section : workspace.sectionsOf(notebook)) {
            const auto pages = workspace.pagesOf(section);
            if ((wholeNotebook || sections.contains(section)) && !pages.empty()) {
                chosen.push_back({section, pages});
            }
        }
    }
    return chosen;
}

/// Rules 3–4: places the sections from day 1 on, continuing after the last day.
std::vector<PlanBlock> place(const document::Workspace& workspace,
                             const std::vector<ChosenSection>& sections, minutes perPage,
                             minutes perDay) {
    std::vector<PlanBlock> blocks;
    int day = 1;
    minutes left = perDay;
    std::size_t onDay = 0;
    const auto nextDay = [&] {
        ++day;
        left = perDay;
        onDay = 0;
    };
    for (const ChosenSection& section : sections) {
        const std::string& title = workspace.findSection(section.id)->title;
        const auto pageCount = static_cast<minutes::rep>(section.pages.size());
        const minutes whole = perPage * pageCount;
        if (whole <= perDay) {
            if (whole > left || onDay == kMaxBlocksPerDay) {
                nextDay();
            }
            blocks.push_back({.day = day,
                              .title = title,
                              .minutes = whole,
                              .pages = {section.pages.begin(), section.pages.end()}});
            left -= whole;
            ++onDay;
            continue;
        }
        const std::size_t first = blocks.size();
        auto next = section.pages.begin();
        while (next != section.pages.end()) {
            if (onDay == kMaxBlocksPerDay || left < perPage) {
                nextDay();
            }
            const std::ptrdiff_t fit =
                std::min(section.pages.end() - next, static_cast<std::ptrdiff_t>(left / perPage));
            blocks.push_back({.day = day, .minutes = perPage * fit, .pages = {next, next + fit}});
            next += fit;
            left -= perPage * fit;
            ++onDay;
        }
        const std::size_t parts = blocks.size() - first;
        for (std::size_t i = 0; i < parts; ++i) {
            blocks[first + i].title =
                title + " (part " + std::to_string(i + 1) + " of " + std::to_string(parts) + ")";
        }
    }
    return blocks;
}

PlanIssue issue(PlanIssueKind kind, std::string message) {
    return {.kind = kind, .message = std::move(message)};
}

PlanIssue blockIssue(PlanIssueKind kind, std::size_t block, std::string message) {
    return {.kind = kind, .block = block, .message = std::move(message)};
}

std::string blockName(std::size_t block) {
    return "Block " + std::to_string(block + 1);
}

} // namespace

std::vector<core::PageId> pagesInScope(const document::Workspace& workspace,
                                       const PlanScope& scope) {
    std::vector<core::PageId> pages;
    for (const ChosenSection& section : chosenSections(workspace, scope)) {
        pages.insert(pages.end(), section.pages.begin(), section.pages.end());
    }
    return pages;
}

study::CalendarDate planDate(study::CalendarDate start, int day) {
    return study::CalendarDate{std::chrono::sys_days{start} + std::chrono::days{day - 1}};
}

Result<PlanBuild> draftByTreeOrder(const document::Workspace& workspace,
                                   const PlanRequest& request) {
    if (request.days < 1 || request.days > kMaxPlanDays) {
        return makeError(ErrorCode::InvalidArgument, "a plan has 1 to 60 days");
    }
    if (!minutesPerDayInRange(request.minutesPerDay)) {
        return makeError(ErrorCode::InvalidArgument, "minutes per day are 5 to 480, in steps of 5");
    }
    if (!datesStorable(request.start, request.days)) {
        return makeError(ErrorCode::InvalidArgument,
                         "the plan's dates must lie within the years 1 to 9999");
    }
    PlanBuild build;
    const std::vector<ChosenSection> sections = chosenSections(workspace, request.scope);
    std::size_t pageCount = 0;
    for (const ChosenSection& section : sections) {
        build.sections.push_back({section.id, section.pages.size()});
        pageCount += section.pages.size();
    }
    if (pageCount == 0) {
        return build; // NoPages
    }
    // Rule 2: the same minutes for every page, a multiple of 5 within 5 … minutes per day.
    const minutes budget = request.minutesPerDay * request.days;
    const minutes perPage = std::clamp(budget / static_cast<minutes::rep>(pageCount) /
                                           kPlanMinuteStep.count() * kPlanMinuteStep.count(),
                                       kMinPlanMinutes, request.minutesPerDay);
    std::vector<PlanBlock> blocks = place(workspace, sections, perPage, request.minutesPerDay);
    build.blocksNeeded = blocks.size();
    if (blocks.size() > kMaxPlanBlocks) {
        build.outcome = PlanBuildOutcome::TooManyBlocks;
        return build;
    }
    build.outcome = PlanBuildOutcome::Draft;
    build.draft = PlanDraft{.title = std::string(trim(request.goal)),
                            .start = request.start,
                            .days = request.days,
                            .minutesPerDay = request.minutesPerDay,
                            .blocks = std::move(blocks)};
    return build;
}

PlanValidation validatePlanDraft(const document::Workspace& workspace, PlanDraft draft) {
    PlanValidation result;
    auto& errors = result.errors;
    auto& warnings = result.warnings;

    // ---- the plan
    const bool daysInRange = draft.days >= 1 && draft.days <= kMaxPlanDays;
    if (!daysInRange) {
        errors.push_back(issue(PlanIssueKind::DaysOutOfRange, "A plan has 1 to 60 days."));
    } else if (!datesStorable(draft.start, draft.days)) {
        errors.push_back(issue(PlanIssueKind::DatesNotStorable,
                               "The plan's dates must lie within the years 1 to 9999."));
    }
    if (!minutesPerDayInRange(draft.minutesPerDay)) {
        errors.push_back(issue(PlanIssueKind::MinutesPerDayOutOfRange,
                               "Minutes per day are 5 to 480, in steps of 5."));
    }
    if (trim(draft.title).empty()) {
        errors.push_back(issue(PlanIssueKind::BlankTitle, "The plan needs a title."));
    } else if (cutToCharacters(draft.title, kMaxPlanTitleLength)) {
        warnings.push_back(
            issue(PlanIssueKind::TitleCut, "The plan's title was cut to 120 characters."));
    }
    if (draft.blocks.size() > kMaxPlanBlocks) {
        errors.push_back(issue(PlanIssueKind::TooManyBlocks,
                               "The plan has " + std::to_string(draft.blocks.size()) +
                                   " blocks; the limit is 100."));
    }

    // ---- each block
    std::unordered_set<core::PageId> seen;
    std::map<int, std::pair<std::size_t, minutes>> perDay; // day -> blocks, minutes
    for (std::size_t i = 0; i < draft.blocks.size(); ++i) {
        PlanBlock& block = draft.blocks[i];
        if (trim(block.title).empty()) {
            errors.push_back(
                blockIssue(PlanIssueKind::BlankBlockTitle, i, blockName(i) + " needs a title."));
        } else if (cutToCharacters(block.title, kMaxPlanTitleLength)) {
            warnings.push_back(blockIssue(PlanIssueKind::BlockTitleCut, i,
                                          blockName(i) + ": the title was cut to 120 characters."));
        }
        if (cutToCharacters(block.objective, kMaxPlanObjectiveLength)) {
            warnings.push_back(
                blockIssue(PlanIssueKind::ObjectiveCut, i,
                           blockName(i) + ": the objective was cut to 500 characters."));
        }
        if (block.day) {
            if (*block.day < 1) {
                errors.push_back(blockIssue(PlanIssueKind::BlockDayOutOfRange, i,
                                            blockName(i) + " is on a day before day 1."));
            } else if (daysInRange && *block.day > draft.days) {
                errors.push_back(blockIssue(PlanIssueKind::BlockAfterLastDay, i,
                                            blockName(i) + " needs day " +
                                                std::to_string(*block.day) + " of a " +
                                                std::to_string(draft.days) + "-day plan."));
            }
            auto& [count, total] = perDay[*block.day];
            ++count;
            total += block.minutes;
        }
        if (block.minutes < kMinPlanMinutes || block.minutes > kMaxPlanMinutes) {
            errors.push_back(blockIssue(PlanIssueKind::BlockMinutesOutOfRange, i,
                                        blockName(i) + " has " +
                                            std::to_string(block.minutes.count()) +
                                            " minutes; a block has 5 to 480."));
        }
        for (const core::PageId page : block.pages) {
            if (workspace.findPage(page) == nullptr) {
                PlanIssue missing = blockIssue(PlanIssueKind::MissingPage, i,
                                               blockName(i) + " links a page that was deleted.");
                missing.page = page;
                errors.push_back(std::move(missing));
            }
            if (!seen.insert(page).second) {
                PlanIssue repeated =
                    blockIssue(PlanIssueKind::RepeatedPage, i,
                               blockName(i) + " links a page that is already in the plan.");
                repeated.page = page;
                errors.push_back(std::move(repeated));
            }
        }
    }

    // ---- each day
    for (const auto& [day, load] : perDay) {
        const auto& [count, total] = load;
        if (count > kMaxBlocksPerDay) {
            PlanIssue full = issue(PlanIssueKind::TooManyBlocksOnDay,
                                   "Day " + std::to_string(day) + " has " + std::to_string(count) +
                                       " blocks; the limit is 12.");
            full.day = day;
            errors.push_back(std::move(full));
        }
        if (total > draft.minutesPerDay) {
            PlanIssue over = issue(PlanIssueKind::DayOverMinutes,
                                   "Day " + std::to_string(day) + " has " +
                                       std::to_string(total.count()) + " minutes, more than " +
                                       std::to_string(draft.minutesPerDay.count()) + ".");
            over.day = day;
            warnings.push_back(std::move(over));
        }
    }
    result.draft = std::move(draft);
    return result;
}

// ---------------------------------------------------------------------------- PlanPreview

namespace {

bool contains(const std::vector<core::PageId>& pages, core::PageId page) {
    return std::ranges::find(pages, page) != pages.end();
}

/// Removes `page` from `pages`; whether it was there.
bool erase(std::vector<core::PageId>& pages, core::PageId page) {
    return std::erase(pages, page) > 0;
}

Result<void> noBlock() {
    return makeError(ErrorCode::NotFound, "no such block");
}

} // namespace

PlanPreview::PlanPreview(PlanRequest request, PlanDraft draft)
    : request_(std::move(request)), draft_(std::move(draft)), sequence_(draft_.blocks.size()),
      nextSequence_(draft_.blocks.size()) {
    std::iota(sequence_.begin(), sequence_.end(), std::size_t{0});
    sortBlocks();
}

void PlanPreview::sortBlocks() {
    // Scheduled blocks by day (overflow days included), then unscheduled; ties in
    // placement order.
    const auto key = [&](std::size_t i) {
        const auto& day = draft_.blocks[i].day;
        return std::tuple(!day.has_value(), day.value_or(0), sequence_[i]);
    };
    std::vector<std::size_t> order(draft_.blocks.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
    std::vector<PlanBlock> blocks;
    std::vector<std::size_t> sequence;
    blocks.reserve(order.size());
    sequence.reserve(order.size());
    for (const std::size_t i : order) {
        blocks.push_back(std::move(draft_.blocks[i]));
        sequence.push_back(sequence_[i]);
    }
    draft_.blocks = std::move(blocks);
    sequence_ = std::move(sequence);
}

std::size_t PlanPreview::blocksOnDay(int day) const {
    return static_cast<std::size_t>(std::ranges::count_if(
        draft_.blocks, [day](const PlanBlock& block) { return block.day == day; }));
}

std::optional<PlanOverflow> PlanPreview::overflow() const {
    PlanOverflow overflow;
    for (std::size_t i = 0; i < draft_.blocks.size(); ++i) {
        const PlanBlock& block = draft_.blocks[i];
        if (block.day && *block.day > draft_.days) {
            overflow.blocks.push_back(i);
            overflow.daysNeeded = std::max(overflow.daysNeeded, *block.day);
            overflow.minutesOver += block.minutes;
        }
    }
    if (overflow.blocks.empty()) {
        return std::nullopt;
    }
    overflow.canExtend =
        overflow.daysNeeded <= kMaxPlanDays && datesStorable(draft_.start, overflow.daysNeeded);
    return overflow;
}

PlanReview PlanPreview::review(const document::Workspace& workspace) const {
    PlanReview review;
    review.validation = validatePlanDraft(workspace, draft_);

    const std::vector<core::PageId> scope = pagesInScope(workspace, request_.scope);
    const std::unordered_set<core::PageId> inScope(scope.begin(), scope.end());
    std::unordered_set<core::PageId> inPlan;
    for (std::size_t i = 0; i < draft_.blocks.size(); ++i) {
        for (const core::PageId page : draft_.blocks[i].pages) {
            inPlan.insert(page);
            if (workspace.findPage(page) == nullptr) {
                review.invalid.push_back({i, page});
            } else if (!inScope.contains(page)) {
                (contains(kept_, page) ? review.kept : review.changed).push_back({i, page});
            }
        }
    }
    const auto exclude = [&](core::PageId page) {
        inPlan.insert(page);
        if (workspace.findPage(page) != nullptr) {
            review.excluded.push_back(page);
        }
    };
    for (const PlanBlock& block : removed_) {
        std::ranges::for_each(block.pages, exclude);
    }
    std::ranges::for_each(excluded_, exclude);
    for (const core::PageId page : scope) {
        if (!inPlan.contains(page)) {
            review.notInPlan.push_back(page);
        }
    }

    if (overflow()) {
        review.holds.push_back(PlanHold::Overflow);
    }
    if (!review.invalid.empty()) {
        review.holds.push_back(PlanHold::InvalidReferences);
    }
    if (!review.changed.empty()) {
        review.holds.push_back(PlanHold::ChangedReferences);
    }
    // Overflow and deleted pages are holds of their own.
    if (std::ranges::any_of(review.validation.errors, [](const PlanIssue& error) {
            return error.kind != PlanIssueKind::BlockAfterLastDay &&
                   error.kind != PlanIssueKind::MissingPage;
        })) {
        review.holds.push_back(PlanHold::InvalidDraft);
    }
    if (draft_.blocks.empty()) {
        review.holds.push_back(PlanHold::NoBlocks);
    }
    return review;
}

void PlanPreview::setGoal(std::string goal) {
    draft_.title = std::string(trim(goal));
    request_.goal = std::move(goal);
}

Result<void> PlanPreview::setStart(study::CalendarDate start) {
    if (!datesStorable(start, draft_.days)) {
        return makeError(ErrorCode::InvalidArgument,
                         "the plan's dates must lie within the years 1 to 9999");
    }
    draft_.start = start;
    request_.start = start;
    return {};
}

Result<void> PlanPreview::extendToNeededDays() {
    const std::optional<PlanOverflow> over = overflow();
    if (!over) {
        return makeError(ErrorCode::Conflict, "every block fits in the plan's days");
    }
    if (!over->canExtend) {
        return makeError(ErrorCode::Conflict, "the plan would need " +
                                                  std::to_string(over->daysNeeded) +
                                                  " days; the limit is 60");
    }
    draft_.days = over->daysNeeded;
    request_.days = over->daysNeeded;
    return {};
}

Result<void> PlanPreview::keepOverflowUnscheduled() {
    const std::optional<PlanOverflow> over = overflow();
    if (!over) {
        return makeError(ErrorCode::Conflict, "every block fits in the plan's days");
    }
    for (const std::size_t i : over->blocks) {
        draft_.blocks[i].day.reset();
    }
    sortBlocks();
    return {};
}

Result<void> PlanPreview::removeOverflow() {
    const std::optional<PlanOverflow> over = overflow();
    if (!over) {
        return makeError(ErrorCode::Conflict, "every block fits in the plan's days");
    }
    // From the last, so that the indices before it stay valid; removed in placement order.
    const std::size_t first = removed_.size();
    for (auto i = over->blocks.rbegin(); i != over->blocks.rend(); ++i) {
        (void)removeBlock(*i);
    }
    std::reverse(removed_.begin() + static_cast<std::ptrdiff_t>(first), removed_.end());
    std::reverse(removedSequence_.begin() + static_cast<std::ptrdiff_t>(first),
                 removedSequence_.end());
    return {};
}

Result<void> PlanPreview::setBlockMinutes(std::size_t block, minutes value) {
    if (block >= draft_.blocks.size()) {
        return noBlock();
    }
    if (value < kMinPlanMinutes || value > kMaxPlanMinutes ||
        value % kPlanMinuteStep != minutes{0}) {
        return makeError(ErrorCode::InvalidArgument, "a block has 5 to 480 minutes, in steps of 5");
    }
    draft_.blocks[block].minutes = value;
    return {};
}

Result<void> PlanPreview::setBlockTitle(std::size_t block, std::string title) {
    if (block >= draft_.blocks.size()) {
        return noBlock();
    }
    draft_.blocks[block].title = std::move(title);
    return {};
}

Result<void> PlanPreview::moveBlock(std::size_t block, int day) {
    if (block >= draft_.blocks.size()) {
        return noBlock();
    }
    if (day < 1 || day > draft_.days) {
        return makeError(ErrorCode::InvalidArgument,
                         "a block's day is 1 to " + std::to_string(draft_.days));
    }
    if (draft_.blocks[block].day == day) {
        return {};
    }
    if (blocksOnDay(day) >= kMaxBlocksPerDay) {
        return makeError(ErrorCode::Conflict,
                         "Day " + std::to_string(day) + " already has 12 blocks.");
    }
    draft_.blocks[block].day = day;
    sequence_[block] = nextSequence_++;
    sortBlocks();
    return {};
}

Result<void> PlanPreview::removeBlock(std::size_t block) {
    if (block >= draft_.blocks.size()) {
        return noBlock();
    }
    const auto at = static_cast<std::ptrdiff_t>(block);
    removed_.push_back(std::move(draft_.blocks[block]));
    removedSequence_.push_back(sequence_[block]);
    draft_.blocks.erase(draft_.blocks.begin() + at);
    sequence_.erase(sequence_.begin() + at);
    return {};
}

Result<void> PlanPreview::restoreBlock(std::size_t removed) {
    if (removed >= removed_.size()) {
        return makeError(ErrorCode::NotFound, "no such removed block");
    }
    const std::optional<int> day = removed_[removed].day;
    if (day && blocksOnDay(*day) >= kMaxBlocksPerDay) {
        return makeError(ErrorCode::Conflict,
                         "Day " + std::to_string(*day) + " already has 12 blocks.");
    }
    const auto at = static_cast<std::ptrdiff_t>(removed);
    draft_.blocks.push_back(std::move(removed_[removed]));
    sequence_.push_back(removedSequence_[removed]);
    removed_.erase(removed_.begin() + at);
    removedSequence_.erase(removedSequence_.begin() + at);
    sortBlocks();
    return {};
}

Result<void> PlanPreview::addPage(std::size_t block, core::PageId page) {
    if (block >= draft_.blocks.size()) {
        return noBlock();
    }
    bool found = erase(excluded_, page);
    for (PlanBlock& removed : removed_) {
        found = found || erase(removed.pages, page);
    }
    if (!found) {
        return makeError(ErrorCode::NotFound, "the page is not excluded");
    }
    draft_.blocks[block].pages.push_back(page);
    return {};
}

Result<void> PlanPreview::excludePage(const document::Workspace& workspace, core::PageId page) {
    const bool exists = workspace.findPage(page) != nullptr;
    for (PlanBlock& block : draft_.blocks) {
        if (erase(block.pages, page)) {
            erase(kept_, page);
            if (exists) {
                excluded_.push_back(page);
            }
            return {};
        }
    }
    const bool excluded =
        contains(excluded_, page) || std::ranges::any_of(removed_, [page](const PlanBlock& block) {
            return contains(block.pages, page);
        });
    if (excluded) {
        return makeError(ErrorCode::Conflict, "the page is excluded already");
    }
    if (!exists || !contains(pagesInScope(workspace, request_.scope), page)) {
        return makeError(ErrorCode::NotFound, "the page is not in the plan or its scope");
    }
    excluded_.push_back(page);
    return {};
}

Result<void> PlanPreview::keepPage(const document::Workspace& workspace, core::PageId page) {
    if (std::ranges::none_of(draft_.blocks, [page](const PlanBlock& block) {
            return contains(block.pages, page);
        })) {
        return makeError(ErrorCode::NotFound, "no block links the page");
    }
    if (workspace.findPage(page) == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the page was deleted");
    }
    if (contains(pagesInScope(workspace, request_.scope), page)) {
        return makeError(ErrorCode::InvalidArgument, "the page is in the plan's scope");
    }
    if (!contains(kept_, page)) {
        kept_.push_back(page);
    }
    return {};
}

} // namespace studyapp::application
