#include <studyapp/ui/CanvasPlaceholder.hpp>

#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/ui/DesignTokens.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QVBoxLayout>

#include <string_view>

namespace studyapp::ui {

namespace {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

CanvasPlaceholder::CanvasPlaceholder(const ThemeManager& themes, QWidget* parent)
    : QWidget(parent), themes_(&themes) {
    setObjectName(QStringLiteral("placeholder"));

    auto* title = new QLabel(toQString(core::build::kProductName), this);
    title->setObjectName(QStringLiteral("placeholderTitle"));
    title->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel(
        tr("Foundation build. Notebooks, the canvas and the planner are not implemented yet."),
        this);
    subtitle->setObjectName(QStringLiteral("placeholderSubtitle"));
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addStretch();
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addStretch();

    connect(&themes, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
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
