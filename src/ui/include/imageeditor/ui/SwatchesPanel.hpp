#pragma once
#include "imageeditor/core/EditorColors.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include <QPointer>
#include <QWidget>
#include <functional>
#include <vector>

class QListWidget;
class QPushButton;
namespace imageeditor::ui {
class ColorDialog;
class SwatchesPanel final : public QWidget {
public:
    explicit SwatchesPanel(QWidget* parent = nullptr);
    void setColors(core::EditorColors colors);
    void setForegroundColor(core::Rgba8 color);
    void setGradientEnabled(bool enabled);
    [[nodiscard]] core::EditorColors colors() const noexcept { return colors_; }
    [[nodiscard]] core::Rgba8 foregroundColor() const noexcept { return colors_.foreground(); }
    std::function<void(core::EditorColors)> onColorsChanged;
    std::function<void(const std::vector<core::GradientStop>&)> onGradientSelected;
private:
    void chooseSwatch(QListWidget* grid);
    void editCustomSwatch(int row = -1);
    void saveCustomSwatches();
    void updateDeleteButton();
    void chooseGradient(int row);
    QListWidget* defaults_ {};
    QListWidget* custom_ {};
    QListWidget* gradients_ {};
    QPushButton* delete_ {};
    QPointer<ColorDialog> colorDialog_;
    core::EditorColors colors_;
};
} // namespace imageeditor::ui
