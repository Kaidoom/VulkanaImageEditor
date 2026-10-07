#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/ColorPicker.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include <QScrollArea>
#include <QVBoxLayout>

namespace imageeditor::ui {
ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("ColorPanelContent");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto* body = new QWidget;
    auto* content = new QVBoxLayout(body);
    content->setContentsMargins(10, 10, 10, 10); content->setSpacing(8);
    selector_ = new ColorSelector(ColorSelector::Presentation::Detailed, body);
    picker_ = new ColorPicker(body);
    picker_->setManualInputsVisible(false);
    content->addWidget(selector_);
    content->addWidget(picker_, 1);
    scroll->setWidget(body); layout->addWidget(scroll);
    selector_->onColorsChanged = [this](core::EditorColors colors) {
        setColors(colors);
        if (onColorsChanged) onColorsChanged(colors_);
    };
    picker_->onColorChanged = [this](QColor color) {
        const core::Rgba8 rgba {std::uint8_t(color.red()), std::uint8_t(color.green()),
            std::uint8_t(color.blue()), std::uint8_t(color.alpha())};
        if (rgba == colors_.foreground()) return;
        colors_.setColor(colors_.active, rgba);
        selector_->setColors(colors_);
        if (onColorsChanged) onColorsChanged(colors_);
    };
    setColors(colors_);
}
void ColorPanel::setColors(core::EditorColors colors)
{
    colors_ = colors;
    selector_->setColors(colors_);
    const auto c = colors_.foreground();
    picker_->setColor(QColor(c.red, c.green, c.blue, c.alpha));
}
void ColorPanel::setForegroundColor(core::Rgba8 color)
{
    auto colors = colors_; colors.setColor(colors.active, color); setColors(colors);
}
} // namespace imageeditor::ui
