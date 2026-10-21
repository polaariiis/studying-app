#include "StudyPlanDialog.hpp"

#include <studyapp/ui/ShellDialogs.hpp>

#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace studyapp::ui {

using application::PlanBlock;
using application::PlanBuildOutcome;
using application::PlanHold;
using application::PlanIssueKind;
using application::PlanReview;

namespace {

constexpr int kIdRole = Qt::UserRole;        ///< scope, excluded and reference rows
constexpr int kBlockRole = Qt::UserRole + 1; ///< preview rows: the block's index

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

QString dateText(const study::CalendarDate& date) {
    return QLocale().toString(toQDate(date), QStringLiteral("ddd d MMM yyyy"));
}

/// The last storable date (study::isStorableDate).
const QDate kLastDate(9999, 12, 31);

/// Whether a patch changes what the plan refers to: notebooks, sections or pages (never
/// canvas edits or study records).
bool concernsPlan(const document::Patch& patch) {
    return std::ranges::any_of(patch.changes(), [](const document::AnyChange& change) {
        return std::visit(
            [](const auto& c) {
                using Change = std::decay_t<decltype(c)>;
                return std::is_same_v<Change, document::NotebookChange> ||
                       std::is_same_v<Change, document::SectionChange> ||
                       std::is_same_v<Change, document::PageChange>;
            },
            change);
    });
}

/// The step of minutes: 5 (application::kPlanMinuteStep).
int roundToStep(int value) {
    const int step = static_cast<int>(application::kPlanMinuteStep.count());
    return (value + step / 2) / step * step;
}

QPushButton* makeButton(const QString& text, const QString& name, const QString& accessible,
                        QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setAccessibleName(accessible);
    button->setAutoDefault(false);
    return button;
}

} // namespace

StudyPlanDialog::StudyPlanDialog(application::Planner& planner, ShellDialogs& dialogs,
                                 study::CalendarDate today, bool readOnly, QWidget* parent)
    : QDialog(parent), planner_(&planner), dialogs_(&dialogs), readOnly_(readOnly) {
    setObjectName(QStringLiteral("studyPlanDialog"));
    setWindowTitle(tr("New Study Plan"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    buildLayout();
    connectControls();
    fillScope();
    start_->setDate(toQDate(today));
    updateStartRange();
    render();
}

StudyPlanDialog::~StudyPlanDialog() = default;

void StudyPlanDialog::setSavedHandler(std::function<void(core::ProjectId)> handler) {
    saved_ = std::move(handler);
}

const application::PlanPreview* StudyPlanDialog::preview() const noexcept {
    return preview_ ? &*preview_ : nullptr;
}

void StudyPlanDialog::onPatch(const document::Patch& patch) {
    if (!concernsPlan(patch) || refreshPending_) {
        return;
    }
    refreshPending_ = true;
    QMetaObject::invokeMethod(this, [this] { refreshNow(); }, Qt::QueuedConnection);
}

void StudyPlanDialog::refreshNow() {
    refreshPending_ = false;
    fillScope();
    render();
}

void StudyPlanDialog::buildLayout() {
    auto* layout = new QVBoxLayout(this);
    auto* columns = new QHBoxLayout;
    layout->addLayout(columns, 1);

    // ---- the request
    auto* left = new QVBoxLayout;
    columns->addLayout(left, 2);
    auto* form = new QFormLayout;
    goal_ = new QLineEdit(this);
    goal_->setObjectName(QStringLiteral("studyPlanGoal"));
    goal_->setPlaceholderText(tr("For example: Biology exam"));
    goal_->setAccessibleName(tr("Goal"));
    form->addRow(tr("&Goal:"), goal_);
    start_ = new QDateEdit(this);
    start_->setObjectName(QStringLiteral("studyPlanStart"));
    start_->setCalendarPopup(true);
    start_->setAccessibleName(tr("Start date"));
    form->addRow(tr("&Start:"), start_);
    days_ = new QSpinBox(this);
    days_->setObjectName(QStringLiteral("studyPlanDays"));
    days_->setRange(1, application::kMaxPlanDays);
    days_->setValue(7);
    days_->setAccessibleName(tr("Days"));
    form->addRow(tr("&Days:"), days_);
    minutes_ = new QSpinBox(this);
    minutes_->setObjectName(QStringLiteral("studyPlanMinutes"));
    minutes_->setRange(static_cast<int>(application::kMinPlanMinutes.count()),
                       static_cast<int>(application::kMaxPlanMinutes.count()));
    minutes_->setSingleStep(static_cast<int>(application::kPlanMinuteStep.count()));
    minutes_->setValue(60);
    minutes_->setSuffix(tr(" min"));
    minutes_->setAccessibleName(tr("Minutes per day"));
    form->addRow(tr("&Minutes per day:"), minutes_);
    left->addLayout(form);
    auto* scopeLabel = new QLabel(tr("Notebooks and sections to cover:"), this);
    left->addWidget(scopeLabel);
    scope_ = new QTreeWidget(this);
    scope_->setObjectName(QStringLiteral("studyPlanScope"));
    scope_->setHeaderHidden(true);
    scope_->setAccessibleName(tr("Notebooks and sections to cover"));
    scopeLabel->setBuddy(scope_);
    left->addWidget(scope_, 1);
    build_ =
        makeButton(tr("&Build"), QStringLiteral("studyPlanBuild"), tr("Build the preview"), this);
    left->addWidget(build_, 0, Qt::AlignLeft);

    // ---- the preview
    auto* right = new QVBoxLayout;
    columns->addLayout(right, 3);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("studyPlanSummary"));
    summary_->setWordWrap(true);
    right->addWidget(summary_);
    table_ = new QTreeWidget(this);
    table_->setObjectName(QStringLiteral("studyPlanPreview"));
    table_->setColumnCount(3);
    table_->setHeaderLabels({tr("Day and block"), tr("Minutes"), tr("Linked pages")});
    table_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->setAccessibleName(tr("Plan preview"));
    right->addWidget(table_, 2);

    blockBox_ = new QGroupBox(tr("Selected block"), this);
    blockBox_->setObjectName(QStringLiteral("studyPlanBlock"));
    auto* blockForm = new QFormLayout(blockBox_);
    blockTitle_ = new QLineEdit(blockBox_);
    blockTitle_->setObjectName(QStringLiteral("studyPlanBlockTitle"));
    blockTitle_->setAccessibleName(tr("Block title"));
    blockForm->addRow(tr("&Title:"), blockTitle_);
    blockMinutes_ = new QSpinBox(blockBox_);
    blockMinutes_->setObjectName(QStringLiteral("studyPlanBlockMinutes"));
    blockMinutes_->setRange(static_cast<int>(application::kMinPlanMinutes.count()),
                            static_cast<int>(application::kMaxPlanMinutes.count()));
    blockMinutes_->setSingleStep(static_cast<int>(application::kPlanMinuteStep.count()));
    blockMinutes_->setSuffix(tr(" min"));
    blockMinutes_->setKeyboardTracking(false);
    blockMinutes_->setAccessibleName(tr("Block minutes"));
    blockForm->addRow(tr("M&inutes:"), blockMinutes_);
    auto* dayRow = new QHBoxLayout;
    blockDay_ = new QSpinBox(blockBox_);
    blockDay_->setObjectName(QStringLiteral("studyPlanBlockDay"));
    blockDay_->setAccessibleName(tr("Day to move the block to"));
    dayRow->addWidget(blockDay_);
    moveBlock_ = makeButton(tr("&Move to day"), QStringLiteral("studyPlanMoveBlock"),
                            tr("Move the block to the day"), blockBox_);
    dayRow->addWidget(moveBlock_);
    dayRow->addStretch(1);
    blockForm->addRow(tr("Da&y:"), dayRow);
    blockPages_ = new QListWidget(blockBox_);
    blockPages_->setObjectName(QStringLiteral("studyPlanBlockPages"));
    blockPages_->setAccessibleName(tr("Pages of the block"));
    blockPages_->setMaximumHeight(90);
    blockForm->addRow(tr("Pages:"), blockPages_);
    auto* blockButtons = new QHBoxLayout;
    excludePage_ = makeButton(tr("E&xclude page"), QStringLiteral("studyPlanExcludePage"),
                              tr("Exclude the selected page"), blockBox_);
    removeBlock_ = makeButton(tr("&Remove block"), QStringLiteral("studyPlanRemoveBlock"),
                              tr("Remove the block"), blockBox_);
    blockButtons->addWidget(excludePage_);
    blockButtons->addWidget(removeBlock_);
    blockButtons->addStretch(1);
    blockForm->addRow(blockButtons);
    right->addWidget(blockBox_);

    overflowBox_ = new QGroupBox(tr("Does not fit"), this);
    overflowBox_->setObjectName(QStringLiteral("studyPlanOverflow"));
    auto* overflowRow = new QHBoxLayout(overflowBox_);
    extend_ = makeButton(QString(), QStringLiteral("studyPlanExtend"),
                         tr("Extend the plan to the days it needs"), overflowBox_);
    keepUnscheduled_ =
        makeButton(tr("Keep &unscheduled"), QStringLiteral("studyPlanKeepUnscheduled"),
                   tr("Keep the blocks that do not fit without a date"), overflowBox_);
    removeOverflow_ = makeButton(tr("Remove &them"), QStringLiteral("studyPlanRemoveOverflow"),
                                 tr("Remove the blocks that do not fit"), overflowBox_);
    overflowRow->addWidget(extend_);
    overflowRow->addWidget(keepUnscheduled_);
    overflowRow->addWidget(removeOverflow_);
    overflowRow->addStretch(1);
    right->addWidget(overflowBox_);

    excludedBox_ = new QGroupBox(this);
    excludedBox_->setObjectName(QStringLiteral("studyPlanExcludedBox"));
    auto* excludedLayout = new QVBoxLayout(excludedBox_);
    excluded_ = new QTreeWidget(excludedBox_);
    excluded_->setObjectName(QStringLiteral("studyPlanExcluded"));
    excluded_->setHeaderHidden(true);
    excluded_->setItemsExpandable(false); // shown in full while there is any (Q13)
    excluded_->setRootIsDecorated(false);
    excluded_->setAccessibleName(tr("Excluded pages"));
    excluded_->setMaximumHeight(130);
    excludedLayout->addWidget(excluded_);
    auto* excludedButtons = new QHBoxLayout;
    restore_ = makeButton(tr("Re&store block"), QStringLiteral("studyPlanRestore"),
                          tr("Restore the removed block"), excludedBox_);
    addPage_ = makeButton(tr("&Add to selected block"), QStringLiteral("studyPlanAddPage"),
                          tr("Add the page to the selected block"), excludedBox_);
    excludedButtons->addWidget(restore_);
    excludedButtons->addWidget(addPage_);
    excludedButtons->addStretch(1);
    excludedLayout->addLayout(excludedButtons);
    right->addWidget(excludedBox_);

    referencesBox_ = new QGroupBox(tr("Changed pages"), this);
    referencesBox_->setObjectName(QStringLiteral("studyPlanReferencesBox"));
    auto* referencesLayout = new QVBoxLayout(referencesBox_);
    references_ = new QTreeWidget(referencesBox_);
    references_->setObjectName(QStringLiteral("studyPlanReferences"));
    references_->setHeaderHidden(true);
    references_->setRootIsDecorated(false);
    references_->setAccessibleName(tr("Deleted, moved and new pages"));
    references_->setMaximumHeight(110);
    referencesLayout->addWidget(references_);
    auto* referenceButtons = new QHBoxLayout;
    excludeReference_ =
        makeButton(tr("Re&move from the plan"), QStringLiteral("studyPlanExcludeReference"),
                   tr("Remove the page from the plan"), referencesBox_);
    keepReference_ = makeButton(tr("&Keep in the plan"), QStringLiteral("studyPlanKeepReference"),
                                tr("Keep the moved page in the plan"), referencesBox_);
    referenceButtons->addWidget(excludeReference_);
    referenceButtons->addWidget(keepReference_);
    referenceButtons->addStretch(1);
    referencesLayout->addLayout(referenceButtons);
    right->addWidget(referencesBox_);

    messages_ = new QLabel(this);
    messages_->setObjectName(QStringLiteral("studyPlanMessages"));
    messages_->setWordWrap(true);
    right->addWidget(messages_);

    auto* buttons = new QDialogButtonBox(this);
    accept_ = buttons->addButton(tr("&Accept"), QDialogButtonBox::AcceptRole);
    accept_->setObjectName(QStringLiteral("studyPlanAccept"));
    accept_->setAccessibleName(tr("Accept and save the plan"));
    cancel_ = buttons->addButton(QDialogButtonBox::Cancel);
    cancel_->setObjectName(QStringLiteral("studyPlanCancel"));
    cancel_->setAccessibleName(tr("Cancel"));
    layout->addWidget(buttons);
    resize(1040, 820);
}

void StudyPlanDialog::connectControls() {
    connect(build_, &QPushButton::clicked, this, &StudyPlanDialog::build);
    connect(accept_, &QPushButton::clicked, this, &StudyPlanDialog::save);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    // The goal and the start date change titles and dates only; the rest needs a new build.
    connect(goal_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (preview_) {
            preview_->setGoal(text.toStdString());
            render();
        }
    });
    connect(start_, &QDateEdit::dateChanged, this, [this](const QDate& date) {
        if (preview_ && preview_->setStart(fromQDate(date))) {
            render();
        }
    });
    connect(days_, &QSpinBox::valueChanged, this, [this] {
        updateStartRange();
        markStale();
    });
    connect(minutes_, &QSpinBox::valueChanged, this, &StudyPlanDialog::markStale);
    connect(minutes_, &QSpinBox::editingFinished, this,
            [this] { minutes_->setValue(roundToStep(minutes_->value())); });
    connect(scope_, &QTreeWidget::itemChanged, this, &StudyPlanDialog::markStale);

    // ---- preview edits (rule 6); a refused edit changes nothing and says why
    const auto withBlock = [this](auto edit) {
        return [this, edit] {
            if (preview_ && !stale_) {
                if (const auto block = selectedBlock()) {
                    apply(edit(*block));
                }
            }
        };
    };
    connect(table_, &QTreeWidget::currentItemChanged, this, [this] {
        if (!rendering_) {
            loadBlock();
        }
    });
    connect(blockTitle_, &QLineEdit::editingFinished, this, [this] {
        const auto block = preview_ && !stale_ ? selectedBlock() : std::nullopt;
        const std::string title = blockTitle_->text().toStdString();
        if (block && preview_->draft().blocks[*block].title != title) {
            apply(preview_->setBlockTitle(*block, title));
        }
    });
    connect(blockMinutes_, &QSpinBox::valueChanged, this, [this] {
        const auto block = !rendering_ && preview_ && !stale_ ? selectedBlock() : std::nullopt;
        const std::chrono::minutes value{roundToStep(blockMinutes_->value())};
        if (block && preview_->draft().blocks[*block].minutes != value) {
            apply(preview_->setBlockMinutes(*block, value));
        }
    });
    connect(moveBlock_, &QPushButton::clicked, this, [this] {
        const auto block = preview_ && !stale_ ? selectedBlock() : std::nullopt;
        if (!block) {
            return;
        }
        const int day = blockDay_->value();
        const bool sameDay = preview_->draft().blocks[*block].day == day; // nothing moves
        auto moved = preview_->moveBlock(*block, day);
        // The moved block is now the last one on its day (Q15): it stays selected.
        std::optional<std::size_t> index;
        const auto& blocks = preview_->draft().blocks;
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (blocks[i].day == day) {
                index = i;
            }
        }
        const std::optional<std::size_t> select = moved && !sameDay ? index : block;
        apply(std::move(moved), select);
    });
    connect(removeBlock_, &QPushButton::clicked, this,
            withBlock([this](std::size_t block) { return preview_->removeBlock(block); }));
    connect(excludePage_, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = blockPages_->currentItem();
        const auto page =
            item != nullptr ? idFrom<core::PageId>(item->data(kIdRole).toString()) : std::nullopt;
        if (preview_ && !stale_ && page) {
            apply(preview_->excludePage(planner_->workspace(), *page));
        }
    });
    connect(addPage_, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem* item = excluded_->currentItem();
        const QString chosen = item != nullptr ? item->data(0, kIdRole).toString() : QString();
        const auto page = chosen.startsWith(QStringLiteral("p:"))
                              ? idFrom<core::PageId>(chosen.mid(2))
                              : std::nullopt;
        const auto block = preview_ && !stale_ ? selectedBlock() : std::nullopt;
        if (block && page) {
            apply(preview_->addPage(*block, *page));
        }
    });
    connect(restore_, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem* item = excluded_->currentItem();
        const QString data = item != nullptr ? item->data(0, kIdRole).toString() : QString();
        if (preview_ && !stale_ && data.startsWith(QStringLiteral("r:"))) {
            apply(preview_->restoreBlock(data.mid(2).toULongLong()));
        }
    });
    const auto withReference = [this](bool keep) {
        return [this, keep] {
            const QTreeWidgetItem* item = references_->currentItem();
            const QString data = item != nullptr ? item->data(0, kIdRole).toString() : QString();
            const auto page = idFrom<core::PageId>(data.mid(2));
            if (!preview_ || stale_ || !page) {
                return;
            }
            apply(keep ? preview_->keepPage(planner_->workspace(), *page)
                       : preview_->excludePage(planner_->workspace(), *page));
        };
    };
    connect(excludeReference_, &QPushButton::clicked, this, withReference(false));
    connect(keepReference_, &QPushButton::clicked, this, withReference(true));

    // ---- overflow resolutions (rule 4); (b) is Rebuild
    const auto resolve = [this](auto resolution) {
        return [this, resolution] {
            if (preview_ && !stale_) {
                apply(resolution(*preview_));
            }
        };
    };
    connect(extend_, &QPushButton::clicked, this, [this] {
        if (!preview_ || stale_) {
            return;
        }
        auto extended = preview_->extendToNeededDays();
        if (extended) {
            // (a) keeps the placement: the form shows the new days without a rebuild.
            const QSignalBlocker blocker(days_);
            days_->setValue(preview_->draft().days);
            updateStartRange();
        }
        apply(std::move(extended));
    });
    connect(keepUnscheduled_, &QPushButton::clicked, this,
            resolve([](application::PlanPreview& plan) { return plan.keepOverflowUnscheduled(); }));
    connect(removeOverflow_, &QPushButton::clicked, this,
            resolve([](application::PlanPreview& plan) { return plan.removeOverflow(); }));

    for (QTreeWidget* tree : {excluded_, references_}) {
        connect(tree, &QTreeWidget::currentItemChanged, this, &StudyPlanDialog::updateControls);
    }
    connect(blockPages_, &QListWidget::currentRowChanged, this, &StudyPlanDialog::updateControls);
}

void StudyPlanDialog::fillScope() {
    const document::Workspace& ws = planner_->workspace();
    std::set<QString> checked;
    for (QTreeWidgetItemIterator it(scope_, QTreeWidgetItemIterator::Checked); *it != nullptr;
         ++it) {
        checked.insert((*it)->data(0, kIdRole).toString());
    }
    const QSignalBlocker blocker(scope_);
    scope_->clear();
    const auto add = [&](QTreeWidgetItem* item, const QString& kind, const QString& id) {
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setData(0, kIdRole, kind + id);
        // Independent check boxes (Q18); a deleted notebook or section drops out.
        item->setCheckState(0, checked.contains(kind + id) ? Qt::Checked : Qt::Unchecked);
    };
    for (const core::NotebookId notebook : ws.notebooks()) {
        auto* item = new QTreeWidgetItem(scope_, {toQString(ws.findNotebook(notebook)->title)});
        add(item, QStringLiteral("n:"), idText(notebook));
        for (const core::SectionId section : ws.sectionsOf(notebook)) {
            const auto pages = static_cast<int>(ws.pagesOf(section).size());
            auto* child =
                new QTreeWidgetItem(item, {tr("%1 (%n page(s))", nullptr, pages)
                                               .arg(toQString(ws.findSection(section)->title))});
            add(child, QStringLiteral("s:"), idText(section));
        }
    }
    scope_->expandAll();
}

application::PlanRequest StudyPlanDialog::request() const {
    application::PlanRequest request{
        .goal = goal_->text().toStdString(),
        .start = fromQDate(start_->date()),
        .days = days_->value(),
        .minutesPerDay = std::chrono::minutes{minutes_->value()},
    };
    for (QTreeWidgetItemIterator it(scope_, QTreeWidgetItemIterator::Checked); *it != nullptr;
         ++it) {
        const QString chosen = (*it)->data(0, kIdRole).toString();
        if (chosen.startsWith(QStringLiteral("n:"))) {
            if (auto id = idFrom<core::NotebookId>(chosen.mid(2))) {
                request.scope.notebooks.push_back(*id);
            }
        } else if (auto id = idFrom<core::SectionId>(chosen.mid(2))) {
            request.scope.sections.push_back(*id);
        }
    }
    return request;
}

void StudyPlanDialog::build() {
    // Rebuilding discards the edits made in the preview: only after asking (Q14).
    if (preview_ && edited_ && !dialogs_->confirmDiscardPlanEdits(this)) {
        return;
    }
    preview_.reset();
    refused_.reset();
    buildError_.clear();
    notice_.clear();
    stale_ = false;
    edited_ = false;
    const application::PlanRequest planRequest = request();
    auto built = application::draftByTreeOrder(planner_->workspace(), planRequest);
    if (!built) {
        buildError_ = toQString(built.error().message);
    } else if (built->outcome == PlanBuildOutcome::Draft) {
        preview_.emplace(planRequest, *built->draft);
    } else {
        refused_ = std::move(*built);
    }
    build_->setText(tr("Re&build"));
    render();
}

void StudyPlanDialog::save() {
    if (!preview_ || stale_ || readOnly_) {
        return;
    }
    // The final check, against the workspace as it is now (rule 7): nothing is saved
    // unless it allows Accept.
    if (!preview_->review(planner_->workspace()).canAccept()) {
        render();
        return;
    }
    auto project = planner_->createPlan(preview_->draft(), preview_->request());
    if (!project) {
        dialogs_->showError(this, tr("The study plan could not be saved."),
                            toQString(project.error().message));
        render();
        return;
    }
    if (saved_) {
        saved_(*project);
    }
    accept(); // deletes the dialog (WA_DeleteOnClose)
}

void StudyPlanDialog::markStale() {
    if (preview_ || refused_ || !buildError_.isEmpty()) {
        stale_ = true;
        render();
    }
}

void StudyPlanDialog::updateStartRange() {
    // The last day must be storable (rule 7): a later start cannot be chosen.
    start_->setMaximumDate(kLastDate.addDays(-(days_->value() - 1)));
}

void StudyPlanDialog::apply(core::Result<void> result, std::optional<std::size_t> select) {
    if (result) {
        edited_ = true;
        notice_.clear();
    } else {
        notice_ = toQString(result.error().message);
    }
    select_ = select;
    render();
}

std::optional<std::size_t> StudyPlanDialog::selectedBlock() const {
    const QTreeWidgetItem* item = table_->currentItem();
    if (!preview_ || item == nullptr || !item->data(0, kBlockRole).isValid()) {
        return std::nullopt;
    }
    const auto block = item->data(0, kBlockRole).toULongLong();
    return block < preview_->draft().blocks.size() ? std::optional<std::size_t>(block)
                                                   : std::nullopt;
}

QString StudyPlanDialog::pageTitle(core::PageId page) const {
    const document::PageInfo* info = planner_->workspace().findPage(page);
    return info == nullptr       ? tr("(deleted page)")
           : info->title.empty() ? tr("Untitled page")
                                 : toQString(info->title);
}

void StudyPlanDialog::render() {
    const document::Workspace& ws = planner_->workspace();
    rendering_ = true;
    const std::optional<std::size_t> selected =
        select_ ? std::exchange(select_, std::nullopt) : selectedBlock();
    table_->clear();
    QStringList messages;
    bool canAccept = false;
    std::optional<PlanReview> review;

    const auto addBlock = [&](QTreeWidgetItem* parent, std::size_t index, const QString& title) {
        const PlanBlock& block = preview_->draft().blocks[index];
        QStringList titles;
        for (const core::PageId page : block.pages) {
            titles << pageTitle(page);
        }
        auto* item = new QTreeWidgetItem(
            parent, {title, tr("%1 min").arg(block.minutes.count()),
                     titles.isEmpty() ? tr("no linked pages") : titles.join(QStringLiteral(", "))});
        item->setData(0, kBlockRole, static_cast<qulonglong>(index));
        item->setToolTip(2, item->text(2));
        if (selected == index) {
            table_->setCurrentItem(item);
        }
    };

    if (!buildError_.isEmpty()) {
        summary_->setText(tr("The plan cannot be built: %1.").arg(buildError_));
    } else if (refused_ && refused_->outcome == PlanBuildOutcome::NoPages) {
        summary_->setText(tr("No pages in the chosen scope."));
    } else if (refused_) {
        // Rule 5: nothing is offered, every input is kept.
        summary_->setText(
            tr("The scope needs %1 blocks; the limit is %2. Choose fewer notebooks or sections, "
               "or more minutes per day so that large sections are split less, then rebuild.")
                .arg(refused_->blocksNeeded)
                .arg(application::kMaxPlanBlocks));
        for (const application::ScopeSection& section : refused_->sections) {
            const document::SectionInfo* info = ws.findSection(section.section);
            if (info != nullptr) {
                new QTreeWidgetItem(table_,
                                    {toQString(info->title), QString(),
                                     tr("%n page(s)", nullptr, static_cast<int>(section.pages))});
            }
        }
    } else if (preview_) {
        const application::PlanDraft& draft = preview_->draft();
        const auto perDay = draft.minutesPerDay.count();
        for (int day = 1; day <= draft.days; ++day) {
            auto* header = new QTreeWidgetItem(
                table_, {tr("Day %1 · %2")
                             .arg(day)
                             .arg(dateText(application::planDate(draft.start, day)))});
            std::chrono::minutes used{0};
            for (std::size_t i = 0; i < draft.blocks.size(); ++i) {
                if (draft.blocks[i].day == day) {
                    addBlock(header, i, toQString(draft.blocks[i].title));
                    used += draft.blocks[i].minutes;
                }
            }
            header->setText(1, tr("%1 of %2 min").arg(used.count()).arg(perDay));
        }
        const std::optional<application::PlanOverflow> overflow = preview_->overflow();
        if (overflow) {
            auto* group =
                new QTreeWidgetItem(table_, {tr("Does not fit in %n day(s)", nullptr, draft.days)});
            for (const std::size_t i : overflow->blocks) {
                addBlock(group, i,
                         tr("%1 (needs day %2)")
                             .arg(toQString(draft.blocks[i].title))
                             .arg(*draft.blocks[i].day));
            }
        }
        if (std::ranges::any_of(draft.blocks,
                                [](const PlanBlock& block) { return !block.day.has_value(); })) {
            auto* group = new QTreeWidgetItem(table_, {tr("No date")});
            for (std::size_t i = 0; i < draft.blocks.size(); ++i) {
                if (!draft.blocks[i].day) {
                    addBlock(group, i, toQString(draft.blocks[i].title));
                }
            }
        }
        table_->expandAll();

        review = preview_->review(ws);
        QString summary =
            tr("%n block(s) from %1 to %2.", nullptr, static_cast<int>(draft.blocks.size()))
                .arg(dateText(draft.start),
                     dateText(application::planDate(draft.start, draft.days)));
        if (overflow) {
            summary += QLatin1Char(' ') + tr("Needs %1 days, %2 requested; %3 minutes over.")
                                              .arg(overflow->daysNeeded)
                                              .arg(draft.days)
                                              .arg(overflow->minutesOver.count());
        }
        if (!review->excluded.empty()) {
            summary += QLatin1Char(' ') + tr("%n page(s) excluded.", nullptr,
                                             static_cast<int>(review->excluded.size()));
        }
        summary_->setText(summary);
        overflowBox_->setVisible(overflow.has_value());
        if (overflow) {
            extend_->setText(tr("&Extend to %1 days").arg(overflow->daysNeeded));
            extend_->setEnabled(overflow->canExtend);
        }

        for (const application::PlanIssue& warning : review->validation.warnings) {
            messages << toQString(warning.message);
        }
        for (const PlanHold hold : review->holds) {
            switch (hold) {
            case PlanHold::Overflow:
                messages << tr("Some blocks do not fit in the plan's days.");
                break;
            case PlanHold::InvalidReferences:
                messages << tr("%n linked page(s) no longer exist.", nullptr,
                               static_cast<int>(review->invalid.size()));
                break;
            case PlanHold::ChangedReferences:
                messages << tr("%n linked page(s) were moved out of the chosen scope.", nullptr,
                               static_cast<int>(review->changed.size()));
                break;
            case PlanHold::InvalidDraft:
                for (const application::PlanIssue& error : review->validation.errors) {
                    if (error.kind != PlanIssueKind::BlockAfterLastDay &&
                        error.kind != PlanIssueKind::MissingPage) {
                        messages << toQString(error.message);
                    }
                }
                break;
            case PlanHold::NoBlocks:
                messages << tr("The plan has no study blocks.");
                break;
            }
        }
        canAccept = review->canAccept() && !stale_ && !readOnly_;
    } else {
        summary_->setText(tr("Choose what to cover, then press Build to see the plan."));
    }
    if (!preview_) {
        overflowBox_->hide();
    }
    renderExcluded(review.value_or(PlanReview{}));
    renderReferences(review.value_or(PlanReview{}));
    if (!notice_.isEmpty()) {
        messages.prepend(notice_);
    }
    if (stale_) {
        messages << tr("Days, minutes per day or the scope changed: press Rebuild to update "
                       "the preview.");
    }
    if (readOnly_) {
        messages << tr("The workspace is open read-only: the plan can be previewed but not "
                       "saved.");
    }
    messages_->setText(messages.join(QLatin1Char('\n')));
    messages_->setVisible(!messages.isEmpty());
    accept_->setEnabled(canAccept);
    blockBox_->setVisible(preview_.has_value());
    rendering_ = false;
    loadBlock();
}

void StudyPlanDialog::loadBlock() {
    rendering_ = true; // filling the fields is not an edit
    const std::optional<std::size_t> block = selectedBlock();
    blockPages_->clear();
    if (block) {
        const PlanBlock& shown = preview_->draft().blocks[*block];
        if (!blockTitle_->hasFocus()) {
            blockTitle_->setText(toQString(shown.title));
        }
        blockMinutes_->setValue(static_cast<int>(shown.minutes.count()));
        blockDay_->setRange(1, preview_->draft().days);
        blockDay_->setValue(shown.day.value_or(1));
        for (const core::PageId page : shown.pages) {
            auto* item = new QListWidgetItem(pageTitle(page), blockPages_);
            item->setData(kIdRole, idText(page));
        }
    } else {
        blockTitle_->clear();
    }
    rendering_ = false;
    updateControls();
}

void StudyPlanDialog::renderExcluded(const PlanReview& review) {
    excluded_->clear();
    const bool any = preview_ && (!preview_->removedBlocks().empty() || !review.excluded.empty());
    excludedBox_->setVisible(any);
    if (!any) {
        return;
    }
    excludedBox_->setTitle(tr("Excluded pages (%1)").arg(review.excluded.size()));
    const std::set<core::PageId> existing(review.excluded.begin(), review.excluded.end());
    const auto addPage = [&](QTreeWidgetItem* parent, core::PageId page) {
        if (existing.contains(page)) {
            auto* item = new QTreeWidgetItem(parent, {pageTitle(page)});
            item->setData(0, kIdRole, QStringLiteral("p:") + idText(page));
        }
    };
    const auto& removed = preview_->removedBlocks();
    for (std::size_t i = 0; i < removed.size(); ++i) {
        auto* item = new QTreeWidgetItem(
            excluded_, {tr("%1 (removed block)").arg(toQString(removed[i].title))});
        item->setData(0, kIdRole, QStringLiteral("r:%1").arg(i));
        for (const core::PageId page : removed[i].pages) {
            addPage(item, page);
        }
    }
    // Pages excluded one at a time, grouped by section.
    const document::Workspace& ws = planner_->workspace();
    std::map<QString, QTreeWidgetItem*> sections;
    for (const core::PageId page : preview_->excludedPages()) {
        const document::PageInfo* info = ws.findPage(page);
        if (info == nullptr) {
            continue;
        }
        const QString key = idText(info->section);
        auto [at, inserted] = sections.try_emplace(key, nullptr);
        if (inserted) {
            const document::SectionInfo* section = ws.findSection(info->section);
            at->second = new QTreeWidgetItem(
                excluded_, {section != nullptr ? toQString(section->title) : QString()});
        }
        addPage(at->second, page);
    }
    excluded_->expandAll();
}

void StudyPlanDialog::renderReferences(const PlanReview& review) {
    references_->clear();
    const auto blockTitle = [&](std::size_t block) {
        return toQString(preview_->draft().blocks[block].title);
    };
    const auto add = [&](const QString& kind, core::PageId page, const QString& text) {
        auto* item = new QTreeWidgetItem(references_, {text});
        item->setData(0, kIdRole, kind + idText(page));
        item->setToolTip(0, text);
    };
    for (const application::PlanReference& reference : review.invalid) {
        add(QStringLiteral("x:"), reference.page,
            tr("A page linked by “%1” was deleted.").arg(blockTitle(reference.block)));
    }
    for (const application::PlanReference& reference : review.changed) {
        add(QStringLiteral("c:"), reference.page,
            tr("“%1” (in “%2”) was moved out of the chosen scope.")
                .arg(pageTitle(reference.page), blockTitle(reference.block)));
    }
    for (const application::PlanReference& reference : review.kept) {
        add(QStringLiteral("k:"), reference.page,
            tr("“%1” (in “%2”) is kept, outside the chosen scope.")
                .arg(pageTitle(reference.page), blockTitle(reference.block)));
    }
    for (const core::PageId page : review.notInPlan) {
        add(QStringLiteral("n:"), page,
            tr("“%1” is in the scope but not in the plan: rebuild to include it, or remove it.")
                .arg(pageTitle(page)));
    }
    referencesBox_->setVisible(references_->topLevelItemCount() > 0);
}

void StudyPlanDialog::updateControls() {
    const bool editable = preview_.has_value() && !stale_;
    const std::optional<std::size_t> block = editable ? selectedBlock() : std::nullopt;
    for (QWidget* widget : std::initializer_list<QWidget*>{blockTitle_, blockMinutes_, blockDay_,
                                                           moveBlock_, removeBlock_}) {
        widget->setEnabled(block.has_value());
    }
    excludePage_->setEnabled(block && blockPages_->currentItem() != nullptr);
    for (QWidget* widget :
         std::initializer_list<QWidget*>{extend_, keepUnscheduled_, removeOverflow_}) {
        if (!editable) {
            widget->setEnabled(false);
        }
    }
    if (editable && preview_->overflow()) {
        extend_->setEnabled(preview_->overflow()->canExtend);
        keepUnscheduled_->setEnabled(true);
        removeOverflow_->setEnabled(true);
    }
    const QTreeWidgetItem* excluded = excluded_->currentItem();
    const QString excludedData =
        excluded != nullptr ? excluded->data(0, kIdRole).toString() : QString();
    restore_->setEnabled(editable && excludedData.startsWith(QStringLiteral("r:")));
    addPage_->setEnabled(block && excludedData.startsWith(QStringLiteral("p:")));
    const QTreeWidgetItem* reference = references_->currentItem();
    const QString referenceData =
        reference != nullptr ? reference->data(0, kIdRole).toString() : QString();
    excludeReference_->setEnabled(editable && !referenceData.isEmpty());
    keepReference_->setEnabled(editable && referenceData.startsWith(QStringLiteral("c:")));
}

} // namespace studyapp::ui
