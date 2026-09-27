#pragma once
#include "imageeditor/core/Shape.hpp"
#include <QWidget>
#include <array>
#include <functional>

class QAbstractButton;
class QButtonGroup;
class QVBoxLayout;
namespace imageeditor::ui {
class ToolOptionsButton;
class ToolOptionsNumber;

// Created once. Geometry mode is a creation choice; changing it never converts
// existing content. All other controls edit the selected shape or future style.
class ShapeOptionsPage final : public QWidget {
public:
    explicit ShapeOptionsPage(QWidget* parent = nullptr);
    QWidget* modeWidget() const { return modes_; }
    void populateProperties(QVBoxLayout*);
    void setShape(const core::ShapeLayer&, bool editable);
    void setCreationMode(core::ShapeKind);
    void setConstructionActive(bool);
    void finishNumericInput();
    std::function<void(core::ShapeKind)> onModeChanged;
    std::function<void(const core::ShapeLayer&)> onShapeChanged;
    std::function<void(bool)> onNumericFinished;
    std::function<void(bool)> onColorRequested;
    std::function<void()> onTransformRequested;

private:
    void publish();
    void refresh();
    void refreshVertex();
    QWidget* modes_;
    QButtonGroup* modeGroup_;
    ToolOptionsButton *fill_, *stroke_, *fillColor_, *strokeColor_;
    ToolOptionsNumber *width_, *radius_, *geometryWidth_ { nullptr }, *geometryHeight_ { nullptr }, *vertex_ { nullptr }, *vertexX_ { nullptr }, *vertexY_ { nullptr };
    std::array<ToolOptionsNumber*, 7> numbers_ { };
    core::ShapeLayer shape_;
    bool updating_ { false }, editable_ { false };
};
}
