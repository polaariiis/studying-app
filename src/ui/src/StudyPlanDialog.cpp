#include "StudyPlanDialog.hpp"

#include <studyapp/ui/ShellDialogs.hpp>

#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
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
#include <string>
#include <utility>
#include <vector>

namespace studyapp::ui {

using application::PlanBlock;
using application::PlanBuildOutcome;
using application::PlanHold;
using application::PlanIssueKind;
using application::PlanReview;

namespace {

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

QString dateText(const study::CalendarDate& date) {
    return QLocale().toString(toQDate(date), QStringLiteral("ddd d MMM yyyy"));
}

/// The last storable date (study::isStorableDate).
const QDate kLastDate(9999, 12, 31);

} // namespace

StudyPlanDialog::StudyPlanDialog(application::Planner& planner, ShellDialogs& dialogs,
                                 study::CalendarDate today, bool readOnly, QWidget* parent)
    : QDialog(parent), planner_(&planner), dialogs_(&dialogs), readOnly_(readOnly) {
    setObjectName(QStringLiteral("studyPlanDialog"));
    setWindowTitle(tr("New Study Plan"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    buildLayout();
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

void StudyPlanDialog::buildLayout() {
    auto* layout = new QVBoxLayout(this);

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
    layout->addLayout(form);

    auto* scopeLabel = new QLabel(tr("Notebooks and sections to cover:"), this);
    layout->addWidget(scopeLabel);
    scope_ = new QTreeWidget(this);
    scope_->setObjectName(QStringLiteral("studyPlanScope"));
    scope_->setHeaderHidden(true);
    scope_->setAccessibleName(tr("Notebooks and sections to cover"));
    scopeLabel->setBuddy(scope_);
    layout->addWidget(scope_, 1);

    build_ = new QPushButton(tr("&Build"), this);
    build_->setObjectName(QStringLiteral("studyPlanBuild"));
    build_->setAccessibleName(tr("Build the preview"));
    build_->setAutoDefault(false);
    layout->addWidget(build_, 0, Qt::AlignLeft);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("studyPlanSummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    table_ = new QTreeWidget(this);
    table_->setObjectName(QStringLiteral("studyPlanPreview"));
    table_->setColumnCount(3);
    table_->setHeaderLabels({tr("Day and block"), tr("Minutes"), tr("Linked pages")});
    table_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->setAccessibleName(tr("Plan preview"));
    layout->addWidget(table_, 2);
    messages_ = new QLabel(this);
    messages_->setObjectName(QStringLiteral("studyPlanMessages"));
    messages_->setWordWrap(true);
    layout->addWidget(messages_);

    auto* buttons = new QDialogButtonBox(this);
    accept_ = buttons->addButton(tr("&Accept"), QDialogButtonBox::AcceptRole);
    accept_->setObjectName(QStringLiteral("studyPlanAccept"));
    accept_->setAccessibleName(tr("Accept and save the plan"));
    cancel_ = buttons->addButton(QDialogButtonBox::Cancel);
    cancel_->setObjectName(QStringLiteral("studyPlanCancel"));
    cancel_->setAccessibleName(tr("Cancel"));
    layout->addWidget(buttons);

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
    connect(minutes_, &QSpinBox::editingFinished, this, [this] {
        const int step = static_cast<int>(application::kPlanMinuteStep.count());
        minutes_->setValue((minutes_->value() + step / 2) / step * step); // steps of 5
    });
    connect(scope_, &QTreeWidget::itemChanged, this, &StudyPlanDialog::markStale);
    resize(640, 720);
}

void StudyPlanDialog::fillScope() {
    const document::Workspace& ws = planner_->workspace();
    const QSignalBlocker blocker(scope_);
    scope_->clear();
    const auto add = [](QTreeWidgetItem* item, const QString& kind, const QString& id) {
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked); // independent check boxes (Q18)
        item->setData(0, kIdRole, kind + id);
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
    preview_.reset();
    refused_.reset();
    buildError_.clear();
    stale_ = false;
    const application::PlanRequest planRequest = request();
    auto built = application::draftByTreeOrder(planner_->workspace(), planRequest);
    if (!built) {
        buildError_ = toQString(built.error().message);
    } else if (built->outcome == PlanBuildOutcome::Draft) {
        preview_.emplace(planRequest, *built->draft);
    } else {
        refused_ = std::move(*built);
    }
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

void StudyPlanDialog::render() {
    const document::Workspace& ws = planner_->workspace();
    table_->clear();
    QStringList messages;
    bool canAccept = false;

    const auto pageTitles = [&](const PlanBlock& block) {
        if (block.pages.empty()) {
            return tr("no linked pages");
        }
        QStringList titles;
        for (const core::PageId page : block.pages) {
            const document::PageInfo* info = ws.findPage(page);
            titles << (info == nullptr       ? tr("(deleted page)")
                       : info->title.empty() ? tr("Untitled page")
                                             : toQString(info->title));
        }
        return titles.join(QStringLiteral(", "));
    };
    const auto addBlock = [&](QTreeWidgetItem* parent, const PlanBlock& block,
                              const QString& title) {
        auto* item = new QTreeWidgetItem(
            parent, {title, tr("%1 min").arg(block.minutes.count()), pageTitles(block)});
        item->setToolTip(2, item->text(2));
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
            for (const PlanBlock& block : draft.blocks) {
                if (block.day == day) {
                    addBlock(header, block, toQString(block.title));
                    used += block.minutes;
                }
            }
            header->setText(1, tr("%1 of %2 min").arg(used.count()).arg(perDay));
        }
        const std::optional<application::PlanOverflow> overflow = preview_->overflow();
        if (overflow) {
            auto* group =
                new QTreeWidgetItem(table_, {tr("Does not fit in %n day(s)", nullptr, draft.days)});
            for (const std::size_t i : overflow->blocks) {
                const PlanBlock& block = draft.blocks[i];
                addBlock(group, block,
                         tr("%1 (needs day %2)").arg(toQString(block.title)).arg(*block.day));
            }
        }
        const bool unscheduled = std::ranges::any_of(
            draft.blocks, [](const PlanBlock& block) { return !block.day.has_value(); });
        if (unscheduled) {
            auto* group = new QTreeWidgetItem(table_, {tr("No date")});
            for (const PlanBlock& block : draft.blocks) {
                if (!block.day) {
                    addBlock(group, block, toQString(block.title));
                }
            }
        }
        table_->expandAll();

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
        summary_->setText(summary);

        const PlanReview review = preview_->review(ws);
        for (const application::PlanIssue& warning : review.validation.warnings) {
            messages << toQString(warning.message);
        }
        for (const PlanHold hold : review.holds) {
            switch (hold) {
            case PlanHold::Overflow:
                messages << tr("Some blocks do not fit in the plan's days.");
                break;
            case PlanHold::InvalidReferences:
                messages << tr("%n linked page(s) no longer exist.", nullptr,
                               static_cast<int>(review.invalid.size()));
                break;
            case PlanHold::ChangedReferences:
                messages << tr("%n linked page(s) were moved out of the chosen scope.", nullptr,
                               static_cast<int>(review.changed.size()));
                break;
            case PlanHold::InvalidDraft:
                for (const application::PlanIssue& error : review.validation.errors) {
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
        canAccept = review.canAccept() && !stale_ && !readOnly_;
    } else {
        summary_->setText(tr("Choose what to cover, then press Build to see the plan."));
    }
    if (stale_) {
        messages << tr("Days, minutes per day or the scope changed: press Build to update "
                       "the preview.");
    }
    if (readOnly_) {
        messages << tr("The workspace is open read-only: the plan can be previewed but not "
                       "saved.");
    }
    messages_->setText(messages.join(QLatin1Char('\n')));
    messages_->setVisible(!messages.isEmpty());
    accept_->setEnabled(canAccept);
}

} // namespace studyapp::ui
