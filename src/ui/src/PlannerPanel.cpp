#include "PlannerPanel.hpp"

#include <studyapp/application/WorkspaceStructure.hpp>
#include <studyapp/ui/ShellDialogs.hpp>

#include <QApplication>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimeEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <type_traits>

namespace studyapp::ui {

namespace {

using std::chrono::days;
using std::chrono::minutes;

constexpr int kIdRole = Qt::UserRole;

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

template <class Id>
QString idText(const Id& id) {
    return toQString(id.toString());
}

template <class Id>
std::optional<Id> idFrom(const QString& text) {
    auto uuid = core::Uuid::parse(text.toStdString());
    return uuid ? std::optional(Id{*uuid}) : std::nullopt;
}

QDate toQDate(const study::CalendarDate& date) {
    return {static_cast<int>(date.year()), static_cast<int>(static_cast<unsigned>(date.month())),
            static_cast<int>(static_cast<unsigned>(date.day()))};
}

study::CalendarDate fromQDate(const QDate& date) {
    return study::CalendarDate{std::chrono::year{date.year()},
                               std::chrono::month{static_cast<unsigned>(date.month())},
                               std::chrono::day{static_cast<unsigned>(date.day())}};
}

QString clockText(minutes time) {
    const auto count = time.count();
    return QStringLiteral("%1:%2")
        .arg(count / 60, 2, 10, QLatin1Char('0'))
        .arg(count % 60, 2, 10, QLatin1Char('0'));
}

/// Whether a patch changes what the planner shows: study records or pages (titles, tags).
bool concernsPlanner(const document::Patch& patch) {
    return std::any_of(patch.changes().begin(), patch.changes().end(), [](const auto& change) {
        return std::visit(
            [](const auto& c) {
                using Change = std::decay_t<decltype(c)>;
                return !std::is_same_v<Change, document::ElementChange> &&
                       !std::is_same_v<Change, document::LayerChange>;
            },
            change);
    });
}

void setBold(QTreeWidgetItem* item) {
    QFont font = item->font(0);
    font.setBold(true);
    item->setFont(0, font);
}

QTreeWidget* makeTaskTree(QWidget* parent, const QString& name) {
    auto* tree = new QTreeWidget(parent);
    tree->setObjectName(name);
    tree->setColumnCount(2);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setIndentation(14);
    tree->setUniformRowHeights(true);
    tree->setFrameShape(QFrame::NoFrame);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    return tree;
}

QLineEdit* makeQuickAdd(QWidget* parent, const QString& name, const QString& placeholder) {
    auto* edit = new QLineEdit(parent);
    edit->setObjectName(name);
    edit->setPlaceholderText(placeholder);
    edit->setClearButtonEnabled(true);
    return edit;
}

} // namespace

PlannerPanel::PlannerPanel(ShellDialogs& dialogs, QWidget* parent)
    : QWidget(parent), dialogs_(&dialogs) {
    setObjectName(QStringLiteral("plannerPanel"));
    // The widgets are made when the panel is first shown: it is off by default, and a
    // window that never shows it should not pay for date editors, calendars and lists.
}

void PlannerPanel::ensureBuilt() {
    if (built_) {
        return;
    }
    built_ = true;
    buildLayout();
    applyAccessibleNames();
}

void PlannerPanel::applyAccessibleNames() {
    // Names for screen readers (Phase 9 accessibility): the visible labels of the editor
    // are not attached to their fields, and lists and quick-add fields have none.
    const std::pair<const char*, QString> names[] = {
        {"plannerScope", tr("Show tasks of")},
        {"plannerScopeMore", tr("Course or project actions")},
        {"plannerWeekPrevious", tr("Previous week")},
        {"plannerWeekNext", tr("Next week")},
        {"plannerAgenda", tr("Today's tasks")},
        {"plannerTodayAdd", tr("New task for today")},
        {"plannerTasks", tr("Tasks")},
        {"plannerTaskAdd", tr("New task")},
        {"plannerWeek", tr("This week's tasks")},
        {"plannerPageTags", tr("Tags of this page")},
        {"plannerBacklinks", tr("Tasks linked to this page")},
        {"plannerPageAdd", tr("New task for this page")},
        {"plannerEditorArea", tr("Task details")},
        {"plannerTaskTitle", tr("Task title")},
        {"plannerTaskPriority", tr("Priority")},
        {"plannerTaskHasDue", tr("Has a due date")},
        {"plannerTaskDueDate", tr("Due date")},
        {"plannerTaskHasDueTime", tr("Has a due time")},
        {"plannerTaskDueTime", tr("Due time")},
        {"plannerTaskScheduled", tr("Scheduled")},
        {"plannerTaskBlockStart", tr("Scheduled start")},
        {"plannerTaskLinks", tr("Linked pages")},
        {"plannerTaskNotes", tr("Notes")},
    };
    for (const auto& [objectName, name] : names) {
        if (auto* widget = findChild<QWidget*>(QLatin1String(objectName))) {
            widget->setAccessibleName(name);
        }
    }
}

PlannerPanel::~PlannerPanel() = default;

// ---------------------------------------------------------------------------- layout

void PlannerPanel::buildLayout() {
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("plannerTabs"));
    tabs_->setDocumentMode(true);
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->addTab(tr("Today"));
    tabs_->addTab(tr("Tasks"));
    tabs_->addTab(tr("Week"));
    tabs_->addTab(tr("Page"));

    views_ = new QStackedWidget(this);

    // Today: the agenda and a quick "due today" entry.
    auto* todayView = new QWidget(views_);
    agenda_ = makeTaskTree(todayView, QStringLiteral("plannerAgenda"));
    todayAdd_ =
        makeQuickAdd(todayView, QStringLiteral("plannerTodayAdd"), tr("Add a task for today…"));
    auto* todayLayout = new QVBoxLayout(todayView);
    todayLayout->setContentsMargins(0, 0, 0, 0);
    todayLayout->addWidget(agenda_, 1);
    todayLayout->addWidget(todayAdd_);
    views_->addWidget(todayView);

    // Tasks: a scope (all, a course, a project), its tasks, a quick entry.
    auto* list = new QWidget(views_);
    scope_ = new QComboBox(list);
    scope_->setObjectName(QStringLiteral("plannerScope"));
    scope_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    scopeNew_ = new QToolButton(list);
    scopeNew_->setObjectName(QStringLiteral("plannerScopeNew"));
    scopeNew_->setText(tr("New"));
    scopeNew_->setPopupMode(QToolButton::InstantPopup);
    auto* newMenu = new QMenu(scopeNew_);
    newMenu->addAction(tr("Course…"), this, &PlannerPanel::newCourse)
        ->setObjectName(QStringLiteral("actionPlannerNewCourse"));
    newMenu->addAction(tr("Project…"), this, &PlannerPanel::newProject)
        ->setObjectName(QStringLiteral("actionPlannerNewProject"));
    scopeNew_->setMenu(newMenu);
    scopeMore_ = new QToolButton(list);
    scopeMore_->setObjectName(QStringLiteral("plannerScopeMore"));
    scopeMore_->setText(QStringLiteral("…"));
    scopeMore_->setToolTip(tr("Course or project"));
    scopeMore_->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(scopeMore_);
    moreMenu->addAction(tr("Rename…"), this, &PlannerPanel::renameScope)
        ->setObjectName(QStringLiteral("actionPlannerRenameScope"));
    moreMenu->addAction(tr("Mark project done"), this, [this] { setScopeProjectDone(true); })
        ->setObjectName(QStringLiteral("actionPlannerProjectDone"));
    moreMenu->addAction(tr("Mark project active"), this, [this] { setScopeProjectDone(false); })
        ->setObjectName(QStringLiteral("actionPlannerProjectActive"));
    moreMenu->addSeparator();
    moreMenu->addAction(tr("Delete…"), this, &PlannerPanel::deleteScope)
        ->setObjectName(QStringLiteral("actionPlannerDeleteScope"));
    scopeMore_->setMenu(moreMenu);
    progress_ = new QLabel(list);
    progress_->setObjectName(QStringLiteral("plannerProgress"));
    tasks_ = makeTaskTree(list, QStringLiteral("plannerTasks"));
    taskAdd_ = makeQuickAdd(list, QStringLiteral("plannerTaskAdd"), tr("Add a task…"));
    auto* scopeRow = new QHBoxLayout;
    scopeRow->setContentsMargins(8, 4, 4, 0);
    scopeRow->setSpacing(4);
    scopeRow->addWidget(scope_, 1);
    scopeRow->addWidget(scopeNew_);
    scopeRow->addWidget(scopeMore_);
    auto* listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(2);
    listLayout->addLayout(scopeRow);
    progress_->setContentsMargins(10, 0, 8, 0);
    listLayout->addWidget(progress_);
    listLayout->addWidget(tasks_, 1);
    listLayout->addWidget(taskAdd_);
    views_->addWidget(list);

    // Week: seven days, navigable.
    auto* weekView = new QWidget(views_);
    auto* previous = new QToolButton(weekView);
    previous->setObjectName(QStringLiteral("plannerWeekPrevious"));
    previous->setText(QStringLiteral("‹"));
    previous->setToolTip(tr("Previous week"));
    auto* next = new QToolButton(weekView);
    next->setObjectName(QStringLiteral("plannerWeekNext"));
    next->setText(QStringLiteral("›"));
    next->setToolTip(tr("Next week"));
    auto* thisWeek = new QToolButton(weekView);
    thisWeek->setObjectName(QStringLiteral("plannerWeekToday"));
    thisWeek->setText(tr("This week"));
    weekLabel_ = new QLabel(weekView);
    weekLabel_->setObjectName(QStringLiteral("plannerWeekLabel"));
    weekLabel_->setAlignment(Qt::AlignCenter);
    week_ = makeTaskTree(weekView, QStringLiteral("plannerWeek"));
    auto* weekRow = new QHBoxLayout;
    weekRow->setContentsMargins(8, 4, 4, 0);
    weekRow->addWidget(previous);
    weekRow->addWidget(weekLabel_, 1);
    weekRow->addWidget(next);
    weekRow->addWidget(thisWeek);
    auto* weekLayout = new QVBoxLayout(weekView);
    weekLayout->setContentsMargins(0, 0, 0, 0);
    weekLayout->addLayout(weekRow);
    weekLayout->addWidget(week_, 1);
    views_->addWidget(weekView);
    const auto shiftWeek = [this](int weeks) {
        weekStart_ = study::CalendarDate(std::chrono::sys_days(weekStart_) + days{7 * weeks});
        refreshNow();
    };
    connect(previous, &QToolButton::clicked, this, [shiftWeek] { shiftWeek(-1); });
    connect(next, &QToolButton::clicked, this, [shiftWeek] { shiftWeek(+1); });
    connect(thisWeek, &QToolButton::clicked, this, [this] {
        weekStart_ = study::weekStart(today());
        refreshNow();
    });

    // Page: the open page's tags and the tasks linking to it.
    auto* page = new QWidget(views_);
    pageTitle_ = new QLabel(page);
    pageTitle_->setObjectName(QStringLiteral("plannerPageTitle"));
    pageTitle_->setWordWrap(true);
    pageTags_ = new QLineEdit(page);
    pageTags_->setObjectName(QStringLiteral("plannerPageTags"));
    pageTags_->setPlaceholderText(tr("Tags, separated by commas"));
    auto* linkedLabel = new QLabel(tr("Tasks linked to this page"), page);
    backlinks_ = makeTaskTree(page, QStringLiteral("plannerBacklinks"));
    pageAdd_ =
        makeQuickAdd(page, QStringLiteral("plannerPageAdd"), tr("Add a task for this page…"));
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(8, 6, 8, 0);
    pageLayout->addWidget(pageTitle_);
    pageLayout->addWidget(pageTags_);
    pageLayout->addSpacing(6);
    pageLayout->addWidget(linkedLabel);
    pageLayout->addWidget(backlinks_, 1);
    pageLayout->addWidget(pageAdd_);
    views_->addWidget(page);

    buildEditor();
    editorArea_ = new QScrollArea(this);
    editorArea_->setObjectName(QStringLiteral("plannerEditorArea"));
    editorArea_->setWidget(editor_);
    editorArea_->setWidgetResizable(true);
    editorArea_->setFrameShape(QFrame::NoFrame);
    editorArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    split->addWidget(views_);
    split->addWidget(editorArea_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabs_);
    layout->addWidget(split, 1);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        views_->setCurrentIndex(index);
        refreshNow();
    });
    for (QTreeWidget* tree : {agenda_, tasks_, week_, backlinks_}) {
        tree->installEventFilter(this);
        connect(tree, &QTreeWidget::itemChanged, this, &PlannerPanel::onItemChanged);
        connect(tree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem* item) { onCurrentItemChanged(item); });
    }
    for (QLineEdit* edit : {todayAdd_, taskAdd_, pageAdd_}) {
        connect(edit, &QLineEdit::returnPressed, this, [this, edit] { createTaskFrom(edit); });
    }
    connect(scope_, &QComboBox::activated, this, [this] { refreshNow(); });
    connect(pageTags_, &QLineEdit::editingFinished, this, &PlannerPanel::commitPageTags);
}

void PlannerPanel::buildEditor() {
    editor_ = new QWidget(this);
    editor_->setObjectName(QStringLiteral("plannerTaskEditor"));
    title_ = new QLineEdit(editor_);
    title_->setObjectName(QStringLiteral("plannerTaskTitle"));
    done_ = new QCheckBox(tr("Done"), editor_);
    done_->setObjectName(QStringLiteral("plannerTaskDone"));
    priority_ = new QComboBox(editor_);
    priority_->setObjectName(QStringLiteral("plannerTaskPriority"));
    priority_->addItems({tr("No priority"), tr("Low"), tr("Medium"), tr("High")});
    hasDue_ = new QCheckBox(editor_);
    hasDue_->setObjectName(QStringLiteral("plannerTaskHasDue"));
    dueDate_ = new QDateEdit(editor_);
    dueDate_->setObjectName(QStringLiteral("plannerTaskDueDate"));
    dueDate_->setCalendarPopup(true);
    hasDueTime_ = new QCheckBox(editor_);
    hasDueTime_->setObjectName(QStringLiteral("plannerTaskHasDueTime"));
    dueTime_ = new QTimeEdit(editor_);
    dueTime_->setObjectName(QStringLiteral("plannerTaskDueTime"));
    dueTime_->setDisplayFormat(QStringLiteral("HH:mm"));
    scheduled_ = new QCheckBox(editor_);
    scheduled_->setObjectName(QStringLiteral("plannerTaskScheduled"));
    blockStart_ = new QDateTimeEdit(editor_);
    blockStart_->setObjectName(QStringLiteral("plannerTaskBlockStart"));
    blockStart_->setCalendarPopup(true);
    blockStart_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    blockMinutes_ = new QSpinBox(editor_);
    blockMinutes_->setObjectName(QStringLiteral("plannerTaskBlockMinutes"));
    blockMinutes_->setRange(5, 24 * 60);
    blockMinutes_->setSingleStep(15);
    blockMinutes_->setSuffix(tr(" min"));
    course_ = new QComboBox(editor_);
    course_->setObjectName(QStringLiteral("plannerTaskCourse"));
    project_ = new QComboBox(editor_);
    project_->setObjectName(QStringLiteral("plannerTaskProject"));
    tags_ = new QLineEdit(editor_);
    tags_->setObjectName(QStringLiteral("plannerTaskTags"));
    tags_->setPlaceholderText(tr("Tags, separated by commas"));
    links_ = new QListWidget(editor_);
    links_->setObjectName(QStringLiteral("plannerTaskLinks"));
    links_->setMaximumHeight(90);
    links_->installEventFilter(this);
    linkPage_ = new QPushButton(tr("Link open page"), editor_);
    linkPage_->setObjectName(QStringLiteral("plannerTaskLinkPage"));
    unlinkPage_ = new QPushButton(tr("Unlink"), editor_);
    unlinkPage_->setObjectName(QStringLiteral("plannerTaskUnlinkPage"));
    notes_ = new QPlainTextEdit(editor_);
    notes_->setObjectName(QStringLiteral("plannerTaskNotes"));
    notes_->setPlaceholderText(tr("Notes"));
    notes_->setTabChangesFocus(true);
    notes_->installEventFilter(this);
    addSubtask_ = new QPushButton(tr("Add subtask"), editor_);
    addSubtask_->setObjectName(QStringLiteral("plannerTaskAddSubtask"));
    deleteTask_ = new QPushButton(tr("Delete task"), editor_);
    deleteTask_->setObjectName(QStringLiteral("plannerTaskDelete"));

    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(done_);
    statusRow->addWidget(priority_, 1);
    // One value per row: the panel is narrow.
    const auto checkedRow = [](QCheckBox* check, QWidget* field) {
        auto* row = new QHBoxLayout;
        row->addWidget(check);
        row->addWidget(field, 1);
        return row;
    };
    for (QWidget* field : std::initializer_list<QWidget*>{dueDate_, dueTime_, blockStart_}) {
        field->setMinimumWidth(0);
        field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    auto* linkRow = new QHBoxLayout;
    linkRow->addWidget(linkPage_);
    linkRow->addWidget(unlinkPage_);
    linkRow->addStretch(1);
    auto* actionRow = new QHBoxLayout;
    actionRow->addWidget(addSubtask_);
    actionRow->addStretch(1);
    actionRow->addWidget(deleteTask_);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(title_);
    form->addRow(statusRow);
    form->addRow(tr("Due"), checkedRow(hasDue_, dueDate_));
    form->addRow(tr("Time"), checkedRow(hasDueTime_, dueTime_));
    form->addRow(tr("Scheduled"), checkedRow(scheduled_, blockStart_));
    form->addRow(tr("Length"), blockMinutes_);
    form->addRow(tr("Course"), course_);
    form->addRow(tr("Project"), project_);
    form->addRow(tr("Tags"), tags_);
    form->addRow(tr("Pages"), links_);
    form->addRow(linkRow);
    form->addRow(notes_);
    form->addRow(actionRow);
    auto* layout = new QVBoxLayout(editor_);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->addLayout(form);
    layout->addStretch(1);

    // Each field is saved when it is finished (one command each); unchanged values record
    // nothing (the commands return an empty patch).
    connect(title_, &QLineEdit::editingFinished, this, [this] {
        editSelected([&](study::Task& t) { t.title = title_->text().trimmed().toStdString(); });
    });
    connect(done_, &QCheckBox::clicked, this, [this](bool checked) {
        if (const study::Task* task = selected()) {
            commit(planner_->setTaskDone(task->id, checked), tr("save the task"));
        }
    });
    connect(priority_, &QComboBox::activated, this, [this](int index) {
        editSelected([&](study::Task& t) { t.priority = static_cast<study::Priority>(index); });
    });
    const auto saveDue = [this] {
        editSelected([&](study::Task& t) {
            if (!hasDue_->isChecked()) {
                t.dueDate.reset();
                t.dueTime.reset();
                return;
            }
            t.dueDate = fromQDate(dueDate_->date());
            const QTime time = dueTime_->time();
            t.dueTime = hasDueTime_->isChecked()
                            ? std::optional(minutes{time.hour() * 60 + time.minute()})
                            : std::nullopt;
        });
    };
    connect(hasDue_, &QCheckBox::clicked, this, [this, saveDue](bool checked) {
        if (checked && !dueDate_->date().isValid()) {
            dueDate_->setDate(toQDate(today()));
        }
        saveDue();
    });
    connect(hasDueTime_, &QCheckBox::clicked, this, [this, saveDue](bool checked) {
        if (checked && !hasDue_->isChecked()) {
            hasDue_->setChecked(true);
        }
        saveDue();
    });
    connect(dueDate_, &QDateEdit::editingFinished, this, [this, saveDue] {
        if (hasDue_->isChecked()) {
            saveDue();
        }
    });
    connect(dueDate_->calendarWidget(), &QCalendarWidget::clicked, this, [this, saveDue] {
        if (hasDue_->isChecked()) {
            saveDue();
        }
    });
    connect(dueTime_, &QTimeEdit::editingFinished, this, [this, saveDue] {
        if (hasDueTime_->isChecked()) {
            saveDue();
        }
    });
    const auto saveBlock = [this] {
        editSelected([&](study::Task& t) {
            if (!scheduled_->isChecked()) {
                t.scheduled.reset();
                return;
            }
            const QDateTime local = blockStart_->dateTime();
            const QTime time = local.time();
            const study::LocalTime wall(std::chrono::local_days(fromQDate(local.date())) +
                                        minutes{time.hour() * 60 + time.minute()});
            const core::Timestamp start = study::toInstant(wall, *zone_);
            t.scheduled = study::TimeBlock{start, start + minutes{blockMinutes_->value()}};
        });
    };
    connect(scheduled_, &QCheckBox::clicked, this, saveBlock);
    connect(blockStart_, &QDateTimeEdit::editingFinished, this, [this, saveBlock] {
        if (scheduled_->isChecked()) {
            saveBlock();
        }
    });
    connect(blockStart_->calendarWidget(), &QCalendarWidget::clicked, this, [this, saveBlock] {
        if (scheduled_->isChecked()) {
            saveBlock();
        }
    });
    connect(blockMinutes_, &QSpinBox::editingFinished, this, [this, saveBlock] {
        if (scheduled_->isChecked()) {
            saveBlock();
        }
    });
    connect(course_, &QComboBox::activated, this, [this] {
        const auto course = idFrom<core::CourseId>(course_->currentData().toString());
        editSelected([&](study::Task& t) { t.course = course; });
    });
    connect(project_, &QComboBox::activated, this, [this] {
        const auto project = idFrom<core::ProjectId>(project_->currentData().toString());
        editSelected([&](study::Task& t) { t.project = project; });
    });
    connect(tags_, &QLineEdit::editingFinished, this, [this] {
        if (const study::Task* task = selected(); task != nullptr && !updating_) {
            commit(planner_->setTaskTagNames(
                       task->id, application::Planner::parseTagNames(tags_->text().toStdString())),
                   tr("tag the task"));
        }
    });
    connect(linkPage_, &QPushButton::clicked, this, [this] {
        if (const study::Task* task = selected(); task != nullptr && activePage_) {
            commit(planner_->setTaskPageLink(task->id, *activePage_, true), tr("link the page"));
        }
    });
    connect(unlinkPage_, &QPushButton::clicked, this, [this] {
        const study::Task* task = selected();
        QListWidgetItem* item = links_->currentItem();
        if (task == nullptr || item == nullptr) {
            return;
        }
        if (const auto page = idFrom<core::PageId>(item->data(kIdRole).toString())) {
            commit(planner_->setTaskPageLink(task->id, *page, false), tr("unlink the page"));
        }
    });
    connect(links_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (const auto page = idFrom<core::PageId>(item->data(kIdRole).toString());
            page && openPage_) {
            openPage_(*page);
        }
    });
    connect(addSubtask_, &QPushButton::clicked, this, [this] {
        const study::Task* task = selected();
        if (task == nullptr || task->parent) {
            return;
        }
        const auto answer = dialogs_->askText(this, tr("New subtask"), tr("Title"), QString());
        if (!answer || answer->trimmed().isEmpty()) {
            return;
        }
        auto created = planner_->createTask({.title = answer->toStdString(),
                                             .course = task->course,
                                             .project = task->project,
                                             .parent = task->id});
        if (!created) {
            commit(tl::unexpected(created.error()), tr("add the subtask"));
        }
    });
    connect(deleteTask_, &QPushButton::clicked, this, [this] {
        if (const study::Task* task = selected()) {
            commit(planner_->deleteTask(task->id), tr("delete the task"));
        }
    });
}

// ---------------------------------------------------------------------------- state

void PlannerPanel::setPlanner(application::Planner* planner, const core::Clock* clock) {
    planner_ = planner;
    clock_ = clock;
    selected_.reset();
    activePage_.reset();
    weekStart_ = {};
    dirty_ = true;
    updateEnabled();
    refreshNow();
}

void PlannerPanel::setTimeZone(const study::TimeZone* zone) {
    zone_ = zone != nullptr ? zone : &utc_;
    weekStart_ = {};
    scheduleRefresh();
}

void PlannerPanel::commitPageTags() {
    // For the page the tags were typed for (the open page may have changed since).
    if (planner_ == nullptr || !pageTagsFor_ || updating_ ||
        planner_->workspace().findPage(*pageTagsFor_) == nullptr) {
        return;
    }
    commit(planner_->setPageTagNames(
               *pageTagsFor_, application::Planner::parseTagNames(pageTags_->text().toStdString())),
           tr("tag the page"));
}

void PlannerPanel::finishEditing() {
    // Fields are saved when they lose focus (editingFinished, the notes' focus-out).
    if (QWidget* focused = QApplication::focusWidget();
        focused != nullptr && isAncestorOf(focused)) {
        focused->clearFocus();
    }
}

void PlannerPanel::setActivePage(std::optional<core::PageId> page) {
    if (activePage_ == page) {
        return;
    }
    if (built_ && pageTags_->hasFocus()) {
        commitPageTags(); // typed for the page being left
    }
    activePage_ = page;
    scheduleRefresh();
}

void PlannerPanel::setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    updateEnabled();
    scheduleRefresh();
}

void PlannerPanel::setOpenPageHandler(std::function<void(core::PageId)> handler) {
    openPage_ = std::move(handler);
}

void PlannerPanel::setFailureHandler(std::function<void(const QString&, const QString&)> handler) {
    failed_ = std::move(handler);
}

void PlannerPanel::onPatch(const document::Patch& patch) {
    if (concernsPlanner(patch)) {
        scheduleRefresh();
    }
}

void PlannerPanel::showView(View view) {
    ensureBuilt();
    tabs_->setCurrentIndex(static_cast<int>(view));
}

void PlannerPanel::selectTask(std::optional<core::TaskId> task) {
    selected_ = task;
    refreshNow();
}

void PlannerPanel::scheduleRefresh() {
    dirty_ = true;
    if (!isVisible() || refreshPending_) {
        return; // hidden: refreshed when shown
    }
    // After the current event (a patch arrives while one of our own handlers runs, e.g. a
    // checkbox's itemChanged): rebuilding right away would delete the emitting item.
    refreshPending_ = true;
    QMetaObject::invokeMethod(this, [this] { refreshNow(); }, Qt::QueuedConnection);
}

void PlannerPanel::showEvent(QShowEvent* event) {
    ensureBuilt();
    QWidget::showEvent(event);
    if (dirty_) {
        scheduleRefresh();
    }
}

void PlannerPanel::refreshNow() {
    refreshPending_ = false;
    if (!isVisible() || !built_) {
        dirty_ = true;
        return;
    }
    dirty_ = false;
    ++refreshes_;
    if (planner_ != nullptr && selected_ && planner_->workspace().findTask(*selected_) == nullptr) {
        selected_.reset(); // deleted (or its creation undone)
    }
    if (weekStart_ == study::CalendarDate{}) {
        weekStart_ = study::weekStart(today());
    }
    switch (static_cast<View>(tabs_->currentIndex())) {
    case View::Today:
        refreshAgenda();
        break;
    case View::Tasks:
        refreshTasks();
        break;
    case View::Week:
        refreshWeek();
        break;
    case View::Page:
        refreshPage();
        break;
    }
    loadEditor();
    updateEnabled();
}

void PlannerPanel::updateEnabled() {
    if (!built_) {
        return;
    }
    const bool editable = planner_ != nullptr && !readOnly_;
    for (QWidget* w : std::initializer_list<QWidget*>{todayAdd_, taskAdd_, pageAdd_, scopeNew_,
                                                      scopeMore_, pageTags_}) {
        w->setEnabled(editable);
    }
    pageTags_->setEnabled(editable && activePage_.has_value());
    pageAdd_->setEnabled(editable && activePage_.has_value());
    scopeMore_->setEnabled(editable && (scopeCourse() || scopeProject()));
    editor_->setEnabled(editable);
}

study::CalendarDate PlannerPanel::today() const {
    return study::localDate(clock_ != nullptr ? clock_->now() : core::Timestamp{}, *zone_);
}

QString PlannerPanel::dueText(const study::Task& task) const {
    const study::CalendarDate now = today();
    const auto dayText = [&](study::CalendarDate date) {
        const auto delta = (std::chrono::sys_days(date) - std::chrono::sys_days(now)).count();
        if (delta == 0) {
            return tr("Today");
        }
        if (delta == 1) {
            return tr("Tomorrow");
        }
        return QLocale().toString(toQDate(date), QStringLiteral("ddd d MMM"));
    };
    if (task.dueDate) {
        QString text = dayText(*task.dueDate);
        if (task.dueTime) {
            text += QLatin1Char(' ') + clockText(*task.dueTime);
        }
        return text;
    }
    if (task.scheduled) {
        const study::LocalTime start = study::toLocal(task.scheduled->start, *zone_);
        const study::CalendarDate date(std::chrono::floor<days>(start));
        const auto time = std::chrono::floor<minutes>(start - std::chrono::floor<days>(start));
        return dayText(date) + QLatin1Char(' ') + clockText(time);
    }
    return {};
}

// ---------------------------------------------------------------------------- views

QTreeWidgetItem* PlannerPanel::addTaskItem(QTreeWidget* tree, QTreeWidgetItem* parent,
                                           const study::Task& task, const QString& detail) {
    auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, toQString(task.title));
    item->setText(1, detail);
    item->setData(0, kIdRole, idText(task.id));
    item->setToolTip(0, toQString(task.title));
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (!readOnly_) {
        flags |= Qt::ItemIsUserCheckable;
    }
    item->setFlags(flags);
    item->setCheckState(0, task.status == study::TaskStatus::Done ? Qt::Checked : Qt::Unchecked);
    if (task.status == study::TaskStatus::Cancelled) {
        QFont font = item->font(0);
        font.setStrikeOut(true);
        item->setFont(0, font);
    }
    return item;
}

void PlannerPanel::refreshAgenda() {
    updating_ = true;
    const int scroll = agenda_->verticalScrollBar()->value();
    agenda_->clear();
    if (planner_ != nullptr && clock_ != nullptr) {
        const auto tasks = planner_->allTasks();
        const study::Agenda agenda = study::buildAgenda(tasks, clock_->now(), *zone_);
        const auto group = [&](const QString& title, const std::vector<std::size_t>& indices) {
            if (indices.empty()) {
                return;
            }
            auto* header = new QTreeWidgetItem(agenda_);
            header->setText(0, title);
            header->setFlags(Qt::ItemIsEnabled);
            setBold(header);
            for (const std::size_t i : indices) {
                addTaskItem(agenda_, header, *tasks[i], dueText(*tasks[i]));
            }
            header->setExpanded(true);
        };
        group(tr("Overdue"), agenda.overdue);
        group(tr("Today"), agenda.today);
        group(tr("Upcoming"), agenda.upcoming);
        group(tr("No date"), agenda.unscheduled);
    }
    restoreSelection(agenda_);
    agenda_->verticalScrollBar()->setValue(scroll);
    updating_ = false;
}

void PlannerPanel::refreshScopes() {
    const QString current = scope_->currentData().toString();
    scope_->clear();
    scope_->addItem(tr("All tasks"), QString());
    if (planner_ != nullptr) {
        const document::Workspace& ws = planner_->workspace();
        for (const core::CourseId course : ws.courses()) {
            scope_->addItem(toQString(ws.findCourse(course)->title),
                            QStringLiteral("c:") + idText(course));
            for (const core::ProjectId project : ws.projects()) {
                const study::Project& p = *ws.findProject(project);
                if (p.course == course) {
                    scope_->addItem(QStringLiteral("    ") + toQString(p.title),
                                    QStringLiteral("p:") + idText(project));
                }
            }
        }
        for (const core::ProjectId project : ws.projects()) {
            const study::Project& p = *ws.findProject(project);
            if (!p.course) {
                scope_->addItem(toQString(p.title), QStringLiteral("p:") + idText(project));
            }
        }
    }
    const int index = scope_->findData(current);
    scope_->setCurrentIndex(index >= 0 ? index : 0);
}

std::optional<core::CourseId> PlannerPanel::scopeCourse() const {
    const QString scope = scope_->currentData().toString();
    return scope.startsWith(QStringLiteral("c:")) ? idFrom<core::CourseId>(scope.mid(2))
                                                  : std::nullopt;
}

std::optional<core::ProjectId> PlannerPanel::scopeProject() const {
    const QString scope = scope_->currentData().toString();
    return scope.startsWith(QStringLiteral("p:")) ? idFrom<core::ProjectId>(scope.mid(2))
                                                  : std::nullopt;
}

void PlannerPanel::refreshTasks() {
    updating_ = true;
    refreshScopes();
    const int scroll = tasks_->verticalScrollBar()->value();
    tasks_->clear();
    progress_->clear();
    if (planner_ != nullptr) {
        const auto course = scopeCourse();
        const auto project = scopeProject();
        const auto list = planner_->tasksInScope(course, project);
        QTreeWidgetItem* lastTop = nullptr;
        std::vector<const study::Task*> counted;
        for (const study::Task* task : list) {
            const bool sub = task->parent.has_value();
            auto* item = addTaskItem(tasks_, sub ? lastTop : nullptr, *task, dueText(*task));
            if (!sub) {
                lastTop = item;
                counted.push_back(task);
            }
        }
        tasks_->expandAll();
        if (course || project) {
            const study::Progress progress = study::progressOf(counted);
            progress_->setText(tr("%1 of %2 done").arg(progress.done).arg(progress.total) +
                               (project && planner_->workspace().findProject(*project)->status ==
                                               study::ProjectStatus::Done
                                    ? tr(" · project done")
                                    : QString()));
        }
    }
    restoreSelection(tasks_);
    tasks_->verticalScrollBar()->setValue(scroll);
    updating_ = false;
}

void PlannerPanel::refreshWeek() {
    updating_ = true;
    const int scroll = week_->verticalScrollBar()->value();
    week_->clear();
    const QLocale locale;
    const study::CalendarDate last(std::chrono::sys_days(weekStart_) + days{6});
    // Compact for the narrow panel ("21–27 Sep", "28 Sep – 4 Oct"); the year in the tooltip.
    const QDate first = toQDate(weekStart_);
    const QDate end = toQDate(last);
    weekLabel_->setText(first.month() == end.month()
                            ? QStringLiteral("%1–%2")
                                  .arg(first.day())
                                  .arg(locale.toString(end, QStringLiteral("d MMM")))
                            : locale.toString(first, QStringLiteral("d MMM")) +
                                  QStringLiteral(" – ") +
                                  locale.toString(end, QStringLiteral("d MMM")));
    weekLabel_->setToolTip(locale.toString(first, QLocale::LongFormat) + QStringLiteral(" – ") +
                           locale.toString(end, QLocale::LongFormat));
    if (planner_ != nullptr) {
        const auto tasks = planner_->allTasks();
        const auto week = study::buildWeek(tasks, weekStart_, *zone_);
        const study::CalendarDate now = today();
        for (const study::WeekDay& day : week) {
            auto* header = new QTreeWidgetItem(week_);
            header->setText(0, locale.toString(toQDate(day.date), QStringLiteral("dddd d MMM")) +
                                   (day.date == now ? tr(" · today") : QString()));
            header->setFlags(Qt::ItemIsEnabled);
            setBold(header);
            for (const study::DayEntry& entry : day.entries) {
                const study::Task& task = *tasks[entry.task];
                QString detail;
                if (entry.blockStart) {
                    detail = clockText(*entry.blockStart) + QStringLiteral("–") +
                             clockText(*entry.blockEnd);
                } else {
                    detail = task.dueTime ? tr("due %1").arg(clockText(*task.dueTime)) : tr("due");
                }
                addTaskItem(week_, header, task, detail);
            }
            header->setExpanded(true);
        }
    }
    restoreSelection(week_);
    week_->verticalScrollBar()->setValue(scroll);
    updating_ = false;
}

void PlannerPanel::refreshPage() {
    updating_ = true;
    backlinks_->clear();
    const document::PageInfo* page =
        planner_ != nullptr && activePage_ ? planner_->workspace().findPage(*activePage_) : nullptr;
    if (page == nullptr) {
        pageTitle_->setText(tr("No page is open."));
        pageTags_->clear();
        pageTagsFor_.reset();
    } else {
        const document::Workspace& ws = planner_->workspace();
        pageTitle_->setText(toQString(application::WorkspaceStructure::displayTitle(ws, page->id)));
        if (!pageTags_->hasFocus() || pageTagsFor_ != page->id) {
            pageTags_->setText(toQString(planner_->tagNames(page->tags)));
        }
        pageTagsFor_ = page->id;
        std::vector<const study::Task*> linked;
        for (const core::TaskId id : ws.tasksLinkedTo(page->id)) {
            linked.push_back(ws.findTask(id));
        }
        std::sort(linked.begin(), linked.end(), [](const study::Task* a, const study::Task* b) {
            return a->order != b->order ? a->order < b->order : a->id < b->id;
        });
        for (const study::Task* task : linked) {
            addTaskItem(backlinks_, nullptr, *task, dueText(*task));
        }
    }
    restoreSelection(backlinks_);
    updating_ = false;
}

void PlannerPanel::restoreSelection(QTreeWidget* tree) {
    if (!selected_) {
        tree->setCurrentItem(nullptr);
        return;
    }
    const QString id = idText(*selected_);
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        if ((*it)->data(0, kIdRole).toString() == id) {
            tree->setCurrentItem(*it);
            return;
        }
    }
    tree->setCurrentItem(nullptr);
}

void PlannerPanel::loadEditor() {
    const study::Task* task = selected();
    editorArea_->setVisible(task != nullptr); // no empty editor below the list
    if (task == nullptr) {
        return;
    }
    updating_ = true;
    const document::Workspace& ws = planner_->workspace();
    // A field being typed in keeps what the user typed.
    if (!title_->hasFocus()) {
        title_->setText(toQString(task->title));
    }
    done_->setChecked(task->status == study::TaskStatus::Done);
    priority_->setCurrentIndex(static_cast<int>(task->priority));
    hasDue_->setChecked(task->dueDate.has_value());
    dueDate_->setDate(toQDate(task->dueDate.value_or(today())));
    dueDate_->setEnabled(task->dueDate.has_value());
    hasDueTime_->setChecked(task->dueTime.has_value());
    const auto dueMinutes = task->dueTime.value_or(minutes{9 * 60}).count();
    dueTime_->setTime(QTime(static_cast<int>(dueMinutes / 60), static_cast<int>(dueMinutes % 60)));
    dueTime_->setEnabled(task->dueTime.has_value());
    scheduled_->setChecked(task->scheduled.has_value());
    if (task->scheduled) {
        const study::LocalTime start = study::toLocal(task->scheduled->start, *zone_);
        const auto day = std::chrono::floor<days>(start);
        const auto time = std::chrono::floor<minutes>(start - day).count();
        blockStart_->setDateTime(
            QDateTime(toQDate(study::CalendarDate(day)),
                      QTime(static_cast<int>(time / 60), static_cast<int>(time % 60))));
        blockMinutes_->setValue(static_cast<int>(
            std::chrono::duration_cast<minutes>(task->scheduled->end - task->scheduled->start)
                .count()));
    } else {
        blockStart_->setDateTime(QDateTime(toQDate(today()), QTime(9, 0)));
        blockMinutes_->setValue(60);
    }
    blockStart_->setEnabled(task->scheduled.has_value());
    blockMinutes_->setEnabled(task->scheduled.has_value());
    course_->clear();
    course_->addItem(tr("No course"), QString());
    for (const core::CourseId id : ws.courses()) {
        course_->addItem(toQString(ws.findCourse(id)->title), idText(id));
    }
    course_->setCurrentIndex(task->course ? course_->findData(idText(*task->course)) : 0);
    project_->clear();
    project_->addItem(tr("No project"), QString());
    for (const core::ProjectId id : ws.projects()) {
        project_->addItem(toQString(ws.findProject(id)->title), idText(id));
    }
    project_->setCurrentIndex(task->project ? project_->findData(idText(*task->project)) : 0);
    if (!tags_->hasFocus()) {
        tags_->setText(toQString(planner_->tagNames(task->tags)));
    }
    links_->clear();
    for (const core::PageId page : task->linkedPages) {
        auto* item = new QListWidgetItem(
            toQString(application::WorkspaceStructure::displayTitle(ws, page)), links_);
        item->setData(kIdRole, idText(page));
    }
    linkPage_->setEnabled(activePage_.has_value());
    if (!notes_->hasFocus()) {
        notes_->setPlainText(toQString(task->notes));
    }
    addSubtask_->setEnabled(!task->parent);
    updating_ = false;
}

// ---------------------------------------------------------------------------- actions

const study::Task* PlannerPanel::selected() const {
    return planner_ != nullptr && selected_ ? planner_->workspace().findTask(*selected_) : nullptr;
}

void PlannerPanel::onCurrentItemChanged(QTreeWidgetItem* item) {
    if (updating_ || item == nullptr) {
        return;
    }
    const auto id = idFrom<core::TaskId>(item->data(0, kIdRole).toString());
    if (!id || id == selected_) {
        return;
    }
    selected_ = id;
    loadEditor();
}

void PlannerPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (updating_ || column != 0 || planner_ == nullptr) {
        return;
    }
    if (const auto id = idFrom<core::TaskId>(item->data(0, kIdRole).toString())) {
        commit(planner_->setTaskDone(*id, item->checkState(0) == Qt::Checked), tr("save the task"));
    }
}

void PlannerPanel::createTaskFrom(QLineEdit* edit) {
    const QString title = edit->text().trimmed();
    if (planner_ == nullptr || title.isEmpty()) {
        return;
    }
    document::commands::NewTask task{.title = title.toStdString()};
    if (edit == todayAdd_) {
        task.dueDate = today();
    } else if (edit == taskAdd_) {
        task.course = scopeCourse();
        task.project = scopeProject();
    } else if (edit == pageAdd_ && activePage_) {
        task.linkedPages = {*activePage_};
    }
    auto created = planner_->createTask(std::move(task));
    if (!created) {
        commit(tl::unexpected(created.error()), tr("add the task"));
        return;
    }
    edit->clear();
    selected_ = *created;
}

void PlannerPanel::commit(core::Result<void> result, const QString& what) {
    if (!result && failed_) {
        failed_(tr("The planner could not %1.").arg(what), toQString(result.error().message));
    }
}

void PlannerPanel::editSelected(const std::function<void(study::Task&)>& change) {
    const study::Task* task = selected();
    if (task == nullptr || updating_ || readOnly_) {
        return;
    }
    study::Task edited = *task;
    change(edited);
    commit(planner_->updateTask(std::move(edited)), tr("save the task"));
}

void PlannerPanel::newCourse() {
    if (planner_ == nullptr) {
        return;
    }
    const auto name = dialogs_->askText(this, tr("New course"), tr("Name"), QString());
    if (!name) {
        return;
    }
    auto course = planner_->createCourse(name->trimmed().toStdString());
    if (!course) {
        commit(tl::unexpected(course.error()), tr("create the course"));
        return;
    }
    refreshScopes();
    scope_->setCurrentIndex(scope_->findData(QStringLiteral("c:") + idText(*course)));
    showView(View::Tasks);
    scheduleRefresh();
}

void PlannerPanel::newProject() {
    if (planner_ == nullptr) {
        return;
    }
    std::optional<core::CourseId> course = scopeCourse();
    if (const auto project = scopeProject()) {
        course = planner_->workspace().findProject(*project)->course;
    }
    const auto name = dialogs_->askText(this, tr("New project"), tr("Name"), QString());
    if (!name) {
        return;
    }
    auto project = planner_->createProject(course, name->trimmed().toStdString());
    if (!project) {
        commit(tl::unexpected(project.error()), tr("create the project"));
        return;
    }
    refreshScopes();
    scope_->setCurrentIndex(scope_->findData(QStringLiteral("p:") + idText(*project)));
    showView(View::Tasks);
    scheduleRefresh();
}

void PlannerPanel::renameScope() {
    if (planner_ == nullptr) {
        return;
    }
    const document::Workspace& ws = planner_->workspace();
    if (const auto course = scopeCourse()) {
        const auto name = dialogs_->askText(this, tr("Rename course"), tr("Name"),
                                            toQString(ws.findCourse(*course)->title));
        if (name) {
            commit(planner_->renameCourse(*course, name->toStdString()), tr("rename the course"));
        }
    } else if (const auto project = scopeProject()) {
        const auto name = dialogs_->askText(this, tr("Rename project"), tr("Name"),
                                            toQString(ws.findProject(*project)->title));
        if (name) {
            commit(planner_->renameProject(*project, name->toStdString()),
                   tr("rename the project"));
        }
    }
}

void PlannerPanel::deleteScope() {
    if (planner_ == nullptr) {
        return;
    }
    const document::Workspace& ws = planner_->workspace();
    if (const auto course = scopeCourse()) {
        const QString what = tr("the course “%1” (its tasks and projects stay)")
                                 .arg(toQString(ws.findCourse(*course)->title));
        if (dialogs_->confirmDelete(this, what)) {
            commit(planner_->deleteCourse(*course), tr("delete the course"));
        }
    } else if (const auto project = scopeProject()) {
        const QString what =
            tr("the project “%1” (its tasks stay)").arg(toQString(ws.findProject(*project)->title));
        if (dialogs_->confirmDelete(this, what)) {
            commit(planner_->deleteProject(*project), tr("delete the project"));
        }
    }
}

void PlannerPanel::setScopeProjectDone(bool done) {
    const auto project = scopeProject();
    if (planner_ == nullptr || !project) {
        return;
    }
    study::Project edited = *planner_->workspace().findProject(*project);
    edited.status = done ? study::ProjectStatus::Done : study::ProjectStatus::Active;
    commit(planner_->updateProject(std::move(edited)), tr("save the project"));
}

bool PlannerPanel::eventFilter(QObject* watched, QEvent* event) {
    const bool isTree = watched == agenda_ || watched == tasks_ || watched == week_ ||
                        watched == backlinks_ || watched == links_;
    if (isTree && event->type() == QEvent::ShortcutOverride) {
        // As in the navigation tree: plain typing is the list's keyboard search, not the
        // window's single-key tool shortcuts.
        const auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers chord =
            key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if (chord == Qt::NoModifier && ((!key->text().isEmpty() && key->text().front().isPrint()) ||
                                        key->key() == Qt::Key_Delete)) {
            event->accept();
            return true;
        }
    }
    if (isTree && watched != links_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Delete) {
        if (const study::Task* task = selected(); task != nullptr && !readOnly_) {
            commit(planner_->deleteTask(task->id), tr("delete the task"));
            return true;
        }
    }
    if (watched == notes_ && event->type() == QEvent::FocusOut) {
        const std::string text = notes_->toPlainText().toStdString();
        const study::Task* task = selected();
        if (task != nullptr && task->notes != text) {
            editSelected([&](study::Task& t) { t.notes = text; });
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace studyapp::ui
