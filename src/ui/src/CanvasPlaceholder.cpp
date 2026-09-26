#include <studyapp/ui/CanvasPlaceholder.hpp>

#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/ui/DesignTokens.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QVBoxLayout>

#include <string_view>

namespace studyapp::ui {

namespace {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

constexpr int kMaxRecent = 5;

} // namespace

CanvasPlaceholder::CanvasPlaceholder(const ThemeManager& themes, QWidget* parent)
    : QWidget(parent), themes_(&themes) {
    setObjectName(QStringLiteral("placeholder"));

    auto* title = new QLabel(toQString(core::build::kProductName), this);
    title->setObjectName(QStringLiteral("placeholderTitle"));
    title->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel(tr("No workspace is open. A workspace is a folder that holds your "
                                   "notebooks; everything in it is saved automatically."),
                                this);
    subtitle->setObjectName(QStringLiteral("placeholderSubtitle"));
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);

    actions_ = new QWidget(this);
    auto* newButton = new QPushButton(tr("New Workspace…"), actions_);
    newButton->setObjectName(QStringLiteral("welcomeNewWorkspace"));
    auto* openButton = new QPushButton(tr("Open Workspace…"), actions_);
    openButton->setObjectName(QStringLiteral("welcomeOpenWorkspace"));
    connect(newButton, &QPushButton::clicked, this, &CanvasPlaceholder::newWorkspaceRequested);
    connect(openButton, &QPushButton::clicked, this, &CanvasPlaceholder::openWorkspaceRequested);
    auto* buttons = new QHBoxLayout(actions_);
    buttons->setContentsMargins(0, 8, 0, 0);
    buttons->addStretch();
    buttons->addWidget(newButton);
    buttons->addWidget(openButton);
    buttons->addStretch();

    recentBox_ = new QWidget(this);
    recentBox_->setObjectName(QStringLiteral("welcomeRecent"));
    auto* recentLayout = new QVBoxLayout(recentBox_);
    recentLayout->setContentsMargins(0, 12, 0, 0);
    recentLayout->setSpacing(2);
    auto* recentTitle = new QLabel(tr("Recent"), recentBox_);
    recentTitle->setObjectName(QStringLiteral("placeholderSubtitle"));
    recentTitle->setAlignment(Qt::AlignCenter);
    recentLayout->addWidget(recentTitle);
    recentList_ = new QVBoxLayout;
    recentList_->setSpacing(0);
    recentLayout->addLayout(recentList_);
    recentBox_->hide();

    // A fixed-width column, so the wrapped explanation gets the height it needs.
    auto* column = new QWidget(this);
    column->setObjectName(QStringLiteral("welcomeColumn"));
    column->setFixedWidth(480);
    title->setParent(column);
    subtitle->setParent(column);
    actions_->setParent(column);
    recentBox_->setParent(column);
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(6);
    columnLayout->addWidget(title);
    columnLayout->addWidget(subtitle);
    columnLayout->addWidget(actions_);
    columnLayout->addWidget(recentBox_);

    // Centred with stretches, not an alignment flag: an aligned item gets its size hint,
    // which ignores the wrapped label's height for the column's width.
    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(column);
    row->addStretch();
    auto* layout = new QVBoxLayout(this);
    layout->addStretch();
    layout->addLayout(row);
    layout->addStretch();

    connect(&themes, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void CanvasPlaceholder::setActionsAvailable(bool available) {
    actionsAvailable_ = available;
    actions_->setVisible(available);
    recentBox_->setVisible(available && recentList_->count() > 0);
}

void CanvasPlaceholder::setRecentWorkspaces(const QStringList& paths) {
    while (QLayoutItem* item = recentList_->takeAt(0)) {
        if (QWidget* button = item->widget()) {
            button->hide();
            button->deleteLater(); // it may be the button whose click led here
        }
        delete item;
    }
    for (const QString& path : paths.mid(0, kMaxRecent)) {
        const QFileInfo info(path);
        auto* button = new QPushButton(info.completeBaseName(), recentBox_);
        button->setObjectName(QStringLiteral("welcomeRecentItem"));
        button->setFlat(true);
        button->setToolTip(QDir::toNativeSeparators(path));
        connect(button, &QPushButton::clicked, this,
                [this, path] { Q_EMIT openRecentRequested(path); });
        recentList_->addWidget(button);
    }
    recentBox_->setVisible(actionsAvailable_ && recentList_->count() > 0);
}

void CanvasPlaceholder::paintEvent(QPaintEvent* /*event*/) {
    const ColorTokens& tokens = themes_->tokens();
    const int spacing = metricTokens().canvasGridSpacing;

    QPainter painter(this);
    painter.fillRect(rect(), toQColor(tokens.canvas));

    // Subtle dot grid, one logical pixel per dot.
    QPen pen(toQColor(tokens.canvasGrid));
    pen.setWidthF(1.0);
    painter.setPen(pen);
    for (int y = spacing; y < height(); y += spacing) {
        for (int x = spacing; x < width(); x += spacing) {
            painter.drawPoint(x, y);
        }
    }
}

} // namespace studyapp::ui
