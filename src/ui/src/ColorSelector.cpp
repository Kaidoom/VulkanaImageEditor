#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include <QHBoxLayout>
#include <QHideEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWindow>

namespace imageeditor::ui {
namespace {
const QPoint primaryHome {3, 3};
const QPoint secondaryHome {13, 13};
const QPoint primarySpread {0, 0};
const QPoint secondarySpread {16, 16};

QString colorHex(core::Rgba8 color)
{
    auto hex = QStringLiteral("#%1%2%3")
        .arg(color.red, 2, 16, QLatin1Char('0'))
        .arg(color.green, 2, 16, QLatin1Char('0'))
        .arg(color.blue, 2, 16, QLatin1Char('0'));
    if (color.alpha != 255) {
        hex += QStringLiteral("%1").arg(color.alpha, 2, 16, QLatin1Char('0'));
    }
    return hex.toUpper();
}

class SwatchButton final : public QPushButton {
public:
    SwatchButton(bool compact, QWidget* parent) : QPushButton(parent), compact_(compact)
    {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumWidth(0);
    }
    core::Rgba8 color;
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text.clear();
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        const auto area = rect().adjusted(4, 4, -4, compact_ ? -4 : -22);
        painter.save();
        painter.setClipRect(area);
        for (int y = area.top(); y <= area.bottom(); y += 5) {
            for (int x = area.left(); x <= area.right(); x += 5) {
                const bool dark = (((x - area.left()) / 5 + (y - area.top()) / 5) & 1) != 0;
                painter.fillRect(x, y, 5, 5, dark ? QColor(94, 98, 111) : QColor(160, 164, 175));
            }
        }
        painter.fillRect(area, QColor(color.red, color.green, color.blue, color.alpha));
        painter.restore();
        if (!compact_) {
            painter.setPen(palette().color(QPalette::ButtonText));
            painter.drawText(QRect(3, height() - 21, width() - 6, 18), Qt::AlignCenter, text());
        }
    }
private:
    bool compact_;
};
} // namespace

ColorSelector::ColorSelector(Presentation presentation, QWidget* parent)
    : QWidget(parent), presentation_(presentation)
{
    const bool compact = presentation == Presentation::Compact;
    setObjectName(compact ? QStringLiteral("ToolRailColors") : QStringLiteral("ColorPanelColors"));
    primary_ = new SwatchButton(compact, this);
    secondary_ = new SwatchButton(compact, this);
    primary_->setObjectName(compact ? QStringLiteral("RailPrimaryColor") : QStringLiteral("ForegroundColorButton"));
    secondary_->setObjectName(compact ? QStringLiteral("RailSecondaryColor") : QStringLiteral("SecondaryColorButton"));
    swap_ = new QToolButton(this);
    swap_->setObjectName(compact ? QStringLiteral("RailSwapColors") : QStringLiteral("PanelSwapColors"));
    swap_->setText(QStringLiteral("⇄"));
    swap_->setToolTip(QStringLiteral("Switch active color (X)"));
    swap_->setAccessibleName(QStringLiteral("Switch active color"));
    if (compact) {
        setFixedSize(40, 40);
        primary_->setGeometry(QRect(primaryHome, QSize(24, 24)));
        secondary_->setGeometry(QRect(secondaryHome, QSize(24, 24)));
        swap_->setGeometry(28, 0, 12, 13);
        swap_->setStyleSheet(QStringLiteral("padding: 0; border: none; font-size: 8pt;"));
        primary_->setStyleSheet(QStringLiteral("padding: 0; min-height: 0; border-radius: 3px;"));
        secondary_->setStyleSheet(primary_->styleSheet());
        selectionAnimation_ = new QVariantAnimation(this);
        selectionAnimation_->setDuration(150);
        selectionAnimation_->setStartValue(0.0);
        selectionAnimation_->setEndValue(1.0);
        connect(selectionAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                const auto t = value.toReal();
                const bool returning = t >= 0.5;
                const auto blend = QEasingCurve(QEasingCurve::InOutCubic)
                    .valueForProgress(returning ? 2.0 * t - 1.0 : 2.0 * t);
                const auto interpolate = [blend](QPoint from, QPoint to) {
                    return from + (to - from) * blend;
                };
                primary_->move(interpolate(returning ? primarySpread : animationPrimaryStart_,
                    returning ? primaryHome : primarySpread));
                secondary_->move(interpolate(returning ? secondarySpread : animationSecondaryStart_,
                    returning ? secondaryHome : secondarySpread));
                if (returning) {
                    displayedActive_ = colors_.active;
                    paintSelection();
                }
            });
        connect(selectionAnimation_, &QVariantAnimation::finished, this,
            [this] { settleSelection(); });
    } else {
        // A top-aligned selector must advertise the entire swatch+hex row,
        // not QPushButton's smaller text-only size hint. Small docks scroll.
        setFixedHeight(46);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        primary_->setFixedHeight(46);
        secondary_->setFixedHeight(46);
        primary_->setStyleSheet(QStringLiteral("padding: 0; min-width: 0;"));
        secondary_->setStyleSheet(primary_->styleSheet());
        swap_->setFixedSize(22, 30);
        swap_->setStyleSheet(QStringLiteral("padding: 0;"));
        layout->addWidget(primary_, 1);
        layout->addWidget(swap_);
        layout->addWidget(secondary_, 1);
    }
    connect(primary_, &QPushButton::clicked, this, [this] { activateOrEdit(core::ColorSlot::Primary); });
    connect(secondary_, &QPushButton::clicked, this, [this] { activateOrEdit(core::ColorSlot::Secondary); });
    connect(swap_, &QToolButton::clicked, this, [this] { colors_.switchActive(); publish(); });
    refresh();
}

void ColorSelector::setColors(core::EditorColors colors)
{
    colors_ = colors;
    refresh();
}

void ColorSelector::activateOrEdit(core::ColorSlot slot)
{
    if (colors_.active != slot) {
        colors_.active = slot;
        publish();
        return;
    }
    refresh();
    if (colorDialog_) {
        colorDialog_->raise();
        colorDialog_->setFocus(Qt::OtherFocusReason);
        return;
    }
    const auto old = colors_.color(slot);
    // The nearest QWidget window is the native panel-overlay subsurface.
    // Dialog ownership must reach the real editor top-level above that shell.
    QWidget* owner = this;
    while (owner->parentWidget()) {
        owner = owner->parentWidget();
    }
    auto* dialog = new ColorDialog(QColor(old.red, old.green, old.blue, old.alpha), owner);
    colorDialog_ = dialog;
    dialog->setObjectName(QStringLiteral("WorkingColorDialog"));
    dialog->setWindowTitle(QStringLiteral("Working color"));
    dialog->setOptions(ColorDialog::ShowAlphaChannel | ColorDialog::DontUseNativeDialog);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &ColorDialog::colorSelected, this, [this, slot](const QColor& picked) {
        colors_.setColor(slot, {static_cast<std::uint8_t>(picked.red()),
            static_cast<std::uint8_t>(picked.green()), static_cast<std::uint8_t>(picked.blue()),
            static_cast<std::uint8_t>(picked.alpha())});
        publish();
    });
    // Asynchronous modality blocks the editor without nesting its event loop.
    dialog->show();
}

void ColorSelector::hideEvent(QHideEvent* event)
{
    settleSelection();
    QWidget::hideEvent(event);
}

void ColorSelector::paintSelection()
{
    primary_->setChecked(displayedActive_ == core::ColorSlot::Primary);
    secondary_->setChecked(displayedActive_ == core::ColorSlot::Secondary);
    (displayedActive_ == core::ColorSlot::Primary ? primary_ : secondary_)->raise();
    swap_->raise();
}

void ColorSelector::settleSelection()
{
    if (selectionAnimation_) {
        selectionAnimation_->stop();
        primary_->move(primaryHome);
        secondary_->move(secondaryHome);
    }
    animationTarget_ = displayedActive_ = colors_.active;
    paintSelection();
}

void ColorSelector::updateSelection()
{
    if (presentation_ != Presentation::Compact || !isVisible()) {
        settleSelection();
    } else if (animationTarget_ != colors_.active) {
        // One interruptible animation: rapid switches never accumulate a queue
        // and the synchronous MainWindow feedback never restarts the motion.
        selectionAnimation_->stop();
        animationPrimaryStart_ = primary_->pos();
        animationSecondaryStart_ = secondary_->pos();
        animationTarget_ = colors_.active;
        selectionAnimation_->start();
    }
    paintSelection();
}

void ColorSelector::refresh()
{
    const auto update = [this](QPushButton* button, core::ColorSlot slot, const QString& name) {
        const auto color = colors_.color(slot);
        static_cast<SwatchButton*>(button)->color = color;
        const auto hex = colorHex(color);
        button->setText(hex);
        button->setAccessibleName(name + QStringLiteral(" color ") + hex);
        button->setToolTip(name + QStringLiteral(" · ") + hex
            + QStringLiteral("\nClick to activate; click the active swatch to edit.\nRGBA includes sampled alpha."));
        button->update();
    };
    update(primary_, core::ColorSlot::Primary, QStringLiteral("Primary"));
    update(secondary_, core::ColorSlot::Secondary, QStringLiteral("Secondary"));
    updateSelection();
}

void ColorSelector::publish()
{
    refresh();
    if (onColorsChanged) {
        onColorsChanged(colors_);
    }
}
} // namespace imageeditor::ui
