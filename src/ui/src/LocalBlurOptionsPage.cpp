#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/LocalBlurOptionsPage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>

namespace imageeditor::ui {
namespace {
CompactValueControl* control(QWidget* parent, const char* name, const i18n::Message& label,
    double minimum, double maximum, int decimals, const QString& suffix)
{
    auto* field = new CompactValueControl(parent);
    field->setObjectName(QString::fromLatin1(name));
    i18n::setAccessibleName(field, label);
    i18n::setPrefix(field, label + QStringLiteral(": "));
    field->setRange(minimum, maximum);
    field->setDecimals(decimals);
    field->setSuffix(suffix);
    field->setSingleStep(1);
    field->setFixedSize(146, 30);
    return field;
}
}
LocalBlurOptionsPage::LocalBlurOptionsPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("LocalBlurOptionsPage"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    size_ = control(this, "LocalBlurSize", i18n::text("local.blur.options.page.size"), 1, 1000, 1, QStringLiteral(" px"));
    hardness_ = control(this, "LocalBlurHardness", i18n::text("local.blur.options.page.hardness"), 0, 100, 0, QStringLiteral("%"));
    strength_ = control(this, "LocalBlurStrength", i18n::text("local.blur.options.page.strength"), 0, 100, 0, QStringLiteral("%"));
    radius_ = control(this, "LocalBlurRadius", i18n::text("local.blur.options.page.blur.radius"), 0, 64, 1, QStringLiteral(" px"));
    i18n::setToolTip(size_, i18n::text("local.blur.options.page.brush.footprint.diameter.in.document.pixels"));
    i18n::setToolTip(hardness_, i18n::text("local.blur.options.page.brush.edge.coverage.independent.of.blur.radius"));
    i18n::setToolTip(strength_, i18n::text("local.blur.options.page.maximum.color.softening.influence.per.stroke.flow.controls.buildup"));
    i18n::setToolTip(radius_, i18n::text("local.blur.options.page.gaussian.neighborhood.radius.in.document.pixels.independent.of.layer"));
    for (auto* field : {size_, hardness_, strength_, radius_}) {
        row->addWidget(field);
        connect(field, &QDoubleSpinBox::valueChanged, this, [this, field](double value) {
            if (updating_) return;
            const QScopedValueRollback publishing(publishing_, field);
            if (field == size_ || field == hardness_) {
                if (field == size_) brush_.sizePixels = value;
                else brush_.hardness = value / 100;
                if (onBrushSettingsChanged) onBrushSettingsChanged(brush_);
            } else {
                if (field == strength_) settings_.strength = value / 100;
                else settings_.radius = value;
                if (onBlurSettingsChanged) onBlurSettingsChanged(settings_);
            }
        });
    }
    row->addStretch();
    setBrushSettings(brush_);
    setBlurSettings(settings_);
}
void LocalBlurOptionsPage::setBrushSettings(const core::BrushSettings& value)
{
    brush_ = value;
    const QScopedValueRollback guard(updating_, true);
    const QSignalBlocker a(size_), b(hardness_);
    if (publishing_ != size_ || !size_->isManualEntryActive()) size_->setValue(value.sizePixels);
    if (publishing_ != hardness_ || !hardness_->isManualEntryActive()) hardness_->setValue(value.hardness * 100);
}
void LocalBlurOptionsPage::setBlurSettings(const core::BlurSettings& value)
{
    settings_ = value;
    const QScopedValueRollback guard(updating_, true);
    const QSignalBlocker a(strength_), b(radius_);
    if (publishing_ != strength_ || !strength_->isManualEntryActive()) strength_->setValue(value.strength * 100);
    if (publishing_ != radius_ || !radius_->isManualEntryActive()) radius_->setValue(value.radius);
}
}
