#pragma once

#include "imageeditor/core/Geometry.hpp"
#include <QFont>
#include <QImage>
#include <QRectF>
#include <QString>
#include <cstdint>
#include <string>
#include <string_view>

namespace imageeditor::render {

// Tool-independent, single-line readout. Dimensions are document pixels;
// placement, padding and font size are logical viewport pixels, never zoomed.
[[nodiscard]] std::string pointerSizeText(core::Extent2d size);
[[nodiscard]] std::string pointerAngleText(double degrees);
[[nodiscard]] QRectF pointerTooltipRect(QPointF pointer, QSizeF content, QSizeF viewport);
[[nodiscard]] QRectF pointerTooltipRect(QPointF pointer, QSizeF content, QRectF availableArea);

// Disposable text/background raster, not a document surface. Position changes
// do not touch this cache. Vulkan uploads a changed raster once per frame slot.
class PointerTooltipCache final {
public:
    bool update(std::string_view text, double devicePixelRatio, const QFont& font);
    [[nodiscard]] const QImage& image() const noexcept { return image_; }
    [[nodiscard]] QSizeF logicalSize() const noexcept { return logicalSize_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
private:
    QString text_;
    QFont font_;
    double scale_ {0};
    QImage image_;
    QSizeF logicalSize_;
    std::uint64_t revision_ {0};
};
}
