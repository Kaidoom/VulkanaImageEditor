#pragma once

#include <QSlider>

class QWheelEvent;

namespace imageeditor::ui {

// A normal QSlider with one deliberate desktop-editor affordance: pressing the
// groove moves the handle directly under the pointer and the same press then
// continues through QSlider's standard drag lifecycle. Wheel input is ignored
// so a slider inside a scrollable panel never steals panel scrolling.
class AbsolutePositionSlider final : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
};

} // namespace imageeditor::ui
