#pragma once

#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/BrushSymmetry.hpp"

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
    void setSymmetry(const core::BrushSymmetry&, bool available, bool placing);
    [[nodiscard]] const core::BrushSettings& brushSettings() const noexcept
    {
        return brushSettings_;
    }

    std::function<void(const core::BrushSettings&)> onBrushSettingsChanged;
    std::function<void(bool)> onSymmetryToggled;
    std::function<void(core::BrushSymmetryAxis)> onSymmetryAxisChanged;
    std::function<void()> onSymmetryReposition;
    std::function<void()> onSymmetryMenuOpened;
    std::function<void()> onSymmetryMenuClosed;

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
    QToolButton* symmetryButton_ {nullptr};
    QToolButton* symmetryOptionsButton_ {nullptr};
    QAction* symmetryVertical_ {nullptr};
    QAction* symmetryHorizontal_ {nullptr};
    QAction* symmetryReposition_ {nullptr};
    CompactValueControl* publishingControl_ {nullptr};
    core::BrushSettings brushSettings_;
    bool updating_ {false};
};

} // namespace imageeditor::ui
