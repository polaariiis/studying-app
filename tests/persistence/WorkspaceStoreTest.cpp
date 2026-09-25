#include <studyapp/persistence/WorkspaceStore.hpp>

#include <studyapp/persistence/AssetStore.hpp>
#include <studyapp/persistence/WorkspaceFile.hpp>
#include <studyapp/testing/TempDirectory.hpp>
#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace studyapp::persistence {
namespace {

using document::Command;
using document::Element;
using document::Patch;
using document::Workspace;
using document::test::makeConnector;
using document::test::makeStroke;
using document::test::makeText;
using document::test::millis;

/// A workspace directory plus an in-memory document edited through commands. save()
/// persists the patch of the most recent command exactly as the Editor applied it;
/// reload() closes the database and loads a fresh Workspace from disk.
struct WorkspaceStoreTest : ::testing::Test {
    testing::TempDirectory dir;
    document::test::TestWorkspace doc;
    std::optional<WorkspaceFile> file;
    std::unique_ptr<WorkspaceStore> store;

    void SetUp() override {
        auto created = WorkspaceFile::create(root(), doc.workspace.info(), "test");
        ASSERT_OK(created);
        file.emplace(std::move(*created));
        store = std::make_unique<WorkspaceStore>(file->database(), doc.clock);
    }

    [[nodiscard]] std::filesystem::path root() const { return dir / "Workspace.studyws"; }
    Database& db() { return file->database(); }

    /// Persists the patch of the command executed last.
    void save() {
        const Command* last = doc.editor.history().nextUndo();
        ASSERT_NE(last, nullptr);
        ASSERT_OK(store->write(last->patch));
    }

    /// Executes a hand-made patch (e.g. an update, which Phase 2 has no command for) and
    /// persists it.
    void executeAndSave(Patch patch) {
        ASSERT_OK(doc.editor.execute(Command{"test edit", std::move(patch)}));
        save();
    }

    void undoAndSave() {
        const Patch inverse = doc.editor.history().nextUndo()->patch.inverted();
        ASSERT_OK(doc.editor.undo());
        ASSERT_OK(store->write(inverse));
    }

    void redoAndSave() {
        const Patch patch = doc.editor.history().nextRedo()->patch;
        ASSERT_OK(doc.editor.redo());
        ASSERT_OK(store->write(patch));
    }

    core::Result<Workspace> reload() {
        store.reset();
        EXPECT_OK(file->close());
        auto reopened = WorkspaceFile::open(root(), AccessMode::ReadWrite, doc.clock.now(), "test");
        if (!reopened) {
            return tl::unexpected<core::Error>(reopened.error());
        }
        file.emplace(std::move(*reopened));
        store = std::make_unique<WorkspaceStore>(file->database(), doc.clock);
        return store->load();
    }

    /// Reloads and expects the result to equal the in-memory document and to be valid.
    void expectRoundTrip() {
        auto loaded = reload();
        ASSERT_OK(loaded);
        EXPECT_OK(loaded->validate());
        EXPECT_TRUE(*loaded == doc.workspace) << "reloaded workspace differs from memory";
    }

    core::AssetId importAsset(std::string_view content, std::string_view mediaType = "image/png") {
        const auto source = dir / "source.bin";
        {
            std::ofstream out(source, std::ios::binary);
            out << content;
        }
        AssetStore assets(db(), root());
        auto id = assets.import(source, mediaType, doc.ids, doc.clock);
        EXPECT_TRUE(id.has_value()) << (id ? "" : id.error().message);
        return id.value_or(core::AssetId{});
    }

    std::int64_t count(const std::string& sql) { return db().queryInt(sql).value_or(-1); }

    /// Adds `payload` in a new element and persists it.
    core::ElementId addSaved(core::LayerId layer, document::ElementPayload payload,
                             document::Transform transform = {}) {
        const auto id = doc.addElement(layer, std::move(payload), transform);
        save();
        return id;
    }

    core::LayerId addSavedPath() {
        const auto notebook = doc.addNotebook("Notebook");
        save();
        const auto section = doc.addSection(notebook, "Section");
        save();
        const auto page = doc.addPage(section, "Page");
        save();
        return doc.firstLayer(page);
    }
};

// ---------------------------------------------------------------------------- structure

TEST_F(WorkspaceStoreTest, EmptyWorkspaceRoundTripsWithMetadata) {
    auto loaded = reload();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->info().id, doc.workspace.info().id);
    EXPECT_EQ(loaded->info().name, "Test workspace");
    EXPECT_EQ(millis(loaded->info().created), millis(doc.workspace.info().created));
    EXPECT_EQ(loaded->notebookCount(), 0U);
    EXPECT_TRUE(*loaded == doc.workspace);
}

TEST_F(WorkspaceStoreTest, HierarchyRoundTripsWithIdsAndParents) {
    const auto notebookA = doc.addNotebook("Mathematics");
    save();
    const auto notebookB = doc.addNotebook("Physik \xC3\xBC\xC3\x9F"); // UTF-8 title
    save();
    const auto section = doc.addSection(notebookA, "Linear algebra");
    save();
    const auto sectionB = doc.addSection(notebookB, "Mechanics");
    save();
    const auto infinite = doc.addPage(section, "Notes");
    save();
    const auto bounded =
        doc.addPage(sectionB, "",
                    {.extent = document::PageExtent::Bounded,
                     .size = document::kA4PortraitSize,
                     .background = {.color = core::Color::fromRgba(250, 245, 230, 255),
                                    .pattern = document::BackgroundPattern::Grid,
                                    .spacing = 12.5F},
                     .firstLayerName = "Ink"});
    save();
    const auto extraLayer = doc.addLayer(bounded, "Annotations");
    save();

    auto loaded = reload();
    ASSERT_OK(loaded);
    EXPECT_TRUE(*loaded == doc.workspace);
    EXPECT_OK(loaded->validate());

    // Identity and parent links are the persisted UUIDs, not new ones.
    ASSERT_NE(loaded->findSection(section), nullptr);
    EXPECT_EQ(loaded->findSection(section)->notebook, notebookA);
    EXPECT_EQ(loaded->findPage(infinite)->section, section);
    EXPECT_EQ(loaded->findLayer(extraLayer)->page, bounded);
    const auto* page = loaded->findPage(bounded);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->extent, document::PageExtent::Bounded);
    EXPECT_EQ(page->size, document::kA4PortraitSize);
    EXPECT_EQ(page->background.pattern, document::BackgroundPattern::Grid);
    EXPECT_EQ(page->background.spacing, 12.5F);
    EXPECT_EQ(loaded->layersOf(bounded).size(), 2U);
    EXPECT_EQ(loaded->notebooks().size(), 2U);
}

TEST_F(WorkspaceStoreTest, OrderingKeysArePreserved) {
    const auto first = doc.addNotebook("First");
    save();
    const auto second = doc.addNotebook("Second");
    save();
    const auto third = doc.addNotebook("Third");
    save();
    // Move "Third" between "First" and "Second" by giving it a key between theirs.
    const auto* thirdInfo = doc.workspace.findNotebook(third);
    auto moved = *thirdInfo;
    auto key = core::FractionalIndex::between(doc.workspace.findNotebook(first)->order,
                                              doc.workspace.findNotebook(second)->order);
    ASSERT_OK(key);
    moved.order = *key;
    executeAndSave(Patch({document::updated(*thirdInfo, moved)}));

    auto loaded = reload();
    ASSERT_OK(loaded);
    const std::vector<core::NotebookId> expected{first, third, second};
    EXPECT_EQ(std::vector<core::NotebookId>(loaded->notebooks().begin(), loaded->notebooks().end()),
              expected);
    EXPECT_EQ(loaded->findNotebook(third)->order, *key);
    EXPECT_TRUE(*loaded == doc.workspace);
}

TEST_F(WorkspaceStoreTest, ElementOrderWithinLayersIsPreserved) {
    const auto layer = addSavedPath();
    std::vector<core::ElementId> ids;
    for (int i = 0; i < 20; ++i) {
        ids.push_back(addSaved(layer, makeText("item " + std::to_string(i))));
    }
    auto loaded = reload();
    ASSERT_OK(loaded);
    EXPECT_EQ(std::vector<core::ElementId>(loaded->elementsOf(layer).begin(),
                                           loaded->elementsOf(layer).end()),
              ids);
}

TEST_F(WorkspaceStoreTest, RenamesPersist) {
    const auto notebook = doc.addNotebook("Draft");
    save();
    const auto section = doc.addSection(notebook, "Draft section");
    save();
    const auto page = doc.addPage(section, "Draft page");
    save();
    const auto layer = doc.firstLayer(page);
    doc.clock.advance(std::chrono::minutes(5));
    doc.run(document::commands::renameNotebook(doc.workspace, notebook, "Final", doc.clock));
    save();
    doc.run(document::commands::renameSection(doc.workspace, section, "Final section", doc.clock));
    save();
    doc.run(document::commands::renamePage(doc.workspace, page, "Final page", doc.clock));
    save();
    doc.run(document::commands::renameLayer(doc.workspace, layer, "Final layer"));
    save();

    auto loaded = reload();
    ASSERT_OK(loaded);
    EXPECT_EQ(loaded->findNotebook(notebook)->title, "Final");
    EXPECT_EQ(millis(loaded->findNotebook(notebook)->modified), millis(doc.clock.now()));
    EXPECT_EQ(loaded->findPage(page)->title, "Final page");
    EXPECT_EQ(loaded->findLayer(layer)->name, "Final layer");
    EXPECT_TRUE(*loaded == doc.workspace);
}

TEST_F(WorkspaceStoreTest, DeletionsPersistAndCascadeCleanly) {
    const auto layer = addSavedPath();
    const auto stroke = addSaved(layer, makeStroke());
    const auto shape = addSaved(layer, document::Shape{.size = {10, 10}});
    addSaved(layer, makeConnector(stroke, shape));
    const auto page = doc.workspace.findLayer(layer)->page;
    const auto second = doc.addLayer(page, "Second");
    save();
    addSaved(second, makeText("on second layer"));

    // Deleting an attached element detaches the connector in the same patch.
    doc.run(document::commands::deleteElement(doc.workspace, stroke));
    save();
    expectRoundTrip();
    EXPECT_EQ(count("SELECT count(*) FROM stroke"), 0);
    EXPECT_EQ(count("SELECT count(*) FROM connector WHERE start_element_id IS NULL"), 1);

    doc.run(document::commands::deleteLayer(doc.workspace, second));
    save();
    expectRoundTrip();

    const auto notebook = doc.workspace.notebooks().front();
    doc.run(document::commands::deleteNotebook(doc.workspace, notebook));
    save();
    expectRoundTrip();
    for (const char* table : {"notebook", "section", "page", "layer", "element", "stroke",
                              "text_box", "shape", "image", "connector"}) {
        EXPECT_EQ(count(std::string("SELECT count(*) FROM ") + table), 0) << table;
    }
    EXPECT_OK(file->integrityCheck());
}

// ---------------------------------------------------------------------------- elements

TEST_F(WorkspaceStoreTest, EveryElementKindRoundTripsWithAllFields) {
    const auto layer = addSavedPath();
    const document::Transform transform{
        .position = {1.0e9 + 0.125, -3.5}, .rotation = 0.75F, .scale = {-2.0F, 0.5F}};

    const auto stroke = addSaved(
        layer,
        document::Stroke{.brush = document::Brush::Highlighter,
                         .color = core::Color::fromRgba(10, 20, 30, 40),
                         .baseWidth = 3.5F,
                         .points = document::makeStrokePoints(
                             {{0.0F, 0.0F, 0.0F}, {0.1F, -7.25F, 0.5F}, {1e-7F, 3e6F, 1.0F}})},
        transform);
    addSaved(layer,
             document::TextBox{.size = {120.5F, 40.25F},
                               .text = "Hello, w\xC3\xB6rld \xE2\x9C\x93\n\"quoted\" \\ back\ttab"},
             transform);
    addSaved(layer, document::TextBox{.size = {0, 0}, .text = ""});
    const auto shape =
        addSaved(layer, document::Shape{.kind = document::ShapeKind::Ellipse,
                                        .size = {30, 40},
                                        .strokeColor = std::nullopt,
                                        .strokeWidth = 0.0F,
                                        .fillColor = core::Color::fromRgba(1, 2, 3, 4)});
    addSaved(layer, document::Shape{.kind = document::ShapeKind::Line,
                                    .size = {5, 0},
                                    .strokeColor = core::Color::black(),
                                    .strokeWidth = 1.5F,
                                    .fillColor = std::nullopt});
    const auto asset = importAsset("\x89PNG fake image bytes");
    addSaved(layer, document::Image{.asset = asset, .size = {640, 480}}, transform);
    auto connector = makeConnector(stroke, shape);
    connector.color = core::Color::fromRgba(200, 100, 50, 128);
    connector.width = 4.0F;
    connector.start.position = {-1.5, 2.25};
    addSaved(layer, connector);
    addSaved(layer, makeConnector(std::nullopt, std::nullopt));

    // Locked flag on the header.
    Element locked = *doc.workspace.findElement(stroke);
    Element lockedAfter = locked;
    lockedAfter.locked = true;
    executeAndSave(Patch({document::updated(locked, lockedAfter)}));

    auto loaded = reload();
    ASSERT_OK(loaded);
    EXPECT_OK(loaded->validate());
    EXPECT_TRUE(*loaded == doc.workspace);
    ASSERT_EQ(loaded->elementCount(), 8U);
    for (const auto id : doc.workspace.elementsOf(layer)) {
        ASSERT_NE(loaded->findElement(id), nullptr);
        EXPECT_EQ(*loaded->findElement(id), *doc.workspace.findElement(id)) << id.toString();
    }
    EXPECT_TRUE(loaded->findElement(stroke)->locked);
    EXPECT_EQ(loaded->findElement(stroke)->transform, transform);

    // The derived columns are filled from the model.
    EXPECT_EQ(count("SELECT count(*) FROM text_box WHERE json_valid(content) AND plain_text = "
                    "json_extract(content, '$.text')"),
              2);
    EXPECT_EQ(count("SELECT count(*) FROM element WHERE min_x <= max_x AND min_y <= max_y"), 8);
}

TEST_F(WorkspaceStoreTest, ElementUpdatesPersist) {
    const auto layer = addSavedPath();
    const auto id = addSaved(layer, makeStroke());
    const Element before = *doc.workspace.findElement(id);

    // Move + restyle + new points in one update.
    Element after = before;
    after.transform.position = {50, 60};
    auto stroke = std::get<document::Stroke>(after.payload);
    stroke.color = core::Color::fromRgba(255, 0, 0);
    stroke.points = document::makeStrokePoints({{1, 1, 1}, {2, 2, 0.5F}, {3, 1, 0.25F}});
    after.payload = stroke;
    executeAndSave(Patch({document::updated(before, after)}));
    expectRoundTrip();

    // Kind change: the stroke becomes a text box (kind row replaced).
    const Element current = *doc.workspace.findElement(id);
    Element text = current;
    text.payload = makeText("was a stroke");
    executeAndSave(Patch({document::updated(current, text)}));
    expectRoundTrip();
    EXPECT_EQ(count("SELECT count(*) FROM stroke"), 0);
    EXPECT_EQ(count("SELECT count(*) FROM text_box"), 1);
    EXPECT_EQ(count("SELECT kind FROM element"), 2); // ElementKind::TextBox
}

TEST_F(WorkspaceStoreTest, MoveDoesNotRewriteStrokePoints) {
    const auto layer = addSavedPath();
    const auto id = addSaved(layer, makeStroke());
    ASSERT_OK(db().execute("CREATE TABLE stroke_writes (what TEXT);"
                           "CREATE TRIGGER stroke_any AFTER UPDATE ON stroke "
                           "BEGIN INSERT INTO stroke_writes VALUES ('row'); END;"
                           "CREATE TRIGGER stroke_points AFTER UPDATE OF points ON stroke "
                           "BEGIN INSERT INTO stroke_writes VALUES ('points'); END;"));

    // Move only (payload shared with the previous state): header update only.
    const Element before = *doc.workspace.findElement(id);
    Element moved = before;
    moved.transform.position = {500, -20};
    executeAndSave(Patch({document::updated(before, moved)}));
    EXPECT_EQ(count("SELECT count(*) FROM stroke_writes"), 0);
    EXPECT_EQ(count("SELECT count(*) FROM element WHERE pos_x = 500"), 1);

    // Restyle: the kind row changes, the point blob does not.
    Element recoloured = moved;
    std::get<document::Stroke>(recoloured.payload).color = core::Color::white();
    executeAndSave(Patch({document::updated(moved, recoloured)}));
    EXPECT_EQ(count("SELECT count(*) FROM stroke_writes WHERE what = 'row'"), 1);
    EXPECT_EQ(count("SELECT count(*) FROM stroke_writes WHERE what = 'points'"), 0);
    expectRoundTrip();
}

TEST_F(WorkspaceStoreTest, MovingALayerToAnotherPageMovesItsElements) {
    const auto layer = addSavedPath();
    addSaved(layer, makeText("travels with its layer"));
    const auto page = doc.workspace.findLayer(layer)->page;
    const auto section = doc.workspace.findPage(page)->section;
    const auto otherPage = doc.addPage(section, "Other");
    save();
    const auto keep = doc.addLayer(page, "Keep"); // the source page must keep a layer
    save();
    (void)keep;

    const document::Layer before = *doc.workspace.findLayer(layer);
    document::Layer after = before;
    after.page = otherPage;
    executeAndSave(Patch({document::updated(before, after)}));
    expectRoundTrip();

    auto statement = db().prepare("SELECT count(*) FROM element WHERE page_id = ?1");
    ASSERT_OK(statement);
    statement->bindId(1, otherPage);
    ASSERT_OK(statement->step());
    EXPECT_EQ(statement->columnInt(0), 1);
}

TEST_F(WorkspaceStoreTest, ContentEditsBumpThePageContentVersion) {
    const auto layer = addSavedPath();
    const auto page = doc.workspace.findLayer(layer)->page;
    const auto version = [&] {
        auto statement = db().prepare("SELECT content_version FROM page WHERE id = ?1");
        EXPECT_TRUE(statement.has_value());
        statement->bindId(1, page);
        EXPECT_TRUE(statement->step().value_or(false));
        return statement->columnInt(0);
    };
    const auto initial = version();
    addSaved(layer, makeStroke());
    EXPECT_EQ(version(), initial + 1);
    doc.run(document::commands::renamePage(doc.workspace, page, "Renamed", doc.clock));
    save();
    EXPECT_EQ(version(), initial + 1); // metadata only
}

TEST_F(WorkspaceStoreTest, UndoAndRedoPersist) {
    const auto layer = addSavedPath();
    addSaved(layer, makeStroke());
    const auto text = addSaved(layer, makeText("to be removed"));
    doc.run(document::commands::deleteElement(doc.workspace, text));
    save();

    undoAndSave(); // restores the text box with the same id
    expectRoundTrip();
    ASSERT_NE(doc.workspace.findElement(text), nullptr);

    undoAndSave();
    undoAndSave();
    expectRoundTrip();
    redoAndSave();
    expectRoundTrip();
}

// ---------------------------------------------------------------------------- atomicity

TEST_F(WorkspaceStoreTest, FailedPatchLeavesTheDatabaseUnchanged) {
    const auto layer = addSavedPath();
    addSaved(layer, makeStroke());
    const Workspace saved = doc.workspace;

    // A notebook plus an image whose asset was never imported: the asset foreign key fails
    // at commit, and the notebook must not be written either.
    const auto notebook = doc.addNotebook("Must not persist");
    const Command* createNotebook = doc.editor.history().nextUndo();
    Patch patch = createNotebook->patch;
    const auto image = doc.addElement(
        layer, document::Image{.asset = core::AssetId{doc.ids.next()}, .size = {1, 1}});
    for (const auto& change : doc.editor.history().nextUndo()->patch.changes()) {
        patch.add(change);
    }
    const auto written = store->write(patch);
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code, core::ErrorCode::Conflict);
    EXPECT_FALSE(db().inTransaction());

    auto loaded = reload();
    ASSERT_OK(loaded);
    EXPECT_TRUE(*loaded == saved);
    EXPECT_EQ(loaded->findNotebook(notebook), nullptr);
    EXPECT_EQ(loaded->findElement(image), nullptr);
}

TEST_F(WorkspaceStoreTest, PatchThatDisagreesWithTheDatabaseIsRejected) {
    const auto notebook = doc.addNotebook("In memory only"); // executed but never saved
    const auto* info = doc.workspace.findNotebook(notebook);
    auto renamed = *info;
    renamed.title = "Renamed";
    const auto written = store->write(Patch({document::updated(*info, renamed)}));
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().code, core::ErrorCode::Conflict);
    EXPECT_EQ(count("SELECT count(*) FROM notebook"), 0);
}

TEST_F(WorkspaceStoreTest, RepeatedSaveLoadCyclesAreStable) {
    const auto layer = addSavedPath();
    for (int cycle = 0; cycle < 5; ++cycle) {
        addSaved(layer, makeStroke({{static_cast<float>(cycle), 0, 1}}));
        addSaved(layer, makeText("cycle " + std::to_string(cycle)));
        auto loaded = reload();
        ASSERT_OK(loaded);
        ASSERT_TRUE(*loaded == doc.workspace) << "cycle " << cycle;
    }
    EXPECT_EQ(doc.workspace.elementCount(), 10U);
}

// ---------------------------------------------------------------------------- corrupt data

struct CorruptDataTest : WorkspaceStoreTest {
    core::LayerId layer;
    core::ElementId stroke;
    core::ElementId connector;

    void SetUp() override {
        WorkspaceStoreTest::SetUp();
        layer = addSavedPath();
        stroke = addSaved(layer, makeStroke());
        const auto shape = addSaved(layer, document::Shape{.size = {1, 1}});
        connector = addSaved(layer, makeConnector(stroke, shape));
        ASSERT_OK(reload()); // sanity: the untampered workspace loads
    }

    /// Tampers with the database (foreign keys and CHECK constraints off, as a buggy
    /// writer or disk corruption might) and expects the next load to fail with `code`.
    void expectLoadFails(const std::string& sql,
                         core::ErrorCode code = core::ErrorCode::ParseError) {
        ASSERT_OK(
            db().execute("PRAGMA foreign_keys = OFF; PRAGMA ignore_check_constraints = ON;" + sql +
                         ";PRAGMA ignore_check_constraints = OFF; PRAGMA foreign_keys = ON;"));
        auto loaded = reload();
        ASSERT_FALSE(loaded.has_value()) << "tampering was not detected: " << sql;
        EXPECT_EQ(loaded.error().code, code) << loaded.error().message;
    }
};

TEST_F(CorruptDataTest, RejectsMalformedIds) {
    expectLoadFails("UPDATE notebook SET id = x'0102'");
}

TEST_F(CorruptDataTest, RejectsNilIds) {
    expectLoadFails("UPDATE layer SET id = zeroblob(16)");
}

TEST_F(CorruptDataTest, RejectsInvalidOrderingKeys) {
    expectLoadFails("UPDATE element SET z_key = 'not a key!' WHERE kind = 1");
}

TEST_F(CorruptDataTest, RejectsUnknownElementKinds) {
    expectLoadFails("UPDATE element SET kind = 9 WHERE kind = 1");
}

TEST_F(CorruptDataTest, RejectsElementsWithoutKindRow) {
    expectLoadFails("DELETE FROM shape");
}

TEST_F(CorruptDataTest, RejectsKindRowOfTheWrongKind) {
    expectLoadFails("UPDATE element SET kind = 2 WHERE kind = 1"); // stroke row, text kind
}

TEST_F(CorruptDataTest, RejectsCorruptStrokeBlobs) {
    expectLoadFails("UPDATE stroke SET points = x'0103'");
}

TEST_F(CorruptDataTest, RejectsPointCountMismatch) {
    expectLoadFails("UPDATE stroke SET point_count = point_count + 1");
}

TEST_F(CorruptDataTest, RejectsOrphanedRows) {
    expectLoadFails("UPDATE layer SET page_id = randomblob(16)");
}

TEST_F(CorruptDataTest, RejectsDanglingParents) {
    expectLoadFails("UPDATE section SET notebook_id = randomblob(16)");
}

TEST_F(CorruptDataTest, RejectsPagesWithoutLayers) {
    expectLoadFails("DELETE FROM layer");
}

TEST_F(CorruptDataTest, RejectsElementOnAnotherPageThanItsLayer) {
    expectLoadFails("UPDATE element SET page_id = randomblob(16) WHERE kind = 3");
}

TEST_F(CorruptDataTest, RejectsInvalidValuesThroughDocumentInvariants) {
    expectLoadFails("UPDATE layer SET opacity = 2.5");
}

TEST_F(CorruptDataTest, RejectsBlankNames) {
    expectLoadFails("UPDATE notebook SET title = '   '");
}

TEST_F(CorruptDataTest, RejectsWrongStorageClasses) {
    expectLoadFails("UPDATE element SET pos_x = 'left' WHERE kind = 1");
}

TEST_F(CorruptDataTest, RejectsBrokenConnectorAttachments) {
    expectLoadFails("UPDATE connector SET start_element_id = element_id"); // attached to itself
}

TEST_F(CorruptDataTest, RejectsInvalidTextContent) {
    addSaved(layer, makeText("x"));
    expectLoadFails("UPDATE text_box SET content = json_object('v', 7, 'text', 'x')");
}

TEST_F(CorruptDataTest, RejectsMissingMetadata) {
    expectLoadFails("DELETE FROM workspace_meta WHERE key = 'workspace_id'");
}

TEST_F(CorruptDataTest, ReportsUnsupportedTrash) {
    expectLoadFails("UPDATE notebook SET deleted_at = 1", core::ErrorCode::Unsupported);
}

TEST_F(CorruptDataTest, ReportsUnsupportedNestedSections) {
    expectLoadFails("INSERT INTO section (id, notebook_id, parent_id, title, sort_key, created_at, "
                    "updated_at) SELECT randomblob(16), notebook_id, id, 'Child', 'a0', 0, 0 "
                    "FROM section",
                    core::ErrorCode::Unsupported);
}

} // namespace
} // namespace studyapp::persistence
