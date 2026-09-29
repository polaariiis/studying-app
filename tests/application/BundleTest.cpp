// Phase 8, step 6: workspace and notebook bundles (zip archives of a database snapshot and
// assets). Export → import into an empty workspace gives equal content; notebooks import
// into other workspaces as one undoable command with ids remapped on conflict; bundles are
// untrusted input and anything unexpected is refused without leaving files behind.

#include <studyapp/persistence/Bundle.hpp>

#include <studyapp/application/Planner.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/document/StudyCommands.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Zip.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/ResultMacros.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/testing/TempDirectory.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
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

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& file, const std::string& content) {
    std::ofstream out(file, std::ios::binary);
    out << content;
}

struct BundleTest : ::testing::Test {
    testing::TempDirectory dir;
    testing::ManualClock clock;
    testing::SequentialIds ids;
    NoLocks locker;
    std::unique_ptr<WorkspaceSession> session;
    core::NotebookId notebook;
    core::PageId page;
    core::PageId pdfPage;
    core::AssetId photo;

    SessionServices services() { return {clock, ids, locker}; }
    const document::Workspace& ws() const { return session->workspace(); }

    std::unique_ptr<WorkspaceSession> create(const std::string& name) {
        auto created = WorkspaceSession::create(dir / name, name, services());
        EXPECT_TRUE(created.has_value()) << (created ? "" : created.error().message);
        return created ? std::move(*created) : nullptr;
    }

    void execute(WorkspaceSession& s, core::Result<document::Command> command) {
        ASSERT_OK(command);
        ASSERT_OK(s.execute(std::move(*command)));
    }

    /// A notebook with text, an image, a PDF section and a tag; a second notebook; a task.
    void SetUp() override {
        session = create("source");
        ASSERT_NE(session, nullptr);
        WorkspaceStructure structure(*session, clock, ids);
        auto created = structure.createNotebook("Biology");
        ASSERT_OK(created);
        notebook = created->notebook;
        page = created->page;
        const auto layer = ws().layersOf(page).front();
        writeFile(dir / "photo.png", std::string("\x89PNG fake image bytes", 21));
        writeFile(dir / "slides.pdf", "%PDF-1.7 fake document bytes");
        auto image = session->importAsset(dir / "photo.png", "image/png");
        auto pdf = session->importAsset(dir / "slides.pdf", "application/pdf");
        ASSERT_OK(image);
        ASSERT_OK(pdf);
        photo = *image;
        auto text = document::commands::createElement(
            ws(), layer,
            {.payload = document::TextBox{.size = {200, 40}, .text = "Chlorophyll \xC3\xBC"}}, ids);
        ASSERT_OK(text);
        ASSERT_OK(session->execute(std::move(text->command)));
        auto picture = document::commands::createElement(
            ws(), layer,
            {.transform = {.position = {10, 100}},
             .payload = document::Image{.asset = photo, .size = {50, 50}}},
            ids);
        ASSERT_OK(picture);
        ASSERT_OK(session->execute(std::move(picture->command)));
        const std::vector<core::DVec2> sizes{{400, 300}, {300, 400}};
        auto imported = structure.importDocument(notebook, "Slides", *pdf, sizes);
        ASSERT_OK(imported);
        pdfPage = imported->firstPage;
        auto tag = document::commands::createTag(ws(), "exam", clock, ids);
        ASSERT_OK(tag);
        ASSERT_OK(session->execute(std::move(tag->command)));
        execute(*session, document::commands::setPageTags(ws(), page, {tag->id}, clock));
        ASSERT_OK(structure.createNotebook("Physics"));
        Planner planner(*session, clock, ids);
        ASSERT_OK(planner.createTask({.title = "Revise chapter 3"}));
    }
};

TEST_F(BundleTest, AWorkspaceBundleOpensAsAnEqualWorkspace) {
    const auto bundle = dir / "all.studybundle";
    ASSERT_OK(session->exportBundle(bundle));
    EXPECT_FALSE(std::filesystem::exists(dir / "all.studybundle.part"));
    const document::Workspace original = ws();

    ASSERT_OK(WorkspaceSession::extractBundle(bundle, dir / "restored"));
    auto restored = WorkspaceSession::open(dir / "restored", {}, services());
    ASSERT_OK(restored);
    EXPECT_TRUE((*restored)->workspace() == original);
    auto path = (*restored)->assetPath(photo);
    ASSERT_OK(path);
    EXPECT_EQ(readFile(*path), readFile(dir / "photo.png"));
    // The export only read the source.
    EXPECT_TRUE(ws() == original);
    ASSERT_OK((*restored)->close());
}

TEST_F(BundleTest, NotebookBundlesImportAsOneStepAndTwiceAsACopy) {
    const auto bundle = dir / "biology.studybundle";
    ASSERT_OK(session->exportBundle(bundle, notebook));

    auto target = create("target");
    ASSERT_NE(target, nullptr);
    const auto undoBefore = target->history().undoCount();
    auto first = target->importBundle(bundle);
    ASSERT_OK(first);
    ASSERT_EQ(first->size(), 1U);
    const document::Workspace& tw = target->workspace();
    EXPECT_EQ(tw.findNotebook(first->front())->title, "Biology");
    EXPECT_EQ(tw.notebookCount(), 1U); // only the exported notebook
    EXPECT_EQ(tw.taskCount(), 0U);     // planning stays in its workspace
    EXPECT_EQ(tw.pageCount(), 3U);     // the page and the two PDF pages
    EXPECT_EQ(tw.findPage(page)->title, ws().findPage(page)->title); // ids kept: no conflict
    ASSERT_EQ(tw.findPage(page)->tags.size(), 1U);
    EXPECT_EQ(tw.findTag(tw.findPage(page)->tags[0])->name, "exam");
    const auto& document = *tw.findPage(pdfPage)->document;
    auto pdfFile = target->assetPath(document.asset);
    ASSERT_OK(pdfFile);
    EXPECT_EQ(readFile(*pdfFile), readFile(dir / "slides.pdf"));
    EXPECT_EQ(target->history().undoCount(), undoBefore + 1);
    EXPECT_EQ(target->history().nextUndo()->label, "Import notebook");

    // Again: every id is taken, so the second copy gets new ones; the tag is reused.
    auto second = target->importBundle(bundle);
    ASSERT_OK(second);
    EXPECT_NE(second->front(), first->front());
    EXPECT_EQ(tw.notebookCount(), 2U);
    EXPECT_EQ(tw.pageCount(), 6U);
    EXPECT_EQ(tw.tagCount(), 1U);
    EXPECT_TRUE(tw.validate().has_value());

    // Undo and redo, then everything survives a reopen.
    ASSERT_OK(target->undo());
    EXPECT_EQ(tw.notebookCount(), 1U);
    ASSERT_OK(target->redo());
    const document::Workspace imported = tw;
    ASSERT_OK(target->close());
    auto reopened = WorkspaceSession::open(dir / "target", {}, services());
    ASSERT_OK(reopened);
    EXPECT_TRUE((*reopened)->workspace() == imported);
    ASSERT_OK((*reopened)->close());
}

TEST_F(BundleTest, TheWorkspaceDirectoryIsRecognisedHoweverItIsSpelled) {
    const auto root = dir / "source";
    EXPECT_TRUE(session->isInsideWorkspace(root / "x.pdf"));
    EXPECT_TRUE(session->isInsideWorkspace(root / "assets" / "new" / "x.pdf"));
    EXPECT_TRUE(session->isInsideWorkspace(root / "sub" / ".." / "x.pdf"));
    EXPECT_TRUE(session->isInsideWorkspace(root));
    EXPECT_FALSE(session->isInsideWorkspace(root / ".." / "x.pdf"));
    EXPECT_FALSE(session->isInsideWorkspace(dir / "source-copy" / "x.pdf")); // a prefix only
    EXPECT_FALSE(session->isInsideWorkspace(dir / "x.pdf"));
}

TEST_F(BundleTest, ExportRefusesTargetsInsideTheWorkspaceAndReadOnlyImports) {
    const auto inside = dir / "source" / "copy.studybundle";
    EXPECT_FALSE(session->exportBundle(inside).has_value());
    EXPECT_FALSE(std::filesystem::exists(inside));
    EXPECT_FALSE(session->exportBundle(dir / "x.studybundle", core::NotebookId{ids.next()}));

    const auto bundle = dir / "biology.studybundle";
    ASSERT_OK(session->exportBundle(bundle, notebook));
    ASSERT_OK(session->close());
    auto readOnly =
        WorkspaceSession::open(dir / "source", {.mode = AccessMode::ReadOnly}, services());
    ASSERT_OK(readOnly);
    EXPECT_FALSE((*readOnly)->importBundle(bundle).has_value());
    ASSERT_OK((*readOnly)->exportBundle(dir / "from-read-only.studybundle")); // reading is fine
    ASSERT_OK((*readOnly)->close());
    session.reset();
}

// ---------------------------------------------------------------------------- untrusted

struct HostileBundleTest : BundleTest {
    std::filesystem::path valid;
    std::filesystem::path unpacked;

    void SetUp() override {
        BundleTest::SetUp();
        valid = dir / "valid.studybundle";
        ASSERT_OK(session->exportBundle(valid));
        unpacked = dir / "unpacked";
        ASSERT_OK(WorkspaceSession::extractBundle(valid, unpacked));
    }

    /// A bundle of the unpacked workspace's files, plus `extra` entries.
    std::filesystem::path rezip(const std::string& name,
                                const std::vector<std::pair<std::string, std::string>>& extra = {},
                                std::string manifest = "format=studyboard-bundle\nversion=1\n"
                                                       "kind=workspace\nschema=1\n") {
        const auto file = dir / name;
        auto zip = persistence::ZipWriter::create(file);
        EXPECT_TRUE(zip.has_value());
        const auto bytes = [](const std::string& s) {
            return std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        };
        EXPECT_TRUE(zip->addBytes("studyboard-bundle.txt", bytes(manifest)).has_value());
        EXPECT_TRUE(zip->addFile("workspace.db", unpacked / "workspace.db").has_value());
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(unpacked / "assets")) {
            if (entry.is_regular_file()) {
                const auto relative = entry.path().lexically_relative(unpacked).generic_string();
                EXPECT_TRUE(zip->addFile(relative, entry.path()).has_value());
            }
        }
        for (const auto& [entryName, content] : extra) {
            EXPECT_TRUE(zip->addBytes(entryName, bytes(content)).has_value());
        }
        EXPECT_TRUE(zip->finish().has_value());
        return file;
    }

    /// Extraction fails with `code` and leaves nothing behind.
    void expectRefused(const std::filesystem::path& bundle, core::ErrorCode code) {
        const auto root = dir / "refused";
        auto extracted = WorkspaceSession::extractBundle(bundle, root);
        ASSERT_FALSE(extracted.has_value());
        EXPECT_EQ(extracted.error().code, code) << extracted.error().message;
        EXPECT_FALSE(std::filesystem::exists(root));
        EXPECT_FALSE(std::filesystem::exists(dir / "evil.txt"));
        // Importing into a workspace refuses it the same way, changing nothing.
        const auto notebooks = ws().notebookCount();
        EXPECT_FALSE(session->importBundle(bundle).has_value());
        EXPECT_EQ(ws().notebookCount(), notebooks);
    }
};

TEST_F(HostileBundleTest, TheUnchangedFilesAreAccepted) {
    const auto again = rezip("again.studybundle");
    ASSERT_OK(WorkspaceSession::extractBundle(again, dir / "again"));
}

TEST_F(HostileBundleTest, EntriesOutsideTheLayoutAreRefused) {
    using core::ErrorCode;
    expectRefused(rezip("traversal.studybundle", {{"../evil.txt", "x"}}), ErrorCode::ParseError);
    expectRefused(rezip("absolute.studybundle", {{"/evil.txt", "x"}}), ErrorCode::ParseError);
    expectRefused(rezip("other.studybundle", {{"assets/../../evil.txt", "x"}}),
                  ErrorCode::ParseError);
    expectRefused(rezip("backslash.studybundle", {{"assets\\..\\evil.txt", "x"}}),
                  ErrorCode::ParseError);
}

TEST_F(HostileBundleTest, ForeignSchemaObjectsAreRefused) {
    {
        auto db = persistence::Database::open(unpacked / "workspace.db",
                                              persistence::OpenMode::ReadWrite);
        ASSERT_OK(db);
        ASSERT_OK(db->execute("CREATE TRIGGER evil AFTER INSERT ON page BEGIN "
                              "DELETE FROM notebook; END;"));
        ASSERT_OK(db->close());
    }
    expectRefused(rezip("trigger.studybundle"), core::ErrorCode::ParseError);
}

TEST_F(HostileBundleTest, DamagedOrForeignArchivesAreRefused) {
    using core::ErrorCode;
    writeFile(dir / "text.studybundle", "not a zip archive at all");
    expectRefused(dir / "text.studybundle", ErrorCode::ParseError);
    writeFile(dir / "empty.studybundle", "");
    expectRefused(dir / "empty.studybundle", ErrorCode::ParseError);
    // A flipped byte in the stored data fails its CRC.
    std::string bytes = readFile(valid);
    const auto at = bytes.find("SQLite format 3");
    ASSERT_NE(at, std::string::npos);
    bytes[at + 200] = static_cast<char>(bytes[at + 200] ^ 0x5A);
    writeFile(dir / "flipped.studybundle", bytes);
    expectRefused(dir / "flipped.studybundle", ErrorCode::ParseError);
    // Truncated.
    writeFile(dir / "truncated.studybundle", readFile(valid).substr(0, 1000));
    expectRefused(dir / "truncated.studybundle", ErrorCode::ParseError);
    // A newer version, an unknown format, a missing database.
    expectRefused(rezip("newer.studybundle", {},
                        "format=studyboard-bundle\nversion=1\nkind=workspace\nschema=99\n"),
                  ErrorCode::Unsupported);
    expectRefused(rezip("foreign.studybundle", {}, "format=other\nversion=1\nkind=workspace\n"),
                  ErrorCode::ParseError);
    // An asset whose content does not match its name.
    std::filesystem::path asset;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(unpacked / "assets")) {
        if (entry.is_regular_file()) {
            asset = entry.path();
        }
    }
    ASSERT_FALSE(asset.empty());
    writeFile(asset, "tampered");
    expectRefused(rezip("tampered.studybundle"), ErrorCode::ParseError);
}

TEST_F(HostileBundleTest, ANonEmptyTargetIsNeverOverwritten) {
    std::filesystem::create_directories(dir / "occupied");
    writeFile(dir / "occupied" / "keep.txt", "mine");
    auto extracted = WorkspaceSession::extractBundle(valid, dir / "occupied");
    ASSERT_FALSE(extracted.has_value());
    EXPECT_EQ(readFile(dir / "occupied" / "keep.txt"), "mine");
}

} // namespace
} // namespace studyapp::application
