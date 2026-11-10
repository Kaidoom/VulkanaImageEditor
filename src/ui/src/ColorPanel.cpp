#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ColorPicker.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/core/LayerMask.hpp"
#include <QFrame>
#include <QSignalBlocker>
#include <cmath>
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
    scalarControls_ = new QWidget(body);
    scalarControls_->setObjectName(QStringLiteral("ScalarColorControls"));
    auto* scalarLayout = new QVBoxLayout(scalarControls_);
    scalarLayout->setContentsMargins(0, 0, 0, 0);
    auto* ramp = new QFrame(scalarControls_);
    ramp->setFixedHeight(20);
    ramp->setStyleSheet(QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 black,stop:1 white);border:1px solid palette(mid);"));
    scalarLayout->addWidget(ramp);
    scalarValue_ = new CompactValueControl(scalarControls_);
    scalarValue_->setObjectName(QStringLiteral("ScalarChannelValue"));
    scalarValue_->setDecimals(0); scalarValue_->setRange(0, 255);
    i18n::setPrefix(scalarValue_, i18n::text("channels.value.prefix"));
    i18n::setToolTip(scalarValue_, i18n::text("channels.value.hint"));
    scalarLayout->addWidget(scalarValue_);
    content->addWidget(scalarControls_);
    scalarControls_->hide();
    connect(scalarValue_, &QDoubleSpinBox::valueChanged, this, [this](double number) {
        if (!scalarMode_) return;
        const auto value = std::uint8_t(std::lround(number));
        colors_.setColor(colors_.active, {value, value, value, 255});
        selector_->setColors(colors_);
        if (onColorsChanged) onColorsChanged(colors_);
    });
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
    const QSignalBlocker guard(scalarValue_);
    scalarValue_->setValue(core::maskGray(c));
}
void ColorPanel::setScalarMode(bool enabled)
{
    if (scalarMode_ == enabled) return;
    scalarMode_ = enabled;
    picker_->setVisible(!enabled);
    scalarControls_->setVisible(enabled);
    selector_->setVisible(!enabled);
    setColors(colors_);
}
void ColorPanel::setForegroundColor(core::Rgba8 color)
{
    auto colors = colors_; colors.setColor(colors.active, color); setColors(colors);
}
} // namespace imageeditor::ui
