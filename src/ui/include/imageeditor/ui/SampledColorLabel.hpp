#pragma once

#include "imageeditor/core/Geometry.hpp"
#include <QLabel>
#include <QPainter>
#include <optional>

namespace imageeditor::ui {
// Shared sampled-reference display. The owning options page retains control
// of geometry, while this label draws the existing checker-backed swatch.
class SampledColorLabel final : public QLabel {
public:
    using QLabel::QLabel;
    void setSample(std::optional<core::Rgba8> value)
    {
        if (sample_ == value && !text().isEmpty())
            return;
        sample_ = value;
        setText(value ? QColor(value->red, value->green, value->blue).name().toUpper()
                    + QStringLiteral(" · A: %1").arg(value->alpha)
                      : QStringLiteral("Click a color"));
        setContentsMargins(32, 0, 0, 0);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QLabel::paintEvent(event);
        QPainter p(this);
        const QRect box(1, 4, 24, height() - 8);
        p.setClipRect(box);
        for (int y = box.top(); y <= box.bottom(); y += 6)
            for (int x = box.left(); x <= box.right(); x += 6)
                p.fillRect(QRect(x, y, 6, 6),
                    palette().color(((x / 6 + y / 6) % 2) ? QPalette::Midlight : QPalette::Base));
        if (sample_)
            p.fillRect(box, QColor(sample_->red, sample_->green, sample_->blue, sample_->alpha));
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(box.adjusted(0, 0, -1, -1));
    }

private:
    std::optional<core::Rgba8> sample_;
};
}
