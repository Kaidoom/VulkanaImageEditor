#include "imageeditor/ui/AbsolutePositionSlider.hpp"

#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace imageeditor::ui {

void AbsolutePositionSlider::mousePressEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const auto handle = style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        if (!handle.contains(event->position().toPoint())) {
            const auto groove = style()->subControlRect(
                QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
            const bool horizontal = orientation() == Qt::Horizontal;
            const int handleLength = horizontal ? handle.width() : handle.height();
            const int grooveStart = horizontal ? groove.x() : groove.y();
            const int grooveLength = horizontal ? groove.width() : groove.height();
            const int available = std::max(0, grooveLength - handleLength);
            const double pointerPosition = horizontal
                ? event->position().x() : event->position().y();
            const int centeredPosition = static_cast<int>(std::lround(pointerPosition))
                - grooveStart - handleLength / 2;
            setValue(QStyle::sliderValueFromPosition(minimum(), maximum(),
                centeredPosition, available, option.upsideDown));
        }
    }

    // After the direct set, the handle is underneath the original event. Qt's
    // native slider implementation can therefore own the press, drag, release,
    // keyboard, tracking, and accessibility semantics as usual.
    QSlider::mousePressEvent(event);
}

void AbsolutePositionSlider::wheelEvent(QWheelEvent* event)
{
    // Ignoring the wheel event lets Qt propagate it to the surrounding
    // Properties-page scroll area. Deliberate pointer drags and keyboard
    // adjustments keep the normal QSlider behavior.
    if (event) {
        event->ignore();
    }
}

} // namespace imageeditor::ui
