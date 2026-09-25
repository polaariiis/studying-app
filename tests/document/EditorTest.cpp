// Undo/redo through the Editor.

#include <studyapp/document/Editor.hpp>

#include "TestWorkspace.hpp"

#include <studyapp/document/UndoStack.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace studyapp::document::test {
namespace {

using core::ErrorCode;

TEST(EditorTest, UndoAndRedoOnEmptyHistoryAreSafe) {
    TestWorkspace t;
    const Workspace before = t.workspace;
    EXPECT_FALSE(t.editor.canUndo());
    EXPECT_FALSE(t.editor.canRedo());
    EXPECT_EQ(t.editor.undo().error().code, ErrorCode::NotFound);
    EXPECT_EQ(t.editor.redo().error().code, ErrorCode::NotFound);
    EXPECT_EQ(t.workspace, before);
}

TEST(EditorTest, ExecuteThenUndoRestoresPreviousState) {
    TestWorkspace t;
    const Workspace before = t.workspace;
    const auto notebook = t.addNotebook("N");
    EXPECT_TRUE(t.editor.canUndo());
    EXPECT_EQ(t.editor.history().undoLabel(), "Create notebook");

    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(t.workspace, before);
    EXPECT_EQ(t.workspace.findNotebook(notebook), nullptr);
    EXPECT_TRUE(t.editor.canRedo());
    EXPECT_EQ(t.editor.history().redoLabel(), "Create notebook");
}

TEST(EditorTest, RedoRestoresPostCommandStateWithSameIds) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const Workspace after = t.workspace;
    ASSERT_OK(t.editor.undo());
    ASSERT_OK(t.editor.redo());
    EXPECT_EQ(t.workspace, after);
    EXPECT_NE(t.workspace.findNotebook(notebook), nullptr); // identity preserved
    EXPECT_FALSE(t.editor.canRedo());
}

TEST(EditorTest, MultipleUndoAndRedo) {
    TestWorkspace t;
    std::vector<Workspace> states{t.workspace};
    const auto notebook = t.addNotebook("N");
    states.push_back(t.workspace);
    const auto section = t.addSection(notebook, "S");
    states.push_back(t.workspace);
    t.addPage(section, "P");
    states.push_back(t.workspace);
    t.run(commands::renameNotebook(t.workspace, notebook, "Renamed", t.clock));
    states.push_back(t.workspace);

    for (std::size_t i = states.size() - 1; i-- > 0;) {
        ASSERT_OK(t.editor.undo());
        EXPECT_EQ(t.workspace, states[i]) << "after undo to state " << i;
    }
    EXPECT_FALSE(t.editor.canUndo());
    for (std::size_t i = 1; i < states.size(); ++i) {
        ASSERT_OK(t.editor.redo());
        EXPECT_EQ(t.workspace, states[i]) << "after redo to state " << i;
    }
    EXPECT_FALSE(t.editor.canRedo());
}

TEST(EditorTest, NewCommandAfterUndoClearsRedoBranch) {
    TestWorkspace t;
    t.addNotebook("A");
    t.addNotebook("B");
    ASSERT_OK(t.editor.undo());
    EXPECT_TRUE(t.editor.canRedo());

    t.addNotebook("C");
    EXPECT_FALSE(t.editor.canRedo());
    EXPECT_EQ(t.editor.redo().error().code, ErrorCode::NotFound);
    EXPECT_EQ(t.editor.history().undoCount(), 2U);
    EXPECT_EQ(t.workspace.notebookCount(), 2U);
}

TEST(EditorTest, FailedCommandIsNotRecordedAndChangesNothing) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const Workspace before = t.workspace;
    const auto undoCount = t.editor.history().undoCount();

    // Build a command against a stale view: the notebook is renamed in between.
    auto stale = commands::renameNotebook(t.workspace, notebook, "Stale", t.clock);
    t.run(commands::renameNotebook(t.workspace, notebook, "Fresh", t.clock));
    const Workspace afterFresh = t.workspace;
    ASSERT_TRUE(stale.has_value());
    const auto result = t.editor.execute(std::move(*stale));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
    EXPECT_EQ(t.workspace, afterFresh);
    EXPECT_EQ(t.editor.history().undoCount(), undoCount + 1); // only "Fresh" was recorded

    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(t.workspace, before);
}

TEST(EditorTest, FailedCommandDoesNotClearRedo) {
    TestWorkspace t;
    t.addNotebook("A");
    ASSERT_OK(t.editor.undo());
    const auto bad = t.editor.execute(
        Command{"Bad", Patch({removed(NotebookInfo{.id = core::NotebookId{t.ids.next()},
                                                   .title = "missing",
                                                   .order = core::FractionalIndex::first()})})});
    EXPECT_FALSE(bad.has_value());
    EXPECT_TRUE(t.editor.canRedo());
}

TEST(EditorTest, EmptyCommandIsNotRecorded) {
    TestWorkspace t;
    EXPECT_OK(t.editor.execute(Command{"Nothing", Patch{}}));
    EXPECT_FALSE(t.editor.canUndo());
}

TEST(EditorTest, UndoOfDeleteRestoresSubtreeExactly) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto a = t.addElement(layer, makeText("a"));
    const auto b = t.addElement(layer, makeStroke());
    t.addElement(layer, makeConnector(a, b));
    const Workspace before = t.workspace;

    t.run(commands::deleteNotebook(t.workspace, t.workspace.notebooks()[0]));
    EXPECT_EQ(t.workspace.notebookCount(), 0U);
    ASSERT_OK(t.editor.undo());
    EXPECT_EQ(t.workspace, before);
    EXPECT_OK(t.workspace.validate());
    ASSERT_OK(t.editor.redo());
    EXPECT_EQ(t.workspace.elementCount(), 0U);
    EXPECT_OK(t.workspace.validate());
}

TEST(EditorTest, HistoryCapacityDropsOldestEntries) {
    TestWorkspace t;
    Editor editor(t.workspace, 3);
    for (int i = 0; i < 5; ++i) {
        auto created =
            commands::createNotebook(t.workspace, "N" + std::to_string(i), t.clock, t.ids);
        ASSERT_OK(editor.execute(std::move(created->command)));
    }
    EXPECT_EQ(editor.history().undoCount(), 3U);
    for (int i = 0; i < 3; ++i) {
        ASSERT_OK(editor.undo());
    }
    EXPECT_FALSE(editor.canUndo());
    EXPECT_EQ(t.workspace.notebookCount(), 2U); // the two oldest can no longer be undone
}

TEST(EditorTest, UndoSnapshotsShareStrokePoints) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const Stroke stroke = makeStroke(std::vector<StrokePoint>(1000, StrokePoint{1, 2, 0.5F}));
    const auto element = t.addElement(layer, stroke);
    t.run(commands::deleteElement(t.workspace, element));
    ASSERT_OK(t.editor.undo());

    const auto& restored = std::get<Stroke>(t.workspace.findElement(element)->payload);
    EXPECT_EQ(restored.points.get(), stroke.points.get()); // same array, not a copy
}

// The nested scenario from the Phase 2 requirements: build every level, modify several
// levels, then undo and redo each step, checking identity and hierarchy every time.
TEST(EditorTest, NestedHierarchyUndoRedoScenario) {
    TestWorkspace t;
    std::vector<Workspace> states{t.workspace};
    const auto snapshot = [&] {
        EXPECT_OK(t.workspace.validate());
        states.push_back(t.workspace);
    };

    const auto notebook = t.addNotebook("Biology");
    snapshot();
    const auto section = t.addSection(notebook, "Cells");
    snapshot();
    const auto page = t.addPage(section, "Mitosis");
    snapshot();
    const auto layer = t.addLayer(page, "Sketches");
    snapshot();
    const auto element = t.addElement(layer, makeStroke());
    snapshot();
    t.run(commands::renameNotebook(t.workspace, notebook, "Biology 101", t.clock));
    snapshot();
    t.run(commands::renamePage(t.workspace, page, "Mitosis phases", t.clock));
    snapshot();
    t.run(commands::renameLayer(t.workspace, layer, "Diagrams"));
    snapshot();

    const auto checkHierarchy = [&](std::size_t state) {
        // Ids never change; each object exists exactly in the states after its creation.
        EXPECT_EQ(t.workspace.findNotebook(notebook) != nullptr, state >= 1);
        EXPECT_EQ(t.workspace.findSection(section) != nullptr, state >= 2);
        EXPECT_EQ(t.workspace.findPage(page) != nullptr, state >= 3);
        EXPECT_EQ(t.workspace.findLayer(layer) != nullptr, state >= 4);
        EXPECT_EQ(t.workspace.findElement(element) != nullptr, state >= 5);
        if (state >= 5) {
            EXPECT_EQ(t.workspace.findElement(element)->layer, layer);
            EXPECT_EQ(t.workspace.findLayer(layer)->page, page);
            EXPECT_EQ(t.workspace.findPage(page)->section, section);
            EXPECT_EQ(t.workspace.findSection(section)->notebook, notebook);
        }
        EXPECT_OK(t.workspace.validate());
    };

    for (std::size_t i = states.size() - 1; i-- > 0;) {
        ASSERT_OK(t.editor.undo());
        EXPECT_EQ(t.workspace, states[i]) << "undo to state " << i;
        checkHierarchy(i);
    }
    for (std::size_t i = 1; i < states.size(); ++i) {
        ASSERT_OK(t.editor.redo());
        EXPECT_EQ(t.workspace, states[i]) << "redo to state " << i;
        checkHierarchy(i);
    }
    EXPECT_EQ(t.workspace.findNotebook(notebook)->title, "Biology 101");
    EXPECT_EQ(t.workspace.findPage(page)->title, "Mitosis phases");
    EXPECT_EQ(t.workspace.findLayer(layer)->name, "Diagrams");
}

} // namespace
} // namespace studyapp::document::test
