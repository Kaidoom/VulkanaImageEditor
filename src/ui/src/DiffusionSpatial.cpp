#include "imageeditor/ui/DiffusionSpatial.hpp"
#include <QColorSpace>
#include <QJsonObject>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace imageeditor::ui {
DiffusionInputCrop diffusionCropToCanvas(const QImage& source, const QTransform& transform, QSize canvas)
{
    if (source.isNull() || canvas.isEmpty() || !transform.isInvertible())
        throw std::invalid_argument("Cannot crop this input to its canvas.");
    const QRectF sourceRect(QPointF{}, source.size());
    const auto mapped = transform.mapRect(sourceRect);
    if (!std::isfinite(mapped.left()) || !std::isfinite(mapped.top())
        || !std::isfinite(mapped.right()) || !std::isfinite(mapped.bottom()))
        throw std::invalid_argument("The input has invalid canvas coordinates.");
    // A projective horizon crossing the source rectangle is not a bounded image.
    double minimum = 1e300, maximum = -1e300;
    for (const auto& point : {sourceRect.topLeft(), sourceRect.topRight(), sourceRect.bottomLeft(), sourceRect.bottomRight()}) {
        const auto w = point.x() * transform.m13() + point.y() * transform.m23() + transform.m33();
        minimum = std::min(minimum, w); maximum = std::max(maximum, w);
    }
    if (minimum <= 0 && maximum >= 0)
        throw std::invalid_argument("The input crosses a projective horizon.");
    const auto bounds = mapped.intersected(QRectF(QPointF{}, canvas)).toAlignedRect()
                            .intersected(QRect(QPoint{}, canvas));
    if (bounds.isEmpty()) return {};
    if (qint64(bounds.width()) * bounds.height() > 67108864)
        throw std::invalid_argument("Cropped AI inputs must contain at most 64 megapixels.");
    QImage image(bounds.size(), QImage::Format_RGBA8888);
    if (image.isNull()) throw std::bad_alloc();
    image.fill(Qt::transparent);
    image.setColorSpace(QColorSpace::SRgb);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setTransform(transform * QTransform::fromTranslate(-bounds.x(), -bounds.y()));
    painter.drawImage(QPointF{}, source);
    painter.end();
    return {image, QTransform::fromTranslate(bounds.x(), bounds.y())};
}
QVector<DiffusionRegion> diffusionMapInputRegions(const QVector<DiffusionRegion>& regions,
    QSize fromSize, const QTransform& fromDocument, QSize toSize, const QTransform& toDocument)
{
    QVector<DiffusionRegion> result;
    if (fromSize.isEmpty() || toSize.isEmpty()) return result;
    bool valid = false;
    const auto inverse = toDocument.inverted(&valid);
    if (!valid) throw std::invalid_argument("The region coordinate transform is invalid.");
    const auto map = fromDocument * inverse;
    for (auto region : regions) {
        const auto r = region.bounds;
        const auto bounds = map.mapRect(QRectF(r.x() * fromSize.width(), r.y() * fromSize.height(),
            r.width() * fromSize.width(), r.height() * fromSize.height()))
                                .intersected(QRectF(QPointF{}, toSize));
        if (bounds.isEmpty()) continue;
        region.bounds = {bounds.x() / toSize.width(), bounds.y() / toSize.height(),
                         bounds.width() / toSize.width(), bounds.height() / toSize.height()};
        result.push_back(std::move(region));
    }
    return result;
}
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
