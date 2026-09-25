// Patch representation, application, inversion and atomicity.

#include <studyapp/document/Patch.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

namespace studyapp::document::test {
namespace {

using core::ErrorCode;
using core::FractionalIndex;

NotebookInfo notebook(core::NotebookId id, std::string title) {
    return NotebookInfo{.id = id, .title = std::move(title), .order = FractionalIndex::first()};
}

TEST(PatchTest, ChangeKinds) {
    const NotebookInfo n = notebook(core::NotebookId{core::Uuid{{1}}}, "N");
    const NotebookChange create{std::nullopt, n};
    const NotebookChange remove{n, std::nullopt};
    const NotebookChange update{n, n};
    EXPECT_TRUE(create.isCreate());
    EXPECT_TRUE(remove.isRemove());
    EXPECT_TRUE(update.isUpdate());
    EXPECT_EQ(create.inverted(), remove);
}

TEST(PatchTest, InvertedReversesOrderAndSwapsStates) {
    TestWorkspace t;
    const NotebookInfo a = notebook(core::NotebookId{t.ids.next()}, "A");
    NotebookInfo renamed = a;
    renamed.title = "A2";
    const Patch patch({created(a), updated(a, renamed)});

    const Patch inverse = patch.inverted();
    ASSERT_EQ(inverse.size(), 2U);
    EXPECT_EQ(std::get<NotebookChange>(inverse.changes()[0]), (NotebookChange{renamed, a}));
    EXPECT_EQ(std::get<NotebookChange>(inverse.changes()[1]), (NotebookChange{a, std::nullopt}));
    EXPECT_EQ(inverse.inverted(), patch);
}

TEST(PatchTest, ApplyAndReverseRestoresState) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const Workspace before = t.workspace;

    auto command =
        commands::createElement(t.workspace, layer, {.payload = makeText("hello")}, t.ids);
    ASSERT_TRUE(command.has_value());
    ASSERT_OK(t.workspace.apply(command->command.patch));
    EXPECT_NE(t.workspace, before);
    ASSERT_OK(t.workspace.apply(command->command.patch.inverted()));
    EXPECT_EQ(t.workspace, before);
    EXPECT_OK(t.workspace.validate());
}

TEST(PatchTest, EmptyPatchIsNoOp) {
    TestWorkspace t;
    t.addNotebook("N");
    const Workspace before = t.workspace;
    EXPECT_OK(t.workspace.apply(Patch{}));
    EXPECT_EQ(t.workspace, before);
}

TEST(PatchTest, RejectsMalformedChanges) {
    TestWorkspace t;
    const NotebookInfo a = notebook(core::NotebookId{t.ids.next()}, "A");
    const NotebookInfo b = notebook(core::NotebookId{t.ids.next()}, "B");

    EXPECT_EQ(t.workspace.apply(Patch({NotebookChange{}})).error().code,
              ErrorCode::InvalidArgument); // neither before nor after
    ASSERT_OK(t.workspace.apply(Patch({created(a)})));
    EXPECT_EQ(t.workspace.apply(Patch({updated(a, b)})).error().code,
              ErrorCode::InvalidArgument); // id mismatch
    EXPECT_EQ(t.workspace.apply(Patch({removed(b)})).error().code, ErrorCode::NotFound);
}

TEST(PatchTest, StaleBeforeStateIsAConflict) {
    TestWorkspace t;
    const auto id = t.addNotebook("Current");
    NotebookInfo stale = *t.workspace.findNotebook(id);
    stale.title = "Something else";
    NotebookInfo after = stale;
    after.title = "New";

    const auto result = t.workspace.apply(Patch({updated(stale, after)}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict);
    EXPECT_EQ(t.workspace.findNotebook(id)->title, "Current");
}

TEST(PatchTest, FailingPatchIsRolledBackCompletely) {
    TestWorkspace t;
    const auto existing = t.addNotebook("Existing");
    const Workspace before = t.workspace;

    const NotebookInfo fresh = notebook(core::NotebookId{t.ids.next()}, "Fresh");
    NotebookInfo renamed = *t.workspace.findNotebook(existing);
    renamed.title = "Renamed";
    // Two valid changes followed by an invalid one (duplicate id).
    const Patch patch({created(fresh), updated(*t.workspace.findNotebook(existing), renamed),
                       created(notebook(existing, "Duplicate"))});

    const auto result = t.workspace.apply(patch);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, core::ErrorCode::AlreadyExists);
    EXPECT_NE(result.error().message.find("change 2"), std::string::npos) << result.error().message;
    EXPECT_EQ(t.workspace, before);
    EXPECT_OK(t.workspace.validate());
}

TEST(PatchTest, EndOfPatchInvariantFailureIsRolledBack) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto page = t.workspace.findLayer(layer)->page;
    t.addElement(layer, makeText("content"));
    const Workspace before = t.workspace;

    // Removing the element succeeds, removing the page's only layer then violates
    // "every page has a layer" at the end of the patch.
    const auto element = t.workspace.elementsOf(layer)[0];
    const Patch patch(
        {removed(*t.workspace.findElement(element)), removed(*t.workspace.findLayer(layer))});
    const auto result = t.workspace.apply(patch);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(t.workspace, before);
    EXPECT_EQ(t.workspace.layersOf(page).size(), 1U);
    EXPECT_OK(t.workspace.validate());
}

TEST(PatchTest, PatchesAreDataAndComparable) {
    TestWorkspace a;
    TestWorkspace b;
    const auto first = commands::createNotebook(a.workspace, "N", a.clock, a.ids);
    const auto second = commands::createNotebook(b.workspace, "N", b.clock, b.ids);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->command, second->command); // deterministic given clock + ids
}

} // namespace
} // namespace studyapp::document::test
