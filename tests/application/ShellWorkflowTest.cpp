// Phase 5 application layer of the shell against a real WorkspaceSession (SQLite):
// structure edits (create/rename/delete/move) as one undoable, persisted command each,
// the active page (PageNavigator) across deletes and undo, and the end-to-end workflow
// create → edit → switch page → return → close → reopen.

#include <studyapp/application/PageNavigator.hpp>
#include <studyapp/application/StartPage.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace studyapp::application {
namespace {

class NoLocks final : public WorkspaceLocker {
public:
    core::Result<LockStatus> inspect(const std::filesystem::path&) override { return LockStatus{}; }
    core::Result<std::unique_ptr<WorkspaceLock>> acquire(const std::filesystem::path&,
                                                         bool) override {
        return std::unique_ptr<WorkspaceLock>(std::make_unique<Held>());
    }

private:
    class Held final : public WorkspaceLock {};
};

template <class Id>
std::vector<Id> ids(std::span<const Id> span) {
    return {span.begin(), span.end()};
}

struct ShellWorkflowTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds idGenerator;
    NoLocks locker;
    std::unique_ptr<WorkspaceSession> session;

    SessionServices services() { return {clock, idGenerator, locker}; }

    void SetUp() override {
        auto created = WorkspaceSession::create(dir / "ws", "Shell", services());
        ASSERT_OK(created);
        session = std::move(*created);
    }

    void reopen() {
        ASSERT_OK(session->close());
        session.reset();
        auto opened = WorkspaceSession::open(dir / "ws", {}, services());
        ASSERT_OK(opened);
        session = std::move(*opened);
    }

    WorkspaceStructure structure() { return {*session, clock, idGenerator}; }
    const document::Workspace& ws() const { return session->workspace(); }

    core::ElementId draw(core::PageId page) {
        auto created = document::commands::createElement(ws(), ws().layersOf(page).front(),
                                                         {.payload = document::test::makeStroke()},
                                                         idGenerator);
        EXPECT_TRUE(created.has_value());
        const auto id = created->id;
        EXPECT_TRUE(session->execute(std::move(created->command)).has_value());
        return id;
    }
};

// ---------------------------------------------------------------------------- structure

TEST_F(ShellWorkflowTest, NewNotebookComesWithASectionAndAPageAsOneUndoStep) {
    auto s = structure();
    auto created = s.createNotebook();
    ASSERT_OK(created);
    EXPECT_EQ(ws().findNotebook(created->notebook)->title, "Notebook 1");
    EXPECT_EQ(ws().findSection(created->section)->notebook, created->notebook);
    EXPECT_EQ(ws().findPage(created->page)->section, created->section);
    EXPECT_EQ(ws().layersOf(created->page).size(), 1U);
    EXPECT_EQ(session->history().undoCount(), 1U);
    EXPECT_EQ(session->history().nextUndo()->label, "Create notebook");
    EXPECT_EQ(session->pendingWriteCount(), 0U); // persisted at once

    ASSERT_OK(session->undo());
    EXPECT_EQ(ws().notebookCount(), 0U);
    EXPECT_EQ(ws().pageCount(), 0U);
    ASSERT_OK(session->redo());
    EXPECT_EQ(ws().pageCount(), 1U);
}

TEST_F(ShellWorkflowTest, DefaultTitlesAreUniqueAmongSiblings) {
    auto s = structure();
    auto first = s.createNotebook();
    ASSERT_OK(first);
    auto second = s.createNotebook();
    ASSERT_OK(second);
    EXPECT_EQ(ws().findNotebook(second->notebook)->title, "Notebook 2");
    // A renamed or deleted sibling does not produce a duplicate.
    ASSERT_OK(s.rename(first->notebook, "Notebook 3"));
    auto third = s.createNotebook();
    ASSERT_OK(third);
    EXPECT_EQ(ws().findNotebook(third->notebook)->title, "Notebook 4");

    auto page = s.createPage(first->section);
    ASSERT_OK(page);
    EXPECT_EQ(ws().findPage(*page)->title, "Page 2");
    auto section = s.createSection(first->notebook, "  ");
    ASSERT_OK(section);
    EXPECT_EQ(ws().findSection(section->section)->title, "Section 2");
    EXPECT_EQ(WorkspaceStructure::displayTitle(ws(), section->page), "Page 1");
    auto titled = s.createPage(section->section, "Lecture 3");
    ASSERT_OK(titled);
    EXPECT_EQ(ws().findPage(*titled)->title, "Lecture 3");
}

TEST_F(ShellWorkflowTest, NewPagesFollowTheirSectionsFormat) {
    auto s = structure();
    auto created = s.createNotebook();
    ASSERT_OK(created);
    const document::commands::PageFormat ruledA4{
        .extent = document::PageExtent::Bounded,
        .size = document::kA4PortraitSize,
        .background = {.color = core::Color::white(),
                       .pattern = document::BackgroundPattern::Ruled,
                       .spacing = 30.0F}};
    ASSERT_OK(
        session->execute(*document::commands::setPageFormat(ws(), created->page, ruledA4, clock)));
    auto page = s.createPage(created->section);
    ASSERT_OK(page);
    const document::PageInfo& info = *ws().findPage(*page);
    EXPECT_EQ(info.extent, document::PageExtent::Bounded);
    EXPECT_EQ(info.background.pattern, document::BackgroundPattern::Ruled);
    EXPECT_EQ(info.background.spacing, 30.0F);
    // The first page of a new section uses the default format.
    auto section = s.createSection(created->notebook);
    ASSERT_OK(section);
    EXPECT_EQ(ws().findPage(section->page)->extent, document::PageExtent::Infinite);
    EXPECT_EQ(ws().findPage(section->page)->background, defaultPageOptions().background);
}

TEST_F(ShellWorkflowTest, RenameDeleteAndMoveAreUndoablePersistedCommands) {
    auto s = structure();
    auto a = s.createNotebook("A");
    auto b = s.createNotebook("B");
    ASSERT_OK(a);
    ASSERT_OK(b);
    auto p2 = s.createPage(a->section, "Two");
    auto p3 = s.createPage(a->section, "Three");
    ASSERT_OK(p2);
    ASSERT_OK(p3);

    ASSERT_OK(s.rename(a->page, "One"));
    EXPECT_EQ(ws().findPage(a->page)->title, "One");
    ASSERT_OK(s.rename(a->page, "")); // pages may be untitled
    EXPECT_EQ(WorkspaceStructure::displayTitle(ws(), a->page), "Untitled page");
    EXPECT_FALSE(s.rename(a->notebook, " ").has_value()); // notebooks may not
    ASSERT_OK(session->undo());
    EXPECT_EQ(ws().findPage(a->page)->title, "One");

    // Move up/down within the section; no-ops at the ends record nothing.
    const auto undoCount = session->history().undoCount();
    ASSERT_OK(s.moveBy(a->page, -1));
    ASSERT_OK(s.moveBy(*p3, +1));
    EXPECT_EQ(session->history().undoCount(), undoCount);
    ASSERT_OK(s.moveBy(*p3, -1));
    EXPECT_EQ(ids(ws().pagesOf(a->section)), (std::vector{a->page, *p3, *p2}));
    ASSERT_OK(s.moveBy(b->notebook, -1));
    EXPECT_EQ(ids(ws().notebooks()), (std::vector{b->notebook, a->notebook}));

    // Across parents.
    ASSERT_OK(s.movePage(*p2, b->section, 0));
    EXPECT_EQ(ws().findPage(*p2)->section, b->section);
    ASSERT_OK(s.moveSection(a->section, b->notebook, 1));
    EXPECT_TRUE(ws().sectionsOf(a->notebook).empty());

    // Delete a notebook with everything in it, then bring it back.
    ASSERT_OK(s.remove(b->notebook));
    EXPECT_EQ(ws().notebookCount(), 1U);
    EXPECT_EQ(ws().findPage(*p2), nullptr);
    ASSERT_OK(session->undo());
    EXPECT_NE(ws().findPage(*p2), nullptr);
    EXPECT_EQ(session->pendingWriteCount(), 0U);

    // Everything above is in the database.
    const document::Workspace expected = ws();
    reopen();
    EXPECT_TRUE(ws() == expected);
}

TEST_F(ShellWorkflowTest, ReadOnlySessionsRejectStructureEdits) {
    auto created = structure().createNotebook();
    ASSERT_OK(created);
    ASSERT_OK(session->close());
    session.reset();
    auto opened = WorkspaceSession::open(dir / "ws", {.mode = AccessMode::ReadOnly}, services());
    ASSERT_OK(opened);
    session = std::move(*opened);
    auto s = structure();
    EXPECT_EQ(s.createPage(created->section).error().code, core::ErrorCode::Unsupported);
    EXPECT_EQ(s.rename(created->page, "x").error().code, core::ErrorCode::Unsupported);
    EXPECT_EQ(s.remove(created->notebook).error().code, core::ErrorCode::Unsupported);
    EXPECT_EQ(ws().pageCount(), 1U);
}

// ---------------------------------------------------------------------------- navigation

TEST_F(ShellWorkflowTest, ActivePageFallsBackToANeighbourWhenDeleted) {
    auto s = structure();
    auto a = s.createNotebook("A");
    ASSERT_OK(a);
    auto p2 = s.createPage(a->section);
    auto p3 = s.createPage(a->section);
    auto other = s.createSection(a->notebook);
    auto b = s.createNotebook("B");
    ASSERT_OK(p2);
    ASSERT_OK(p3);
    ASSERT_OK(other);
    ASSERT_OK(b);

    PageNavigator navigator(ws());
    session->setPatchListener([&](const document::Patch& patch) { navigator.onPatch(patch); });
    ASSERT_OK(navigator.open(*p2));
    EXPECT_EQ(navigator.activeSection(), a->section);
    EXPECT_EQ(navigator.activeNotebook(), a->notebook);

    ASSERT_OK(s.remove(*p2)); // the page that took its place
    EXPECT_EQ(navigator.activePage(), *p3);
    ASSERT_OK(s.remove(*p3)); // last in the section: the one before it
    EXPECT_EQ(navigator.activePage(), a->page);
    ASSERT_OK(s.remove(a->section)); // empty section: first page of the notebook
    EXPECT_EQ(navigator.activePage(), other->page);
    ASSERT_OK(s.remove(a->notebook)); // empty notebook: first page of the workspace
    EXPECT_EQ(navigator.activePage(), b->page);
    ASSERT_OK(s.remove(b->notebook)); // nothing left
    EXPECT_FALSE(navigator.activePage().has_value());

    // Moving the active page keeps it active.
    ASSERT_OK(session->undo()); // B is back
    ASSERT_OK(navigator.open(b->page));
    auto moved = s.createSection(b->notebook);
    ASSERT_OK(moved);
    ASSERT_OK(s.movePage(b->page, moved->section, 1));
    EXPECT_EQ(navigator.activePage(), b->page);
    EXPECT_EQ(navigator.activeSection(), moved->section);
    EXPECT_EQ(navigator.open(core::PageId{}).error().code, core::ErrorCode::NotFound);
    session->setPatchListener({});
}

TEST_F(ShellWorkflowTest, UndoingThePageCreationLeavesAValidActivePage) {
    auto s = structure();
    auto a = s.createNotebook();
    ASSERT_OK(a);
    PageNavigator navigator(ws());
    session->setPatchListener([&](const document::Patch& patch) { navigator.onPatch(patch); });
    auto page = s.createPage(a->section);
    ASSERT_OK(page);
    ASSERT_OK(navigator.open(*page));
    ASSERT_OK(session->undo()); // the page is gone again
    EXPECT_EQ(navigator.activePage(), a->page);
    session->setPatchListener({});
}

TEST_F(ShellWorkflowTest, NavigatorOpensAPageAgainWhenPagesReturn) {
    auto s = structure();
    auto a = s.createNotebook();
    ASSERT_OK(a);
    auto second = s.createPage(a->section);
    ASSERT_OK(second);
    PageNavigator navigator(ws());
    session->setPatchListener([&](const document::Patch& patch) { navigator.onPatch(patch); });
    ASSERT_OK(navigator.open(a->page));
    ASSERT_OK(s.remove(a->notebook)); // nothing left to show
    EXPECT_FALSE(navigator.activePage().has_value());
    ASSERT_OK(session->undo()); // several pages come back at once
    EXPECT_EQ(navigator.activePage(), a->page);
    session->setPatchListener({});
}

TEST_F(ShellWorkflowTest, RenamingWithoutAChangeRecordsNothing) {
    auto s = structure();
    auto a = s.createNotebook("Maths");
    ASSERT_OK(a);
    const auto undoCount = session->history().undoCount();
    ASSERT_OK(s.rename(a->notebook, "Maths")); // e.g. F2, then Enter
    ASSERT_OK(s.rename(a->page, "Page 1"));
    EXPECT_EQ(session->history().undoCount(), undoCount);
}

TEST_F(ShellWorkflowTest, PreviousAndNextFollowWorkspaceOrder) {
    auto s = structure();
    auto a = s.createNotebook();
    ASSERT_OK(a);
    auto a2 = s.createPage(a->section);
    auto b = s.createNotebook();
    ASSERT_OK(a2);
    ASSERT_OK(b);
    PageNavigator navigator(ws());
    ASSERT_OK(navigator.open(*a2));
    EXPECT_EQ(navigator.previousPage(), a->page);
    EXPECT_EQ(navigator.nextPage(), b->page); // across notebooks
    ASSERT_OK(navigator.open(b->page));
    EXPECT_FALSE(navigator.nextPage().has_value());
    ASSERT_OK(navigator.open(a->page));
    EXPECT_FALSE(navigator.previousPage().has_value());
}

TEST_F(ShellWorkflowTest, PageChangedByNamesThePageAnEditTouched) {
    auto s = structure();
    auto a = s.createNotebook();
    ASSERT_OK(a);
    auto other = s.createPage(a->section);
    ASSERT_OK(other);
    draw(a->page);
    // The stroke's patch touches exactly the first page; its inverse too.
    const document::Command* last = session->history().nextUndo();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(pageChangedBy(last->patch, ws()), a->page);
    EXPECT_EQ(pageChangedBy(last->patch.inverted(), ws()), a->page);
    // Structure edits of several pages, or of none, name no page.
    ASSERT_OK(s.rename(a->section, "Renamed"));
    EXPECT_FALSE(pageChangedBy(session->history().nextUndo()->patch, ws()).has_value());
    ASSERT_OK(s.remove(a->section));
    EXPECT_FALSE(pageChangedBy(session->history().nextUndo()->patch.inverted(), ws()).has_value());
}

// ---------------------------------------------------------------------------- end to end

TEST_F(ShellWorkflowTest, EditsSurviveSwitchingPagesAndReopening) {
    auto start = ensureStartPage(*session, clock, idGenerator); // a new workspace's first page
    ASSERT_OK(start);
    auto s = structure();
    const core::SectionId section = ws().findPage(*start)->section;
    auto second = s.createPage(section, "Second");
    ASSERT_OK(second);
    PageNavigator navigator(ws());
    session->setPatchListener([&](const document::Patch& patch) { navigator.onPatch(patch); });

    ASSERT_OK(navigator.open(*start));
    const auto first = draw(*start);
    ASSERT_OK(navigator.open(*second)); // switching pages is not an edit
    const auto undoCount = session->history().undoCount();
    const auto other = draw(*second);
    ASSERT_OK(navigator.open(*start));
    EXPECT_EQ(session->history().undoCount(), undoCount + 1);

    // One workspace-wide history: undo from the first page removes the second page's
    // stroke, and names that page so the shell can show it.
    const document::Patch inverse = session->history().nextUndo()->patch.inverted();
    ASSERT_OK(session->undo());
    EXPECT_EQ(pageChangedBy(inverse, ws()), *second);
    EXPECT_EQ(ws().findElement(other), nullptr);
    ASSERT_OK(session->redo());
    session->setPatchListener({});

    reopen();
    EXPECT_NE(ws().findElement(first), nullptr);
    EXPECT_NE(ws().findElement(other), nullptr);
    EXPECT_EQ(ws().findPage(*second)->title, "Second");
    EXPECT_EQ(ids(ws().pagesOf(section)), (std::vector{*start, *second}));
    EXPECT_EQ(session->history().undoCount(), 0U); // history is per session
}

} // namespace
} // namespace studyapp::application
