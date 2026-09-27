#include "imageeditor/render/PointerTooltip.hpp"
#include <QFontMetricsF>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace imageeditor::render {
std::string pointerSizeText(core::Extent2d size)
{
    if (!std::isfinite(size.width) || !std::isfinite(size.height)) return {};
    const auto dimension = [](double n) { return std::round(std::abs(n)); };
    return QStringLiteral("H: %1 px, W: %2 px")
        .arg(dimension(size.height), 0, 'f', 0).arg(dimension(size.width), 0, 'f', 0).toStdString();
}

std::string pointerAngleText(double degrees)
{
    if (!std::isfinite(degrees)) return {};
    // Match the transform controls' signed angle; don't infer another angle
    // from a flipped affine basis. Suppress negative zero after rounding.
    const double rounded = std::round(degrees * 10.0) / 10.0;
    // Keep the fractional digit even at whole degrees so slow rotation and
    // snapping don't repeatedly shrink/expand the readout's background.
    const auto number = QString::number(rounded == 0 ? 0 : rounded, 'f', 1);
    return QStringLiteral("Angle: %1°").arg(number).toStdString();
}

QRectF pointerTooltipRect(QPointF pointer, QSizeF content, QSizeF viewport)
{
    constexpr double margin = 6, gap = 18;
    if (!std::isfinite(pointer.x()) || !std::isfinite(pointer.y())
        || content.isEmpty() || viewport.isEmpty()) return {};
    double x = pointer.x() + 12, y = pointer.y() + gap;
    if (y + content.height() > viewport.height() - margin)
        y = pointer.y() - gap - content.height();
    x = std::clamp(x, margin, std::max(margin, viewport.width() - content.width() - margin));
    y = std::clamp(y, margin, std::max(margin, viewport.height() - content.height() - margin));
    return {QPointF(x, y), content};
}

QRectF pointerTooltipRect(QPointF pointer, QSizeF content, QRectF area)
{
    const auto local = pointerTooltipRect(pointer - area.topLeft(), content, area.size());
    return local.isEmpty() ? local : local.translated(area.topLeft());
}

bool PointerTooltipCache::update(std::string_view text, double devicePixelRatio, const QFont& font)
{
    // Bounded generic text, including Unicode. Tool callers normally provide
    // fewer than 40 characters. No unbounded string/image cache or history.
    auto label = QString::fromUtf8(text.data(), qsizetype(std::min<std::size_t>(text.size(), 4096)))
                     .left(256).simplified();
    const double scale = std::isfinite(devicePixelRatio) ? std::clamp(devicePixelRatio, 1.0, 4.0) : 1.0;
    if (text_ == label && scale_ == scale && font_ == font) return false;
    text_ = label; scale_ = scale; font_ = font;
    ++revision_;
    if (label.isEmpty()) {
        image_ = {}; logicalSize_ = {}; return true;
    }
    const QFontMetricsF metrics(font);
    label = metrics.elidedText(label, Qt::ElideRight, 620);
    logicalSize_ = {std::ceil(metrics.horizontalAdvance(label)) + 20,
        std::ceil(metrics.height()) + 12};
    QImage raster(QSize(int(std::ceil(logicalSize_.width() * scale)),
        int(std::ceil(logicalSize_.height() * scale))), QImage::Format_ARGB32_Premultiplied);
    raster.setDevicePixelRatio(scale);
    raster.fill(Qt::transparent);
    QPainter painter(&raster);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(21, 24, 30, 220));
    painter.drawRoundedRect(QRectF(QPointF(), logicalSize_), 5, 5);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(QPointF(10, 6), logicalSize_ - QSizeF(20, 12)),
        Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, label);
    painter.end();
    // Same straight sRGB texel convention as Vulkan's sampled images. The
    // tooltip shader converts to premultiplied linear output for composition.
    image_ = raster.convertToFormat(QImage::Format_RGBA8888);
    return true;
}
}
