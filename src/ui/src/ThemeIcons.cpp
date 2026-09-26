#include "ThemeIcons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

#include <cmath>
#include <numbers>

namespace studyapp::ui {

namespace {

constexpr int kLogicalSize = 20;
constexpr qreal kScale = 3.0; // drawn at 60 px; Qt scales down per screen

void drawSun(QPainter& painter, const QColor& color) {
    const QPointF centre(kLogicalSize / 2.0, kLogicalSize / 2.0);
    QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, 3.6, 3.6);
    for (int i = 0; i < 8; ++i) {
        const double angle = i * std::numbers::pi / 4.0;
        const QPointF direction(std::cos(angle), std::sin(angle));
        painter.drawLine(centre + direction * 6.0, centre + direction * 8.2);
    }
}

void drawMoon(QPainter& painter, const QColor& color) {
    // A crescent: a disc minus an offset disc, outlined.
    QPainterPath disc;
    disc.addEllipse(QPointF(9.5, 10.5), 6.8, 6.8);
    QPainterPath bite;
    bite.addEllipse(QPointF(13.6, 7.2), 6.0, 6.0);
    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(disc.subtracted(bite));
}

} // namespace

QIcon themeToggleIcon(bool darkThemeShown, const QColor& color) {
    QPixmap pixmap(static_cast<int>(kLogicalSize * kScale),
                   static_cast<int>(kLogicalSize * kScale));
    pixmap.setDevicePixelRatio(kScale);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        if (darkThemeShown) {
            drawSun(painter, color);
        } else {
            drawMoon(painter, color);
        }
    }
    return QIcon(pixmap);
}

} // namespace studyapp::ui
