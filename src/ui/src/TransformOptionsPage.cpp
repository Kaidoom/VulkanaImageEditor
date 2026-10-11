#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QAction>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QToolButton>
#include <cmath>

namespace imageeditor::ui {
void TransformOptionsPage::setRelativeRotation(bool relative)
{
    auto* angle=controls_[4];
    if(!angle) return;
    const auto prefix=relative?i18n::text("transform.options.page.rotate.by"):QStringLiteral("Angle: ");
    if(angle->prefix()==prefix) return;
    const QSignalBlocker guard(angle);
    angle->setPrefix(prefix);
    const auto hint=relative?i18n::text("transform.options.page.rotate.the.selected.set.clockwise.about.its.shared.center")
                            :i18n::text("transform.options.page.clockwise.rotation.in.document.space");
    angle->setToolTip(hint);
    angle->setAccessibleName(hint);
}
TransformOptionsPage::TransformOptionsPage(QWidget* parent, Mode mode)
    : QWidget(parent)
{
    const auto prefix = mode == Mode::Move ? QStringLiteral("Move") : QStringLiteral("Transform");
    setObjectName(prefix + QStringLiteral("OptionsPage"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    if (mode == Mode::Move) {
        row->addWidget(i18n::make<QLabel>(i18n::text("transform.options.page.selection.mode"), this));
        auto* selectionModes = new QButtonGroup(this);
        auto* activeOnly = new ToolOptionsButton(QStringLiteral("MoveActiveLayerOnly"),
            i18n::text("transform.options.page.active.layer"),
            i18n::text("transform.options.page.active.layer.move.the.selected.set.without.picking.another"),
            ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
        activeOnly->setIcon(toolGlyph(ToolGlyph::ActiveLayer));
        auto* underMouse = new ToolOptionsButton(QStringLiteral("MoveSelectUnderMouse"),
            i18n::text("transform.options.page.under.mouse"),
            i18n::text("transform.options.page.under.mouse.click.content.to.select.shift.click.toggles"),
            ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
        underMouse->setIcon(toolGlyph(ToolGlyph::UnderMouse));
        selectionModes->addButton(activeOnly);
        selectionModes->addButton(underMouse);
        underMouse->setChecked(true);
        row->addWidget(activeOnly);
        row->addWidget(underMouse);
        connect(activeOnly, &QToolButton::toggled, this, [this](bool checked) {
            if (onActiveLayerOnlyChanged) onActiveLayerOnlyChanged(checked);
        });
    }
    const std::array names { "X", "Y", "ScaleX", "ScaleY", "Angle" };
    const std::array labels { "X", "Y", "W", "H", "Angle" };
    const std::array hints { "Target center X in document pixels",
        "Target center Y in document pixels",
        "Horizontal scale relative to original pixels; negative values flip",
        "Vertical scale relative to original pixels; negative values flip",
        "Clockwise rotation in document space" };
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        if (mode == Mode::Move && (i == 2 || i == 3))
            continue;
        auto* control = new ToolOptionsNumber(this);
        controls_[i] = control;
        control->onInteractionFinished = [this] {
            if (onNumericActionFinished)
                onNumericActionFinished();
        };
        control->setObjectName(
            QStringLiteral("%1%2Control").arg(prefix, QString::fromLatin1(names[i])));
        control->setAccessibleName(QString::fromLatin1(hints[i]));
        control->setToolTip(QString::fromLatin1(hints[i]));
        control->setPrefix(QString::fromLatin1(labels[i]) + QStringLiteral(": "));
        control->setSuffix(i < 2 ? QStringLiteral(" px")
                : i < 4          ? QStringLiteral("%")
                                 : QStringLiteral("°"));
        control->setDecimals(i < 2 ? 2 : i < 4 ? 4 : 2);
        const double limit = i < 2 ? 1.0e9 : i < 4 ? core::kMaximumLayerScale * 100 : 36000;
        control->setRange(-limit, limit);
        control->setFixedWidth(i < 2 ? 134 : 138);
        row->addWidget(control);
        connect(control, &QDoubleSpinBox::valueChanged, this, [this, control, i](double value) {
            auto v = values_;
            if (i == 0)
                v.center.x = value;
            else if (i == 1)
                v.center.y = value;
            else if (i == 4)
                v.rotationDegrees = value;
            else {
                auto& changed = i == 2 ? v.scaleX : v.scaleY;
                auto& other = i == 2 ? v.scaleY : v.scaleX;
                if (aspectLocked() && std::abs(changed) > 0.0 && std::abs(other) > 0.0) {
                    auto factor = value / (changed * 100.0);
                    const auto minimum = std::max(core::kMinimumLayerScale / std::abs(changed),
                        core::kMinimumLayerScale / std::abs(other));
                    const auto maximum = std::min(core::kMaximumLayerScale / std::abs(changed),
                        core::kMaximumLayerScale / std::abs(other));
                    factor = std::copysign(std::clamp(std::abs(factor), minimum, maximum),
                        factor == 0.0 ? 1.0 : factor);
                    changed *= factor;
                    other *= factor;
                } else {
                    changed = value / 100.0;
                }
            }
            {
                const QScopedValueRollback<QDoubleSpinBox*> guard(publishing_, control);
                if (onValuesChanged)
                    onValuesChanged(v);
            }
            // Keyboard tracking is off: this is a completed numeric edit or
            // step, not an intermediate decimal. Show the canonical angle or
            // clamped signed minimum now, including on the publishing field.
            setValues(values_);
            if (!control->interactionActive() && onNumericActionFinished)
                onNumericActionFinished();
        });
    }
    const auto button
        = [this, row](const QString& name, const i18n::Message& label, const i18n::Message& hint,
              ToolOptionsButton::Presentation presentation = ToolOptionsButton::Presentation::Text) {
              auto* b = new ToolOptionsButton(name, label, hint,
                  ToolOptionsButton::Kind::Action, this, presentation);
              row->addWidget(b);
              return b;
          };
    if (mode == Mode::Transform) {
        aspectLock_ = button(QStringLiteral("TransformAspectLock"), i18n::text("transform.options.page.lock.ratio"),
            i18n::text("transform.options.page.lock.ratio.keep.aspect.ratio.shift.temporarily.toggles.while"),
            ToolOptionsButton::Presentation::Icon);
        aspectLock_->setCheckable(true);
        aspectLock_->setChecked(true);
        aspectLock_->setIcon(toolGlyph(ToolGlyph::AspectLock));
    }
    auto* flipH = button(prefix + QStringLiteral("FlipHorizontal"), i18n::text("transform.options.page.flip.horizontally"),
        i18n::text("transform.options.page.flip.horizontally.in.target.local.space"), ToolOptionsButton::Presentation::Icon);
    flipH->setIcon(toolGlyph(ToolGlyph::FlipHorizontal));
    auto* flipV = button(prefix + QStringLiteral("FlipVertical"), i18n::text("transform.options.page.flip.vertically"),
        i18n::text("transform.options.page.flip.vertically.in.target.local.space"), ToolOptionsButton::Presentation::Icon);
    flipV->setIcon(toolGlyph(ToolGlyph::FlipVertical));
    connect(flipH, &QToolButton::clicked, this, [this] {
        finishNumericInput();
        if (onFlip)
            onFlip(true);
    });
    connect(flipV, &QToolButton::clicked, this, [this] {
        finishNumericInput();
        if (onFlip)
            onFlip(false);
    });
    if (mode == Mode::Move) {
        transform_ = new ToolOptionsButton(QStringLiteral("MoveTransform"), i18n::text("transform.options.page.transform"), i18n::text("transform.options.page.transform.layer.layertransformaction"), ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
        row->addWidget(transform_);
        row->addStretch();
        return;
    }
    auto* apply = button(QStringLiteral("TransformApply"), QStringLiteral("Apply"),
        i18n::text("transform.options.page.apply.transform.finishoperationaction"));
    apply->setProperty("toolOptionsPrimary", true);
    auto* cancel = button(QStringLiteral("TransformCancel"), QStringLiteral("Cancel"),
        i18n::text("transform.options.page.cancel.transform.escape"));
    connect(apply, &QToolButton::clicked, this, [this] {
        finishNumericInput();
        if (onApply)
            onApply();
    });
    connect(cancel, &QToolButton::clicked, this, [this] {
        if (onCancel)
            onCancel();
    });
    transform_ = new ToolOptionsButton(QStringLiteral("TransformModeActive"), i18n::text("transform.options.page.transform"), i18n::text("transform.options.page.transform.active.finishoperationaction.applies.escape.cancels"), ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
    row->addWidget(transform_);
    row->addStretch();
}
void TransformOptionsPage::setTransformAction(QAction* action)
{
    transform_->setDefaultAction(action);
    transform_->setToolButtonStyle(Qt::ToolButtonIconOnly);
}
bool TransformOptionsPage::aspectLocked() const
{
    return aspectLock_ && aspectLock_->isChecked();
}
void TransformOptionsPage::finishNumericInput()
{
    // Qt's embedded native canvas does not always move QWidget focus on
    // press. Flush before the canvas snapshots its gesture anchor.
    for (auto* control : controls_) {
        if (!control)
            continue;
        control->interpretText();
        if (control->hasFocus())
            control->clearFocus();
        static_cast<ToolOptionsNumber*>(control)->finishInteraction();
    }
}
void TransformOptionsPage::setValues(const core::TransformValues& values)
{
    values_ = values;
    const std::array numbers { values.center.x, values.center.y, values.scaleX * 100,
        values.scaleY * 100, values.rotationDegrees };
    for (std::size_t i = 0; i < controls_.size(); ++i) {
        if (!controls_[i] || controls_[i] == publishing_)
            continue;
        const QSignalBlocker blocker(controls_[i]);
        if(i==2||i==3) {
            const bool projected=!values.distortion.isAffine();
            i18n::setPrefix(controls_[i], projected?(i==2?i18n::text("transform.options.page.scale.x"):i18n::text("transform.options.page.scale.y")):(i==2?i18n::text("transform.options.page.w"):i18n::text("transform.options.page.h")));
            i18n::setToolTip(controls_[i], projected?i18n::text("transform.options.page.local.scale.at.the.quad.center.edits.preserve.the")
                :i18n::text("transform.options.page.scale.relative.to.original.pixels.negative.values.flip"));
        }
        controls_[i]->setValue(numbers[i]);
    }
}
}
