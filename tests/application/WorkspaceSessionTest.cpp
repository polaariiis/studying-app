#include <studyapp/application/WorkspaceSession.hpp>

#include <studyapp/document/Commands.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace studyapp::application {
namespace {

namespace commands = document::commands;
using document::test::makeConnector;
using document::test::makeStroke;
using document::test::makeText;

/// In-memory lock service: tests set the state other processes would have left.
class FakeLocker final : public WorkspaceLocker {
public:
    std::map<std::filesystem::path, LockStatus> external;
    std::set<std::filesystem::path> held;

    core::Result<LockStatus> inspect(const std::filesystem::path& lockFile) override {
        if (held.contains(lockFile)) {
            return LockStatus{
                .state = LockState::Active,
                .owner = LockOwner{.processId = 1, .hostName = "here", .applicationName = "test"}};
        }
        const auto it = external.find(lockFile);
        return it != external.end() ? it->second : LockStatus{};
    }

    core::Result<std::unique_ptr<WorkspaceLock>> acquire(const std::filesystem::path& lockFile,
                                                         bool takeOverStale) override {
        auto status = inspect(lockFile);
        if (status->state == LockState::Active ||
            (status->state == LockState::Stale && !takeOverStale)) {
            return core::makeError(core::ErrorCode::Conflict, "locked");
        }
        external.erase(lockFile);
        held.insert(lockFile);
        return std::unique_ptr<WorkspaceLock>(std::make_unique<Lock>(*this, lockFile));
    }

private:
    class Lock final : public WorkspaceLock {
    public:
        Lock(FakeLocker& owner, std::filesystem::path file)
            : owner_(&owner), file_(std::move(file)) {}
        ~Lock() override { owner_->held.erase(file_); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        Lock(Lock&&) = delete;
        Lock& operator=(Lock&&) = delete;

    private:
        FakeLocker* owner_;
        std::filesystem::path file_;
    };
};

struct WorkspaceSessionTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    FakeLocker locker;

    [[nodiscard]] std::filesystem::path root() const { return dir / "Semester.studyws"; }
    [[nodiscard]] std::filesystem::path lockFile() const { return root() / ".lock"; }
    SessionServices services() { return {clock, ids, locker}; }

    std::unique_ptr<WorkspaceSession> create() {
        auto session = WorkspaceSession::create(root(), "Semester", services());
        EXPECT_TRUE(session.has_value()) << (session ? "" : session.error().message);
        return session ? std::move(*session) : nullptr;
    }

    std::unique_ptr<WorkspaceSession> open(OpenOptions options = {}) {
        auto session = WorkspaceSession::open(root(), options, services());
        EXPECT_TRUE(session.has_value()) << (session ? "" : session.error().message);
        return session ? std::move(*session) : nullptr;
    }

    template <class Id>
    Id run(WorkspaceSession& session, core::Result<commands::Created<Id>> created) {
        EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().message);
        if (!created) {
            return Id{};
        }
        const auto executed = session.execute(std::move(created->command));
        EXPECT_TRUE(executed.has_value()) << (executed ? "" : executed.error().message);
        return created->id;
    }

    void run(WorkspaceSession& session, core::Result<document::Command> command) {
        ASSERT_TRUE(command.has_value()) << command.error().message;
        ASSERT_OK(session.execute(std::move(*command)));
    }

    core::LayerId addPath(WorkspaceSession& session) {
        const auto& ws = session.workspace();
        const auto notebook = run(session, commands::createNotebook(ws, "Notebook", clock, ids));
        const auto section =
            run(session, commands::createSection(ws, notebook, "Section", clock, ids));
        const auto page = run(session, commands::createPage(ws, section, "Page", {}, clock, ids));
        return ws.layersOf(page).front();
    }

    core::ElementId addElement(WorkspaceSession& session, core::LayerId layer,
                               document::ElementPayload payload,
                               document::Transform transform = {}) {
        return run(session, commands::createElement(
                                session.workspace(), layer,
                                {.transform = transform, .payload = std::move(payload)}, ids));
    }

    std::filesystem::path writeFile(const std::string& name, const std::string& content) {
        const auto path = dir / name;
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    /// Closes the session, reopens the workspace and checks the model is identical.
    std::unique_ptr<WorkspaceSession> reopenAndCompare(std::unique_ptr<WorkspaceSession> session) {
        const document::Workspace expected = session->workspace();
        EXPECT_OK(session->close());
        session.reset();
        auto reopened = open();
        if (reopened) {
            EXPECT_OK(reopened->workspace().validate());
            EXPECT_TRUE(reopened->workspace() == expected) << "reopened workspace differs";
        }
        return reopened;
    }
};

// ---------------------------------------------------------------------------- lifecycle

TEST_F(WorkspaceSessionTest, CreateCloseReopenEmptyWorkspace) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->workspace().info().name, "Semester");
    EXPECT_FALSE(session->isReadOnly());
    EXPECT_TRUE(locker.held.contains(lockFile()));
    EXPECT_TRUE(std::filesystem::is_regular_file(root() / "workspace.db"));
    const auto id = session->workspace().info().id;
    session = reopenAndCompare(std::move(session));
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->workspace().info().id, id);
}

TEST_F(WorkspaceSessionTest, CloseReleasesTheLock) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    ASSERT_OK(session->close());
    EXPECT_TRUE(session->isClosed());
    EXPECT_FALSE(locker.held.contains(lockFile()));
    EXPECT_FALSE(session->execute({"x", {}}).has_value()); // closed sessions reject edits
}

TEST_F(WorkspaceSessionTest, CreateRejectsBlankNamesAndUsedDirectories) {
    EXPECT_FALSE(WorkspaceSession::create(root(), "  ", services()).has_value());
    std::filesystem::create_directories(root());
    std::ofstream(root() / "unrelated.txt") << "x";
    const auto refused = WorkspaceSession::create(root(), "Semester", services());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, core::ErrorCode::AlreadyExists);
    EXPECT_TRUE(locker.held.empty());
}

TEST_F(WorkspaceSessionTest, OpenRequiresAnExistingWorkspace) {
    const auto missing = WorkspaceSession::open(root(), {}, services());
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, core::ErrorCode::NotFound);
    EXPECT_TRUE(locker.held.empty());
}

// ---------------------------------------------------------------------------- acceptance

// Phase 3 exit criterion: create a complex workspace → apply commands → persist → destroy the
// in-memory workspace → reopen the database → load → identical logical state.
TEST_F(WorkspaceSessionTest, ComplexWorkspaceSurvivesCloseAndReopen) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    const auto& ws = session->workspace();

    const auto maths = run(*session, commands::createNotebook(ws, "Mathematics", clock, ids));
    const auto physics = run(*session, commands::createNotebook(ws, "Physics", clock, ids));
    const auto scratch = run(*session, commands::createNotebook(ws, "Scratch", clock, ids));
    const auto algebra = run(*session, commands::createSection(ws, maths, "Algebra", clock, ids));
    const auto analysis = run(*session, commands::createSection(ws, maths, "Analysis", clock, ids));
    const auto mechanics =
        run(*session, commands::createSection(ws, physics, "Mechanics", clock, ids));
    const auto lecture =
        run(*session, commands::createPage(ws, algebra, "Lecture 1", {}, clock, ids));
    const auto sheet =
        run(*session,
            commands::createPage(ws, analysis, "Exercise sheet",
                                 {.extent = document::PageExtent::Bounded,
                                  .size = document::kA4PortraitSize,
                                  .background = {.pattern = document::BackgroundPattern::Ruled},
                                  .firstLayerName = "Background"},
                                 clock, ids));
    const auto notes = run(*session, commands::createPage(ws, mechanics, "Notes", {}, clock, ids));
    const auto ink = run(*session, commands::createLayer(ws, sheet, "Ink", ids));
    const auto lectureLayer = ws.layersOf(lecture).front();

    const auto asset =
        session->importAsset(writeFile("diagram.png", "\x89PNG diagram"), "image/png");
    ASSERT_OK(asset);

    clock.advance(std::chrono::seconds(30));
    const auto stroke =
        addElement(*session, lectureLayer,
                   document::Stroke{.brush = document::Brush::Pencil,
                                    .color = core::Color::fromRgba(20, 40, 60),
                                    .baseWidth = 1.25F,
                                    .points = document::makeStrokePoints(
                                        {{0, 0, 0.2F}, {5, 5, 0.8F}, {10, 0, 1}})},
                   {.position = {10.5, -20.25}, .rotation = 0.3F, .scale = {1.5F, 1.5F}});
    const auto box =
        addElement(*session, lectureLayer, makeText("Definition: a group is \xE2\x80\xA6"));
    const auto rect =
        addElement(*session, lectureLayer,
                   document::Shape{.kind = document::ShapeKind::Rectangle,
                                   .size = {100, 50},
                                   .fillColor = core::Color::fromRgba(255, 255, 0, 64)});
    addElement(*session, lectureLayer, makeConnector(stroke, rect));
    addElement(*session, lectureLayer, makeConnector(box, std::nullopt));
    addElement(*session, ink, document::Image{.asset = *asset, .size = {320, 200}},
               {.position = {40, 40}});
    addElement(*session, ink, makeStroke({{1, 1, 1}}));
    addElement(*session, ws.layersOf(notes).front(),
               document::Shape{.kind = document::ShapeKind::Line, .size = {80, 0}});

    // Renames, deletes (with connector detachment), undo and redo.
    run(*session, commands::renameNotebook(ws, physics, "Physics I", clock));
    run(*session, commands::renamePage(ws, lecture, "Lecture 1: Groups", clock));
    run(*session, commands::deleteElement(ws, rect)); // detaches the first connector
    run(*session, commands::deleteNotebook(ws, scratch));
    ASSERT_OK(session->undo()); // scratch notebook comes back
    ASSERT_OK(session->undo()); // rect comes back, connector re-attached
    ASSERT_OK(session->redo()); // rect deleted again
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    EXPECT_FALSE(session->lastWriteError().has_value());
    ASSERT_OK(session->workspace().validate());

    session = reopenAndCompare(std::move(session));
    ASSERT_NE(session, nullptr);
    const auto& loaded = session->workspace();
    EXPECT_EQ(loaded.notebookCount(), 3U);
    EXPECT_NE(loaded.findNotebook(scratch), nullptr);
    EXPECT_EQ(loaded.findElement(rect), nullptr);
    EXPECT_EQ(loaded.findNotebook(physics)->title, "Physics I");
    EXPECT_EQ(loaded.layersOf(sheet).size(), 2U);
    auto assetFile = session->assetPath(*asset);
    ASSERT_OK(assetFile);
    EXPECT_TRUE(std::filesystem::is_regular_file(*assetFile));

    // Undo history is an in-memory editing concern: a reopened session starts fresh.
    EXPECT_FALSE(session->canUndo());
}

TEST_F(WorkspaceSessionTest, RepeatedEditReopenCycles) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    const auto layer = addPath(*session);
    for (int cycle = 0; cycle < 5; ++cycle) {
        addElement(*session, layer, makeText("cycle " + std::to_string(cycle)));
        addElement(*session, layer, makeStroke({{static_cast<float>(cycle), 1, 1}}));
        session = reopenAndCompare(std::move(session));
        ASSERT_NE(session, nullptr);
    }
    EXPECT_EQ(session->workspace().elementCount(), 10U);
}

TEST_F(WorkspaceSessionTest, UndoneStateIsWhatGetsReopened) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    const auto layer = addPath(*session);
    const auto kept = addElement(*session, layer, makeText("kept"));
    const auto undone = addElement(*session, layer, makeText("undone"));
    ASSERT_OK(session->undo());
    session = reopenAndCompare(std::move(session));
    ASSERT_NE(session, nullptr);
    EXPECT_NE(session->workspace().findElement(kept), nullptr);
    EXPECT_EQ(session->workspace().findElement(undone), nullptr);
}

// ---------------------------------------------------------------------------- validation

TEST_F(WorkspaceSessionTest, FailedCommandsChangeNothing) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    const auto layer = addPath(*session);
    const document::Workspace before = session->workspace();
    const auto historyBefore = session->history().undoCount();

    // An image whose asset was never imported is rejected up front.
    auto image = commands::createElement(
        session->workspace(), layer,
        {.payload = document::Image{.asset = core::AssetId{ids.next()}, .size = {1, 1}}}, ids);
    ASSERT_OK(image);
    const auto rejected = session->execute(std::move(image->command));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, core::ErrorCode::NotFound);

    // Invalid patches are rejected by the document model.
    document::Patch conflicting({document::removed(*session->workspace().findLayer(layer))});
    EXPECT_FALSE(session->execute({"remove last layer", conflicting}).has_value());

    EXPECT_TRUE(session->workspace() == before);
    EXPECT_EQ(session->history().undoCount(), historyBefore);
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    session = reopenAndCompare(std::move(session));
}

// ---------------------------------------------------------------------------- write failures

TEST_F(WorkspaceSessionTest, WriteFailureKeepsTheEditQueuedAndRetries) {
    auto session = create();
    ASSERT_NE(session, nullptr);
    const auto layer = addPath(*session);

    // Fault injection from a second connection: every element insert now fails.
    {
        auto other =
            persistence::Database::open(root() / "workspace.db", persistence::OpenMode::ReadWrite);
        ASSERT_OK(other);
        ASSERT_OK(other->execute("CREATE TRIGGER inject_failure BEFORE INSERT ON element "
                                 "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;"));
    }
    const auto first = addElement(*session, layer, makeText("unsaved 1"));
    const auto second = addElement(*session, layer, makeText("unsaved 2"));
    // The edits stand in memory (never discarded) and are queued in order.
    EXPECT_NE(session->workspace().findElement(first), nullptr);
    EXPECT_NE(session->workspace().findElement(second), nullptr);
    EXPECT_EQ(session->pendingWriteCount(), 2U);
    ASSERT_TRUE(session->lastWriteError().has_value());
    EXPECT_NE(session->lastWriteError()->message.find("injected write failure"), std::string::npos);
    EXPECT_FALSE(session->flush().has_value());

    // Closing does not silently drop them: it fails and the session stays usable.
    EXPECT_FALSE(session->close().has_value());
    EXPECT_FALSE(session->isClosed());

    {
        auto other =
            persistence::Database::open(root() / "workspace.db", persistence::OpenMode::ReadWrite);
        ASSERT_OK(other);
        ASSERT_OK(other->execute("DROP TRIGGER inject_failure"));
    }
    ASSERT_OK(session->flush());
    EXPECT_EQ(session->pendingWriteCount(), 0U);
    EXPECT_FALSE(session->lastWriteError().has_value());
    session = reopenAndCompare(std::move(session));
    ASSERT_NE(session, nullptr);
    EXPECT_NE(session->workspace().findElement(second), nullptr);
}

// Audit P3-07: the destructor writes what is still pending. Writes are synchronous, so a
// pending write is produced with the fault injection used above, then the fault is removed
// before the session goes away without close().
TEST_F(WorkspaceSessionTest, DestructorFlushesPendingWrites) {
    document::Workspace expected{document::WorkspaceInfo{}};
    core::ElementId unsaved;
    {
        auto session = create();
        ASSERT_NE(session, nullptr);
        const auto layer = addPath(*session);
        {
            auto other = persistence::Database::open(root() / "workspace.db",
                                                     persistence::OpenMode::ReadWrite);
            ASSERT_OK(other);
            ASSERT_OK(other->execute("CREATE TRIGGER inject_failure BEFORE INSERT ON element "
                                     "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;"));
        }
        unsaved = addElement(*session, layer, makeText("pending at destruction"));
        ASSERT_EQ(session->pendingWriteCount(), 1U);
        {
            auto other = persistence::Database::open(root() / "workspace.db",
                                                     persistence::OpenMode::ReadWrite);
            ASSERT_OK(other);
            ASSERT_OK(other->execute("DROP TRIGGER inject_failure"));
        }
        ASSERT_EQ(session->pendingWriteCount(), 1U); // still only in memory
        expected = session->workspace();
    } // destroyed without close(): the destructor must write the pending patch
    auto reopened = open();
    ASSERT_NE(reopened, nullptr);
    EXPECT_NE(reopened->workspace().findElement(unsaved), nullptr);
    EXPECT_TRUE(reopened->workspace() == expected);
}

// ---------------------------------------------------------------------------- locking

TEST_F(WorkspaceSessionTest, ActiveLockAllowsOnlyReadOnlyAccess) {
    auto writer = create();
    ASSERT_NE(writer, nullptr);
    addPath(*writer);

    auto status = WorkspaceSession::inspectLock(root(), locker);
    ASSERT_OK(status);
    EXPECT_EQ(status->state, LockState::Active);

    const auto second = WorkspaceSession::open(root(), {}, services());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, core::ErrorCode::Conflict);
    EXPECT_NE(second.error().message.find("read-only"), std::string::npos);

    auto reader = open({.mode = AccessMode::ReadOnly});
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->isReadOnly());
    EXPECT_TRUE(reader->workspace() == writer->workspace());
    auto created = commands::createNotebook(reader->workspace(), "Nope", clock, ids);
    ASSERT_OK(created);
    const auto edit = reader->execute(std::move(created->command));
    ASSERT_FALSE(edit.has_value());
    EXPECT_EQ(edit.error().code, core::ErrorCode::Unsupported);
    EXPECT_FALSE(reader->importAsset(writeFile("a.png", "a"), "image/png").has_value());
}

TEST_F(WorkspaceSessionTest, StaleLockRequiresExplicitRecovery) {
    {
        auto session = create();
        ASSERT_NE(session, nullptr);
        addPath(*session);
        ASSERT_OK(session->close());
    }
    // A crashed session left its lock and a half-finished import behind.
    locker.external[lockFile()] = LockStatus{
        .state = LockState::Stale,
        .owner = LockOwner{.processId = 4242, .hostName = "here", .applicationName = "studyapp"}};
    std::ofstream(root() / "temporary" / "import.part") << "partial";

    const auto refused = WorkspaceSession::open(root(), {}, services());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, core::ErrorCode::Conflict);
    EXPECT_NE(refused.error().message.find("4242"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(root() / "temporary" / "import.part"));

    auto recovered = open({.mode = AccessMode::ReadWrite, .recoverStaleLock = true});
    ASSERT_NE(recovered, nullptr);
    EXPECT_TRUE(recovered->recoveredStaleLock());
    EXPECT_TRUE(std::filesystem::is_empty(root() / "temporary"));
    EXPECT_EQ(recovered->workspace().notebookCount(), 1U);
}

TEST_F(WorkspaceSessionTest, RecoveryRefusesACorruptDatabase) {
    {
        auto session = create();
        ASSERT_NE(session, nullptr);
        addPath(*session);
        ASSERT_OK(session->close());
    }
    {
        auto db =
            persistence::Database::open(root() / "workspace.db", persistence::OpenMode::ReadWrite);
        ASSERT_OK(db);
        ASSERT_OK(db->execute(
            "PRAGMA foreign_keys = OFF; UPDATE section SET notebook_id = randomblob(16);"));
    }
    locker.external[lockFile()] = LockStatus{.state = LockState::Stale, .owner = std::nullopt};
    const auto recovered = WorkspaceSession::open(root(), {.recoverStaleLock = true}, services());
    ASSERT_FALSE(recovered.has_value());
    EXPECT_NE(recovered.error().message.find("integrity"), std::string::npos);
    EXPECT_FALSE(locker.held.contains(lockFile())); // not left locked after the failure
}

} // namespace
} // namespace studyapp::application
