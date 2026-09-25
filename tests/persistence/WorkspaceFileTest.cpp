#include <studyapp/persistence/WorkspaceFile.hpp>

#include <studyapp/persistence/WorkspaceStore.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace studyapp::persistence {
namespace {

struct WorkspaceFileTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    document::WorkspaceInfo info{.id = core::WorkspaceId::generate(ids),
                                 .name = "Semester \xE2\x80\x94 1",
                                 .created = clock.now()};

    [[nodiscard]] std::filesystem::path root() const { return dir / "My Notes.studyws"; }
};

TEST_F(WorkspaceFileTest, CreateLaysOutTheDocumentedDirectory) {
    auto file = WorkspaceFile::create(root(), info, "0.1.0");
    ASSERT_OK(file);
    const WorkspaceLayout& layout = file->layout();
    EXPECT_TRUE(std::filesystem::is_regular_file(layout.database()));
    EXPECT_TRUE(std::filesystem::is_directory(layout.assets()));
    EXPECT_TRUE(std::filesystem::is_directory(layout.backups()));
    EXPECT_TRUE(std::filesystem::is_directory(layout.temporary()));
    EXPECT_EQ(layout.database().filename(), "workspace.db");
    EXPECT_FALSE(file->isReadOnly());
    EXPECT_EQ(file->migration().toVersion, currentSchemaVersion());
    EXPECT_OK(file->integrityCheck());

    auto meta = file->database().queryText(
        "SELECT value FROM workspace_meta WHERE key = 'created_by_version'");
    ASSERT_OK(meta);
    EXPECT_EQ(*meta, "0.1.0");
}

TEST_F(WorkspaceFileTest, CreateAcceptsAnEmptyDirectoryButNotAUsedOne) {
    std::filesystem::create_directories(root());
    EXPECT_OK(WorkspaceFile::create(root(), info, "0.1.0"));

    const auto used = dir / "used";
    std::filesystem::create_directories(used);
    std::ofstream(used / "notes.txt") << "unrelated";
    const auto refused = WorkspaceFile::create(used, info, "0.1.0");
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, core::ErrorCode::AlreadyExists);
    EXPECT_TRUE(std::filesystem::exists(used / "notes.txt")); // untouched
}

TEST_F(WorkspaceFileTest, CreateOverAnExistingWorkspaceFails) {
    ASSERT_OK(WorkspaceFile::create(root(), info, "0.1.0"));
    EXPECT_FALSE(WorkspaceFile::create(root(), info, "0.1.0").has_value());
}

TEST_F(WorkspaceFileTest, OpenRequiresAWorkspace) {
    const auto missing =
        WorkspaceFile::open(dir / "nothing here", AccessMode::ReadWrite, clock.now(), "0.1.0");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, core::ErrorCode::NotFound);
    EXPECT_FALSE(std::filesystem::exists(dir / "nothing here")); // nothing created
}

TEST_F(WorkspaceFileTest, CloseAndReopenPreservesMetadataAndRecordsWriter) {
    {
        auto file = WorkspaceFile::create(root(), info, "0.1.0");
        ASSERT_OK(file);
        EXPECT_OK(file->close());
    }
    auto reopened = WorkspaceFile::open(root(), AccessMode::ReadWrite, clock.now(), "0.2.0");
    ASSERT_OK(reopened);
    auto loaded = WorkspaceStore(reopened->database(), clock).load();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->info(), info);
    EXPECT_EQ(
        reopened->database()
            .queryText("SELECT value FROM workspace_meta WHERE key = 'last_written_by_version'")
            .value_or(""),
        "0.2.0");
    EXPECT_EQ(reopened->database()
                  .queryText("SELECT value FROM workspace_meta WHERE key = 'created_by_version'")
                  .value_or(""),
              "0.1.0");
}

TEST_F(WorkspaceFileTest, ReadOnlyOpenNeverWrites) {
    {
        auto file = WorkspaceFile::create(root(), info, "0.1.0");
        ASSERT_OK(file);
    }
    auto readOnly = WorkspaceFile::open(root(), AccessMode::ReadOnly, clock.now(), "9.9.9");
    ASSERT_OK(readOnly);
    EXPECT_TRUE(readOnly->isReadOnly());
    EXPECT_FALSE(readOnly->database().execute("DELETE FROM workspace_meta").has_value());
    EXPECT_EQ(
        readOnly->database()
            .queryText("SELECT value FROM workspace_meta WHERE key = 'last_written_by_version'")
            .value_or(""),
        "0.1.0");
    EXPECT_OK(WorkspaceStore(readOnly->database(), clock).load());
}

TEST_F(WorkspaceFileTest, BackupIsAConsistentOpenableSnapshot) {
    auto file = WorkspaceFile::create(root(), info, "0.1.0");
    ASSERT_OK(file);
    auto backup = file->backup("manual", clock.now());
    ASSERT_OK(backup);
    EXPECT_EQ(backup->parent_path(), file->layout().backups());
    EXPECT_EQ(backup->extension(), ".db");

    auto snapshot = Database::open(*backup, OpenMode::ReadOnly);
    ASSERT_OK(snapshot);
    auto loaded = WorkspaceStore(*snapshot, clock).load();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->info(), info);

    // Same name twice in the same millisecond is refused rather than overwritten.
    EXPECT_FALSE(file->backup("manual", clock.now()).has_value());
}

TEST_F(WorkspaceFileTest, CleanTemporaryEmptiesOnlyTemporary) {
    auto file = WorkspaceFile::create(root(), info, "0.1.0");
    ASSERT_OK(file);
    std::ofstream(file->layout().temporary() / "half-import.part") << "partial";
    std::filesystem::create_directories(file->layout().temporary() / "export");
    std::ofstream(file->layout().assets() / "keep.bin") << "asset";
    ASSERT_OK(file->cleanTemporary());
    EXPECT_TRUE(std::filesystem::is_empty(file->layout().temporary()));
    EXPECT_TRUE(std::filesystem::exists(file->layout().assets() / "keep.bin"));
}

TEST_F(WorkspaceFileTest, IntegrityCheckReportsForeignKeyViolations) {
    auto file = WorkspaceFile::create(root(), info, "0.1.0");
    ASSERT_OK(file);
    ASSERT_OK(file->database().execute(
        "PRAGMA foreign_keys = OFF;"
        "INSERT INTO section (id, notebook_id, title, sort_key, created_at, updated_at) "
        "VALUES (randomblob(16), randomblob(16), 'Orphan', 'a0', 0, 0);"
        "PRAGMA foreign_keys = ON;"));
    const auto checked = file->integrityCheck();
    ASSERT_FALSE(checked.has_value());
    EXPECT_NE(checked.error().message.find("section"), std::string::npos);
}

TEST_F(WorkspaceFileTest, WorkspaceDirectoryCanBeMoved) {
    {
        auto file = WorkspaceFile::create(root(), info, "0.1.0");
        ASSERT_OK(file);
    }
    // No absolute paths inside: a moved/copied workspace opens elsewhere.
    const auto moved = dir / "elsewhere" / "Renamed.studyws";
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::rename(root(), moved);
    auto reopened = WorkspaceFile::open(moved, AccessMode::ReadWrite, clock.now(), "0.1.0");
    ASSERT_OK(reopened);
    EXPECT_OK(WorkspaceStore(reopened->database(), clock).load());
}

std::string readBytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Audit P3-03: a crash during create() must not leave a directory that can neither be opened
// nor created again.
TEST_F(WorkspaceFileTest, InterruptedCreationCanBeCompleted) {
    // What an interrupted creation leaves behind: the subdirectories and a database whose
    // non-transactional settings (and, with earlier builds, the application id) were
    // written, but no schema.
    std::filesystem::create_directories(root() / "assets");
    std::filesystem::create_directories(root() / "backups");
    std::filesystem::create_directories(root() / "temporary");
    {
        auto leftover = Database::open(root() / "workspace.db", OpenMode::Create);
        ASSERT_OK(leftover);
        ASSERT_OK(leftover->execute("PRAGMA page_size = 4096; PRAGMA journal_mode = WAL;"
                                    "PRAGMA application_id = " +
                                    std::to_string(kApplicationId)));
    }

    // open() never initialises it (only create() writes schema + metadata together).
    const auto opened = WorkspaceFile::open(root(), AccessMode::ReadWrite, clock.now(), "0.1.0");
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().code, core::ErrorCode::Unsupported);
    {
        auto untouched = Database::open(root() / "workspace.db", OpenMode::ReadOnly);
        ASSERT_OK(untouched);
        EXPECT_EQ(schemaVersion(*untouched).value_or(-1), 0);
    }

    // create() completes it, and the result opens normally.
    EXPECT_OK(WorkspaceFile::checkCanCreate(root()));
    {
        auto created = WorkspaceFile::create(root(), info, "0.1.0");
        ASSERT_OK(created);
    }
    auto reopened = WorkspaceFile::open(root(), AccessMode::ReadWrite, clock.now(), "0.1.0");
    ASSERT_OK(reopened);
    auto loaded = WorkspaceStore(reopened->database(), clock).load();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->info(), info);
}

// Audit P3-08: arbitrary non-SQLite data in workspace.db is refused everywhere and never
// overwritten, reinitialised or removed.
TEST_F(WorkspaceFileTest, GarbageDatabaseIsRefusedAndLeftUntouched) {
    std::filesystem::create_directories(root());
    std::string garbage;
    for (int i = 0; i < 8192; ++i) {
        garbage.push_back(static_cast<char>((i * 31 + 7) % 251));
    }
    std::ofstream(root() / "workspace.db", std::ios::binary) << garbage;

    for (const AccessMode mode : {AccessMode::ReadWrite, AccessMode::ReadOnly}) {
        const auto opened = WorkspaceFile::open(root(), mode, clock.now(), "0.1.0");
        ASSERT_FALSE(opened.has_value());
        EXPECT_EQ(opened.error().code, core::ErrorCode::ParseError) << opened.error().message;
    }
    const auto creatable = WorkspaceFile::checkCanCreate(root());
    ASSERT_FALSE(creatable.has_value());
    EXPECT_EQ(creatable.error().code, core::ErrorCode::AlreadyExists);
    EXPECT_FALSE(WorkspaceFile::create(root(), info, "0.1.0").has_value());

    EXPECT_EQ(readBytes(root() / "workspace.db"), garbage);
    EXPECT_FALSE(std::filesystem::exists(root() / "assets")); // nothing was set up around it
}

} // namespace
} // namespace studyapp::persistence
