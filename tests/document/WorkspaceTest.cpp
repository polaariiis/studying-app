// Workspace hierarchy, lookup, ordering and invariants, exercised through raw patches and
// through commands.

#include <studyapp/document/Workspace.hpp>

#include <studyapp/testing/TestWorkspace.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_set>

namespace studyapp::document::test {
namespace {

using core::ErrorCode;
using core::FractionalIndex;

NotebookInfo notebookRecord(core::NotebookId id, std::string title, FractionalIndex order) {
    return NotebookInfo{.id = id, .title = std::move(title), .order = std::move(order)};
}

TEST(WorkspaceTest, StartsEmptyWithMetadata) {
    TestWorkspace t;
    EXPECT_EQ(t.workspace.info().name, "Test workspace");
    EXPECT_FALSE(t.workspace.info().id.isNull());
    EXPECT_EQ(millis(t.workspace.info().created), millis(t.clock.now()));
    EXPECT_TRUE(t.workspace.notebooks().empty());
    EXPECT_EQ(t.workspace.notebookCount(), 0U);
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, BuildsFullHierarchy) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("Physics");
    const auto section = t.addSection(notebook, "Mechanics");
    const auto page = t.addPage(section, "Newton's laws");
    const auto layer = t.addLayer(page, "Annotations");
    const auto element = t.addElement(layer, makeText("F = ma"));

    ASSERT_NE(t.workspace.findNotebook(notebook), nullptr);
    EXPECT_EQ(t.workspace.findNotebook(notebook)->title, "Physics");
    EXPECT_EQ(t.workspace.findSection(section)->notebook, notebook);
    EXPECT_EQ(t.workspace.findPage(page)->section, section);
    EXPECT_EQ(t.workspace.findLayer(layer)->page, page);
    EXPECT_EQ(t.workspace.findElement(element)->layer, layer);
    EXPECT_EQ(t.workspace.pageOf(element), page);

    EXPECT_EQ(t.workspace.layersOf(page).size(), 2U); // default layer + "Annotations"
    EXPECT_EQ(t.workspace.elementsOf(layer).size(), 1U);
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, LookupOfUnknownIdsIsDefined) {
    TestWorkspace t;
    const core::NotebookId unknown{t.ids.next()};
    EXPECT_EQ(t.workspace.findNotebook(unknown), nullptr);
    EXPECT_EQ(t.workspace.findSection(core::SectionId{}), nullptr);
    EXPECT_EQ(t.workspace.findPage(core::PageId{}), nullptr);
    EXPECT_EQ(t.workspace.findLayer(core::LayerId{}), nullptr);
    EXPECT_EQ(t.workspace.findElement(core::ElementId{}), nullptr);
    EXPECT_TRUE(t.workspace.sectionsOf(unknown).empty());
    EXPECT_TRUE(t.workspace.pagesOf(core::SectionId{}).empty());
    EXPECT_FALSE(t.workspace.pageOf(core::ElementId{}).has_value());
}

TEST(WorkspaceTest, ChildrenAreOrderedByOrderKeyThenId) {
    TestWorkspace t;
    const core::NotebookId a{t.ids.next()};
    const core::NotebookId b{t.ids.next()};
    const core::NotebookId c{t.ids.next()};
    // Created out of order; c and a share an order key (tie broken by id).
    Patch patch;
    patch.add(created(notebookRecord(b, "B", *FractionalIndex::parse("a2"))));
    patch.add(created(notebookRecord(c, "C", *FractionalIndex::parse("a1"))));
    patch.add(created(notebookRecord(a, "A", *FractionalIndex::parse("a1"))));
    ASSERT_OK(t.workspace.apply(patch));

    const auto order = t.workspace.notebooks();
    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order[0], a);
    EXPECT_EQ(order[1], c);
    EXPECT_EQ(order[2], b);
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, AppendingCommandsPreserveCreationOrder) {
    TestWorkspace t;
    std::vector<core::NotebookId> created;
    for (int i = 0; i < 20; ++i) {
        created.push_back(t.addNotebook("Notebook " + std::to_string(i)));
    }
    const auto order = t.workspace.notebooks();
    EXPECT_TRUE(std::equal(order.begin(), order.end(), created.begin(), created.end()));
}

TEST(WorkspaceTest, ReorderingViaUpdateMovesOnlyTheRecord) {
    TestWorkspace t;
    const auto first = t.addNotebook("First");
    const auto second = t.addNotebook("Second");
    NotebookInfo moved = *t.workspace.findNotebook(second);
    moved.order = FractionalIndex::before(t.workspace.findNotebook(first)->order);
    ASSERT_OK(t.workspace.apply(Patch({updated(*t.workspace.findNotebook(second), moved)})));
    EXPECT_EQ(t.workspace.notebooks()[0], second);
    EXPECT_EQ(t.workspace.notebooks()[1], first);
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, IdsAreUniqueAcrossTheHierarchy) {
    TestWorkspace t;
    const auto layer = t.addPath();
    t.addElement(layer, makeStroke());
    t.addElement(layer, makeText("x"));
    std::unordered_set<core::Uuid> seen;
    const auto insert = [&](const core::Uuid& id) {
        EXPECT_TRUE(seen.insert(id).second);
    };
    insert(t.workspace.info().id.value());
    for (const auto notebook : t.workspace.notebooks()) {
        insert(notebook.value());
        for (const auto section : t.workspace.sectionsOf(notebook)) {
            insert(section.value());
            for (const auto page : t.workspace.pagesOf(section)) {
                insert(page.value());
                for (const auto l : t.workspace.layersOf(page)) {
                    insert(l.value());
                    for (const auto e : t.workspace.elementsOf(l)) {
                        insert(e.value());
                    }
                }
            }
        }
    }
    EXPECT_EQ(seen.size(), 7U);
}

TEST(WorkspaceTest, RejectsDuplicateIds) {
    TestWorkspace t;
    const auto id = t.addNotebook("Original");
    const auto result =
        t.workspace.apply(Patch({created(notebookRecord(id, "Copy", FractionalIndex::first()))}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(t.workspace.findNotebook(id)->title, "Original");
}

TEST(WorkspaceTest, RejectsNullIds) {
    TestWorkspace t;
    const auto result = t.workspace.apply(
        Patch({created(notebookRecord(core::NotebookId{}, "Null", FractionalIndex::first()))}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(WorkspaceTest, RejectsOrphans) {
    TestWorkspace t;
    const SectionInfo orphan{.id = core::SectionId{t.ids.next()},
                             .notebook = core::NotebookId{t.ids.next()}, // does not exist
                             .title = "Orphan",
                             .order = FractionalIndex::first()};
    const auto result = t.workspace.apply(Patch({created(orphan)}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
    EXPECT_EQ(t.workspace.sectionCount(), 0U);
}

TEST(WorkspaceTest, CannotRemoveRecordWithChildren) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("Notebook");
    t.addSection(notebook, "Section");
    const auto result = t.workspace.apply(Patch({removed(*t.workspace.findNotebook(notebook))}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(t.workspace.findNotebook(notebook), nullptr);
}

TEST(WorkspaceTest, PageMustKeepAtLeastOneLayer) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto result = t.workspace.apply(Patch({removed(*t.workspace.findLayer(layer))}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(t.workspace.findLayer(layer), nullptr); // rolled back
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, PageCreatedWithoutLayerIsRejected) {
    TestWorkspace t;
    const auto notebook = t.addNotebook("N");
    const auto section = t.addSection(notebook, "S");
    const PageInfo page{.id = core::PageId{t.ids.next()},
                        .section = section,
                        .title = "No layer",
                        .order = FractionalIndex::first()};
    const auto result = t.workspace.apply(Patch({created(page)}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(t.workspace.findPage(page.id), nullptr);
}

TEST(WorkspaceTest, RejectsBlankNamesAndInvalidValues) {
    TestWorkspace t;
    EXPECT_FALSE(t.workspace
                     .apply(Patch({created(notebookRecord(core::NotebookId{t.ids.next()}, "  \t",
                                                          FractionalIndex::first()))}))
                     .has_value());

    const auto layer = t.addPath();
    Layer bad = *t.workspace.findLayer(layer);
    bad.opacity = 1.5F;
    EXPECT_FALSE(
        t.workspace.apply(Patch({updated(*t.workspace.findLayer(layer), bad)})).has_value());

    const Element emptyStroke{.id = core::ElementId{t.ids.next()},
                              .layer = layer,
                              .z = FractionalIndex::first(),
                              .payload = Stroke{}}; // no points
    EXPECT_FALSE(t.workspace.apply(Patch({created(emptyStroke)})).has_value());

    const Element zeroScale{.id = core::ElementId{t.ids.next()},
                            .layer = layer,
                            .z = FractionalIndex::first(),
                            .transform = {.scale = {0.0F, 1.0F}},
                            .payload = makeText("x")};
    EXPECT_FALSE(t.workspace.apply(Patch({created(zeroScale)})).has_value());

    const PageInfo page = *t.workspace.findPage(t.workspace.findLayer(layer)->page);
    PageInfo bounded = page;
    bounded.extent = PageExtent::Bounded;
    bounded.size = {0.0, 100.0};
    EXPECT_FALSE(t.workspace.apply(Patch({updated(page, bounded)})).has_value());
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, ConnectorAttachmentRules) {
    TestWorkspace t;
    const auto layer = t.addPath();
    const auto box = t.addElement(layer, makeText("A"));
    const auto other = t.addElement(layer, makeText("B"));
    const auto link = t.addElement(layer, makeConnector(box, other));

    // Attached element cannot be removed directly...
    const auto remove = t.workspace.apply(Patch({removed(*t.workspace.findElement(box))}));
    ASSERT_FALSE(remove.has_value());
    EXPECT_NE(t.workspace.findElement(box), nullptr);

    // ...and connectors cannot attach to connectors, missing elements or other pages.
    const auto attachToConnector = commands::createElement(
        t.workspace, layer, {.payload = makeConnector(link, std::nullopt)}, t.ids);
    ASSERT_TRUE(attachToConnector.has_value());
    EXPECT_FALSE(t.workspace.apply(attachToConnector->command.patch).has_value());

    const auto attachToMissing = commands::createElement(
        t.workspace, layer, {.payload = makeConnector(core::ElementId{t.ids.next()}, std::nullopt)},
        t.ids);
    EXPECT_FALSE(t.workspace.apply(attachToMissing->command.patch).has_value());

    const auto otherPageLayer = t.addPath();
    const auto crossPage = commands::createElement(
        t.workspace, otherPageLayer, {.payload = makeConnector(box, std::nullopt)}, t.ids);
    EXPECT_FALSE(t.workspace.apply(crossPage->command.patch).has_value());
    EXPECT_OK(t.workspace.validate());
}

TEST(WorkspaceTest, LogicalEqualityIgnoresHistory) {
    TestWorkspace a;
    TestWorkspace b;
    EXPECT_EQ(a.workspace, b.workspace); // same deterministic ids and clock
    a.addNotebook("N");
    EXPECT_NE(a.workspace, b.workspace);
    b.addNotebook("N");
    EXPECT_EQ(a.workspace, b.workspace);
}

} // namespace
} // namespace studyapp::document::test
