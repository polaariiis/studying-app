// Phase 5 shell: the navigation tree model (targeted updates, drag-and-drop requests) and
// the MainWindow workflows — create/open/close workspaces, navigate pages, structure
// edits through the session, undo across pages, locks, no save questions. Runs on the
// offscreen platform (the canvas has no OpenGL there; everything else is real).

#include "WorkspaceTreeModel.hpp"

#include <studyapp/application/PageNavigator.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/platform/QtWorkspaceLocker.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/ui/MainWindow.hpp>
#include <studyapp/ui/ShellDialogs.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAbstractItemModelTester>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include <filesystem>
#include <memory>
#include <vector>

using namespace studyapp;
using namespace Qt::StringLiterals;
using application::HierarchyItem;
using ui::WorkspaceTreeModel;

namespace {

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

/// A document with an editor; every applied patch is forwarded to the model under test.
struct Document {
    testing::ManualClock clock;
    testing::SequentialIds ids;
    document::Workspace workspace{document::WorkspaceInfo{
        .id = core::WorkspaceId::generate(ids), .name = "Test", .created = clock.now()}};
    document::Editor editor{workspace};
    WorkspaceTreeModel* model = nullptr;

    void run(core::Result<document::Command> command) {
        QVERIFY(command.has_value());
        const document::Patch patch = command->patch;
        QVERIFY(editor.execute(std::move(*command)).has_value());
        if (model != nullptr) {
            model->onPatch(patch);
        }
    }
    template <class Id>
    Id run(core::Result<document::commands::Created<Id>> created) {
        const Id id = created->id;
        run(core::Result<document::Command>(std::move(created->command)));
        return id;
    }
    core::NotebookId notebook(std::string title) {
        return run(document::commands::createNotebook(workspace, std::move(title), clock, ids));
    }
    core::SectionId section(core::NotebookId notebook, std::string title) {
        return run(
            document::commands::createSection(workspace, notebook, std::move(title), clock, ids));
    }
    core::PageId page(core::SectionId section, std::string title) {
        return run(
            document::commands::createPage(workspace, section, std::move(title), {}, clock, ids));
    }
};

/// Records model signals by kind.
struct SignalLog {
    explicit SignalLog(QAbstractItemModel& model)
        : reset(&model, &QAbstractItemModel::modelReset),
          inserted(&model, &QAbstractItemModel::rowsInserted),
          removed(&model, &QAbstractItemModel::rowsRemoved),
          moved(&model, &QAbstractItemModel::rowsMoved),
          changed(&model, &QAbstractItemModel::dataChanged),
          layout(&model, &QAbstractItemModel::layoutChanged) {}
    void clear() {
        for (QSignalSpy* spy : {&reset, &inserted, &removed, &moved, &changed, &layout}) {
            spy->clear();
        }
    }
    QSignalSpy reset, inserted, removed, moved, changed, layout;
};

/// Scripted answers for the shell's questions; counts every question.
class ScriptedDialogs final : public ui::ShellDialogs {
public:
    std::optional<std::filesystem::path> newWorkspace;
    std::optional<std::filesystem::path> openWorkspace;
    bool openReadOnly = true;
    StaleLockChoice staleLock = StaleLockChoice::Recover;
    bool confirmDeletes = true;
    UnsavedChoice unsaved = UnsavedChoice::Cancel;
    int questions = 0;
    int deleteQuestions = 0;
    QStringList errors;

    std::optional<std::filesystem::path> chooseNewWorkspace(QWidget*) override {
        ++questions;
        return newWorkspace;
    }
    std::optional<std::filesystem::path> chooseWorkspaceToOpen(QWidget*) override {
        ++questions;
        return openWorkspace;
    }
    bool confirmOpenReadOnly(QWidget*, const QString&) override {
        ++questions;
        return openReadOnly;
    }
    StaleLockChoice askStaleLock(QWidget*, const QString&) override {
        ++questions;
        return staleLock;
    }
    bool confirmDelete(QWidget*, const QString&) override {
        ++questions;
        ++deleteQuestions;
        return confirmDeletes;
    }
    UnsavedChoice askUnsavedChanges(QWidget*, std::size_t, const QString&) override {
        ++questions;
        return unsaved;
    }
    void showError(QWidget*, const QString& summary, const QString&) override { errors << summary; }
};

std::vector<QString> childTitles(const QAbstractItemModel& model, const QModelIndex& parent = {}) {
    std::vector<QString> titles;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        titles.push_back(model.index(row, 0, parent).data().toString());
    }
    return titles;
}

} // namespace

class ShellTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;
    int counter_ = 0;

    std::filesystem::path freshPath(const QString& name) {
        return toPath(
            dir_.filePath(name + QString::number(++counter_) + QStringLiteral(".studyws")));
    }
    std::unique_ptr<QSettings> settings(const QString& name) {
        return std::make_unique<QSettings>(dir_.filePath(name + QStringLiteral(".ini")),
                                           QSettings::IniFormat);
    }

    /// A full shell with scripted dialogs.
    struct Shell {
        testing::ManualClock clock;
        testing::SequentialIds ids;
        platform::QtWorkspaceLocker locker;
        std::unique_ptr<QSettings> settings;
        ui::ThemeManager themes;
        ScriptedDialogs* dialogs = nullptr;
        std::unique_ptr<ui::MainWindow> window;

        std::unique_ptr<QAbstractItemModelTester> tester;

        explicit Shell(std::unique_ptr<QSettings> s) : settings(std::move(s)) {
            settings->setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
            auto scripted = std::make_unique<ScriptedDialogs>();
            dialogs = scripted.get();
            window = std::make_unique<ui::MainWindow>(
                themes, *settings, ui::ShellServices{.clock = clock, .ids = ids, .locker = locker},
                std::move(scripted));
            window->resize(1000, 700);
            window->show();
            // Every workflow runs with the model contract checked (row signals, indexes).
            tester = std::make_unique<QAbstractItemModelTester>(
                model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        }
        QAction* action(const char* name) const {
            auto* found = window->findChild<QAction*>(QString::fromLatin1(name));
            if (found == nullptr) {
                qFatal("no action %s", name);
            }
            return found;
        }
        QTreeView* tree() const {
            return window->findChild<QTreeView*>(QStringLiteral("workspaceTree"));
        }
        QAbstractItemModel* model() const { return tree()->model(); }
        const document::Workspace& ws() const { return window->session()->workspace(); }
        QModelIndex indexOf(const HierarchyItem& item) const {
            return static_cast<WorkspaceTreeModel*>(model())->indexOf(item);
        }
        void select(const HierarchyItem& item) const { tree()->setCurrentIndex(indexOf(item)); }
        core::ElementId draw(core::PageId page) {
            auto created = document::commands::createElement(
                ws(), ws().layersOf(page).front(),
                {.payload = document::Stroke{.points = document::makeStrokePoints(
                                                 {{0, 0, 1}, {40, 10, 1}})}},
                ids);
            const auto id = created->id;
            if (!window->session()->execute(std::move(created->command))) {
                qFatal("draw failed");
            }
            return id;
        }
    };

private Q_SLOTS:
    void initTestCase() { QVERIFY(dir_.isValid()); }

    // ---------------------------------------------------------------- tree model

    void treeMirrorsTheDocument() {
        Document doc;
        const auto n1 = doc.notebook("Maths");
        const auto s1 = doc.section(n1, "Algebra");
        doc.page(s1, "Groups");
        doc.page(s1, "");
        const auto n2 = doc.notebook("Physics");
        doc.section(n2, "Mechanics");

        WorkspaceTreeModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setWorkspace(&doc.workspace);
        QCOMPARE(childTitles(model), (std::vector<QString>{u"Maths"_s, u"Physics"_s}));
        const QModelIndex maths = model.index(0, 0);
        QCOMPARE(childTitles(model, maths), (std::vector<QString>{u"Algebra"_s}));
        const QModelIndex algebra = model.index(0, 0, maths);
        QCOMPARE(childTitles(model, algebra),
                 (std::vector<QString>{u"Groups"_s, u"Untitled page"_s}));
        QCOMPARE(model.index(1, 0, algebra).data(Qt::EditRole).toString(), QString()); // raw title
        QCOMPARE(model.index(0, 0, algebra).data(WorkspaceTreeModel::KindRole).toString(),
                 QStringLiteral("page"));
        QVERIFY(model.itemAt(maths).has_value());
        QCOMPARE(model.indexOf(HierarchyItem{n2}), model.index(1, 0));
    }

    void updatesAreTargeted() {
        Document doc;
        const auto n1 = doc.notebook("A");
        const auto s1 = doc.section(n1, "S1");
        const auto s2 = doc.section(n1, "S2");
        const auto p1 = doc.page(s1, "P1");
        const auto p2 = doc.page(s1, "P2");
        WorkspaceTreeModel model;
        doc.model = &model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setWorkspace(&doc.workspace);
        SignalLog log(model);
        const QPersistentModelIndex p1Index = model.indexOf(HierarchyItem{p1});

        // Rename: one dataChanged, no structural signal.
        doc.run(document::commands::renamePage(doc.workspace, p2, "Second", doc.clock));
        QCOMPARE(log.changed.count(), 1);
        QCOMPARE(log.inserted.count() + log.removed.count() + log.moved.count() +
                     log.reset.count() + log.layout.count(),
                 0);
        QCOMPARE(model.indexOf(HierarchyItem{p2}).data().toString(), QStringLiteral("Second"));

        // Canvas edits (elements) do not touch the tree at all.
        log.clear();
        const auto layer = doc.workspace.layersOf(p1).front();
        doc.run(document::commands::createElement(
            doc.workspace, layer,
            {.payload =
                 document::Stroke{.points = document::makeStrokePoints({{0, 0, 1}, {1, 1, 1}})}},
            doc.ids));
        QCOMPARE(log.changed.count() + log.inserted.count() + log.removed.count() +
                     log.moved.count() + log.reset.count(),
                 0);

        // A new page: exactly one inserted row, at its place.
        log.clear();
        const auto p3 = doc.page(s2, "P3");
        QCOMPARE(log.inserted.count(), 1);
        QCOMPARE(log.reset.count(), 0);
        QCOMPARE(model.indexOf(HierarchyItem{p3}).parent(), model.indexOf(HierarchyItem{s2}));

        // Moving a page to another section is a row move: persistent indexes (the view's
        // selection) follow the page.
        log.clear();
        doc.run(document::commands::movePage(doc.workspace, p1, s2, 1, doc.clock));
        QCOMPARE(log.moved.count(), 1);
        QCOMPARE(log.inserted.count() + log.removed.count() + log.reset.count(), 0);
        QVERIFY(p1Index.isValid());
        QCOMPARE(model.itemAt(p1Index), std::optional<HierarchyItem>(p1));
        QCOMPARE(childTitles(model, model.indexOf(HierarchyItem{s2})),
                 (std::vector<QString>{u"P3"_s, u"P1"_s}));

        // Reorder within a parent: a row move too.
        log.clear();
        doc.run(document::commands::moveSection(doc.workspace, s2, n1, 0, doc.clock));
        QCOMPARE(log.moved.count(), 1);
        QCOMPARE(childTitles(model, model.indexOf(HierarchyItem{n1})),
                 (std::vector<QString>{u"S2"_s, u"S1"_s}));

        // Deleting a section removes its row (and its pages with it).
        log.clear();
        doc.run(document::commands::deleteSection(doc.workspace, s2));
        QCOMPARE(log.removed.count(), 1);
        QVERIFY(!model.indexOf(HierarchyItem{p1}).isValid());
        QVERIFY(!p1Index.isValid());

        // Undo restores it with inserts, never a reset.
        log.clear();
        const document::Command* last = doc.editor.history().nextUndo();
        const document::Patch inverse = last->patch.inverted();
        QVERIFY(doc.editor.undo().has_value());
        model.onPatch(inverse);
        QCOMPARE(log.reset.count(), 0);
        QVERIFY(log.inserted.count() >= 1);
        QCOMPARE(childTitles(model, model.indexOf(HierarchyItem{s2})),
                 (std::vector<QString>{u"P3"_s, u"P1"_s}));
    }

    void activePageIsMarked() {
        Document doc;
        const auto s = doc.section(doc.notebook("N"), "S");
        const auto a = doc.page(s, "A");
        const auto b = doc.page(s, "B");
        WorkspaceTreeModel model;
        model.setWorkspace(&doc.workspace);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setActivePage(a);
        QVERIFY(model.indexOf(HierarchyItem{a}).data(WorkspaceTreeModel::ActiveRole).toBool());
        model.setActivePage(b);
        QVERIFY(!model.indexOf(HierarchyItem{a}).data(WorkspaceTreeModel::ActiveRole).toBool());
        QVERIFY(model.indexOf(HierarchyItem{b}).data(WorkspaceTreeModel::ActiveRole).toBool());
        QCOMPARE(changed.count(), 3); // a on; a off + b on
    }

    void dragAndDropRequestsValidMoves() {
        Document doc;
        const auto n1 = doc.notebook("N1");
        const auto n2 = doc.notebook("N2");
        const auto s1 = doc.section(n1, "S1");
        const auto s2 = doc.section(n2, "S2");
        const auto p1 = doc.page(s1, "P1");
        doc.page(s1, "P2");
        doc.page(s1, "P3");
        WorkspaceTreeModel model;
        model.setWorkspace(&doc.workspace);
        struct Request {
            HierarchyItem item;
            std::optional<HierarchyItem> parent;
            std::size_t index;
        };
        std::vector<Request> requests;
        model.setMoveHandler([&](const HierarchyItem& item,
                                 const std::optional<HierarchyItem>& parent, std::size_t index) {
            requests.push_back({item, parent, index});
            return true;
        });
        const std::unique_ptr<QMimeData> page(model.mimeData({model.indexOf(HierarchyItem{p1})}));
        const std::unique_ptr<QMimeData> section(
            model.mimeData({model.indexOf(HierarchyItem{s1})}));
        const QModelIndex s1Index = model.indexOf(HierarchyItem{s1});
        const QModelIndex s2Index = model.indexOf(HierarchyItem{s2});
        const QModelIndex n2Index = model.indexOf(HierarchyItem{n2});

        // Pages go into sections only; sections into notebooks only.
        QVERIFY(model.canDropMimeData(page.get(), Qt::MoveAction, 0, 0, s2Index));
        QVERIFY(!model.canDropMimeData(page.get(), Qt::MoveAction, 0, 0, n2Index));
        QVERIFY(!model.canDropMimeData(page.get(), Qt::MoveAction, 0, 0, {}));
        QVERIFY(model.canDropMimeData(section.get(), Qt::MoveAction, -1, 0, n2Index));
        QVERIFY(!model.canDropMimeData(section.get(), Qt::MoveAction, 0, 0, s2Index));
        QVERIFY(!model.canDropMimeData(section.get(), Qt::CopyAction, -1, 0, n2Index));

        // Within the section, below P3 (insertion row 3 includes the dragged row): index 2.
        QVERIFY(model.dropMimeData(page.get(), Qt::MoveAction, 3, 0, s1Index)); // the move ran
        QVERIFY(!model.removeRows(0, 1, s1Index)); // and the view cannot remove rows itself
        // Onto another section: appended.
        model.dropMimeData(page.get(), Qt::MoveAction, -1, 0, s2Index);
        model.dropMimeData(section.get(), Qt::MoveAction, 0, 0, n2Index);
        QCOMPARE(requests.size(), std::size_t{3});
        QCOMPARE(requests[0].item, HierarchyItem{p1});
        QCOMPARE(requests[0].parent, std::optional<HierarchyItem>(s1));
        QCOMPARE(requests[0].index, std::size_t{2});
        QCOMPARE(requests[1].parent, std::optional<HierarchyItem>(s2));
        QCOMPARE(requests[1].index, std::size_t{0}); // S2 has no pages yet
        QCOMPARE(requests[2].item, HierarchyItem{s1});
        QCOMPARE(requests[2].index, std::size_t{0});

        // Read-only workspaces accept no drops and no edits.
        model.setReadOnly(true);
        QVERIFY(!model.canDropMimeData(page.get(), Qt::MoveAction, 0, 0, s2Index));
        QVERIFY(!(model.flags(s1Index) & Qt::ItemIsEditable));
    }

    // ---------------------------------------------------------------- shell workflows

    void newWorkspaceStartsOnAPage() {
        Shell shell(settings(QStringLiteral("new")));
        QVERIFY(!shell.window->hasWorkspace());
        const auto path = freshPath(QStringLiteral("Biology "));
        shell.dialogs->newWorkspace = path;
        shell.action("actionNewWorkspace")->trigger();
        QVERIFY(shell.window->hasWorkspace());
        QCOMPARE(shell.dialogs->questions, 1); // only "where"
        QVERIFY(shell.dialogs->errors.isEmpty());

        // Notebook › Notes › Page 1, open, selected in the tree, named in the title bar.
        const auto page = shell.window->activePage();
        QVERIFY(page.has_value());
        QCOMPARE(shell.ws().info().name, std::string("Biology 1"));
        QCOMPARE(shell.tree()->currentIndex(), shell.indexOf(HierarchyItem{*page}));
        QVERIFY(shell.window->windowTitle().startsWith(QStringLiteral("Page 1 — Biology 1")));
        auto* breadcrumb = shell.window->findChild<QLabel*>(QStringLiteral("breadcrumbLabel"));
        QCOMPARE(breadcrumb->text(), QStringLiteral("Notebook  ›  Notes  ›  Page 1"));
        QVERIFY(shell.action("actionNewPage")->isEnabled());
        QVERIFY(shell.action("actionToolPen")->isEnabled());
        QVERIFY(shell.window->recentWorkspaces().size() == 1);
    }

    void navigateCreateAndEditPagesThenReopen() {
        Shell shell(settings(QStringLiteral("navigate")));
        const auto path = freshPath(QStringLiteral("Navigate"));
        QVERIFY(shell.window->createWorkspace(path));
        const core::PageId first = *shell.window->activePage();

        // New Page (Ctrl+N) goes into the open page's section and opens.
        shell.action("actionNewPage")->trigger();
        const core::PageId second = *shell.window->activePage();
        QVERIFY(second != first);
        QCOMPARE(shell.ws().findPage(second)->section, shell.ws().findPage(first)->section);
        QCOMPARE(shell.ws().findPage(second)->title, std::string("Page 2"));
        const auto stroke = shell.draw(second);

        // Selecting a page in the tree opens it; selecting a section does not.
        shell.select(first);
        QCOMPARE(shell.window->activePage(), first);
        shell.select(shell.ws().findPage(first)->section);
        QCOMPARE(shell.window->activePage(), first);
        shell.action("actionNextPage")->trigger();
        QCOMPARE(shell.window->activePage(), second);
        shell.action("actionPreviousPage")->trigger();
        QCOMPARE(shell.window->activePage(), first);

        // Undo from the first page takes back the second page's stroke — and shows it.
        shell.action("actionUndo")->trigger();
        QCOMPARE(shell.ws().findElement(stroke), nullptr);
        QCOMPARE(shell.window->activePage(), second);
        shell.action("actionRedo")->trigger();
        QVERIFY(shell.ws().findElement(stroke) != nullptr);

        // New section and notebook, each with a first page, opened.
        shell.action("actionNewSection")->trigger();
        const core::PageId third = *shell.window->activePage();
        QCOMPARE(shell.ws().findSection(shell.ws().findPage(third)->section)->title,
                 std::string("Section 2"));
        shell.action("actionNewNotebook")->trigger();
        QCOMPARE(shell.ws().notebookCount(), std::size_t{2});

        // Close (no questions: everything is saved), then reopen from the recent list.
        const int questions = shell.dialogs->questions;
        QVERIFY(shell.window->closeWorkspace());
        QCOMPARE(shell.dialogs->questions, questions);
        QVERIFY(!shell.window->hasWorkspace());
        QVERIFY(shell.window->findChild<QWidget*>(QStringLiteral("placeholder"))->isVisible());
        QVERIFY(shell.window->openWorkspace(path));
        QCOMPARE(shell.ws().pageCount(), std::size_t{4});
        QVERIFY(shell.ws().findElement(stroke) != nullptr);
        QCOMPARE(shell.ws().findPage(second)->title, std::string("Page 2"));
        QCOMPARE(shell.window->activePage(), first); // opens on the first page
    }

    void renameDeleteAndMoveGoThroughCommands() {
        Shell shell(settings(QStringLiteral("structure")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Structure"))));
        const core::PageId first = *shell.window->activePage();
        shell.action("actionNewPage")->trigger();
        const core::PageId second = *shell.window->activePage();
        const core::SectionId section = shell.ws().findPage(first)->section;
        auto* model = shell.model();

        // Inline rename (what F2 edits) goes through the session and is undoable.
        QVERIFY(
            model->setData(shell.indexOf(second), QStringLiteral("  Lecture 1 "), Qt::EditRole));
        QCOMPARE(shell.ws().findPage(second)->title, std::string("Lecture 1"));
        QVERIFY(shell.window->windowTitle().startsWith(QStringLiteral("Lecture 1 — ")));
        QVERIFY(!model->setData(shell.indexOf(section), QStringLiteral(" "), Qt::EditRole));
        QCOMPARE(shell.ws().findSection(section)->title, std::string("Notes"));

        // Move up / down in the tree.
        shell.select(second);
        shell.action("actionMoveUp")->trigger();
        QCOMPARE(shell.ws().pagesOf(section).front(), second);
        shell.action("actionMoveDown")->trigger();
        QCOMPARE(shell.ws().pagesOf(section).front(), first);

        // Deleting the open page opens its neighbour. An empty page is not confirmed.
        shell.select(second);
        shell.action("actionDeleteItem")->trigger();
        QCOMPARE(shell.dialogs->deleteQuestions, 0);
        QCOMPARE(shell.ws().findPage(second), nullptr);
        QCOMPARE(shell.window->activePage(), first);
        shell.action("actionUndo")->trigger();
        QVERIFY(shell.ws().findPage(second) != nullptr);

        // A page with ink, a section and a notebook are confirmed; "no" changes nothing.
        shell.draw(first);
        shell.select(first);
        shell.dialogs->confirmDeletes = false;
        shell.action("actionDeleteItem")->trigger();
        QCOMPARE(shell.dialogs->deleteQuestions, 1);
        QVERIFY(shell.ws().findPage(first) != nullptr);
        shell.dialogs->confirmDeletes = true;
        shell.select(section);
        shell.action("actionDeleteItem")->trigger();
        QCOMPARE(shell.dialogs->deleteQuestions, 2);
        QCOMPARE(shell.ws().findSection(section), nullptr);
        QVERIFY(!shell.window->activePage().has_value() ||
                shell.ws().findPage(*shell.window->activePage()) != nullptr);
        shell.action("actionUndo")->trigger();
        QVERIFY(shell.ws().findSection(section) != nullptr);
        QCOMPARE(shell.ws().pagesOf(section).size(), std::size_t{2});
        QVERIFY(shell.dialogs->errors.isEmpty());
        QCOMPARE(shell.window->session()->pendingWriteCount(), std::size_t{0});
    }

    void closingNeverAsksToSave() {
        Shell shell(settings(QStringLiteral("close")));
        const auto path = freshPath(QStringLiteral("Close"));
        QVERIFY(shell.window->createWorkspace(path));
        shell.draw(*shell.window->activePage());
        shell.action("actionNewPage")->trigger();
        QVERIFY(shell.window->close()); // quitting
        QCOMPARE(shell.dialogs->questions, 0);
        QVERIFY(!shell.window->hasWorkspace());
        // The workspace reopens on the next start.
        QCOMPARE(shell.window->lastWorkspace().has_value(), true);
    }

    void aWorkspaceInUseOpensReadOnly() {
        Shell first(settings(QStringLiteral("lock1")));
        const auto path = freshPath(QStringLiteral("Shared"));
        QVERIFY(first.window->createWorkspace(path));
        Shell second(settings(QStringLiteral("lock2")));
        QVERIFY(second.window->openWorkspace(path));
        QCOMPARE(second.dialogs->questions, 1); // "open read-only?"
        QVERIFY(second.window->session()->isReadOnly());
        QVERIFY(!second.action("actionNewPage")->isEnabled());
        QVERIFY(!second.action("actionUndo")->isEnabled());
        QVERIFY(second.window->windowTitle().contains(QStringLiteral("read-only")));
        // Declining leaves the window as it was.
        Shell third(settings(QStringLiteral("lock3")));
        third.dialogs->openReadOnly = false;
        QVERIFY(!third.window->openWorkspace(path));
        QVERIFY(!third.window->hasWorkspace());
    }

    void notAWorkspaceIsReported() {
        Shell shell(settings(QStringLiteral("invalid")));
        QTemporaryDir empty;
        QVERIFY(!shell.window->openWorkspace(toPath(empty.path())));
        QCOMPARE(shell.dialogs->errors.size(), 1);
        QVERIFY(!shell.window->hasWorkspace());
    }

    void themeToggleIsACompactIcon() {
        Shell shell(settings(QStringLiteral("theme")));
        QAction* toggle = shell.action("actionToggleTheme");
        QVERIFY(!toggle->icon().isNull());
        const qint64 lightIcon = toggle->icon().cacheKey();
        QVERIFY(toggle->toolTip().contains(QStringLiteral("dark")));
        toggle->trigger();
        QVERIFY(shell.themes.isDark());
        QVERIFY(toggle->icon().cacheKey() != lightIcon);
        QVERIFY(toggle->toolTip().contains(QStringLiteral("light")));
        toggle->trigger();
        QVERIFY(!shell.themes.isDark());
    }

    void droppingTheOpenPageKeepsItVisible() {
        Shell shell(settings(QStringLiteral("drop")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Drop"))));
        const core::PageId first = *shell.window->activePage();
        shell.action("actionNewSection")->trigger();
        const core::SectionId other = shell.ws().findPage(*shell.window->activePage())->section;
        QVERIFY(shell.window->openPage(first));
        shell.tree()->collapse(shell.indexOf(other));

        // Drag-and-drop in the tree: the move runs as one command through the session.
        auto* model = static_cast<WorkspaceTreeModel*>(shell.model());
        const std::unique_ptr<QMimeData> data(model->mimeData({shell.indexOf(first)}));
        model->dropMimeData(data.get(), Qt::MoveAction, -1, 0, shell.indexOf(other));
        QCOMPARE(shell.ws().findPage(first)->section, other);
        QCOMPARE(shell.window->session()->history().nextUndo()->label, std::string("Move page"));
        QCOMPARE(shell.window->activePage(), first);
        QVERIFY(shell.tree()->isExpanded(shell.indexOf(other))); // the open page is revealed
        QCOMPARE(shell.tree()->currentIndex(), shell.indexOf(first));
    }

    void collapsingTheOpenPagesSectionSticks() {
        Shell shell(settings(QStringLiteral("collapse")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Collapse"))));
        const core::PageId page = *shell.window->activePage();
        const QModelIndex section = shell.indexOf(shell.ws().findPage(page)->section);
        QVERIFY(shell.tree()->isExpanded(section));
        shell.tree()->collapse(section);
        shell.draw(page); // an edit on the open page
        QVERIFY(!shell.tree()->isExpanded(section));
        shell.action("actionNewPage")->trigger(); // another page opens: revealed again
        QVERIFY(shell.tree()->isExpanded(section));
    }

    void aLastWorkspaceThatFailsIsForgotten() {
        Shell shell(settings(QStringLiteral("forget")));
        const auto path = freshPath(QStringLiteral("Vanishing"));
        QVERIFY(shell.window->createWorkspace(path));
        QVERIFY(shell.window->close()); // quitting keeps it as the start-up workspace
        QVERIFY(shell.window->lastWorkspace().has_value());
        std::filesystem::remove_all(path);
        QVERIFY(!shell.window->openWorkspace(path));
        QVERIFY(!shell.window->lastWorkspace().has_value()); // no error at every start
        QVERIFY(!shell.window->recentWorkspaces().contains(
            QDir::toNativeSeparators(QString::fromStdU16String(path.u16string()))));
    }

    void openingAnEmptyWorkspaceWritesNothing() {
        Shell shell(settings(QStringLiteral("empty")));
        const auto path = freshPath(QStringLiteral("Empty"));
        QVERIFY(shell.window->createWorkspace(path));
        const core::NotebookId notebook = shell.ws().notebooks().front();
        shell.select(notebook);
        shell.action("actionDeleteItem")->trigger(); // confirmed by the scripted dialog
        QCOMPARE(shell.ws().pageCount(), std::size_t{0});
        QVERIFY(!shell.window->activePage().has_value());
        QVERIFY(!shell.action("actionToolPen")->isEnabled());
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(shell.window->openWorkspace(path));
        QCOMPARE(shell.ws().pageCount(), std::size_t{0}); // no start page added on open
        QCOMPARE(shell.window->session()->history().undoCount(), std::size_t{0});
        QVERIFY(shell.action("actionNewNotebook")->isEnabled());
        shell.action("actionNewNotebook")->trigger();
        QVERIFY(shell.window->activePage().has_value());
    }

    void aStaleLockOffersRecoverReadOnlyOrCancel() {
        Shell shell(settings(QStringLiteral("stale")));
        const auto path = freshPath(QStringLiteral("Stale"));
        QVERIFY(shell.window->createWorkspace(path));
        QVERIFY(shell.window->closeWorkspace());
        const auto leaveStaleLock = [&] { // as a crashed process leaves it
            QFile lock(QString::fromStdU16String((path / ".lock").u16string()));
            QVERIFY(lock.open(QIODevice::WriteOnly));
            lock.write("not a live lock\n");
        };
        leaveStaleLock();
        shell.dialogs->staleLock = ui::ShellDialogs::StaleLockChoice::Cancel;
        QVERIFY(!shell.window->openWorkspace(path));
        QVERIFY(!shell.window->hasWorkspace());
        shell.dialogs->staleLock = ui::ShellDialogs::StaleLockChoice::ReadOnly;
        QVERIFY(shell.window->openWorkspace(path));
        QVERIFY(shell.window->session()->isReadOnly());
        QVERIFY(shell.window->closeWorkspace());
        shell.dialogs->staleLock = ui::ShellDialogs::StaleLockChoice::Recover;
        QVERIFY(shell.window->openWorkspace(path));
        QVERIFY(!shell.window->session()->isReadOnly());
        QVERIFY(shell.window->session()->recoveredStaleLock());
        QCOMPARE(shell.dialogs->questions, 3);
        QVERIFY(shell.dialogs->errors.isEmpty());
    }

    void welcomeScreenTextIsNotSquashed() {
        Shell shell(settings(QStringLiteral("welcomeLayout")));
        auto* welcome = shell.window->findChild<QWidget*>(QStringLiteral("placeholder"));
        QVERIFY(welcome->isVisible());
        QTest::qWait(0);
        auto* title = welcome->findChild<QLabel*>(QStringLiteral("placeholderTitle"));
        const auto labels = welcome->findChildren<QLabel*>(QStringLiteral("placeholderSubtitle"));
        QVERIFY(title != nullptr && !labels.isEmpty());
        QLabel* subtitle = labels.front(); // the explanation (the first one created)
        // The wrapped explanation gets the height it needs and sits below the title.
        QVERIFY(subtitle->wordWrap());
        QVERIFY(subtitle->height() >= subtitle->heightForWidth(subtitle->width()));
        QVERIFY(subtitle->geometry().top() >= title->geometry().bottom());
        auto* newButton = welcome->findChild<QWidget*>(QStringLiteral("welcomeNewWorkspace"));
        QVERIFY(newButton != nullptr && newButton->isVisible());
        QVERIFY(newButton->mapTo(welcome, QPoint(0, 0)).y() >=
                subtitle->mapTo(welcome, QPoint(0, subtitle->height())).y());
    }

    void navigationCanBeHidden() {
        Shell shell(settings(QStringLiteral("navpanel")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Panel"))));
        auto* panel = shell.window->findChild<QWidget*>(QStringLiteral("navigationPanel"));
        QVERIFY(panel->isVisible());
        shell.action("actionNavigation")->trigger();
        QVERIFY(!panel->isVisible());
        shell.action("actionNavigation")->trigger();
        QVERIFY(panel->isVisible());
    }
};

QTEST_MAIN(ShellTest)
#include "ShellTest.moc"
