#pragma once

#include "imageeditor/core/BrushEngine.hpp"

#include <QWidget>

#include <functional>

class QAction;
class QToolButton;

namespace imageeditor::ui {

class CompactValueControl;

class BrushOptionsPage final : public QWidget {
public:
    explicit BrushOptionsPage(QWidget* parent = nullptr);

    // The same QAction is also used by the tool rail and the E shortcut, so
    // every presentation of erase mode shares one checked state.
    void setEraserAction(QAction* action);
    void setBrushSettings(const core::BrushSettings& settings);
    [[nodiscard]] const core::BrushSettings& brushSettings() const noexcept
    {
        return brushSettings_;
    }

    std::function<void(const core::BrushSettings&)> onBrushSettingsChanged;

private:
    void publishBrushSettings(CompactValueControl* source);

    CompactValueControl* sizeControl_ {nullptr};
    CompactValueControl* opacityControl_ {nullptr};
    CompactValueControl* hardnessControl_ {nullptr};
    CompactValueControl* flowControl_ {nullptr};
    CompactValueControl* scaleControl_ {nullptr};
    CompactValueControl* spacingControl_ {nullptr};
    QToolButton* directionButton_ {nullptr};
    CompactValueControl* angleControl_ {nullptr};
    QToolButton* eraserButton_ {nullptr};
    CompactValueControl* publishingControl_ {nullptr};
    core::BrushSettings brushSettings_;
    bool updating_ {false};
};

} // namespace imageeditor::ui
