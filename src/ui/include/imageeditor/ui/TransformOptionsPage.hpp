#pragma once
#include "imageeditor/core/LayerTransform.hpp"
#include <QWidget>
#include <array>
#include <functional>
class QDoubleSpinBox;
class QToolButton;
class QAction;
namespace imageeditor::ui {
// Cached geometry controls shared by Move and the explicit Transform session.
// Both use identical numeric entry/grouping; Move omits scale/session controls.
class TransformOptionsPage final : public QWidget {
public:
    enum class Mode { Transform, Move };
    explicit TransformOptionsPage(QWidget* parent = nullptr, Mode mode = Mode::Transform);
    void setValues(const core::TransformValues& values);
    void setRelativeRotation(bool relative);
    void finishNumericInput();
    void setTransformAction(QAction* action);
    [[nodiscard]] bool aspectLocked() const;
    std::function<void(const core::TransformValues&)> onValuesChanged;
    std::function<void(bool)> onFlip;
    std::function<void(bool)> onActiveLayerOnlyChanged;
    std::function<void()> onApply, onCancel, onNumericActionFinished;

private:
    core::TransformValues values_;
    std::array<QDoubleSpinBox*, 5> controls_ { };
    QDoubleSpinBox* publishing_ { nullptr };
    QToolButton* aspectLock_ { nullptr };
    QToolButton* transform_ { nullptr };
};
}
