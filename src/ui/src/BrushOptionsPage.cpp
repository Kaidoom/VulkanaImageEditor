#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/BrushOptionsPage.hpp"

#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"

#include <QAction>
#include <QActionGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QToolButton>

namespace imageeditor::ui {
namespace {

CompactValueControl* valueControl(const QString& objectName,
    const i18n::Message& label, double minimum, double maximum, double value,
    const QString& suffix, int decimals, double singleStep, int width,
    QWidget* parent)
{
    auto* control = new CompactValueControl(parent);
    control->setObjectName(objectName);
    control->setRange(minimum, maximum);
    control->setValue(value);
    i18n::setPrefix(control, label + QStringLiteral(": "));
    control->setSuffix(suffix);
    control->setDecimals(decimals);
    control->setSingleStep(singleStep);
    control->setFixedWidth(width);
    control->setFixedHeight(30);
    i18n::setAccessibleName(control, label);
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
        i18n::text("brush.options.page.erase.mode"), i18n::text("brush.options.page.toggle.erase.mode.e"),
        ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
    layout->addWidget(eraserButton_, 0, 0,
        Qt::AlignRight | Qt::AlignVCenter);

    auto* controls = new QWidget(this);
    controls->setObjectName(QStringLiteral("BrushTopControls"));
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);

    sizeControl_ = valueControl(QStringLiteral("BrushSizeControl"), i18n::text("brush.options.page.size"), 1.0, 1000.0, 32.0, QStringLiteral(" px"), 1, 1.0, 132, this);
    opacityControl_ = valueControl(QStringLiteral("BrushOpacityControl"), i18n::text("brush.options.page.opacity"), 1.0, 100.0, 100.0, QStringLiteral("%"), 0, 1.0, 132, this);
    hardnessControl_ = valueControl(QStringLiteral("BrushHardnessControl"), i18n::text("brush.options.page.hardness"), 0.0, 100.0, 80.0, QStringLiteral("%"), 0, 1.0, 134, this);
    controlsLayout->addWidget(sizeControl_);
    controlsLayout->addWidget(opacityControl_);
    controlsLayout->addWidget(hardnessControl_);

    flowControl_ = valueControl(QStringLiteral("BrushFlowControl"), i18n::text("brush.options.page.flow"), 1.0, 100.0, 100.0, QStringLiteral("%"), 0, 1.0, 116, this);
    scaleControl_ = valueControl(QStringLiteral("BrushScaleControl"), i18n::text("brush.options.page.scale"), 5.0, 100.0, 100.0, QStringLiteral("%"), 0, 1.0, 116, this);
    i18n::setAccessibleName(scaleControl_, i18n::text("brush.options.page.tip.vertical.scale"));
    i18n::setToolTip(scaleControl_, i18n::text("brush.options.page.tip.vertical.scale.100.keeps.the.original.shape.lower"));
    spacingControl_ = valueControl(QStringLiteral("BrushSpacingControl"), i18n::text("brush.options.page.spacing"), 1.0, 200.0, 10.0, QStringLiteral("%"), 0, 1.0, 132, this);
    i18n::setToolTip(spacingControl_, i18n::text("brush.options.page.dab.spacing.as.a.percentage.of.brush.diameter"));
    controlsLayout->addWidget(flowControl_);
    controlsLayout->addWidget(scaleControl_);
    controlsLayout->addWidget(spacingControl_);

    directionButton_ = new ToolOptionsButton(QStringLiteral("BrushDirectionButton"), i18n::text("brush.options.page.direction"), i18n::text("brush.options.page.follow.stroke.direction.on.rotate.with.the.stroke.plus"), ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
    directionButton_->setIcon(toolGlyph(ToolGlyph::FollowStrokeDirection));
    directionButton_->setChecked(brushSettings_.tip.rotationMode
        == core::BrushTipRotationMode::FollowStrokeDirection);
    i18n::setAccessibleName(directionButton_, i18n::text("brush.options.page.follow.stroke.direction"));

    angleControl_ = valueControl(QStringLiteral("BrushAngleControl"), i18n::text("brush.options.page.angle"), -180.0, 180.0, 0.0, QStringLiteral("°"), 0, 1.0, 116, this);
    controlsLayout->addWidget(angleControl_);
    controlsLayout->addWidget(directionButton_);
    layout->addWidget(controls, 0, 1, Qt::AlignCenter);
    auto* symmetryControls = new QWidget(this);
    symmetryControls->setObjectName("BrushSymmetryControls");
    symmetryControls->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* symmetryRow = new QHBoxLayout(symmetryControls);
    symmetryRow->setContentsMargins(0, 0, 0, 0);
    symmetryRow->setSpacing(2);
    symmetryButton_ = new ToolOptionsButton("BrushSymmetryButton", i18n::text("brush.symmetry.short"),
        i18n::text("brush.symmetry.hint"), ToolOptionsButton::Kind::Toggle, symmetryControls);
    symmetryButton_->setIcon(toolGlyph(ToolGlyph::BrushSymmetry));
    symmetryButton_->setIconSize({18,18});
    symmetryButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    i18n::setAccessibleName(symmetryButton_, i18n::text("brush.symmetry.title"));
    symmetryOptionsButton_ = new ToolOptionsButton("BrushSymmetryOptionsButton", i18n::text("brush.symmetry.options"),
        i18n::text("brush.symmetry.options"), ToolOptionsButton::Kind::Action, symmetryControls,
        ToolOptionsButton::Presentation::Icon);
    symmetryOptionsButton_->setFixedSize(30,30);
    symmetryOptionsButton_->setArrowType(Qt::DownArrow);
    symmetryOptionsButton_->setPopupMode(QToolButton::InstantPopup);
    // The centered arrow is the whole control; suppress Qt's second corner
    // menu indicator while retaining the shared toolbar button appearance.
    symmetryOptionsButton_->setStyleSheet("QToolButton::menu-indicator { image: none; }");
    symmetryRow->addWidget(symmetryButton_);
    symmetryRow->addWidget(symmetryOptionsButton_);
    auto* symmetryMenu = new QMenu(symmetryOptionsButton_);
    symmetryMenu->setObjectName("BrushSymmetryMenu");
    auto* symmetryAxes = new QActionGroup(symmetryMenu);
    symmetryVertical_ = i18n::addAction(symmetryMenu, i18n::text("brush.symmetry.vertical"));
    symmetryHorizontal_ = i18n::addAction(symmetryMenu, i18n::text("brush.symmetry.horizontal"));
    symmetryVertical_->setObjectName("BrushSymmetryVerticalAction");
    symmetryHorizontal_->setObjectName("BrushSymmetryHorizontalAction");
    for (auto* action : {symmetryVertical_, symmetryHorizontal_}) {
        action->setCheckable(true);
        symmetryAxes->addAction(action);
    }
    symmetryVertical_->setChecked(true);
    symmetryMenu->addSeparator();
    symmetryReposition_ = i18n::addAction(symmetryMenu, toolGlyph(ToolGlyph::UnderMouse),
        i18n::text("brush.symmetry.reposition"));
    symmetryReposition_->setObjectName("BrushSymmetryRepositionAction");
    symmetryOptionsButton_->setMenu(symmetryMenu);
    connect(symmetryMenu, &QMenu::aboutToShow, this, [this] {
        if (onSymmetryMenuOpened) onSymmetryMenuOpened();
    });
    connect(symmetryMenu, &QMenu::aboutToHide, this, [this] {
        if (onSymmetryMenuClosed) onSymmetryMenuClosed();
    });
    layout->addWidget(symmetryControls, 0, 2, Qt::AlignLeft | Qt::AlignVCenter);
    connect(symmetryButton_, &QToolButton::clicked, this, [this](bool enabled) {
        if (onSymmetryToggled) onSymmetryToggled(enabled);
    });
    connect(symmetryVertical_, &QAction::triggered, this, [this] {
        if (onSymmetryAxisChanged) onSymmetryAxisChanged(core::BrushSymmetryAxis::Vertical);
    });
    connect(symmetryHorizontal_, &QAction::triggered, this, [this] {
        if (onSymmetryAxisChanged) onSymmetryAxisChanged(core::BrushSymmetryAxis::Horizontal);
    });
    connect(symmetryReposition_, &QAction::triggered, this, [this] {
        if (onSymmetryReposition) onSymmetryReposition();
    });

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

void BrushOptionsPage::setSymmetry(const core::BrushSymmetry& symmetry, bool available, bool placing)
{
    const QSignalBlocker buttonBlock(symmetryButton_), verticalBlock(symmetryVertical_), horizontalBlock(symmetryHorizontal_);
    symmetryButton_->setEnabled(available);
    symmetryOptionsButton_->setEnabled(available);
    symmetryButton_->setChecked(symmetry.enabled || placing);
    symmetryVertical_->setChecked(symmetry.axis == core::BrushSymmetryAxis::Vertical);
    symmetryHorizontal_->setChecked(symmetry.axis == core::BrushSymmetryAxis::Horizontal);
    symmetryVertical_->setEnabled(available);
    symmetryHorizontal_->setEnabled(available);
    symmetryReposition_->setEnabled(available);
    i18n::setToolTip(symmetryButton_, i18n::text(placing ? "brush.symmetry.placing" : "brush.symmetry.hint"));
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
