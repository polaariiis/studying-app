#include "ThemeIcons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

#include <algorithm>
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

QPixmap blank() {
    QPixmap pixmap(static_cast<int>(kLogicalSize * kScale),
                   static_cast<int>(kLogicalSize * kScale));
    pixmap.setDevicePixelRatio(kScale);
    pixmap.fill(Qt::transparent);
    return pixmap;
}

} // namespace

QIcon swatchIcon(const QColor& fill, const QColor& border) {
    QPixmap pixmap = blank();
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(fill);
        painter.drawEllipse(QPointF(kLogicalSize / 2.0, kLogicalSize / 2.0), 6.5, 6.5);
    }
    return QIcon(pixmap);
}

QIcon lineWidthIcon(double thickness, const QColor& color) {
    QPixmap pixmap = blank();
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, thickness, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(3.5, kLogicalSize / 2.0),
                         QPointF(kLogicalSize - 3.5, kLogicalSize / 2.0));
    }
    return QIcon(pixmap);
}

QIcon penOptionsIcon(const QColor& ink, double width, const QColor& border) {
    QPixmap pixmap = blank();
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(border, 1.0));
        painter.setBrush(ink);
        const double radius = std::clamp(2.5 + width * 1.2, 3.0, 8.0);
        painter.drawEllipse(QPointF(kLogicalSize / 2.0, kLogicalSize / 2.0), radius, radius);
    }
    return QIcon(pixmap);
}

QIcon shapeIcon(document::ShapeKind kind, const QColor& color, bool filled) {
    QPixmap pixmap = blank();
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QColor fill = color;
        fill.setAlphaF(0.25F);
        painter.setBrush(filled ? QBrush(fill) : QBrush(Qt::NoBrush));
        const QRectF box(4.0, 5.5, 12.0, 9.0);
        switch (kind) {
        case document::ShapeKind::Rectangle:
            painter.drawRect(box);
            break;
        case document::ShapeKind::Ellipse:
            painter.drawEllipse(box);
            break;
        case document::ShapeKind::Line:
            painter.drawLine(QPointF(4.0, 15.0), QPointF(16.0, 5.0));
            break;
        case document::ShapeKind::Arrow: {
            painter.drawLine(QPointF(4.0, 15.0), QPointF(15.0, 6.0));
            painter.setBrush(color);
            const QPointF head[] = {QPointF(16.5, 4.5), QPointF(10.5, 6.0), QPointF(15.0, 10.5)};
            painter.drawPolygon(head, 3);
            break;
        }
        }
    }
    return QIcon(pixmap);
}

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
