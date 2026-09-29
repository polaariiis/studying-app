// Phase 8, step 1: full-text search over page titles, text boxes and tasks against a real
// WorkspaceSession (SQLite FTS5). The index follows every edit, undo and redo; results are
// resolved against the workspace; old workspaces are re-indexed once when opened.

#include <studyapp/application/Search.hpp>

#include <studyapp/application/Planner.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
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

struct SearchTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    NoLocks locker;
    std::unique_ptr<WorkspaceSession> session;
    core::PageId page;
    core::LayerId layer;

    SessionServices services() { return {clock, ids, locker}; }
    const document::Workspace& ws() const { return session->workspace(); }

    void SetUp() override {
        auto created = WorkspaceSession::create(dir / "ws", "Search", services());
        ASSERT_OK(created);
        session = std::move(*created);
        WorkspaceStructure structure(*session, clock, ids);
        auto notebook = structure.createNotebook("Biology");
        ASSERT_OK(notebook);
        page = notebook->page;
        ASSERT_OK(structure.rename(page, "Photosynthesis"));
        layer = ws().layersOf(page).front();
    }

    void reopen(OpenOptions options = {}) {
        if (session) {
            ASSERT_OK(session->close());
            session.reset();
        }
        auto opened = WorkspaceSession::open(dir / "ws", options, services());
        ASSERT_OK(opened);
        session = std::move(*opened);
    }

    core::ElementId addText(std::string text) {
        auto created = document::commands::createElement(
            ws(), layer, {.payload = document::TextBox{.size = {200, 40}, .text = std::move(text)}},
            ids);
        EXPECT_TRUE(created.has_value());
        EXPECT_TRUE(session->execute(std::move(created->command)).has_value());
        return created->id;
    }

    std::vector<std::string> titles(std::string_view query) {
        auto results = search(*session, query);
        EXPECT_TRUE(results.has_value()) << (results ? "" : results.error().message);
        std::vector<std::string> out;
        for (const SearchResult& r : results.value_or(std::vector<SearchResult>{})) {
            out.push_back(r.title);
        }
        return out;
    }
};

TEST_F(SearchTest, FindsPageTitlesTextBoxesAndTasksWithJumpTargets) {
    const auto box = addText("Light reactions happen in the thylakoid membrane");
    Planner planner(*session, clock, ids);
    auto task = planner.createTask({.title = "Revise the Calvin cycle"});
    ASSERT_OK(task);
    study::Task withNotes = *ws().findTask(*task);
    withNotes.notes = "Draw the photosynthesis diagram";
    ASSERT_OK(planner.updateTask(withNotes));

    auto results = search(*session, "photosynth");
    ASSERT_OK(results);
    ASSERT_EQ(results->size(), 2U);
    // The page title ranks above a match in the task's notes.
    EXPECT_EQ((*results)[0].kind, SearchResult::Kind::Page);
    EXPECT_EQ((*results)[0].page, page);
    EXPECT_EQ((*results)[0].context, "Biology › Section 1");
    EXPECT_EQ((*results)[1].kind, SearchResult::Kind::Task);
    EXPECT_EQ((*results)[1].task, *task);
    EXPECT_EQ((*results)[1].snippet, "Draw the photosynthesis diagram");

    auto inText = search(*session, "thylak"); // a prefix of the word being typed
    ASSERT_OK(inText);
    ASSERT_EQ(inText->size(), 1U);
    EXPECT_EQ((*inText)[0].kind, SearchResult::Kind::TextBox);
    EXPECT_EQ((*inText)[0].element, box);
    EXPECT_EQ((*inText)[0].page, page);
    EXPECT_EQ((*inText)[0].title, "Light reactions happen in the thylakoid membrane");
    EXPECT_EQ(titles("calvin cycle").size(), 1U); // all words must match
    EXPECT_TRUE(titles("calvin membrane").empty());
    EXPECT_TRUE(titles("   ").empty());
}

TEST_F(SearchTest, TheIndexFollowsEditsUndoRedoAndDeletes) {
    const auto box = addText("mitochondria");
    EXPECT_EQ(titles("mitochondria").size(), 1U);
    auto edit = document::commands::editText(ws(), box, "ribosome", {200, 40});
    ASSERT_OK(edit);
    ASSERT_OK(session->execute(std::move(*edit)));
    EXPECT_TRUE(titles("mitochondria").empty());
    EXPECT_EQ(titles("ribosome").size(), 1U);
    ASSERT_OK(session->undo());
    EXPECT_EQ(titles("mitochondria").size(), 1U);
    EXPECT_TRUE(titles("ribosome").empty());
    ASSERT_OK(session->redo());
    EXPECT_EQ(titles("ribosome").size(), 1U);

    // Moving the box does not touch the index; it is still found once.
    auto move = document::commands::moveElements(ws(), std::vector{box}, {10, 10});
    ASSERT_OK(move);
    ASSERT_OK(session->execute(std::move(*move)));
    EXPECT_EQ(titles("ribosome").size(), 1U);

    // Renaming and deleting the page; undo brings everything back.
    WorkspaceStructure structure(*session, clock, ids);
    ASSERT_OK(structure.rename(page, "Cell organelles"));
    EXPECT_TRUE(titles("photosynthesis").empty());
    EXPECT_EQ(titles("organelles").size(), 1U);
    ASSERT_OK(structure.remove(page));
    EXPECT_TRUE(titles("organelles").empty());
    EXPECT_TRUE(titles("ribosome").empty()); // the page's text boxes went with it
    ASSERT_OK(session->undo());
    EXPECT_EQ(titles("organelles").size(), 1U);
    EXPECT_EQ(titles("ribosome").size(), 1U);

    // Tasks: rename, delete, undo.
    Planner planner(*session, clock, ids);
    auto task = planner.createTask({.title = "Quiz on enzymes"});
    ASSERT_OK(task);
    EXPECT_EQ(titles("enzymes").size(), 1U);
    ASSERT_OK(planner.deleteTask(*task));
    EXPECT_TRUE(titles("enzymes").empty());
    ASSERT_OK(session->undo());
    EXPECT_EQ(titles("enzymes").size(), 1U);
}

TEST_F(SearchTest, UnicodeDiacriticsCaseAndSpecialCharacters) {
    addText("Café Übung naïve résumé");
    addText("Привет мир — 漢字");
    EXPECT_EQ(titles("cafe").size(), 1U);   // diacritics removed
    EXPECT_EQ(titles("UBUNG").size(), 1U);  // case folded
    EXPECT_EQ(titles("привет").size(), 1U); // non-Latin case folding
    EXPECT_EQ(titles("МИР").size(), 1U);
    // FTS operators and punctuation are plain text, never a syntax error.
    for (const char* query : {"\"", "\"café", "AND", "OR NOT", "NEAR(", "*", "a*b", "-", "(", "c++",
                              "résumé\"", "' OR 1=1 --", "col:value", "^caf"}) {
        auto results = search(*session, query);
        EXPECT_TRUE(results.has_value())
            << query << ": " << (results ? "" : results.error().message);
    }
    EXPECT_EQ(titles("\"cafe\"").size(), 1U);
}

TEST_F(SearchTest, SurvivesReopenAndOldWorkspacesAreReindexedOnce) {
    addText("chloroplast");
    reopen();
    EXPECT_EQ(titles("chloroplast").size(), 1U);
    // A workspace from before Phase 8 has an empty index and no version: simulate it.
    ASSERT_OK(session->close());
    session.reset();
    {
        auto db = persistence::Database::open(dir / "ws" / "workspace.db",
                                              persistence::OpenMode::ReadWrite);
        ASSERT_OK(db);
        ASSERT_OK(db->execute("INSERT INTO search_index (search_index) VALUES ('delete-all');"
                              "DELETE FROM search_doc;"
                              "DELETE FROM workspace_meta WHERE key = 'search_index_version';"));
    }
    // Read-only sessions do not write: nothing is found until opened read-write.
    reopen({.mode = AccessMode::ReadOnly});
    EXPECT_TRUE(titles("chloroplast").empty());
    EXPECT_FALSE(session->isSearchIndexed()); // the shell says so instead of "No results"
    reopen();
    EXPECT_TRUE(session->isSearchIndexed());
    EXPECT_EQ(titles("chloroplast").size(), 1U);
    EXPECT_EQ(titles("photosynthesis").size(), 1U);
    reopen({.mode = AccessMode::ReadOnly});
    EXPECT_EQ(titles("chloroplast").size(), 1U);
}

TEST_F(SearchTest, LimitAndSnippets) {
    for (int i = 0; i < 30; ++i) {
        addText("note " + std::to_string(i) + " about glycolysis");
    }
    auto limited = search(*session, "glycolysis", 10);
    ASSERT_OK(limited);
    EXPECT_EQ(limited->size(), 10U);
    const std::string text =
        "The first part is long enough to be cut before the interesting word glycolysis, and "
        "then the text goes on for quite a while after it as well, much longer than a snippet.";
    const std::string snippet = snippetOf(text, "glyco", 60);
    EXPECT_NE(snippet.find("glycolysis"), std::string::npos);
    EXPECT_EQ(snippet.rfind("…", 0), 0U); // cut at the start
    EXPECT_LE(snippet.size(), 60U + 6U);
    EXPECT_EQ(snippetOf("héllo wörld", "wörld", 7).find("…"), 0U); // UTF-8 boundaries
    EXPECT_EQ(snippetOf("", "x"), "");
}

} // namespace
} // namespace studyapp::application
