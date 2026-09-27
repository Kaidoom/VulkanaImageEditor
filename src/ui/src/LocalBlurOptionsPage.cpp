#include "imageeditor/ui/LocalBlurOptionsPage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>

namespace imageeditor::ui {
namespace {
CompactValueControl* control(QWidget* parent, const char* name, const QString& label,
    double minimum, double maximum, int decimals, const QString& suffix)
{
    auto* field = new CompactValueControl(parent);
    field->setObjectName(QString::fromLatin1(name));
    field->setAccessibleName(label);
    field->setPrefix(label + QStringLiteral(": "));
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
    size_ = control(this, "LocalBlurSize", QStringLiteral("Size"), 1, 1000, 1, QStringLiteral(" px"));
    hardness_ = control(this, "LocalBlurHardness", QStringLiteral("Hardness"), 0, 100, 0, QStringLiteral("%"));
    strength_ = control(this, "LocalBlurStrength", QStringLiteral("Strength"), 0, 100, 0, QStringLiteral("%"));
    radius_ = control(this, "LocalBlurRadius", QStringLiteral("Blur Radius"), 0, 64, 1, QStringLiteral(" px"));
    size_->setToolTip(QStringLiteral("Brush footprint diameter in document pixels"));
    hardness_->setToolTip(QStringLiteral("Brush edge coverage; independent of blur radius"));
    strength_->setToolTip(QStringLiteral("Maximum color-softening influence per stroke; Flow controls buildup. Alpha is preserved."));
    radius_->setToolTip(QStringLiteral("Gaussian neighborhood radius in document pixels, independent of layer scale. Radius = 3σ; zero leaves pixels unchanged."));
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
