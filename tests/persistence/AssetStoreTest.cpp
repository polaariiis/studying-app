#include <studyapp/persistence/AssetStore.hpp>

#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace studyapp::persistence {
namespace {

using namespace std::chrono_literals;

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

std::size_t fileCount(const std::filesystem::path& directory) {
    std::size_t count = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file()) {
            ++count;
        }
    }
    return count;
}

struct AssetStoreTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    std::optional<WorkspaceFile> file;
    std::optional<AssetStore> assets;

    void SetUp() override {
        auto created = WorkspaceFile::create(
            dir / "ws",
            document::WorkspaceInfo{
                .id = core::WorkspaceId::generate(ids), .name = "Assets", .created = clock.now()},
            "test");
        ASSERT_OK(created);
        file.emplace(std::move(*created));
        assets.emplace(file->database(), file->layout().root);
    }

    std::filesystem::path source(const std::string& name, const std::string& content) {
        const auto path = dir / name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }

    core::AssetId import(const std::string& content, std::string_view type = "image/png",
                         const std::string& name = "picture.png") {
        auto id = assets->import(source(name, content), type, ids, clock);
        EXPECT_TRUE(id.has_value()) << (id ? "" : id.error().message);
        return id.value_or(core::AssetId{});
    }

    std::int64_t rows() {
        return file->database().queryInt("SELECT count(*) FROM asset").value_or(-1);
    }
};

TEST_F(AssetStoreTest, ImportStoresContentAddressedFile) {
    const std::string content = "\x89PNG\r\n\x1a\n not really a png";
    const auto id = import(content);

    auto info = assets->find(id);
    ASSERT_OK(info);
    ASSERT_TRUE(info->has_value());
    const auto digest =
        Sha256::of({reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
    EXPECT_EQ((*info)->sha256, digest);
    EXPECT_EQ((*info)->mediaType, "image/png");
    EXPECT_EQ((*info)->byteSize, content.size());
    EXPECT_EQ((*info)->originalName, "picture.png");

    // assets/<h0h1>/<h2h3>/<sha256>.png, relative to the workspace root.
    const std::string hex = toHex(digest);
    const auto expected =
        std::filesystem::path("assets") / hex.substr(0, 2) / hex.substr(2, 2) / (hex + ".png");
    EXPECT_EQ(AssetStore::relativePath(digest, "image/png"), expected);
    auto path = assets->pathOf(id);
    ASSERT_OK(path);
    EXPECT_EQ(*path, file->layout().root / expected);
    EXPECT_EQ(readAll(*path), content);

    // Nothing is left behind in temporary/.
    EXPECT_EQ(fileCount(file->layout().temporary()), 0U);
}

TEST_F(AssetStoreTest, IdenticalContentIsStoredOnce) {
    const auto first = import("same bytes", "application/pdf", "a.pdf");
    const auto second = import("same bytes", "application/pdf", "b.pdf");
    EXPECT_EQ(first, second);
    EXPECT_EQ(rows(), 1);
    EXPECT_EQ(fileCount(file->layout().assets()), 1U);

    const auto other = import("different bytes", "application/pdf", "c.pdf");
    EXPECT_NE(other, first);
    EXPECT_EQ(rows(), 2);
    EXPECT_EQ(fileCount(file->layout().assets()), 2U);
}

TEST_F(AssetStoreTest, EmptyFilesAreValidAssets) {
    const auto id = import("", "application/octet-stream", "empty.bin");
    auto path = assets->pathOf(id);
    ASSERT_OK(path);
    EXPECT_EQ(path->extension(), ".bin");
    EXPECT_TRUE(std::filesystem::is_regular_file(*path));
}

TEST_F(AssetStoreTest, MissingSourceFailsWithoutSideEffects) {
    const auto id = assets->import(dir / "does-not-exist.png", "image/png", ids, clock);
    ASSERT_FALSE(id.has_value());
    EXPECT_EQ(id.error().code, core::ErrorCode::NotFound);
    EXPECT_EQ(rows(), 0);
    EXPECT_EQ(fileCount(file->layout().assets()), 0U);
    EXPECT_EQ(fileCount(file->layout().temporary()), 0U);
}

// Audit P3-05: on POSIX, ifstream opens a directory successfully and reads nothing, which
// used to import the directory as an empty asset.
TEST_F(AssetStoreTest, DirectoriesAreNotImported) {
    const auto directory = dir / "a directory.png";
    std::filesystem::create_directories(directory);
    const auto imported = assets->import(directory, "image/png", ids, clock);
    ASSERT_FALSE(imported.has_value());
    EXPECT_EQ(imported.error().code, core::ErrorCode::InvalidArgument);
    EXPECT_EQ(rows(), 0);
    EXPECT_EQ(fileCount(file->layout().assets()), 0U);
    EXPECT_EQ(fileCount(file->layout().temporary()), 0U);
}

TEST_F(AssetStoreTest, UnknownAssetsAreReported) {
    const core::AssetId unknown{ids.next()};
    auto exists = assets->exists(unknown);
    ASSERT_OK(exists);
    EXPECT_FALSE(*exists);
    const auto path = assets->pathOf(unknown);
    ASSERT_FALSE(path.has_value());
    EXPECT_EQ(path.error().code, core::ErrorCode::NotFound);
}

// Crash ordering (docs/DATABASE_SCHEMA.md §3.3): a failure after the file is in place but
// before the row is committed leaves at worst an orphan file, never a row without a file.
TEST_F(AssetStoreTest, FailureBeforeTheRowLeavesOnlyAnOrphanFile) {
    ASSERT_OK(file->database().execute("CREATE TRIGGER fail_import BEFORE INSERT ON asset "
                                       "BEGIN SELECT RAISE(ABORT, 'simulated crash'); END;"));
    const auto failed = assets->import(source("x.png", "orphan content"), "image/png", ids, clock);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(rows(), 0);                                 // no dangling row
    EXPECT_EQ(fileCount(file->layout().assets()), 1U);    // the orphan file
    EXPECT_EQ(fileCount(file->layout().temporary()), 0U); // nothing staged

    // GC removes the orphan; a later import of the same content works normally.
    auto report = assets->collectGarbage(clock.now(), 0ms);
    ASSERT_OK(report);
    EXPECT_EQ(report->filesDeleted, 1U);
    EXPECT_EQ(fileCount(file->layout().assets()), 0U);

    ASSERT_OK(file->database().execute("DROP TRIGGER fail_import"));
    const auto id = import("orphan content");
    EXPECT_EQ(rows(), 1);
    auto problems = assets->verify();
    ASSERT_OK(problems);
    EXPECT_TRUE(problems->empty());
    (void)id;
}

TEST_F(AssetStoreTest, ImportReusesAnOrphanFileWithTheSameContent) {
    ASSERT_OK(file->database().execute("CREATE TRIGGER fail_import BEFORE INSERT ON asset "
                                       "BEGIN SELECT RAISE(ABORT, 'simulated crash'); END;"));
    EXPECT_FALSE(assets->import(source("x.png", "content"), "image/png", ids, clock).has_value());
    ASSERT_OK(file->database().execute("DROP TRIGGER fail_import"));

    const auto id = import("content");
    EXPECT_EQ(fileCount(file->layout().assets()), 1U);
    auto path = assets->pathOf(id);
    ASSERT_OK(path);
    EXPECT_EQ(readAll(*path), "content");
}

TEST_F(AssetStoreTest, GarbageCollectionHonoursReferencesAndGracePeriod) {
    const auto old = import("old and unreferenced", "image/png", "old.png");
    clock.advance(std::chrono::hours(24 * 10));
    const auto young = import("young and unreferenced", "image/png", "young.png");

    // Default grace period (7 days): only the old asset goes, row first, then its file.
    auto report = assets->collectGarbage(clock.now(), std::chrono::hours(24 * 7));
    ASSERT_OK(report);
    EXPECT_EQ(report->rowsDeleted, 1U);
    EXPECT_EQ(report->filesDeleted, 1U);
    EXPECT_FALSE(assets->exists(old).value_or(true));
    EXPECT_TRUE(assets->exists(young).value_or(false));
    auto youngPath = assets->pathOf(young);
    ASSERT_OK(youngPath);
    EXPECT_TRUE(std::filesystem::exists(*youngPath));
}

TEST_F(AssetStoreTest, ReferencedAssetsAreNeverCollectedOrDeleted) {
    const auto id = import("referenced");
    // Minimal hierarchy with an image element that references the asset.
    ASSERT_OK(file->database().execute(
        "INSERT INTO notebook (id, title, sort_key, created_at, updated_at) "
        "VALUES (x'01890000000070008000000000000001', 'N', 'a0', 0, 0);"
        "INSERT INTO section (id, notebook_id, title, sort_key, created_at, updated_at) "
        "VALUES (x'01890000000070008000000000000002', x'01890000000070008000000000000001', 'S', "
        "'a0', 0, 0);"
        "INSERT INTO page (id, section_id, sort_key, extent, created_at, updated_at) "
        "VALUES (x'01890000000070008000000000000003', x'01890000000070008000000000000002', 'a0', "
        "0, 0, 0);"
        "INSERT INTO layer (id, page_id, name, sort_key) VALUES "
        "(x'01890000000070008000000000000004', x'01890000000070008000000000000003', 'L', 'a0');"
        "INSERT INTO element (id, page_id, layer_id, kind, z_key, pos_x, pos_y, min_x, min_y, "
        "max_x, max_y, created_at, updated_at) VALUES (x'01890000000070008000000000000005', "
        "x'01890000000070008000000000000003', x'01890000000070008000000000000004', 4, 'a0', 0, 0, "
        "0, 0, 1, 1, 0, 0);"));
    auto insert =
        file->database().prepare("INSERT INTO image (element_id, asset_id, width, height) "
                                 "VALUES (x'01890000000070008000000000000005', ?1, 1, 1)");
    ASSERT_OK(insert);
    insert->bindId(1, id);
    ASSERT_OK(insert->run());

    clock.advance(std::chrono::hours(24 * 365));
    auto report = assets->collectGarbage(clock.now(), 0ms);
    ASSERT_OK(report);
    EXPECT_EQ(report->rowsDeleted, 0U);
    EXPECT_EQ(report->filesDeleted, 0U);

    // ON DELETE RESTRICT: the database refuses to drop a referenced asset.
    auto remove = file->database().prepare("DELETE FROM asset WHERE id = ?1");
    ASSERT_OK(remove);
    remove->bindId(1, id);
    const auto removed = remove->run();
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().code, core::ErrorCode::Conflict);
}

TEST_F(AssetStoreTest, VerifyReportsMissingAndModifiedFiles) {
    const auto missing = import("will go missing", "image/png", "m.png");
    const auto modified = import("will be modified", "image/png", "x.png");
    const auto intact = import("stays intact", "image/png", "i.png");
    (void)intact;

    std::filesystem::remove(*assets->pathOf(missing));
    {
        std::ofstream out(*assets->pathOf(modified), std::ios::binary | std::ios::trunc);
        out << "WILL BE MODIFIED"; // same size, different content
    }
    auto problems = assets->verify();
    ASSERT_OK(problems);
    ASSERT_EQ(problems->size(), 2U);
    for (const auto& problem : *problems) {
        if (problem.asset == missing) {
            EXPECT_EQ(problem.kind, AssetProblem::Kind::MissingFile);
        } else {
            EXPECT_EQ(problem.asset, modified);
            EXPECT_EQ(problem.kind, AssetProblem::Kind::WrongHash);
        }
    }
}

TEST(AssetExtensionTest, DerivedFromMediaType) {
    EXPECT_EQ(extensionFor("application/pdf"), "pdf");
    EXPECT_EQ(extensionFor("image/jpeg"), "jpg");
    EXPECT_EQ(extensionFor("image/png"), "png");
    EXPECT_EQ(extensionFor("application/x-unknown"), "bin");
}

} // namespace
} // namespace studyapp::persistence
