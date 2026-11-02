#include "imageeditor/ui/DiffusionSpatial.hpp"
#include <QColorSpace>
#include <QJsonObject>
#include <QPainter>
#include <algorithm>
#include <cmath>
namespace imageeditor::ui {
QImage diffusionRegionAnnotation(const QImage& base, const QJsonArray& regions, QSize outputSize)
{
    if (outputSize.isEmpty() || qint64(outputSize.width()) * outputSize.height() > 67108864)
        return { };
    QImage result(outputSize, QImage::Format_RGBA8888);
    if (result.isNull())
        return { };
    result.fill(Qt::white);
    result.setColorSpace(QColorSpace::SRgb);
    QPainter painter(&result);
    if (!base.isNull()) {
        QImage input = base;
        if (input.colorSpace().isValid() && input.colorSpace() != QColorSpace(QColorSpace::SRgb))
            input.convertToColorSpace(QColorSpace::SRgb);
        painter.drawImage(QRect(QPoint { }, outputSize), input);
    }
    QFont font = painter.font();
    font.setPixelSize(std::max(14, outputSize.width() / 45));
    font.setBold(true);
    painter.setFont(font);
    int index = 0;
    for (const auto& value : regions) {
        const auto r = value.toObject();
        const QRectF bounds(r["x"].toDouble() * outputSize.width(), r["y"].toDouble() * outputSize.height(),
            r["width"].toDouble() * outputSize.width(), r["height"].toDouble() * outputSize.height());
        painter.setPen(QPen(QColor(255, 40, 150), std::max(2, outputSize.width() / 300)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bounds);
        const QString text = QString::number(++index);
        const QRectF tag(bounds.topLeft(),
            QSizeF(painter.fontMetrics().horizontalAdvance(text) + 12, painter.fontMetrics().height() + 6));
        painter.fillRect(tag, QColor(255, 40, 150));
        painter.setPen(Qt::white);
        painter.drawText(tag, Qt::AlignCenter, text);
    }
    return result;
}
}
