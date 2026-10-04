#pragma once

#include "imageeditor/core/EditorColors.hpp"

#include <QWidget>
#include <QPointer>

#include <functional>

class QColorDialog;
class QListWidget;
class QPushButton;

namespace imageeditor::ui {
class ColorSelector;

// Editor-level foreground color control. The chosen color is tool state, not
// document data; MainWindow coordinates it with every color-consuming tool.
class ColorPanel final : public QWidget {
public:
    explicit ColorPanel(QWidget* parent = nullptr);

    void setForegroundColor(core::Rgba8 color);
    void setColors(core::EditorColors colors);
    [[nodiscard]] core::EditorColors colors() const noexcept { return colors_; }
    [[nodiscard]] core::Rgba8 foregroundColor() const noexcept
    {
        return colors_.foreground();
    }

    std::function<void(core::EditorColors)> onColorsChanged;

private:
    void chooseSwatch(QListWidget* grid);
    void editCustomSwatch(int row = -1);
    void saveCustomSwatches();
    void updateDeleteButton();
    ColorSelector* selector_ {nullptr};
    QListWidget* defaults_ {nullptr};
    QListWidget* custom_ {nullptr};
    QPushButton* delete_ {nullptr};
    QPointer<QColorDialog> colorDialog_;
    core::EditorColors colors_;
};

} // namespace imageeditor::ui
