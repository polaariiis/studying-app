// Phase 5 shell: the navigation tree model (targeted updates, drag-and-drop requests) and
// the MainWindow workflows — create/open/close workspaces, navigate pages, structure
// edits through the session, undo across pages, locks, no save questions. Runs on the
// offscreen platform (the canvas has no OpenGL there; everything else is real).

#include "PageExport.hpp"
#include "PlannerPanel.hpp"
#include "SessionDocumentRasterizer.hpp"
#include "SessionImageSource.hpp"
#include "WorkspaceTreeModel.hpp"

#include <studyapp/application/PageNavigator.hpp>
#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/canvas/ElementGeometry.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/document/Editor.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/platform/QtTextLayout.hpp>
#include <studyapp/platform/QtWorkspaceLocker.hpp>
#include <studyapp/testing/ManualClock.hpp>
#include <studyapp/testing/SequentialIds.hpp>
#include <studyapp/ui/MainWindow.hpp>
#include <studyapp/ui/ShellDialogs.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QAbstractButton>
#include <QAbstractItemModelTester>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPrinter>
#include <QSettings>
#include <QSignalSpy>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
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
    std::optional<std::filesystem::path> insertImage;
    std::optional<std::filesystem::path> importDocument;
    std::optional<std::filesystem::path> exportTarget;
    std::optional<std::filesystem::path> bundleTarget;
    std::optional<std::filesystem::path> bundleToOpen;
    QStringList exportSuggestions;
    bool exportPdfOnly = false;
    std::optional<QString> printTo;                 ///< nullopt: printing is cancelled
    std::optional<QPrinter::PrintRange> printRange; ///< nullopt: the default (all)
    int printFrom = 0;
    int printToPage = 0;
    int printPages = 0;
    bool openReadOnly = true;
    StaleLockChoice staleLock = StaleLockChoice::Recover;
    bool confirmDeletes = true;
    UnsavedChoice unsaved = UnsavedChoice::Cancel;
    /// Called when asked about unsaved changes (e.g. to repair the fault before Retry).
    std::function<void()> onUnsavedQuestion;
    int questions = 0;
    int unsavedQuestions = 0;
    int deleteQuestions = 0;
    QStringList errors;
    QStringList information;

    std::optional<std::filesystem::path> chooseNewWorkspace(QWidget*) override {
        ++questions;
        return newWorkspace;
    }
    std::optional<std::filesystem::path> chooseWorkspaceToOpen(QWidget*) override {
        ++questions;
        return openWorkspace;
    }
    std::optional<std::filesystem::path> chooseImageToInsert(QWidget*) override {
        ++questions;
        return insertImage;
    }
    std::optional<std::filesystem::path> chooseDocumentToImport(QWidget*) override {
        ++questions;
        return importDocument;
    }
    std::optional<std::filesystem::path> chooseExportTarget(QWidget*, const QString& name,
                                                            bool pdfOnly) override {
        ++questions;
        exportSuggestions << name;
        exportPdfOnly = pdfOnly;
        return exportTarget;
    }
    std::optional<std::filesystem::path> chooseBundleTarget(QWidget*,
                                                            const QString& name) override {
        ++questions;
        exportSuggestions << name;
        return bundleTarget;
    }
    std::optional<std::filesystem::path> chooseBundleToOpen(QWidget*) override {
        ++questions;
        return bundleToOpen;
    }
    bool setUpPrinter(QWidget*, QPrinter& printer, int pages) override {
        ++questions;
        printPages = pages;
        if (printRange) {
            printer.setPrintRange(*printRange);
            printer.setFromTo(printFrom, printToPage);
        }
        if (!printTo) {
            return false;
        }
        printer.setOutputFormat(QPrinter::PdfFormat); // "print" into a PDF file
        printer.setOutputFileName(*printTo);
        return true;
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
    /// Answers for askText, first to last; cancelled when none is left.
    QStringList texts;
    std::optional<QString> askText(QWidget*, const QString&, const QString&,
                                   const QString&) override {
        ++questions;
        if (texts.isEmpty()) {
            return std::nullopt;
        }
        return texts.takeFirst();
    }
    UnsavedChoice askUnsavedChanges(QWidget*, std::size_t, const QString&) override {
        ++questions;
        ++unsavedQuestions;
        const UnsavedChoice answer = unsaved;
        if (onUnsavedQuestion) {
            onUnsavedQuestion();
        }
        return answer;
    }
    void showError(QWidget*, const QString& summary, const QString&) override { errors << summary; }
    void showInformation(QWidget*, const QString& summary, const QString& details) override {
        information << summary + QStringLiteral("\n") + details;
    }
};

/// Makes every element insert fail (a trigger created from a second connection), or
/// removes that fault again — as the persistence tests inject write failures.
void injectWriteFailure(const std::filesystem::path& workspace, bool on) {
    auto db =
        persistence::Database::open(workspace / "workspace.db", persistence::OpenMode::ReadWrite);
    QVERIFY(db.has_value());
    const auto done = db->execute(on ? "CREATE TRIGGER inject_failure BEFORE INSERT ON element "
                                       "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;"
                                     : "DROP TRIGGER inject_failure");
    QVERIFY(done.has_value());
}

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
        platform::QtTextLayout textLayout;
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
            window->setTextLayout(&textLayout);
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
        /// Imports copy files on pool threads (Phase 9); waits until they are stored.
        void waitForImports() const { QTRY_COMPARE(window->backgroundJobCount(), 0); }
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

    // P3-01, Phase 9: when writes keep failing, "Save a Copy" writes everything — the
    // unsaved changes too — into a new workspace, closes the failing one and opens the copy.
    void unsavedChangesCanBeSavedAsACopy() {
        Shell shell(settings(QStringLiteral("savecopy")));
        const auto path = freshPath(QStringLiteral("Failing"));
        QVERIFY(shell.window->createWorkspace(path));
        const auto page = *shell.window->activePage();
        const auto layer = shell.ws().layersOf(page).front();
        injectWriteFailure(path, true);
        auto note = document::commands::createElement(
            shell.ws(), layer,
            {.payload = document::TextBox{.size = {200, 40}, .text = "only in memory"}}, shell.ids);
        QVERIFY(note.has_value());
        QVERIFY(shell.window->session()->execute(std::move(note->command)).has_value());
        QVERIFY(shell.window->session()->pendingWriteCount() > 0);
        const auto copy = freshPath(QStringLiteral("Rescued"));
        shell.dialogs->unsaved = ScriptedDialogs::UnsavedChoice::SaveCopy;
        shell.dialogs->newWorkspace = copy;
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(shell.window->session() != nullptr);
        QCOMPARE(shell.window->session()->root(), copy);
        QVERIFY(shell.ws().findElement(note->id) != nullptr);
        QCOMPARE(shell.window->session()->pendingWriteCount(), std::size_t{0});
        injectWriteFailure(path, false);
    }

    // Save a Copy chosen while another workspace is being opened: the other one opens (not
    // the copy, which is only remembered), and exactly one canvas remains.
    void savingACopyWhileOpeningAnotherOpensTheOther() {
        Shell shell(settings(QStringLiteral("savecopyopen")));
        const auto other = freshPath(QStringLiteral("OpenedNext"));
        QVERIFY(shell.window->createWorkspace(other));
        const auto path = freshPath(QStringLiteral("FailingToo"));
        QVERIFY(shell.window->createWorkspace(path));
        injectWriteFailure(path, true);
        const auto kept = shell.draw(*shell.window->activePage());
        QVERIFY(shell.window->session()->pendingWriteCount() > 0);
        const auto copy = freshPath(QStringLiteral("RescuedToo"));
        shell.dialogs->unsaved = ScriptedDialogs::UnsavedChoice::SaveCopy;
        shell.dialogs->newWorkspace = copy;
        QVERIFY(shell.window->openWorkspace(other));
        QCOMPARE(shell.window->session()->root(), other);
        int canvases = 0;
        for (QWidget* widget : shell.window->findChildren<QWidget*>()) {
            canvases += widget->inherits("studyapp::ui::CanvasWidget") ? 1 : 0;
        }
        QCOMPARE(canvases, 1);
        QVERIFY(shell.window->recentWorkspaces().contains(
            QDir::toNativeSeparators(QString::fromStdU16String(copy.u16string()))));
        QApplication::processEvents(); // a paint of a stale canvas would crash here
        injectWriteFailure(path, false);
        auto rescued = application::WorkspaceSession::open(
            copy, {}, {.clock = shell.clock, .ids = shell.ids, .locker = shell.locker});
        QVERIFY(rescued.has_value());
        QVERIFY((*rescued)->workspace().findElement(kept) != nullptr);
        QVERIFY((*rescued)->close().has_value());
    }

    // An import finishing while closing asks about unsaved changes waits for the answer:
    // applied when the user keeps the workspace.
    void importsFinishingDuringTheCloseQuestionWait() {
        Shell shell(settings(QStringLiteral("importwait")));
        const auto path = freshPath(QStringLiteral("ImportWait"));
        QVERIFY(shell.window->createWorkspace(path));
        const auto page = *shell.window->activePage();
        const auto layer = shell.ws().layersOf(page).front();
        injectWriteFailure(path, true);
        (void)shell.draw(page);
        QImage red(64, 32, QImage::Format_RGB32);
        red.fill(QColor(200, 30, 30));
        const QString png = dir_.filePath(QStringLiteral("waiting.png"));
        QVERIFY(red.save(png));
        shell.dialogs->insertImage = toPath(png);
        shell.action("actionInsertImage")->trigger();
        const auto before = shell.ws().elementsOf(layer).size();
        bool deferred = false;
        shell.dialogs->unsaved = ScriptedDialogs::UnsavedChoice::Cancel;
        shell.dialogs->onUnsavedQuestion = [&] {
            // The dialog's event loop delivers the result; the image must not appear yet.
            (void)QTest::qWaitFor([&] { return shell.window->backgroundJobCount() == 0; });
            QApplication::processEvents();
            deferred = shell.ws().elementsOf(layer).size() == before;
        };
        QVERIFY(!shell.window->closeWorkspace());
        shell.dialogs->onUnsavedQuestion = {};
        QVERIFY(deferred);
        QCOMPARE(shell.ws().elementsOf(layer).size(), before + 1); // applied after "Cancel"
        injectWriteFailure(path, false);
    }

    void failedWritesAreNeverClosedOverSilently() {
        Shell shell(settings(QStringLiteral("unsaved")));
        const auto path = freshPath(QStringLiteral("Unsaved"));
        QVERIFY(shell.window->createWorkspace(path));
        const core::PageId page = *shell.window->activePage();
        injectWriteFailure(path, true);
        const auto kept = shell.draw(page); // in memory, not in the database
        QCOMPARE(shell.window->session()->pendingWriteCount(), std::size_t{1});
        QTRY_VERIFY(shell.window->findChild<QLabel*>(QStringLiteral("saveStatusLabel"))
                        ->text()
                        .startsWith(QStringLiteral("Not saved")));

        // Cancel: the workspace stays open with the change; quitting is refused too.
        shell.dialogs->unsaved = ui::ShellDialogs::UnsavedChoice::Cancel;
        QVERIFY(!shell.window->closeWorkspace());
        QVERIFY(shell.window->hasWorkspace());
        QVERIFY(!shell.window->close());
        QVERIFY(shell.window->hasWorkspace());
        QCOMPARE(shell.dialogs->unsavedQuestions, 2);

        // Retry after the cause is gone: written, then closed.
        shell.dialogs->unsaved = ui::ShellDialogs::UnsavedChoice::Retry;
        shell.dialogs->onUnsavedQuestion = [&] {
            injectWriteFailure(path, false);
        };
        QVERIFY(shell.window->closeWorkspace());
        QCOMPARE(shell.dialogs->unsavedQuestions, 3);
        shell.dialogs->onUnsavedQuestion = {};
        QVERIFY(shell.window->openWorkspace(path));
        QVERIFY(shell.ws().findElement(kept) != nullptr); // it reached the database

        // Close without saving: the user's explicit choice; the change is gone.
        injectWriteFailure(path, true);
        const auto lost = shell.draw(page);
        shell.dialogs->unsaved = ui::ShellDialogs::UnsavedChoice::Discard;
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(!shell.window->hasWorkspace());
        injectWriteFailure(path, false);
        QVERIFY(shell.window->openWorkspace(path));
        QCOMPARE(shell.ws().findElement(lost), nullptr);
        QVERIFY(shell.ws().findElement(kept) != nullptr);
    }

    // Phase 6, step 1: the pen style (ink, width) is tool state of the shell — it decides new
    // strokes, is stored with each stroke in the document, and is remembered per user.
    void penStyleDecidesNewStrokesAndIsRemembered() {
        const QString name = QStringLiteral("penstyle");
        const auto path = freshPath(QStringLiteral("Pen"));
        const auto drawOnCanvas = [](ui::MainWindow& window, QPoint from, QPoint to) {
            auto* canvas = window.findChild<QWidget*>(QStringLiteral("canvasWidget"));
            QTest::mousePress(canvas, Qt::LeftButton, {}, from);
            for (int i = 1; i <= 8; ++i) {
                QTest::mouseMove(canvas, from + (to - from) * i / 8, 2);
            }
            QTest::mouseRelease(canvas, Qt::LeftButton, {}, to);
        };
        const auto lastStroke = [](const document::Workspace& ws, core::PageId page) {
            const auto elements = ws.elementsOf(ws.layersOf(page).front());
            return std::get<document::Stroke>(ws.findElement(elements.back())->payload);
        };
        {
            Shell shell(settings(name));
            QVERIFY(shell.window->createWorkspace(path));
            // Defaults: black, medium, both shown as checked.
            QVERIFY(shell.action("actionPenColor_black")->isChecked());
            QVERIFY(shell.action("actionPenWidth_medium")->isChecked());
            const auto undoBefore = shell.window->session()->history().undoCount();
            shell.action("actionToolEraser")->trigger();
            shell.action("actionPenColor_blue")->trigger(); // also switches back to the pen
            QVERIFY(shell.action("actionToolPen")->isChecked());
            shell.action("actionPenWidth_thick")->trigger();
            QVERIFY(shell.action("actionPenColor_blue")->isChecked());
            QVERIFY(!shell.action("actionPenColor_black")->isChecked());
            // Choosing a style is not an edit.
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore);

            drawOnCanvas(*shell.window, {120, 120}, {320, 160});
            const document::Stroke stroke = lastStroke(shell.ws(), *shell.window->activePage());
            QCOMPARE(stroke.brush, document::Brush::Pen);
            QCOMPARE(stroke.color, core::Color::fromRgba(0x2F, 0x5F, 0xA8));
            QCOMPARE(stroke.baseWidth, 4.0F);
            QVERIFY(shell.window->close());
        }
        {
            // A new window remembers the style; a stroke drawn with it and the one before
            // survive reopening with their own styles.
            Shell shell(settings(name));
            QVERIFY(shell.action("actionPenColor_blue")->isChecked());
            QVERIFY(shell.action("actionPenWidth_thick")->isChecked());
            QVERIFY(shell.window->openWorkspace(path));
            shell.action("actionPenColor_red")->trigger();
            drawOnCanvas(*shell.window, {120, 260}, {320, 300});
            const core::PageId page = *shell.window->activePage();
            QCOMPARE(lastStroke(shell.ws(), page).color, core::Color::fromRgba(0xB3, 0x36, 0x2F));
            QVERIFY(shell.window->closeWorkspace());
            QVERIFY(shell.window->openWorkspace(path));
            const auto elements = shell.ws().elementsOf(shell.ws().layersOf(page).front());
            QCOMPARE(elements.size(), std::size_t{2});
            QCOMPARE(std::get<document::Stroke>(shell.ws().findElement(elements[0])->payload).color,
                     core::Color::fromRgba(0x2F, 0x5F, 0xA8));
            QCOMPARE(std::get<document::Stroke>(shell.ws().findElement(elements[1])->payload).color,
                     core::Color::fromRgba(0xB3, 0x36, 0x2F));
        }
    }

    // Phase 6, step 2: the highlighter is a tool of its own with its own remembered style;
    // the style button follows the ink tool chosen last; neither choice is an edit.
    void highlighterIsAToolWithItsOwnStyle() {
        const QString name = QStringLiteral("highlighter");
        const auto path = freshPath(QStringLiteral("Highlight"));
        const auto drawOnCanvas = [](ui::MainWindow& window, QPoint from, QPoint to) {
            auto* canvas = window.findChild<QWidget*>(QStringLiteral("canvasWidget"));
            QTest::mousePress(canvas, Qt::LeftButton, {}, from);
            for (int i = 1; i <= 8; ++i) {
                QTest::mouseMove(canvas, from + (to - from) * i / 8, 2);
            }
            QTest::mouseRelease(canvas, Qt::LeftButton, {}, to);
        };
        const auto strokes = [](const document::Workspace& ws, core::PageId page) {
            std::vector<document::Stroke> out;
            for (const core::ElementId id : ws.elementsOf(ws.layersOf(page).front())) {
                out.push_back(std::get<document::Stroke>(ws.findElement(id)->payload));
            }
            return out;
        };
        const core::Color green = core::Color::fromRgba(0x8C, 0xCF, 0x7E, 0x73);
        const core::Color pink = core::Color::fromRgba(0xEE, 0x9D, 0xB6, 0x73);
        {
            Shell shell(settings(name));
            QVERIFY(shell.window->createWorkspace(path));
            auto* style = shell.window->findChild<QToolButton*>(QStringLiteral("inkStyleButton"));
            auto* penMenu = shell.window->findChild<QMenu*>(QStringLiteral("menuPenStyle"));
            auto* highlighterMenu =
                shell.window->findChild<QMenu*>(QStringLiteral("menuHighlighterStyle"));
            QVERIFY(style != nullptr && penMenu != nullptr && highlighterMenu != nullptr);
            // Tools has both style menus under their own names; the toolbar button only
            // borrows them.
            auto* tools = shell.window->findChild<QMenu*>(QStringLiteral("menuTools"));
            QVERIFY(tools->actions().contains(penMenu->menuAction()));
            QVERIFY(tools->actions().contains(highlighterMenu->menuAction()));
            QCOMPARE(penMenu->menuAction()->menu(), penMenu);
            QCOMPARE(highlighterMenu->menuAction()->menu(), highlighterMenu);
            QCOMPARE(penMenu->menuAction()->text(), QStringLiteral("Pen St&yle"));
            QCOMPARE(highlighterMenu->menuAction()->text(), QStringLiteral("Highlighter Sty&le"));
            // Defaults: the pen is active; the highlighter is yellow, medium.
            QVERIFY(shell.action("actionToolPen")->isChecked());
            QVERIFY(shell.action("actionHighlighterColor_yellow")->isChecked());
            QVERIFY(shell.action("actionHighlighterWidth_medium")->isChecked());
            QCOMPARE(style->menu(), penMenu);
            QVERIFY(style->toolTip().startsWith(QStringLiteral("Pen style")));
            const auto undoBefore = shell.window->session()->history().undoCount();

            // Choosing the tool shows its style and its cursor; nothing is edited.
            shell.action("actionToolHighlighter")->trigger();
            QVERIFY(shell.action("actionToolHighlighter")->isChecked());
            QVERIFY(!shell.action("actionToolPen")->isChecked());
            QCOMPARE(style->menu(), highlighterMenu);
            QVERIFY(style->toolTip().startsWith(QStringLiteral("Highlighter style: Yellow")));
            auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
            QCOMPARE(canvas->cursor().shape(), Qt::BitmapCursor);
            shell.action("actionHighlighterColor_green")->trigger();
            shell.action("actionHighlighterWidth_thick")->trigger();
            QVERIFY(style->toolTip().startsWith(QStringLiteral("Highlighter style: Green")));
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore);

            drawOnCanvas(*shell.window, {120, 120}, {420, 140});
            const core::PageId page = *shell.window->activePage();
            QCOMPARE(strokes(shell.ws(), page).back().brush, document::Brush::Highlighter);
            QCOMPARE(strokes(shell.ws(), page).back().color, green);
            QCOMPARE(strokes(shell.ws(), page).back().baseWidth, 22.0F);

            // Back to the pen: its own style is still there; the menus kept their entries.
            shell.action("actionToolPen")->trigger();
            QCOMPARE(style->menu(), penMenu);
            QCOMPARE(penMenu->menuAction()->menu(), penMenu);
            QCOMPARE(highlighterMenu->menuAction()->menu(), highlighterMenu);
            QCOMPARE(canvas->cursor().shape(), Qt::CrossCursor);
            drawOnCanvas(*shell.window, {120, 220}, {420, 240});
            QCOMPARE(strokes(shell.ws(), page).back().brush, document::Brush::Pen);
            QCOMPARE(strokes(shell.ws(), page).back().color, core::Color::black());
            QCOMPARE(strokes(shell.ws(), page).back().baseWidth, 2.0F);
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 2);

            // A highlighter ink chosen from the menu means highlighting next.
            shell.action("actionHighlighterColor_pink")->trigger();
            QVERIFY(shell.action("actionToolHighlighter")->isChecked());
            // Undo takes back the pen stroke only; the highlighter stroke is unchanged.
            shell.action("actionUndo")->trigger();
            QCOMPARE(strokes(shell.ws(), page).size(), std::size_t{1});
            QCOMPARE(strokes(shell.ws(), page).front().color, green);
            shell.action("actionRedo")->trigger();
            QCOMPARE(strokes(shell.ws(), page).size(), std::size_t{2});
            QVERIFY(shell.window->close());
        }
        {
            // Both styles are remembered; the strokes reopen as drawn.
            Shell shell(settings(name));
            QVERIFY(shell.action("actionHighlighterColor_pink")->isChecked());
            QVERIFY(shell.action("actionHighlighterWidth_thick")->isChecked());
            QVERIFY(shell.action("actionPenColor_black")->isChecked());
            QVERIFY(shell.action("actionPenWidth_medium")->isChecked());
            QVERIFY(shell.window->openWorkspace(path));
            const auto reopened = strokes(shell.ws(), *shell.window->activePage());
            QCOMPARE(reopened.size(), std::size_t{2});
            QCOMPARE(reopened[0].brush, document::Brush::Highlighter);
            QCOMPARE(reopened[0].color, green);
            QCOMPARE(reopened[1].brush, document::Brush::Pen);
            shell.action("actionToolHighlighter")->trigger();
            drawOnCanvas(*shell.window, {120, 320}, {420, 340});
            QCOMPARE(strokes(shell.ws(), *shell.window->activePage()).back().color, pink);
        }
    }

    // Phase 6, step 3: the eraser erases partially by default (vector pieces remain) or
    // whole strokes; the mode is remembered per user and choosing it selects the eraser.
    void eraserModesThroughTheShell() {
        const QString name = QStringLiteral("erasermode");
        const auto path = freshPath(QStringLiteral("Erase"));
        const auto drag = [](ui::MainWindow& window, QPoint from, QPoint to) {
            auto* canvas = window.findChild<QWidget*>(QStringLiteral("canvasWidget"));
            QTest::mousePress(canvas, Qt::LeftButton, {}, from);
            for (int i = 1; i <= 8; ++i) {
                QTest::mouseMove(canvas, from + (to - from) * i / 8, 2);
            }
            QTest::mouseRelease(canvas, Qt::LeftButton, {}, to);
        };
        const auto inkCount = [](const Shell& shell) {
            const auto page = *shell.window->activePage();
            return shell.ws().elementsOf(shell.ws().layersOf(page).front()).size();
        };
        {
            Shell shell(settings(name));
            QVERIFY(shell.window->createWorkspace(path));
            QVERIFY(shell.action("actionEraserPartial")->isChecked());
            drag(*shell.window, {120, 150}, {420, 150});
            shell.action("actionToolEraser")->trigger();
            drag(*shell.window, {270, 110}, {270, 190});
            QCOMPARE(inkCount(shell), std::size_t{2}); // cut in two
            shell.action("actionEraserWholeStrokes")->trigger();
            QVERIFY(shell.action("actionToolEraser")->isChecked());
            drag(*shell.window, {150, 110}, {150, 190});
            QCOMPARE(inkCount(shell), std::size_t{1}); // the left piece, whole
            QVERIFY(shell.window->close());
        }
        {
            Shell shell(settings(name));
            QVERIFY(shell.action("actionEraserWholeStrokes")->isChecked());
        }
    }

    // Phase 6, step 4: shapes through the shell — kind, ink, width and fill are tool state
    // remembered per user; each drag is one shape, one undo step.
    void shapesThroughTheShell() {
        const QString name = QStringLiteral("shapes");
        const auto path = freshPath(QStringLiteral("Shapes"));
        const auto drag = [](ui::MainWindow& window, QPoint from, QPoint to) {
            auto* canvas = window.findChild<QWidget*>(QStringLiteral("canvasWidget"));
            QTest::mousePress(canvas, Qt::LeftButton, {}, from);
            for (int i = 1; i <= 6; ++i) {
                QTest::mouseMove(canvas, from + (to - from) * i / 6, 2);
            }
            QTest::mouseRelease(canvas, Qt::LeftButton, {}, to);
        };
        const auto shapes = [](const Shell& shell) {
            std::vector<document::Shape> out;
            const auto page = *shell.window->activePage();
            for (const auto id : shell.ws().elementsOf(shell.ws().layersOf(page).front())) {
                out.push_back(std::get<document::Shape>(shell.ws().findElement(id)->payload));
            }
            return out;
        };
        {
            Shell shell(settings(name));
            QVERIFY(shell.window->createWorkspace(path));
            auto* style = shell.window->findChild<QToolButton*>(QStringLiteral("inkStyleButton"));
            const auto undoBefore = shell.window->session()->history().undoCount();
            shell.action("actionShapeKind_arrow")->trigger(); // also selects the shape tool
            QVERIFY(shell.action("actionToolShape")->isChecked());
            QVERIFY(style->toolTip().startsWith(QStringLiteral("Shape style: Arrow")));
            shell.action("actionShapeColor_blue")->trigger();
            shell.action("actionShapeWidth_thick")->trigger();
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore);
            drag(*shell.window, {150, 150}, {350, 250});
            shell.action("actionShapeKind_ellipse")->trigger();
            shell.action("actionShapeFill")->trigger();
            drag(*shell.window, {150, 300}, {350, 400});
            const auto made = shapes(shell);
            QCOMPARE(made.size(), std::size_t{2});
            QCOMPARE(made[0].kind, document::ShapeKind::Arrow);
            QCOMPARE(*made[0].strokeColor, core::Color::fromRgba(0x2F, 0x5F, 0xA8));
            QCOMPARE(made[0].strokeWidth, 4.0F);
            QCOMPARE(made[1].kind, document::ShapeKind::Ellipse);
            QVERIFY(made[1].fillColor.has_value());
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 2);
            shell.action("actionUndo")->trigger();
            QCOMPARE(shapes(shell).size(), std::size_t{1});
            QVERIFY(shell.window->close());
        }
        {
            Shell shell(settings(name));
            QVERIFY(shell.action("actionShapeKind_ellipse")->isChecked());
            QVERIFY(shell.action("actionShapeFill")->isChecked());
            QVERIFY(shell.action("actionShapeColor_blue")->isChecked());
            QVERIFY(shell.window->openWorkspace(path));
            QCOMPARE(shapes(shell).size(), std::size_t{1});
            QCOMPARE(shapes(shell)[0].kind, document::ShapeKind::Arrow);
        }
    }

    // Phase 6, step 5: text boxes are typed into a plain-text editor over the canvas; the
    // editor keeps letters and its own undo from the window's shortcuts; finishing writes
    // one command with the text as UTF-8.
    void textThroughTheShell() {
        Shell shell(settings(QStringLiteral("text")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Text"))));
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        const auto boxes = [&] {
            std::vector<document::TextBox> out;
            const auto page = *shell.window->activePage();
            for (const auto id : shell.ws().elementsOf(shell.ws().layersOf(page).front())) {
                out.push_back(std::get<document::TextBox>(shell.ws().findElement(id)->payload));
            }
            return out;
        };
        const auto undoBefore = shell.window->session()->history().undoCount();
        shell.action("actionToolText")->trigger();
        auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
        QTest::mouseClick(canvas, Qt::LeftButton, {}, {200, 200});
        auto* editor = shell.window->findChild<QTextEdit*>(QStringLiteral("textEditor"));
        QVERIFY(editor != nullptr);
        QVERIFY(editor->isVisible());
        QTRY_VERIFY(editor->hasFocus());
        // Letters are text, not tool shortcuts; Ctrl+Z is the editor's, not the document's.
        QTest::keyClicks(editor, QStringLiteral("Pm"));
        QVERIFY(shell.action("actionToolText")->isChecked());
        QCOMPARE(editor->toPlainText(), QStringLiteral("Pm"));
        QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(shell.window->session()->history().undoCount(), undoBefore);
        QVERIFY(boxes().empty());
        editor->setPlainText(QString::fromUtf8("Pm\nW\xC3\xB6rld \xE2\x9C\x93"));
        QTest::keyClick(editor, Qt::Key_Escape); // finishes (and writes) the edit
        QVERIFY(!editor->isVisible());
        const auto made = boxes();
        QCOMPARE(made.size(), std::size_t{1});
        QCOMPARE(made[0].text, std::string("Pm\nW\xC3\xB6rld \xE2\x9C\x93"));
        QVERIFY(made[0].size.y > 30.0F); // two lines, laid out by the Qt text layout
        QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 1);
        // Editing it again: clicking the box opens the editor with its text.
        QTest::mouseClick(canvas, Qt::LeftButton, {}, {220, 210});
        QVERIFY(editor->isVisible());
        QCOMPARE(editor->toPlainText(), QString::fromUtf8("Pm\nW\xC3\xB6rld \xE2\x9C\x93"));
        editor->setPlainText(QStringLiteral("changed"));
        canvas->setFocus(); // focus leaving the editor finishes it too
        QTRY_VERIFY(!editor->isVisible());
        QCOMPARE(boxes().at(0).text, std::string("changed"));
        shell.action("actionUndo")->trigger();
        QCOMPARE(boxes().at(0).text, std::string("Pm\nW\xC3\xB6rld \xE2\x9C\x93"));
    }

    // 1.2-TXT-02: Tools ▸ Text Size sets the size of new text boxes (remembered), of the box
    // being edited (the editor follows at once; written with the text) and of the selected
    // text boxes (one undoable command). The style button shows it for the text tool.
    void textSizeThroughTheShell() {
        const QString name = QStringLiteral("textsize");
        {
            Shell shell(settings(name));
            QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("TextSize"))));
            shell.window->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
            const auto boxes = [&] {
                std::vector<document::TextBox> out;
                const auto page = *shell.window->activePage();
                for (const auto id : shell.ws().elementsOf(shell.ws().layersOf(page).front())) {
                    out.push_back(std::get<document::TextBox>(shell.ws().findElement(id)->payload));
                }
                return out;
            };
            auto* style = shell.window->findChild<QToolButton*>(QStringLiteral("inkStyleButton"));
            shell.action("actionToolText")->trigger();
            QCOMPARE(style->toolTip(), QStringLiteral("Text size: 16"));
            QVERIFY(shell.action("actionTextSize_16")->isChecked());
            auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
            QTest::mouseClick(canvas, Qt::LeftButton, {}, {200, 200});
            auto* editor = shell.window->findChild<QTextEdit*>(QStringLiteral("textEditor"));
            QVERIFY(editor != nullptr);
            QTRY_VERIFY(editor->hasFocus());
            const int before = editor->font().pixelSize();
            const auto undoBefore = shell.window->session()->history().undoCount();
            shell.action("actionTextSize_32")->trigger();
            QVERIFY(editor->isVisible()); // the edit goes on
            QVERIFY(std::abs(editor->font().pixelSize() - 2 * before) <= 1);
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore);
            QTest::keyClicks(editor, QStringLiteral("Big"));
            QTest::keyClick(editor, Qt::Key_Escape);
            QCOMPARE(boxes().size(), std::size_t{1});
            QCOMPARE(boxes()[0].fontSize, 32.0F);
            QCOMPARE(boxes()[0].text, std::string("Big"));
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 1);

            // The selected box: one command; undo restores it.
            shell.action("actionToolSelect")->trigger();
            QTest::mouseClick(canvas, Qt::LeftButton, {}, {210, 210});
            shell.action("actionTextSize_12")->trigger();
            QCOMPARE(boxes()[0].fontSize, 12.0F);
            QCOMPARE(boxes()[0].text, std::string("Big"));
            QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 2);
            QVERIFY(shell.action("actionTextSize_12")->isChecked());
            QCOMPARE(style->toolTip(), QStringLiteral("Text size: 12"));
            shell.action("actionUndo")->trigger();
            QCOMPARE(boxes()[0].fontSize, 32.0F);
            QVERIFY(shell.window->close());
        }
        {
            Shell shell(settings(name)); // the size for new boxes is remembered
            QVERIFY(shell.action("actionTextSize_12")->isChecked());
        }
    }

    // Text being typed is written, not dropped, when the page changes or the workspace
    // closes from the keyboard (the focus never leaves the editor then); a popup such as the
    // editor's own context menu does not end the edit.
    void typedTextIsKeptWhenThePageChangesOrTheWorkspaceCloses() {
        Shell shell(settings(QStringLiteral("textkeep")));
        const auto path = freshPath(QStringLiteral("TextKeep"));
        QVERIFY(shell.window->createWorkspace(path));
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        const auto first = *shell.window->activePage();
        shell.action("actionNewPage")->trigger();
        const auto second = *shell.window->activePage();
        QVERIFY(first != second);
        const auto texts = [&](core::PageId page) {
            std::vector<std::string> out;
            for (const auto id : shell.ws().elementsOf(shell.ws().layersOf(page).front())) {
                out.push_back(
                    std::get<document::TextBox>(shell.ws().findElement(id)->payload).text);
            }
            return out;
        };
        auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        shell.action("actionToolText")->trigger();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, {200, 200});
        auto* editor = shell.window->findChild<QTextEdit*>(QStringLiteral("textEditor"));
        QVERIFY(editor != nullptr);
        QTRY_VERIFY(editor->hasFocus());
        QTest::keyClicks(editor, QStringLiteral("kept"));
        QFocusEvent popup(QEvent::FocusOut, Qt::PopupFocusReason); // a context menu opens
        QCoreApplication::sendEvent(editor, &popup);
        QVERIFY(editor->isVisible());
        // A right or middle click in the editor (which the editor does not take, so it
        // reaches the canvas behind it) is not a canvas click: editing goes on.
        QTimer::singleShot(150, editor, [] {
            if (QWidget* menu = QApplication::activePopupWidget()) {
                menu->close(); // the editor's context menu, if the platform opened one
            }
        });
        QTest::mouseClick(editor->viewport(), Qt::RightButton, {}, {5, 5});
        QTest::mouseClick(editor->viewport(), Qt::MiddleButton, {}, {5, 5});
        QTest::qWait(250);
        QVERIFY(editor->isVisible());
        QTRY_VERIFY(editor->hasFocus());
        QVERIFY(texts(second).empty());
        QVERIFY(texts(second).empty());
        shell.action("actionPreviousPage")->trigger(); // Ctrl+PgUp
        QCOMPARE(*shell.window->activePage(), first);
        QVERIFY(!editor->isVisible());
        QCOMPARE(texts(second), std::vector<std::string>{"kept"});

        QTest::mouseClick(canvas, Qt::LeftButton, {}, {300, 300});
        QTRY_VERIFY(editor->isVisible() && editor->hasFocus());
        QTest::keyClicks(editor, QStringLiteral("closing"));
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(shell.window->openWorkspace(path));
        QCOMPARE(texts(first), std::vector<std::string>{"closing"});
        QCOMPARE(texts(second), std::vector<std::string>{"kept"});
    }

    // Phase 6, step 6: Insert Image imports the file into the workspace's content-addressed
    // store (once per content) and adds one selected image element; files that are not
    // images are refused with a message.
    void insertImageThroughTheShell() {
        Shell shell(settings(QStringLiteral("images")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Images"))));
        QImage red(64, 32, QImage::Format_RGB32);
        red.fill(QColor(200, 30, 30));
        const QString png = dir_.filePath(QStringLiteral("red.png"));
        QVERIFY(red.save(png));
        const auto images = [&] {
            std::vector<document::Image> out;
            const auto page = *shell.window->activePage();
            for (const auto id : shell.ws().elementsOf(shell.ws().layersOf(page).front())) {
                out.push_back(std::get<document::Image>(shell.ws().findElement(id)->payload));
            }
            return out;
        };
        const auto undoBefore = shell.window->session()->history().undoCount();
        shell.dialogs->insertImage = toPath(png);
        shell.action("actionInsertImage")->trigger();
        shell.action("actionInsertImage")->trigger(); // the same file again
        shell.waitForImports();
        const auto made = images();
        QCOMPARE(made.size(), std::size_t{2});
        // At most one world unit per pixel (smaller when the view is small), aspect kept.
        QVERIFY(made[0].size.x > 0.0F && made[0].size.x <= 64.0F);
        QVERIFY(std::abs(made[0].size.x / made[0].size.y - 2.0F) < 1e-3F);
        QCOMPARE(made[0].asset, made[1].asset); // one stored copy
        QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 2);
        QVERIFY(shell.action("actionToolSelect")->isChecked());
        auto stored = shell.window->session()->assetPath(made[0].asset);
        QVERIFY(stored.has_value());
        QVERIFY(std::filesystem::exists(*stored));
        // Not an image: refused, nothing written.
        const QString text = dir_.filePath(QStringLiteral("notes.txt"));
        {
            QFile file(text);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not an image");
        }
        shell.dialogs->insertImage = toPath(text);
        shell.dialogs->errors.clear();
        shell.action("actionInsertImage")->trigger();
        shell.waitForImports();
        QCOMPARE(shell.dialogs->errors.size(), 1);
        QCOMPARE(images().size(), std::size_t{2});
        shell.action("actionUndo")->trigger();
        QCOMPARE(images().size(), std::size_t{1});
    }

    // Phase 7: the planner beside the canvas. Courses and tasks are created and edited
    // through the panel as one undoable, persisted command each; Today and Page show them;
    // canvas edits do not rebuild the planner; everything is there after reopening.
    void plannerThroughTheShell() {
        Shell shell(settings(QStringLiteral("planner")));
        const auto path = freshPath(QStringLiteral("Planner"));
        QVERIFY(shell.window->createWorkspace(path));
        const study::FixedOffsetZone zone{std::chrono::minutes{120}};
        shell.window->setTimeZone(&zone);
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        auto* panel = shell.window->findChild<ui::PlannerPanel*>(QStringLiteral("plannerPanel"));
        QVERIFY(panel != nullptr);
        QVERIFY(!panel->isVisible()); // off by default: the canvas comes first
        shell.action("actionPlanner")->trigger();
        QVERIFY(panel->isVisible());
        const auto child = [&]<class T>(T*, const char* name) {
            auto* found = panel->findChild<T*>(QString::fromLatin1(name));
            if (found == nullptr) {
                qFatal("no planner widget %s", name);
            }
            return found;
        };
        const auto settle = [&] {
            QCoreApplication::processEvents(); // the deferred refresh
            panel->refreshNow();
        };
        const auto titlesIn = [&](QTreeWidget* tree) {
            QStringList out;
            for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                if (!(*it)->data(0, Qt::UserRole).toString().isEmpty()) {
                    out << (*it)->text(0);
                }
            }
            return out;
        };
        const auto& ws = shell.ws();
        const auto steps = [&] {
            return shell.window->session()->history().undoCount();
        };

        // A course from the Tasks view's New menu (one step), then a task in it.
        panel->showView(ui::PlannerPanel::View::Tasks);
        shell.dialogs->texts = {QStringLiteral("Biology")};
        const auto before = steps();
        child(static_cast<QAction*>(nullptr), "actionPlannerNewCourse")->trigger();
        QCOMPARE(ws.courseCount(), std::size_t{1});
        QCOMPARE(steps(), before + 1);
        settle();
        auto* scope = child(static_cast<QComboBox*>(nullptr), "plannerScope");
        QCOMPARE(scope->currentText(), QStringLiteral("Biology"));
        auto* add = child(static_cast<QLineEdit*>(nullptr), "plannerTaskAdd");
        add->setFocus();
        QTest::keyClicks(add, QStringLiteral("Read chapter 3"));
        QTest::keyClick(add, Qt::Key_Return);
        settle();
        QCOMPARE(ws.taskCount(), std::size_t{1});
        const core::TaskId task = ws.topLevelTasks()[0];
        QVERIFY(ws.findTask(task)->course.has_value()); // in the chosen course
        auto* tasks = child(static_cast<QTreeWidget*>(nullptr), "plannerTasks");
        QCOMPARE(titlesIn(tasks), QStringList{QStringLiteral("Read chapter 3")});
        QCOMPARE(panel->selectedTask(), task); // the new task is selected for editing
        auto* editor = child(static_cast<QWidget*>(nullptr), "plannerTaskEditor");
        QVERIFY(editor->isVisible());

        // Editing: the title, then a due date (today) — each one step.
        auto* title = child(static_cast<QLineEdit*>(nullptr), "plannerTaskTitle");
        title->setText(QStringLiteral("Read chapter 3 and 4"));
        Q_EMIT title->editingFinished();
        QCOMPARE(ws.findTask(task)->title, std::string("Read chapter 3 and 4"));
        child(static_cast<QCheckBox*>(nullptr), "plannerTaskHasDue")->click();
        QCOMPARE(ws.findTask(task)->dueDate, study::localDate(shell.clock.now(), zone));
        panel->showView(ui::PlannerPanel::View::Today);
        settle();
        auto* agenda = child(static_cast<QTreeWidget*>(nullptr), "plannerAgenda");
        QCOMPARE(titlesIn(agenda), QStringList{QStringLiteral("Read chapter 3 and 4")});
        QCOMPARE(agenda->topLevelItem(0)->text(0), QStringLiteral("Today"));

        // Ticking it off in the agenda completes it; Undo reopens it.
        QTreeWidgetItem* row = agenda->topLevelItem(0)->child(0);
        row->setCheckState(0, Qt::Checked);
        QCOMPARE(ws.findTask(task)->status, study::TaskStatus::Done);
        QCOMPARE(shell.window->session()->history().nextUndo()->label, "Complete task");
        shell.action("actionUndo")->trigger();
        QCOMPARE(ws.findTask(task)->status, study::TaskStatus::Todo);

        // The open page: tags by name (created as needed) and a task linked to it.
        const auto page = *shell.window->activePage();
        panel->showView(ui::PlannerPanel::View::Page);
        settle();
        auto* tags = child(static_cast<QLineEdit*>(nullptr), "plannerPageTags");
        tags->setText(QStringLiteral("Exam, reading"));
        Q_EMIT tags->editingFinished();
        QCOMPARE(ws.findPage(page)->tags.size(), std::size_t{2});
        auto* pageAdd = child(static_cast<QLineEdit*>(nullptr), "plannerPageAdd");
        pageAdd->setFocus();
        QTest::keyClicks(pageAdd, QStringLiteral("Summarise cells"));
        QTest::keyClick(pageAdd, Qt::Key_Return);
        settle();
        QCOMPARE(ws.tasksLinkedTo(page).size(), std::size_t{1});
        QCOMPARE(titlesIn(child(static_cast<QTreeWidget*>(nullptr), "plannerBacklinks")),
                 QStringList{QStringLiteral("Summarise cells")});

        // Letters typed in a planner list are its search, not tool shortcuts.
        auto* backlinks = child(static_cast<QTreeWidget*>(nullptr), "plannerBacklinks");
        backlinks->setFocus();
        QTest::keyClick(backlinks, Qt::Key_E);
        QVERIFY(shell.action("actionToolPen")->isChecked());

        // A canvas edit does not rebuild the planner.
        const int refreshes = panel->refreshCount();
        auto stroke = document::commands::createElement(
            ws, ws.layersOf(page).front(),
            {.payload =
                 document::Stroke{.points = document::makeStrokePoints({{0, 0, 1}, {9, 9, 1}})}},
            shell.ids);
        QVERIFY(stroke.has_value());
        QVERIFY(shell.window->session()->execute(std::move(stroke->command)).has_value());
        QCoreApplication::processEvents();
        QCOMPARE(panel->refreshCount(), refreshes);

        // Tags being typed when the page changes are the left page's.
        tags->setFocus();
        tags->selectAll();
        QTest::keyClicks(tags, QStringLiteral("Lab"));
        shell.action("actionNewPage")->trigger();
        const auto second = *shell.window->activePage();
        QVERIFY(second != page);
        QCOMPARE(ws.findPage(page)->tags.size(), std::size_t{1});
        QCOMPARE(ws.findTag(ws.findPage(page)->tags[0])->name, std::string("Lab"));
        QVERIFY(ws.findPage(second)->tags.empty());
        settle();
        QVERIFY(tags->text().isEmpty()); // now the new page's (none)

        // A title being typed when the workspace closes is saved.
        panel->selectTask(task);
        title->setFocus();
        title->selectAll();
        QTest::keyClicks(title, QStringLiteral("Final title"));
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(shell.window->openWorkspace(path));
        QCOMPARE(shell.ws().findTask(task)->title, std::string("Final title"));

        // Everything is in the workspace after reopening.
        const document::Workspace saved = shell.ws();
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(shell.window->openWorkspace(path));
        QVERIFY(shell.ws() == saved);
        panel->showView(ui::PlannerPanel::View::Tasks);
        settle();
        QCOMPARE(scope->currentText(), QStringLiteral("Biology")); // the same records
        QCOMPARE(titlesIn(tasks).size(), 1);
        scope->setCurrentIndex(0); // All tasks
        Q_EMIT scope->activated(0);
        QCOMPARE(titlesIn(tasks).size(), 2);
    }

    // Phase 8: search from the navigation panel. Results replace the tree while a query is
    // shown, follow the workspace, and jump to the page (selecting the text box) or to the
    // task in the planner; Esc returns to the tree.
    void searchThroughTheShell() {
        Shell shell(settings(QStringLiteral("search")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Search"))));
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        const auto& ws = shell.ws();
        const auto first = *shell.window->activePage();
        auto box = document::commands::createElement(
            ws, ws.layersOf(first).front(),
            {.transform = {.position = {5000, 5000}}, // off screen: revealing centres it
             .payload = document::TextBox{.size = {200, 40}, .text = "Thylakoid membranes"}},
            shell.ids);
        QVERIFY(box.has_value());
        QVERIFY(shell.window->session()->execute(std::move(box->command)).has_value());
        shell.action("actionNewPage")->trigger();
        QVERIFY(*shell.window->activePage() != first);

        auto* field = shell.window->findChild<QLineEdit*>(QStringLiteral("searchField"));
        auto* results = shell.window->findChild<QTreeWidget*>(QStringLiteral("searchResults"));
        QVERIFY(field != nullptr && results != nullptr);
        shell.action("actionFind")->trigger();
        QTRY_VERIFY(field->hasFocus());
        QTest::keyClicks(field, QStringLiteral("thylak"));
        QTRY_COMPARE(results->topLevelItemCount(), 1);
        QVERIFY(results->isVisible());
        QVERIFY(
            results->topLevelItem(0)->text(0).startsWith(QStringLiteral("Thylakoid membranes")));
        const auto undoBefore = shell.window->session()->history().undoCount();
        QTest::keyClick(field, Qt::Key_Return); // opens the first result
        QCOMPARE(*shell.window->activePage(), first);
        QCOMPARE(shell.window->session()->history().undoCount(), undoBefore); // no edit

        // The results follow the workspace: the word is gone after an edit.
        auto edit = document::commands::editText(ws, box->id, "Stroma", {200, 40});
        QVERIFY(edit.has_value());
        QVERIFY(shell.window->session()->execute(std::move(*edit)).has_value());
        QTRY_COMPARE(results->topLevelItem(0)->text(0), QStringLiteral("No results"));

        // A task hit opens the planner on it.
        application::Planner planner(*shell.window->session(), shell.clock, shell.ids);
        auto task = planner.createTask({.title = "Quiz on the stroma"});
        QVERIFY(task.has_value());
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("quiz"));
        QTRY_VERIFY(results->topLevelItem(0) != nullptr &&
                    results->topLevelItem(0)->text(0).startsWith(QStringLiteral("Quiz")));
        QCOMPARE(results->topLevelItemCount(), 1);
        results->setCurrentItem(results->topLevelItem(0));
        Q_EMIT results->itemActivated(results->topLevelItem(0), 0);
        auto* panel = shell.window->findChild<ui::PlannerPanel*>(QStringLiteral("plannerPanel"));
        QVERIFY(panel->isVisible());
        QCOMPARE(panel->selectedTask(), *task);

        // Esc: back to the tree.
        field->setFocus();
        QTest::keyClick(field, Qt::Key_Escape);
        QVERIFY(field->text().isEmpty());
        QVERIFY(!results->isVisible());
        QVERIFY(shell.window->findChild<QTreeView*>(QStringLiteral("workspaceTree"))->isVisible());
    }

    // Search results are refreshed by edits that change what search shows (text, titles),
    // not by ink: drawing with the results open keeps the list and the chosen result.
    void searchResultsIgnoreInk() {
        Shell shell(settings(QStringLiteral("searchink")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("SearchInk"))));
        const auto page = *shell.window->activePage();
        const auto layer = shell.ws().layersOf(page).front();
        const auto add = [&](document::ElementPayload payload) {
            auto created = document::commands::createElement(
                shell.ws(), layer, {.payload = std::move(payload)}, shell.ids);
            QVERIFY(created.has_value());
            QVERIFY(shell.window->session()->execute(std::move(created->command)).has_value());
        };
        add(document::TextBox{.size = {200, 40}, .text = "Osmosis one"});
        add(document::TextBox{.size = {200, 40}, .text = "Osmosis two"});
        auto* field = shell.window->findChild<QLineEdit*>(QStringLiteral("searchField"));
        auto* results = shell.window->findChild<QTreeWidget*>(QStringLiteral("searchResults"));
        field->setFocus();
        QTest::keyClicks(field, QStringLiteral("osmosis"));
        QTRY_COMPARE(results->topLevelItemCount(), 2);
        // A one-line box: its text, then where it is (not the same text twice).
        const QStringList lines = results->topLevelItem(0)->text(0).split(u'\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].startsWith(QStringLiteral("Osmosis")));
        QVERIFY(lines[1].contains(QStringLiteral(" › ")));
        QTreeWidgetItem* chosen = results->topLevelItem(1);
        results->setCurrentItem(chosen);
        add(document::Stroke{.points = document::makeStrokePoints({{0, 0, 1}, {9, 9, 1}})});
        QTest::qWait(300);                          // longer than the search delay
        QCOMPARE(results->topLevelItem(1), chosen); // not rebuilt
        // A text edit refreshes the list and keeps the chosen result chosen.
        const QVariant key = chosen->data(0, Qt::UserRole);
        add(document::TextBox{.size = {200, 40}, .text = "Osmosis three"});
        QTRY_COMPARE(results->topLevelItemCount(), 3);
        QVERIFY(results->currentItem() != nullptr);
        QCOMPARE(results->currentItem()->data(0, Qt::UserRole), key);
    }

    // Decoded images wait for the canvas to collect them; ones it never collects (the page
    // changed or the zoom asked for another size meanwhile) are kept only within a budget,
    // and a dropped one is decoded again when asked for.
    void uncollectedImageDecodesAreBounded() {
        Shell shell(settings(QStringLiteral("decodes")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Decodes"))));
        QImage red(64, 32, QImage::Format_RGB32);
        red.fill(QColor(200, 30, 30));
        const QString png = dir_.filePath(QStringLiteral("decode.png"));
        QVERIFY(red.save(png));
        shell.dialogs->insertImage = toPath(png);
        shell.action("actionInsertImage")->trigger();
        shell.waitForImports();
        const auto page = *shell.window->activePage();
        const auto asset =
            std::get<document::Image>(
                shell.ws()
                    .findElement(shell.ws().elementsOf(shell.ws().layersOf(page)[0])[0])
                    ->payload)
                .asset;
        constexpr std::size_t kOne = 64 * 32 * 4; // one decode: 64 × 32 RGBA
        ui::SessionImageSource source(*shell.window->session(), kOne + kOne / 2);
        QVERIFY(!source.load(asset, 64).has_value()); // decoding
        QVERIFY(!source.load(asset, 64).has_value()); // still: not started twice
        source.waitForDecodes();
        QCOMPARE(source.uncollectedBytes(), kOne);
        QVERIFY(!source.load(asset, 128).has_value()); // another size, never collected
        source.waitForDecodes();
        QCOMPARE(source.uncollectedBytes(), kOne);    // over the budget: the oldest went
        QVERIFY(!source.load(asset, 64).has_value()); // dropped: decoded again
        source.waitForDecodes();
        const auto pixels = source.load(asset, 64);
        QVERIFY(pixels.has_value());
        QCOMPARE(pixels->width, 64);
        QCOMPARE(pixels->height, 32);
        QCOMPARE(source.uncollectedBytes(), std::size_t{0}); // collecting the 64 dropped the 128
        QVERIFY(!source.load(asset, 128).has_value());
        QVERIFY(source.load(core::AssetId{shell.ids.next()}, 64)->empty()); // unknown asset
    }

    /// A PDF of three pages (400 × 300, 300 × 500 and 400 × 300 pt), a black 100 pt square
    /// at the top left of the first page, nothing else.
    QString writePdf(const QString& name) {
        const QString file = dir_.filePath(name);
        QPdfWriter writer(file);
        writer.setResolution(72); // one unit per point
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setPageSize(QPageSize(QSizeF(400, 300), QPageSize::Point));
        QPainter painter(&writer);
        painter.fillRect(QRectF(0, 0, 100, 100), Qt::black);
        writer.setPageSize(QPageSize(QSizeF(300, 500), QPageSize::Point));
        writer.newPage();
        writer.setPageSize(QPageSize(QSizeF(400, 300), QPageSize::Point));
        writer.newPage();
        painter.end();
        return file;
    }

    static QByteArray sha256Of(const QString& file) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QCryptographicHash::hash(in.readAll(), QCryptographicHash::Sha256);
    }

    // Phase 8, step 3: File ▸ Import PDF adds a section with one bounded page per PDF page
    // (one undo step), the PDF is copied into the assets and never written, and its pages
    // are rendered in tiles on a worker thread.
    void importPdfThroughTheShell() {
        Shell shell(settings(QStringLiteral("pdf")));
        const auto path = freshPath(QStringLiteral("Pdf"));
        QVERIFY(shell.window->createWorkspace(path));
        const QString pdf = writePdf(QStringLiteral("Lecture notes.pdf"));
        const QByteArray original = sha256Of(pdf);
        QVERIFY(!original.isEmpty());

        const auto info = ui::inspectPdf(toPath(pdf));
        QVERIFY(info.has_value());
        QCOMPARE(info->pageSizes.size(), std::size_t{3});
        QCOMPARE(info->pageSizes[1].x, 300.0 * document::kUnitsPerPoint);
        QCOMPARE(info->pageSizes[1].y, 500.0 * document::kUnitsPerPoint);

        const auto notebooks = shell.ws().notebookCount();
        shell.dialogs->importDocument = toPath(pdf);
        shell.action("actionImportPdf")->trigger();
        shell.waitForImports();
        QCOMPARE(shell.dialogs->errors.size(), 0);
        QCOMPARE(shell.ws().notebookCount(), notebooks); // into the current notebook
        const auto page = *shell.window->activePage();
        const document::PageInfo& first = *shell.ws().findPage(page);
        QCOMPARE(QString::fromStdString(shell.ws().findSection(first.section)->title),
                 QStringLiteral("Lecture notes"));
        const auto span = shell.ws().pagesOf(first.section);
        const std::vector<core::PageId> pages(span.begin(), span.end()); // outlives a reopen
        QCOMPARE(pages.size(), std::size_t{3});
        QVERIFY(first.document.has_value());
        QCOMPARE(first.document->index, 0);
        QCOMPARE(first.extent, document::PageExtent::Bounded);
        QCOMPARE(first.size, info->pageSizes[0]);
        QCOMPARE(shell.ws().findPage(pages[2])->document->index, 2);
        QCOMPARE(QString::fromStdString(shell.window->session()->history().nextUndo()->label),
                 QStringLiteral("Import PDF"));
        const core::AssetId asset = first.document->asset;
        const core::DVec2 size = first.size;

        // Rendering: the preview of the whole page, then a tile at the right edge.
        ui::SessionDocumentRasterizer rasterizer(*shell.window->session());
        const int preview = canvas::documentPreviewLevel(size);
        const canvas::DocumentTileKey whole{.asset = asset, .page = 0, .level = preview};
        QVERIFY(!rasterizer.tile(whole).has_value()); // rendering
        QVERIFY(!rasterizer.tile(whole).has_value()); // not queued twice
        QVERIFY(rasterizer.queuedTiles() <= 1U);
        rasterizer.waitForTiles();
        const auto pixels = rasterizer.tile(whole);
        QVERIFY(pixels.has_value() && !pixels->empty());
        QCOMPARE(pixels->width,
                 static_cast<int>(std::ceil(size.x * canvas::documentScale(preview))));
        QCOMPARE(pixels->height,
                 static_cast<int>(std::ceil(size.y * canvas::documentScale(preview))));
        const auto at = [](const render::ImageData& image, int x, int y) {
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                 static_cast<std::size_t>(x)) *
                4U;
            return std::array<int, 4>{image.pixels[i], image.pixels[i + 1], image.pixels[i + 2],
                                      image.pixels[i + 3]};
        };
        QCOMPARE(at(*pixels, 5, 5), (std::array<int, 4>{0, 0, 0, 255})); // the black square
        // Unpainted paper is opaque white (it covers the coarse preview under the tile).
        QCOMPARE(at(*pixels, pixels->width - 5, pixels->height - 5),
                 (std::array<int, 4>{255, 255, 255, 255}));
        QCOMPARE(rasterizer.uncollectedBytes(), std::size_t{0});
        const canvas::DocumentTileKey edge{.asset = asset, .page = 0, .level = 0, .column = 1};
        QVERIFY(!rasterizer.tile(edge).has_value());
        rasterizer.waitForTiles();
        const auto right = rasterizer.tile(edge);
        QVERIFY(right.has_value());
        QCOMPARE(right->width, static_cast<int>(std::ceil(size.x)) - canvas::kDocumentTilePx);
        QCOMPARE(right->height, static_cast<int>(std::ceil(size.y)));
        // Out of the page, a missing page, a negative page: nothing to draw.
        for (const canvas::DocumentTileKey& bad :
             {canvas::DocumentTileKey{.asset = asset, .page = 0, .level = 0, .column = 5},
              canvas::DocumentTileKey{.asset = asset, .page = 7, .level = 0},
              canvas::DocumentTileKey{.asset = asset, .page = -1, .level = 0}}) {
            QVERIFY(!rasterizer.tile(bad).has_value());
            rasterizer.waitForTiles();
            const auto none = rasterizer.tile(bad);
            QVERIFY(none.has_value() && none->empty());
        }
        // An unknown asset: at once.
        const auto unknown = rasterizer.tile({.asset = core::AssetId{shell.ids.next()}});
        QVERIFY(unknown.has_value() && unknown->empty());
        // Requests the canvas no longer wants are dropped before they are rendered.
        const std::size_t rendered = rasterizer.renderedTiles();
        for (int i = 0; i < 12; ++i) {
            (void)rasterizer.tile(
                {.asset = asset, .page = 1, .level = 3, .column = i % 4, .row = i / 4});
        }
        rasterizer.keepOnly({});
        rasterizer.waitForTiles();
        QVERIFY(rasterizer.renderedTiles() - rendered < 12U);
        QCOMPARE(rasterizer.queuedTiles(), std::size_t{0});

        // The source file is untouched; the workspace has its own identical copy.
        QCOMPARE(sha256Of(pdf), original);
        const auto stored = shell.window->session()->assetPath(asset);
        QVERIFY(stored.has_value());
        QCOMPARE(sha256Of(QString::fromStdU16String(stored->u16string())), original);

        // One undo step removes the import.
        shell.action("actionUndo")->trigger();
        QVERIFY(shell.ws().findPage(page) == nullptr);
        shell.action("actionRedo")->trigger();
        QVERIFY(shell.ws().findPage(page) != nullptr);

        // Step 4: annotations are the page's own elements, above the PDF; they survive a
        // reopen, and the PDF (source and stored copy) stays byte-identical.
        const auto layer = shell.ws().layersOf(pages[1]).front();
        auto note = document::commands::createElement(
            shell.ws(), layer,
            {.transform = {.position = {40, 60}},
             .payload = document::TextBox{.size = {200, 40}, .text = "see figure 2"}},
            shell.ids);
        QVERIFY(note.has_value());
        QVERIFY(shell.window->session()->execute(std::move(note->command)).has_value());
        QVERIFY(shell.window->closeWorkspace());
        QVERIFY(shell.window->openWorkspace(path));
        const document::PageInfo* reopened = shell.ws().findPage(pages[1]);
        QVERIFY(reopened != nullptr && reopened->document.has_value());
        QCOMPARE(reopened->document->index, 1);
        QCOMPARE(shell.ws().elementsOf(shell.ws().layersOf(pages[1]).front()).size(),
                 std::size_t{1});
        QCOMPARE(sha256Of(pdf), original);
        const auto storedAgain = shell.window->session()->assetPath(asset);
        QVERIFY(storedAgain.has_value());
        QCOMPARE(sha256Of(QString::fromStdU16String(storedAgain->u16string())), original);
    }

    /// Share of pixels that differ clearly (any channel by more than 64) between two images
    /// of the same size.
    static double differingShare(const QImage& a, const QImage& b) {
        if (a.size() != b.size() || a.isNull()) {
            return 1.0;
        }
        const QImage x = a.convertToFormat(QImage::Format_RGB32);
        const QImage y = b.convertToFormat(QImage::Format_RGB32);
        qint64 differing = 0;
        for (int row = 0; row < x.height(); ++row) {
            const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
            const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
            for (int column = 0; column < x.width(); ++column) {
                if (std::abs(qRed(p[column]) - qRed(q[column])) > 64 ||
                    std::abs(qGreen(p[column]) - qGreen(q[column])) > 64 ||
                    std::abs(qBlue(p[column]) - qBlue(q[column])) > 64) {
                    ++differing;
                }
            }
        }
        return static_cast<double>(differing) / (static_cast<double>(x.width()) * x.height());
    }

    static QImage renderPdfPage(const QString& file, int page, QSize size) {
        QPdfDocument pdf;
        if (pdf.load(file) != QPdfDocument::Error::None) {
            return {};
        }
        QImage image = pdf.render(page, size);
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter(&flat).drawImage(0, 0, image);
        return flat;
    }

    // Phase 8, step 5: File ▸ Export Page writes PDF, PNG or SVG of the page as the canvas
    // shows it; Export Section as PDF writes every page of the section; Print prints the
    // section, a range or the current page. Exporting never changes the workspace, never
    // writes into the workspace folder and asks for the target once.
    void exportAndPrintPages() {
        Shell shell(settings(QStringLiteral("export")));
        const auto path = freshPath(QStringLiteral("Export"));
        QVERIFY(shell.window->createWorkspace(path));
        const auto page = *shell.window->activePage();
        const auto layer = shell.ws().layersOf(page).front();
        const auto add = [&](document::ElementPayload payload, core::DVec2 at = {}) {
            auto created = document::commands::createElement(
                shell.ws(), layer, {.transform = {.position = at}, .payload = std::move(payload)},
                shell.ids);
            QVERIFY(created.has_value());
            QVERIFY(shell.window->session()->execute(std::move(created->command)).has_value());
        };
        add(document::Stroke{.color = core::Color::black(),
                             .baseWidth = 12.0F,
                             .points = document::makeStrokePoints({{100, 200, 1}, {300, 200, 1}})});
        // A translucent highlighter doubling back on itself: overlaps are not darker.
        add(document::Stroke{
            .brush = document::Brush::Highlighter,
            .color = core::Color::fromRgba(250, 200, 0, 128),
            .baseWidth = 20.0F,
            .points = document::makeStrokePoints({{100, 300, 1}, {300, 300, 1}, {100, 310, 1}})});
        add(document::TextBox{.size = {220, 40}, .text = "Export text"}, {100, 400});
        // 1.2-TXT-02: a text box with its own font size is exported at that size.
        add(document::TextBox{.size = {300, 80}, .text = "Large text", .fontSize = 32}, {400, 400});

        const document::Workspace before = shell.ws();
        const auto undoCount = shell.window->session()->history().undoCount();
        const core::DRect area = ui::exportArea(shell.ws(), page);
        const auto exportTo = [&](const QString& name, const char* action = "actionExportPage") {
            const QString file = dir_.filePath(name);
            shell.dialogs->exportTarget = toPath(file);
            shell.action(action)->trigger();
            return file;
        };

        // PNG: 2 pixels per unit.
        const QString png = exportTo(QStringLiteral("page.png"));
        QCOMPARE(shell.dialogs->errors.size(), 0);
        QVERIFY(!shell.dialogs->exportPdfOnly);
        const QImage image(png);
        QVERIFY(!image.isNull());
        QCOMPARE(image.width(), static_cast<int>(std::ceil(area.width() * 2.0)));
        const auto pixel = [&](const QImage& from, core::DVec2 world) {
            const double sx = from.width() / area.width();
            const double sy = from.height() / area.height();
            return from.pixelColor(static_cast<int>((world.x - area.min.x) * sx),
                                   static_cast<int>((world.y - area.min.y) * sy));
        };
        QVERIFY(pixel(image, {200, 200}).red() < 60);  // the stroke
        QVERIFY(pixel(image, {205, 250}).red() > 230); // paper
        // Text is laid out as on the canvas (platform::QtTextLayout's raster at 2 px/unit), at
        // each box's font size.
        struct ExportedText {
            const char* text;
            core::Vec2 size;
            float fontSize;
            core::DVec2 at;
        };
        for (const ExportedText& box :
             {ExportedText{"Export text", {220, 40}, canvas::kTextSize, {100, 400}},
              ExportedText{"Large text", {300, 80}, 32.0F, {400, 400}}}) {
            const render::ImageData raster =
                shell.textLayout.rasterize(box.text, box.size, box.fontSize, 2.0F);
            QImage canvasText(raster.pixels.data(), raster.width, raster.height,
                              QImage::Format_RGBA8888_Premultiplied);
            QImage onPaper(canvasText.size(), QImage::Format_RGB32);
            onPaper.fill(Qt::white);
            QPainter(&onPaper).drawImage(0, 0, canvasText);
            // With real fonts the glyphs match exactly; the fallback boxes drawn where the test
            // platform has no fonts may land one unit (2 px) off, so the best match within that
            // counts. The dot pattern shows around the glyphs in the export only.
            double best = 1.0;
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dy = -2; dy <= 2; ++dy) {
                    const QImage exported =
                        image.copy(static_cast<int>((box.at.x - area.min.x) * 2) + dx,
                                   static_cast<int>((box.at.y - area.min.y) * 2) + dy,
                                   onPaper.width(), onPaper.height());
                    best = std::min(best, differingShare(onPaper, exported));
                }
            }
            QVERIFY(best < 0.02);
        }
        const QColor once = pixel(image, {120, 292});  // one part of the highlighter
        const QColor twice = pixel(image, {200, 305}); // where it overlaps itself
        QVERIFY(once.blue() < 200);                    // it is there
        QVERIFY(std::abs(once.blue() - twice.blue()) <= 3);
        QVERIFY(std::abs(once.red() - twice.red()) <= 3);

        // PDF: one page of the area's size in points, the same picture as the PNG.
        const QString pdf = exportTo(QStringLiteral("page.pdf"));
        {
            QPdfDocument document;
            QCOMPARE(document.load(pdf), QPdfDocument::Error::None);
            QCOMPARE(document.pageCount(), 1);
            QVERIFY(std::abs(document.pagePointSize(0).width() - area.width() * 0.75) < 1.0);
            QVERIFY(std::abs(document.pagePointSize(0).height() - area.height() * 0.75) < 1.0);
        }
        const QImage fromPdf = renderPdfPage(pdf, 0, image.size());
        // Compared above the text box: glyphs differ where the test platform has no fonts.
        const auto drawing = [&](const QImage& from) {
            return from.copy(
                0, 0, from.width(),
                static_cast<int>((380.0 - area.min.y) * from.height() / area.height()));
        };
        QVERIFY(differingShare(drawing(image), drawing(fromPdf)) < 0.01);
        QVERIFY(std::abs(pixel(fromPdf, {120, 292}).blue() - pixel(fromPdf, {200, 305}).blue()) <=
                4);

        // SVG: vector text, the same picture.
        const QString svg = exportTo(QStringLiteral("page.svg"));
        QFile svgFile(svg);
        QVERIFY(svgFile.open(QIODevice::ReadOnly));
        const QByteArray svgText = svgFile.readAll();
        QVERIFY(svgText.contains("<svg"));
        QVERIFY(svgText.contains("Export text"));
        QSvgRenderer renderer(svg);
        QVERIFY(renderer.isValid());
        QImage fromSvg(image.size(), QImage::Format_RGB32);
        fromSvg.fill(Qt::white);
        {
            QPainter painter(&fromSvg);
            renderer.render(&painter);
        }
        QVERIFY(differingShare(drawing(image), drawing(fromSvg)) < 0.01);
        // The dotted background is there in every format (a dot at a multiple of 24 units;
        // its grey is within the tolerance above, so it is checked on its own).
        for (const QImage* picture : std::array<const QImage*, 3>{&image, &fromPdf, &fromSvg}) {
            QVERIFY(pixel(*picture, {240, 240}).red() < 240); // a dot
            QVERIFY(pixel(*picture, {252, 252}).red() > 250); // between dots
        }

        // Cancelled: nothing is written. Into the workspace folder: refused.
        const auto questions = shell.dialogs->questions;
        shell.dialogs->exportTarget.reset();
        shell.action("actionExportPage")->trigger();
        QCOMPARE(shell.dialogs->questions, questions + 1); // asked once
        QCOMPARE(shell.dialogs->errors.size(), 0);
        const auto inside = path / "export.pdf";
        shell.dialogs->exportTarget = inside;
        shell.action("actionExportPage")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 1);
        QVERIFY(!std::filesystem::exists(inside));
        // An unwritable target is reported.
        shell.dialogs->exportTarget = toPath(dir_.filePath(QStringLiteral("no/such/dir/x.pdf")));
        shell.action("actionExportPage")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 2);

        // A section of an imported PDF: its pages, their sizes, the PDF behind the ink.
        const QString source = writePdf(QStringLiteral("Slides.pdf"));
        const QByteArray sourceHash = sha256Of(source);
        shell.dialogs->importDocument = toPath(source);
        shell.action("actionImportPdf")->trigger();
        shell.waitForImports();
        const auto pdfPage = *shell.window->activePage();
        const auto pdfLayer = shell.ws().layersOf(pdfPage).front();
        auto ink = document::commands::createElement(
            shell.ws(), pdfLayer,
            {.payload = document::Stroke{.color = core::Color::black(),
                                         .baseWidth = 8.0F,
                                         .points = document::makeStrokePoints(
                                             {{300, 300, 1}, {450, 300, 1}})}},
            shell.ids);
        QVERIFY(ink.has_value());
        QVERIFY(shell.window->session()->execute(std::move(ink->command)).has_value());
        const document::Workspace beforeSection = shell.ws();
        const auto undoSection = shell.window->session()->history().undoCount();
        const QString section = exportTo(QStringLiteral("section.pdf"), "actionExportSection");
        QVERIFY(shell.dialogs->exportPdfOnly);
        QCOMPARE(shell.dialogs->exportSuggestions.last(), QStringLiteral("Slides"));
        {
            QPdfDocument document;
            QCOMPARE(document.load(section), QPdfDocument::Error::None);
            QCOMPARE(document.pageCount(), 3);
            QVERIFY(std::abs(document.pagePointSize(0).width() - 400.0) < 0.5);
            QVERIFY(std::abs(document.pagePointSize(1).height() - 500.0) < 0.5);
            // One pixel per point: the PDF's black square and the ink (at 300..450 units).
            const QImage first = renderPdfPage(section, 0, QSize(400, 300));
            QVERIFY(first.pixelColor(20, 20).red() < 60);
            QVERIFY(
                first.pixelColor(static_cast<int>(375 * 0.75), static_cast<int>(300 * 0.75)).red() <
                60);
            QVERIFY(first.pixelColor(300, 100).red() > 230);
        }
        QVERIFY(shell.ws() == beforeSection);
        QCOMPARE(shell.window->session()->history().undoCount(), undoSection);
        QCOMPARE(sha256Of(source), sourceHash);

        // Print: the whole section by default, a range, or the current page.
        const auto printed = [&](const QString& name) {
            QPdfDocument document;
            return document.load(dir_.filePath(name)) == QPdfDocument::Error::None
                       ? document.pageCount()
                       : -1;
        };
        shell.dialogs->printTo = dir_.filePath(QStringLiteral("print-all.pdf"));
        shell.action("actionPrint")->trigger();
        QCOMPARE(shell.dialogs->printPages, 3);
        QCOMPARE(printed(QStringLiteral("print-all.pdf")), 3);
        shell.dialogs->printTo = dir_.filePath(QStringLiteral("print-range.pdf"));
        shell.dialogs->printRange = QPrinter::PageRange;
        shell.dialogs->printFrom = 2;
        shell.dialogs->printToPage = 3;
        shell.action("actionPrint")->trigger();
        QCOMPARE(printed(QStringLiteral("print-range.pdf")), 2);
        shell.dialogs->printTo = dir_.filePath(QStringLiteral("print-current.pdf"));
        shell.dialogs->printRange = QPrinter::CurrentPage;
        shell.action("actionPrint")->trigger();
        QCOMPARE(printed(QStringLiteral("print-current.pdf")), 1);
        shell.dialogs->printTo.reset(); // cancelled
        shell.action("actionPrint")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 2);

        // Nothing of this changed the first page or the history.
        QVERIFY(*shell.ws().findPage(page) == *before.findPage(page));
        QCOMPARE(shell.ws().elementsOf(layer).size(), before.elementsOf(layer).size());
        QVERIFY(shell.window->session()->history().undoCount() > undoCount); // the import only
    }

    // Phase 8, step 6: File ▸ Export Notebook / Export Workspace write bundles; Import
    // Notebook copies a bundle's notebooks into the open workspace (one undo step); Open
    // Bundle as Workspace unpacks one into a new workspace and opens it.
    void bundlesThroughTheShell() {
        Shell shell(settings(QStringLiteral("bundles")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("BundleSource"))));
        const auto page = *shell.window->activePage();
        const auto layer = shell.ws().layersOf(page).front();
        auto note = document::commands::createElement(
            shell.ws(), layer, {.payload = document::TextBox{.size = {200, 40}, .text = "Mitosis"}},
            shell.ids);
        QVERIFY(note.has_value());
        QVERIFY(shell.window->session()->execute(std::move(note->command)).has_value());
        const document::Workspace source = shell.ws();

        const auto notebookBundle = toPath(dir_.filePath(QStringLiteral("notebook.studybundle")));
        shell.dialogs->bundleTarget = notebookBundle;
        shell.action("actionExportNotebook")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 0);
        QVERIFY(std::filesystem::exists(notebookBundle));
        const auto workspaceBundle = toPath(dir_.filePath(QStringLiteral("workspace.studybundle")));
        shell.dialogs->bundleTarget = workspaceBundle;
        shell.action("actionExportWorkspace")->trigger();
        QVERIFY(std::filesystem::exists(workspaceBundle));
        QVERIFY(shell.ws() == source); // exporting changed nothing

        // Import the notebook into a new workspace: it opens on the imported page.
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("BundleTarget"))));
        const auto notebooks = shell.ws().notebookCount();
        shell.dialogs->bundleToOpen = notebookBundle;
        shell.action("actionImportNotebook")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 0);
        QCOMPARE(shell.ws().notebookCount(), notebooks + 1);
        QCOMPARE(*shell.window->activePage(), page); // ids kept: no conflict here
        QCOMPARE(shell.ws().elementsOf(shell.ws().layersOf(page).front()).size(), std::size_t{1});
        shell.action("actionUndo")->trigger();
        QCOMPARE(shell.ws().notebookCount(), notebooks);

        // A damaged bundle is reported and changes nothing.
        const QString junk = dir_.filePath(QStringLiteral("junk.studybundle"));
        {
            QFile out(junk);
            QVERIFY(out.open(QIODevice::WriteOnly));
            out.write("not a bundle");
        }
        shell.dialogs->bundleToOpen = toPath(junk);
        shell.action("actionImportNotebook")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 1);
        QCOMPARE(shell.ws().notebookCount(), notebooks);

        // Open the workspace bundle as a workspace of its own.
        const auto restored = freshPath(QStringLiteral("Restored"));
        shell.dialogs->bundleToOpen = workspaceBundle;
        shell.dialogs->newWorkspace = restored;
        shell.action("actionOpenBundle")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 1);
        QCOMPARE(shell.window->session()->root(), restored);
        QVERIFY(shell.ws() == source);
    }

    // Exported ink is drawn as stroked polylines; it must cover what the canvas's own
    // tessellation covers — also for pressure-varying pen strokes, translucent ones and
    // single points.
    void exportedInkMatchesTheCanvasGeometry() {
        Shell shell(settings(QStringLiteral("exportink")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("ExportInk"))));
        const auto page = *shell.window->activePage();
        const auto layer = shell.ws().layersOf(page).front();
        std::vector<document::Element> elements;
        const auto add = [&](document::Stroke stroke) {
            auto created = document::commands::createElement(
                shell.ws(), layer, {.payload = std::move(stroke)}, shell.ids);
            QVERIFY(created.has_value());
            QVERIFY(shell.window->session()->execute(std::move(created->command)).has_value());
            elements.push_back(*shell.ws().findElement(created->id));
        };
        std::vector<document::StrokePoint> wave;
        for (int i = 0; i <= 60; ++i) {
            const float t = static_cast<float>(i);
            wave.push_back({40.0F + t * 5.0F, 120.0F + 30.0F * std::sin(t * 0.2F),
                            0.2F + 0.8F * std::abs(std::sin(t * 0.13F))});
        }
        add(document::Stroke{.color = core::Color::black(),
                             .baseWidth = 10.0F,
                             .points = document::makeStrokePoints(wave)});
        for (auto& point : wave) {
            point.y += 90.0F;
        }
        add(document::Stroke{.color = core::Color::fromRgba(20, 60, 200, 120),
                             .baseWidth = 12.0F,
                             .points = document::makeStrokePoints(wave)});
        add(document::Stroke{.color = core::Color::black(),
                             .baseWidth = 14.0F,
                             .points = document::makeStrokePoints({{380, 60, 1}})});

        const auto target = toPath(dir_.filePath(QStringLiteral("ink.png")));
        shell.dialogs->exportTarget = target;
        shell.action("actionExportPage")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 0);
        const QImage exported(QString::fromStdU16String(target.u16string()));
        QVERIFY(!exported.isNull());
        // The same page drawn from the canvas meshes (triangles, winding fill).
        const core::DRect area = ui::exportArea(shell.ws(), page);
        QImage reference(exported.size(), QImage::Format_ARGB32_Premultiplied);
        reference.fill(Qt::white);
        {
            QPainter painter(&reference);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(exported.width() / area.width(), exported.height() / area.height());
            painter.translate(-area.min.x, -area.min.y);
            painter.setPen(Qt::NoPen);
            for (const document::Element& element : elements) {
                const core::Affine2 m = canvas::meshToWorld(element);
                painter.save();
                painter.setTransform(QTransform(m.a, m.b, m.c, m.d, m.tx, m.ty), true);
                for (const canvas::MeshPart& part : canvas::buildElementMeshes(element, 4.0F)) {
                    QPainterPath path;
                    path.setFillRule(Qt::WindingFill);
                    const auto& v = part.mesh.vertices;
                    for (std::size_t i = 0; i + 2 < part.mesh.indices.size(); i += 3) {
                        QPointF a(v[part.mesh.indices[i]].x, v[part.mesh.indices[i]].y);
                        QPointF b(v[part.mesh.indices[i + 1]].x, v[part.mesh.indices[i + 1]].y);
                        const QPointF c(v[part.mesh.indices[i + 2]].x,
                                        v[part.mesh.indices[i + 2]].y);
                        const double cross =
                            (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
                        if (cross == 0.0) {
                            continue;
                        }
                        if (cross < 0.0) {
                            std::swap(a, b);
                        }
                        path.moveTo(a);
                        path.lineTo(b);
                        path.lineTo(c);
                        path.closeSubpath();
                    }
                    const core::Color& k = part.color;
                    painter.setBrush(QColor(k.r, k.g, k.b, k.a));
                    painter.drawPath(path);
                }
                painter.restore();
            }
        }
        // Compared on the ink only (the export also has the dotted paper).
        qint64 inked = 0;
        qint64 differing = 0;
        for (int y = 0; y < reference.height(); ++y) {
            for (int x = 0; x < reference.width(); ++x) {
                const QColor want = reference.pixelColor(x, y);
                const QColor got = exported.pixelColor(x, y);
                if (want.red() > 235 && got.red() > 200) {
                    continue; // paper (or a background dot) in both
                }
                ++inked;
                // Anti-aliased edges may land a pixel apart: a pixel differs only if nothing
                // within one pixel of it in the export matches it.
                bool matched = false;
                for (int dy = -1; dy <= 1 && !matched; ++dy) {
                    for (int dx = -1; dx <= 1 && !matched; ++dx) {
                        const QPoint near(std::clamp(x + dx, 0, exported.width() - 1),
                                          std::clamp(y + dy, 0, exported.height() - 1));
                        const QColor other = exported.pixelColor(near);
                        matched = std::abs(want.red() - other.red()) <= 64 &&
                                  std::abs(want.blue() - other.blue()) <= 64;
                    }
                }
                differing += matched ? 0 : 1;
            }
        }
        QVERIFY(inked > 1000);
        QVERIFY2(
            differing < inked / 200,
            qPrintable(QStringLiteral("%1 of %2 ink pixels differ").arg(differing).arg(inked)));
    }

    // Imports copy files on a pool thread (Phase 9). Closing the workspace before one
    // finishes drops its result: nothing reaches the next workspace, nothing crashes.
    void importsFinishingAfterCloseAreDropped() {
        Shell shell(settings(QStringLiteral("importclose")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("ImportFirst"))));
        QImage red(64, 32, QImage::Format_RGB32);
        red.fill(QColor(200, 30, 30));
        const QString png = dir_.filePath(QStringLiteral("late.png"));
        QVERIFY(red.save(png));
        shell.dialogs->insertImage = toPath(png);
        shell.action("actionInsertImage")->trigger();
        shell.dialogs->importDocument = toPath(writePdf(QStringLiteral("late.pdf")));
        shell.action("actionImportPdf")->trigger();
        // The results are delivered through the event loop, so they cannot arrive before
        // this switch to another workspace.
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("ImportSecond"))));
        const auto page = *shell.window->activePage();
        const auto notebooks = shell.ws().notebookCount();
        shell.waitForImports();
        QCOMPARE(shell.dialogs->errors.size(), 0);
        QCOMPARE(shell.ws().notebookCount(), notebooks);
        QVERIFY(shell.ws().elementsOf(shell.ws().layersOf(page).front()).empty());
        QCOMPARE(*shell.window->activePage(), page);
    }

    // Phase 9: File ▸ Back Up Now writes a database snapshot; File ▸ Check Workspace
    // reports a clean workspace, and a damaged stored file by name.
    void backUpAndCheckThroughTheShell() {
        Shell shell(settings(QStringLiteral("maintenance")));
        const auto path = freshPath(QStringLiteral("Maintenance"));
        QVERIFY(shell.window->createWorkspace(path));
        shell.action("actionBackUp")->trigger();
        QCOMPARE(shell.dialogs->errors.size(), 0);
        std::size_t snapshots = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path / "backups")) {
            snapshots += entry.path().extension() == ".db" ? 1U : 0U;
        }
        QCOMPARE(snapshots, std::size_t{1});
        shell.action("actionCheckWorkspace")->trigger();
        QCOMPARE(shell.dialogs->information.size(), 1);
        QVERIFY(shell.dialogs->information.last().startsWith(QStringLiteral("No problems")));
        QImage red(8, 8, QImage::Format_RGB32);
        red.fill(Qt::red);
        const QString png = dir_.filePath(QStringLiteral("check.png"));
        QVERIFY(red.save(png));
        shell.dialogs->insertImage = toPath(png);
        shell.action("actionInsertImage")->trigger();
        shell.waitForImports();
        const auto page = *shell.window->activePage();
        const auto asset =
            std::get<document::Image>(
                shell.ws()
                    .findElement(shell.ws().elementsOf(shell.ws().layersOf(page)[0])[0])
                    ->payload)
                .asset;
        const auto stored = shell.window->session()->assetPath(asset);
        QVERIFY(stored.has_value());
        {
            QFile file(QString::fromStdU16String(stored->u16string()));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("damaged");
        }
        shell.action("actionCheckWorkspace")->trigger();
        QCOMPARE(shell.dialogs->information.size(), 2);
        QVERIFY(
            shell.dialogs->information.last().contains(QString::fromStdString(asset.toString())));
    }

    // Phase 9 accessibility: every control a keyboard or screen-reader user can reach has a
    // name assistive technology can read (QAccessible), in the shell and in the planner.
    void everyControlHasAnAccessibleName() {
        Shell shell(settings(QStringLiteral("accessible")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Accessible"))));
        // Wide enough that no toolbar control is moved into the overflow menu (fonts differ).
        shell.window->resize(1800, 900);
        shell.action("actionPlanner")->setChecked(true); // built on first show
        QApplication::processEvents();
        auto* panel = shell.window->findChild<ui::PlannerPanel*>(QStringLiteral("plannerPanel"));
        QVERIFY(panel != nullptr);
        application::Planner planner(*shell.window->session(), shell.clock, shell.ids);
        auto task = planner.createTask({.title = "Read chapter 2"});
        QVERIFY(task.has_value());
        QStringList unnamed;
        const auto audit = [&] {
            for (QWidget* widget : shell.window->findChildren<QWidget*>()) {
                const bool control = widget->focusPolicy() != Qt::NoFocus ||
                                     qobject_cast<QAbstractButton*>(widget) != nullptr;
                // Qt's own parts: the toolbar overflow button, scroll bars, tab and header
                // bars (named by Qt from their contents), the text field inside a date or
                // time field (the field itself is named).
                if (!control || !widget->isVisibleTo(shell.window.get()) ||
                    qobject_cast<QMenu*>(widget) != nullptr || widget->inherits("QScrollBar") ||
                    widget->inherits("QTabBar") || widget->inherits("QHeaderView") ||
                    widget->objectName() == QStringLiteral("qt_toolbar_ext_button") ||
                    widget->objectName() == QStringLiteral("qt_spinbox_lineedit")) {
                    continue;
                }
                QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(widget);
                const QString name =
                    accessible != nullptr ? accessible->text(QAccessible::Name) : QString();
                const QString entry = QStringLiteral("%1 (%2)").arg(
                    QString::fromLatin1(widget->metaObject()->className()), widget->objectName());
                if (name.trimmed().isEmpty() && !unnamed.contains(entry)) {
                    unnamed << entry;
                }
            }
        };
        for (const auto view : {ui::PlannerPanel::View::Today, ui::PlannerPanel::View::Tasks,
                                ui::PlannerPanel::View::Week, ui::PlannerPanel::View::Page}) {
            panel->showView(view);
            panel->selectTask(*task); // the task editor too
            QApplication::processEvents();
            audit();
        }
        QVERIFY2(unnamed.isEmpty(), qPrintable(unnamed.join(QStringLiteral(", "))));
    }

    // Phase 9 accessibility: F6 and Shift+F6 cycle the keyboard focus through the panes.
    void f6MovesBetweenPanes() {
        Shell shell(settings(QStringLiteral("panes")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Panes"))));
        shell.action("actionPlanner")->setChecked(true);
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        auto* tree = shell.tree();
        auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
        auto* planner = shell.window->findChild<QWidget*>(QStringLiteral("plannerPanel"));
        tree->setFocus();
        QTRY_VERIFY(tree->hasFocus());
        const auto inside = [](QWidget* pane) {
            QWidget* focus = QApplication::focusWidget();
            return focus != nullptr && (focus == pane || pane->isAncestorOf(focus));
        };
        shell.action("actionNextPane")->trigger();
        QTRY_VERIFY(inside(canvas));
        shell.action("actionNextPane")->trigger();
        QTRY_VERIFY(inside(planner));
        shell.action("actionNextPane")->trigger();
        QTRY_VERIFY(inside(tree)); // around
        shell.action("actionPreviousPane")->trigger();
        QTRY_VERIFY(inside(planner));
    }

    // Untrusted PDFs: not a PDF, empty, a bare header — reported, nothing changes; a
    // truncated one never crashes.
    void invalidPdfsAreRejected() {
        Shell shell(settings(QStringLiteral("badpdf")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("BadPdf"))));
        const QString valid = writePdf(QStringLiteral("valid.pdf"));
        QFile in(valid);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray bytes = in.readAll();
        const auto write = [&](const QString& name, const QByteArray& content) {
            const QString file = dir_.filePath(name);
            QFile out(file);
            if (!out.open(QIODevice::WriteOnly)) {
                qFatal("cannot write %s", qPrintable(file));
            }
            out.write(content);
            return file;
        };
        const QStringList bad{write(QStringLiteral("text.pdf"), "just some text, no PDF"),
                              write(QStringLiteral("empty.pdf"), QByteArray()),
                              write(QStringLiteral("header.pdf"), "%PDF-1.7\n%%EOF\n")};
        const auto undoCount = shell.window->session()->history().undoCount();
        for (const QString& file : bad) {
            QVERIFY(!ui::inspectPdf(toPath(file)).has_value());
            shell.dialogs->importDocument = toPath(file);
            shell.action("actionImportPdf")->trigger();
            shell.waitForImports();
        }
        QCOMPARE(shell.dialogs->errors.size(), bad.size());
        QCOMPARE(shell.window->session()->history().undoCount(), undoCount);
        QVERIFY(!ui::inspectPdf(toPath(dir_.filePath(QStringLiteral("missing.pdf")))).has_value());
        const QString truncated =
            write(QStringLiteral("truncated.pdf"), bytes.left(bytes.size() / 2));
        if (const auto info = ui::inspectPdf(toPath(truncated))) {
            QVERIFY(!info->pageSizes.empty()); // repaired by PDFium
        }
    }

    // Phase 6, step 9: Edit ▸ Cut/Copy/Paste (Ctrl+X/C/V on the canvas) duplicate elements
    // through the canvas clipboard as one undo step each; inside the text editor the same
    // keys are the editor's own.
    void copyAndPasteThroughTheShell() {
        Shell shell(settings(QStringLiteral("clipboard")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Clipboard"))));
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        const auto elements = [&] {
            const auto page = *shell.window->activePage();
            const auto ids = shell.ws().elementsOf(shell.ws().layersOf(page).front());
            return std::vector<core::ElementId>(ids.begin(), ids.end());
        };
        auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
        QVERIFY(canvas != nullptr);
        shell.action("actionToolText")->trigger();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, {200, 200});
        auto* editor = shell.window->findChild<QTextEdit*>(QStringLiteral("textEditor"));
        QVERIFY(editor != nullptr);
        QTRY_VERIFY(editor->hasFocus());
        QTest::keyClicks(editor, QStringLiteral("copy me"));
        QTest::keyClick(editor, Qt::Key_Escape);
        QCOMPARE(elements().size(), std::size_t{1});

        shell.action("actionToolSelect")->trigger();
        canvas->setFocus();
        QTest::keyClick(canvas, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier);
        const auto undoBefore = shell.window->session()->history().undoCount();
        QTest::keyClick(canvas, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(elements().size(), std::size_t{2});
        QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 1);
        QVERIFY(shell.action("actionToolSelect")->isChecked());
        const auto made = elements();
        QVERIFY(made[0] != made[1]);
        QCOMPARE(std::get<document::TextBox>(shell.ws().findElement(made[1])->payload).text,
                 std::string("copy me"));
        // Cut through the menu action: the pasted copy (selected) goes, as one step.
        shell.action("actionCut")->trigger();
        QCOMPARE(elements().size(), std::size_t{1});
        QCOMPARE(shell.window->session()->history().undoCount(), undoBefore + 2);
        shell.action("actionPaste")->trigger();
        QCOMPARE(elements().size(), std::size_t{2});

        // In the text editor Ctrl+A, Ctrl+C and Ctrl+V edit the text, not the page.
        shell.action("actionToolText")->trigger();
        QTest::mouseClick(canvas, Qt::LeftButton, {}, {600, 500});
        QTRY_VERIFY(editor->hasFocus());
        QTest::keyClicks(editor, QStringLiteral("ab"));
        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(editor, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(editor->toPlainText(), QStringLiteral("abab"));
        QCOMPARE(elements().size(), std::size_t{2});
        QTest::keyClick(editor, Qt::Key_Escape);
        QCOMPARE(elements().size(), std::size_t{3});
    }

    // Single-key tool shortcuts belong to the canvas: typed into the navigation tree the
    // letters are its keyboard search and leave the tool alone (Phase 5 audit, LOW).
    void toolLettersDoNotFireWhileTheTreeHasFocus() {
        Shell shell(settings(QStringLiteral("letters")));
        QVERIFY(shell.window->createWorkspace(freshPath(QStringLiteral("Letters"))));
        shell.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(shell.window.get()));
        shell.tree()->setFocus();
        QVERIFY(shell.tree()->hasFocus());
        QTest::keyClick(shell.tree(), Qt::Key_E);
        QTest::keyClick(shell.tree(), Qt::Key_M);
        QVERIFY(shell.action("actionToolPen")->isChecked());
        // With the canvas focused the same keys choose tools.
        auto* canvas = shell.window->findChild<QWidget*>(QStringLiteral("canvasWidget"));
        canvas->setFocus();
        QTest::keyClick(canvas, Qt::Key_M);
        QVERIFY(shell.action("actionToolHighlighter")->isChecked());
        QTest::keyClick(canvas, Qt::Key_P);
        QVERIFY(shell.action("actionToolPen")->isChecked());
        // Chords still work from the tree (e.g. Ctrl+Z is not a letter for the search).
        (void)shell.draw(*shell.window->activePage());
        const auto undo = shell.window->session()->history().undoCount();
        shell.tree()->setFocus();
        QTest::keyClick(shell.tree(), Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(shell.window->session()->history().undoCount(), undo - 1);
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
