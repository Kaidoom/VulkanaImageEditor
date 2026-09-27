#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/ColorSelector.hpp"

#include <QScrollArea>
#include <QVBoxLayout>

namespace imageeditor::ui {

ColorPanel::ColorPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("ColorPanelContent"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    // Scrollbar minimum-size hints must not prevent short docks from scrolling
    // their full-size swatches. The body retains its own content minimum.
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto* body = new QWidget;
    auto* content = new QVBoxLayout(body);
    content->setContentsMargins(12, 12, 12, 12);
    selector_ = new ColorSelector(ColorSelector::Presentation::Detailed, body);
    content->addWidget(selector_, 0, Qt::AlignTop);
    content->addStretch(1);
    scroll->setWidget(body);
    layout->addWidget(scroll);
    selector_->onColorsChanged = [this](core::EditorColors colors) {
        colors_ = colors;
        if (onColorsChanged) {
            onColorsChanged(colors_);
        }
    };
}

void ColorPanel::setColors(core::EditorColors colors)
{
    colors_ = colors;
    selector_->setColors(colors_);
}

void ColorPanel::setForegroundColor(core::Rgba8 color)
{
    auto colors = colors_;
    colors.setColor(colors.active, color);
    setColors(colors);
}

} // namespace imageeditor::ui
