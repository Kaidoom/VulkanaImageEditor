#pragma once

#include "imageeditor/core/EditorColors.hpp"

#include <QWidget>

#include <functional>

namespace imageeditor::ui {
class ColorSelector;
class ColorPicker;
class CompactValueControl;

// Editor-level foreground color control. The chosen color is tool state, not
// document data; MainWindow coordinates it with every color-consuming tool.
class ColorPanel final : public QWidget {
public:
    explicit ColorPanel(QWidget* parent = nullptr);

    void setForegroundColor(core::Rgba8 color);
    void setColors(core::EditorColors colors);
    void setScalarMode(bool enabled);
    [[nodiscard]] bool scalarMode() const noexcept { return scalarMode_; }
    [[nodiscard]] core::EditorColors colors() const noexcept { return colors_; }
    [[nodiscard]] core::Rgba8 foregroundColor() const noexcept
    {
        return colors_.foreground();
    }

    std::function<void(core::EditorColors)> onColorsChanged;

private:
    ColorSelector* selector_ {nullptr};
    ColorPicker* picker_ {nullptr};
    QWidget* scalarControls_ {nullptr};
    CompactValueControl* scalarValue_ {nullptr};
    bool scalarMode_ {false};
    core::EditorColors colors_;
};

} // namespace imageeditor::ui
