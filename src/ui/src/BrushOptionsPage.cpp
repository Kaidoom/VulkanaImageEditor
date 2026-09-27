#include "imageeditor/ui/BrushOptionsPage.hpp"

#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"

#include <QAction>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QToolButton>

namespace imageeditor::ui {
namespace {

CompactValueControl* valueControl(const QString& objectName,
    const QString& label, double minimum, double maximum, double value,
    const QString& suffix, int decimals, double singleStep, int width,
    QWidget* parent)
{
    auto* control = new CompactValueControl(parent);
    control->setObjectName(objectName);
    control->setRange(minimum, maximum);
    control->setValue(value);
    control->setPrefix(label + QStringLiteral(": "));
    control->setSuffix(suffix);
    control->setDecimals(decimals);
    control->setSingleStep(singleStep);
    control->setFixedWidth(width);
    control->setFixedHeight(30);
    control->setAccessibleName(label);
    return control;
}

} // namespace

BrushOptionsPage::BrushOptionsPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("BrushOptionsPage"));
    auto* layout = new QGridLayout(this);
    // ToolOptionsBar owns vertical padding. Adding it again here overflows
    // the fixed toolbar budget and clips the bottoms of the 30px controls.
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(6);
    layout->setColumnMinimumWidth(0, 28);
    layout->setColumnMinimumWidth(2, 28);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(2, 1);

    eraserButton_ = new ToolOptionsButton(QStringLiteral("BrushEraseModeButton"),
        QStringLiteral("Erase mode"), QStringLiteral("Toggle erase mode · E"),
        ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
    layout->addWidget(eraserButton_, 0, 0,
        Qt::AlignRight | Qt::AlignVCenter);

    auto* controls = new QWidget(this);
    controls->setObjectName(QStringLiteral("BrushTopControls"));
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);

    sizeControl_ = valueControl(QStringLiteral("BrushSizeControl"),
        QStringLiteral("Size"), 1.0, 1000.0, 32.0, QStringLiteral(" px"),
        1, 1.0, 132, this);
    opacityControl_ = valueControl(QStringLiteral("BrushOpacityControl"),
        QStringLiteral("Opacity"), 1.0, 100.0, 100.0, QStringLiteral("%"),
        0, 1.0, 132, this);
    hardnessControl_ = valueControl(QStringLiteral("BrushHardnessControl"),
        QStringLiteral("Hardness"), 0.0, 100.0, 80.0, QStringLiteral("%"),
        0, 1.0, 134, this);
    controlsLayout->addWidget(sizeControl_);
    controlsLayout->addWidget(opacityControl_);
    controlsLayout->addWidget(hardnessControl_);

    flowControl_ = valueControl(QStringLiteral("BrushFlowControl"),
        QStringLiteral("Flow"), 1.0, 100.0, 100.0, QStringLiteral("%"),
        0, 1.0, 116, this);
    scaleControl_ = valueControl(QStringLiteral("BrushScaleControl"),
        QStringLiteral("Scale"), 5.0, 100.0, 100.0, QStringLiteral("%"),
        0, 1.0, 116, this);
    scaleControl_->setAccessibleName(QStringLiteral("Tip vertical scale"));
    scaleControl_->setToolTip(QStringLiteral(
        "Tip vertical scale\n100% keeps the original shape; lower values flatten the tip."));
    spacingControl_ = valueControl(QStringLiteral("BrushSpacingControl"),
        QStringLiteral("Spacing"), 1.0, 200.0, 10.0, QStringLiteral("%"),
        0, 1.0, 132, this);
    spacingControl_->setToolTip(QStringLiteral("Dab spacing as a percentage of brush diameter"));
    controlsLayout->addWidget(flowControl_);
    controlsLayout->addWidget(scaleControl_);
    controlsLayout->addWidget(spacingControl_);

    directionButton_ = new ToolOptionsButton(QStringLiteral("BrushDirectionButton"),
        QStringLiteral("Direction"), QStringLiteral(
            "Follow stroke direction\nOn: rotate with the stroke plus Angle.\n"
            "Off: keep a fixed Angle."), ToolOptionsButton::Kind::Toggle, this,
        ToolOptionsButton::Presentation::Icon);
    directionButton_->setIcon(toolGlyph(ToolGlyph::FollowStrokeDirection));
    directionButton_->setChecked(brushSettings_.tip.rotationMode
        == core::BrushTipRotationMode::FollowStrokeDirection);
    directionButton_->setAccessibleName(QStringLiteral("Follow stroke direction"));

    angleControl_ = valueControl(QStringLiteral("BrushAngleControl"),
        QStringLiteral("Angle"), -180.0, 180.0, 0.0,
        QStringLiteral("°"), 0, 1.0, 116, this);
    controlsLayout->addWidget(angleControl_);
    controlsLayout->addWidget(directionButton_);
    layout->addWidget(controls, 0, 1, Qt::AlignCenter);

    connect(sizeControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(sizeControl_); });
    connect(opacityControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(opacityControl_); });
    connect(hardnessControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(hardnessControl_); });
    connect(flowControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(flowControl_); });
    connect(scaleControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(scaleControl_); });
    connect(spacingControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(spacingControl_); });
    connect(directionButton_, &QToolButton::toggled, this,
        [this] { publishBrushSettings(nullptr); });
    connect(angleControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(angleControl_); });
}

void BrushOptionsPage::setEraserAction(QAction* action)
{
    eraserButton_->setDefaultAction(action);
}

void BrushOptionsPage::setBrushSettings(const core::BrushSettings& settings)
{
    brushSettings_ = settings;
    updating_ = true;
    const QSignalBlocker sizeBlocker(sizeControl_);
    const QSignalBlocker opacityBlocker(opacityControl_);
    const QSignalBlocker hardnessBlocker(hardnessControl_);
    const QSignalBlocker flowBlocker(flowControl_);
    const QSignalBlocker scaleBlocker(scaleControl_);
    const QSignalBlocker spacingBlocker(spacingControl_);
    const QSignalBlocker rotationBlocker(directionButton_);
    const QSignalBlocker angleBlocker(angleControl_);
    const auto synchronize = [this](CompactValueControl* control,
                                 double value) {
        if (control == publishingControl_
            && control->isManualEntryActive()) {
            return;
        }
        control->setValue(value);
    };
    synchronize(sizeControl_, settings.sizePixels);
    synchronize(opacityControl_, settings.opacity * 100.0);
    synchronize(hardnessControl_, settings.hardness * 100.0);
    synchronize(flowControl_, settings.flow * 100.0);
    synchronize(scaleControl_, settings.tip.aspectRatio * 100.0);
    synchronize(spacingControl_, settings.spacingPercent);
    directionButton_->setChecked(settings.tip.rotationMode
        == core::BrushTipRotationMode::FollowStrokeDirection);
    synchronize(angleControl_, settings.tip.angleDegrees);
    updating_ = false;
}

void BrushOptionsPage::publishBrushSettings(CompactValueControl* source)
{
    if (updating_) {
        return;
    }
    brushSettings_.sizePixels = sizeControl_->value();
    brushSettings_.opacity = opacityControl_->value() / 100.0;
    brushSettings_.hardness = hardnessControl_->value() / 100.0;
    brushSettings_.flow = flowControl_->value() / 100.0;
    brushSettings_.tip.aspectRatio = scaleControl_->value() / 100.0;
    brushSettings_.spacingPercent = spacingControl_->value();
    brushSettings_.tip.rotationMode = directionButton_->isChecked()
        ? core::BrushTipRotationMode::FollowStrokeDirection
        : core::BrushTipRotationMode::Fixed;
    brushSettings_.tip.angleDegrees = angleControl_->value();
    QScopedValueRollback publishing(publishingControl_, source);
    if (onBrushSettingsChanged) {
        onBrushSettingsChanged(brushSettings_);
    }
}

} // namespace imageeditor::ui
