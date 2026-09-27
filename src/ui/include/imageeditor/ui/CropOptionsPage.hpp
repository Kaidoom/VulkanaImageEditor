#pragma once
#include "imageeditor/core/Geometry.hpp"
#include <QWidget>
#include <array>
#include <functional>
class QToolButton;
namespace imageeditor::ui {
class ToolOptionsNumber;
class CropOptionsPage final : public QWidget {
public:
    explicit CropOptionsPage(QWidget* parent = nullptr);
    void setFrame(core::RectD);
    void finishNumericInput();
    bool aspectLocked() const;
    bool showSource() const;
    bool chamferEnabled() const;
    void setChamferEnabled(bool);
    std::function<void(core::RectD)> onChanged;
    std::function<void()> onActionFinished, onApply, onCancel, onRemove,
        onPreviewChanged, onChamferChanged;

private:
    core::RectD frame_;
    std::array<ToolOptionsNumber*, 4> numbers_ { };
    QToolButton* lock_ { };
    QToolButton* preview_ { };
    QToolButton* chamfer_ { };
    ToolOptionsNumber* publishing_ { };
};
} // namespace imageeditor::ui
