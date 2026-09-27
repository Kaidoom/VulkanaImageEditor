#include "imageeditor/ui/CloningOptionsPage.hpp"

#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <algorithm>

namespace imageeditor::ui {
namespace {

CompactValueControl* valueControl(const QString& objectName,
    const QString& label, double minimum, double maximum,
    const QString& suffix, int decimals, int width, QWidget* parent)
{
    auto* control = new CompactValueControl(parent);
    control->setObjectName(objectName);
    control->setRange(minimum, maximum);
    control->setPrefix(label + QStringLiteral(": "));
    control->setSuffix(suffix);
    control->setDecimals(decimals);
    control->setSingleStep(1.0);
    control->setFixedSize(width, 30);
    control->setAccessibleName(label);
    return control;
}

} // namespace

CloningOptionsPage::CloningOptionsPage(QWidget* parent)
    : QWidget(parent)
    , modes_(new QWidget(this))
    , modeGroup_(new QButtonGroup(this))
    , sourceModes_(new QWidget(this))
    , sourceGroup_(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("CloningOptionsPage"));
    modes_->setObjectName(QStringLiteral("CloningModes"));
    auto* modeRow = new QHBoxLayout(modes_);
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->setSpacing(4);
    for (const auto mode : {core::CloneMode::Stamp, core::CloneMode::Heal, core::CloneMode::SpotHeal}) {
        const bool heal = mode == core::CloneMode::Heal;
        const bool spot = mode == core::CloneMode::SpotHeal;
        auto* button = new ToolOptionsButton(
            spot ? QStringLiteral("CloneModeSpotHeal") : heal ? QStringLiteral("CloneModeHeal") : QStringLiteral("CloneModeStamp"),
            spot ? QStringLiteral("Spot Heal") : heal ? QStringLiteral("Heal") : QStringLiteral("Stamp"),
            spot ? QStringLiteral("Spot Heal · mark a blemish or scratch; reconstruct from unmarked surrounding texture on release. No source anchor needed.")
                 : heal ? QStringLiteral("Heal · transfer source texture and adapt it to destination tone and illumination")
                 : QStringLiteral("Stamp · copy corresponding source pixels through the brush footprint"),
            ToolOptionsButton::Kind::Toggle, modes_, ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(spot ? ToolGlyph::SpotHeal : heal ? ToolGlyph::CloneHeal : ToolGlyph::CloneStamp));
        modeGroup_->addButton(button, static_cast<int>(mode));
        modeRow->addWidget(button);
    }

    auto* outer = new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
    controls_=new QWidget(this);outer->addWidget(controls_);
    auto* row = new QHBoxLayout(controls_);
    // The fixed-height ToolOptionsBar owns vertical padding and overflow.
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();

    sourceModes_->setObjectName(QStringLiteral("CloneSourceModes"));
    sourceModes_->setAccessibleName(QStringLiteral("Cloning sample source"));
    auto* sourceRow = new QHBoxLayout(sourceModes_);
    sourceRow->setContentsMargins(0, 0, 0, 0);
    sourceRow->setSpacing(4);
    rawSourceHint_ = QStringLiteral(
        "Source Layer (Raw)\nSample the identified source layer before its adjustments and opacity. "
        "Changing the destination layer does not redirect this source.");
    const auto renderedHint = QStringLiteral(
        "Rendered sampling captures appearance, including adjustments, opacity, blending and visibility. "
        "For retouching, paint onto a clean Normal raster layer at 100% opacity with no adjustments; "
        "additional destination effects will change the sampled appearance.");
    struct SourceMode {
        core::CloneSampleSource source;
        const char* name;
        QString label;
        QString hint;
        ToolGlyph glyph;
    };
    const SourceMode sources[] = {
        {core::CloneSampleSource::SourceLayer, "CloneSourceLayer", QStringLiteral("Source Layer (Raw)"),
            rawSourceHint_, ToolGlyph::ActiveLayer},
        {core::CloneSampleSource::CurrentAndBelow, "CloneSourceCurrentBelow", QStringLiteral("Current & Below"),
            QStringLiteral("Current & Below\nThe destination layer and visible content beneath it.\n")
                + renderedHint, ToolGlyph::CurrentAndBelow},
        {core::CloneSampleSource::AllVisible, "CloneSourceAllVisible", QStringLiteral("All Visible"),
            QStringLiteral("All Visible\nRendered visible document content.\n")
                + renderedHint, ToolGlyph::MergedVisible},
    };
    for (const auto& source : sources) {
        auto* button = new ToolOptionsButton(QString::fromLatin1(source.name), source.label,
            source.hint, ToolOptionsButton::Kind::Toggle, sourceModes_,
            ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(source.glyph));
        sourceGroup_->addButton(button, static_cast<int>(source.source));
        sourceRow->addWidget(button);
    }
    row->addWidget(sourceModes_);

    sizeControl_ = valueControl(QStringLiteral("CloneSizeControl"),
        QStringLiteral("Size"), 1.0, 1000.0, QStringLiteral(" px"), 1, 132, this);
    opacityControl_ = valueControl(QStringLiteral("CloneOpacityControl"),
        QStringLiteral("Opacity"), 1.0, 100.0, QStringLiteral("%"), 0, 132, this);
    hardnessControl_ = valueControl(QStringLiteral("CloneHardnessControl"),
        QStringLiteral("Hardness"), 0.0, 100.0, QStringLiteral("%"), 0, 134, this);
    hardnessControl_->setToolTip(QStringLiteral(
        "Brush edge coverage in Stamp and Heal. Lower values soften the footprint; "
        "healing adaptation is controlled separately."));
    flowControl_ = valueControl(QStringLiteral("CloneFlowControl"),
        QStringLiteral("Flow"), 1.0, 100.0, QStringLiteral("%"), 0, 116, this);
    flowControl_->setToolTip(QStringLiteral(
        "Coverage added by each dab, up to the stroke's Opacity ceiling"));
    spacingControl_ = valueControl(QStringLiteral("CloneSpacingControl"),
        QStringLiteral("Spacing"), 1.0, 200.0, QStringLiteral("%"), 0, 132, this);
    spacingControl_->setToolTip(QStringLiteral("Dab spacing as a percentage of brush diameter"));
    for (auto* control : {sizeControl_, opacityControl_, hardnessControl_, flowControl_, spacingControl_}) {
        row->addWidget(control);
        connect(control, &QDoubleSpinBox::valueChanged, this,
            [this, control] { publishBrushSettings(control); });
    }

    alignedButton_ = new ToolOptionsButton(QStringLiteral("CloneAlignedButton"),
        QStringLiteral("Aligned"), QStringLiteral(
            "Aligned sampling\nOn: preserve the source-to-destination offset between strokes.\n"
            "Off: each stroke starts at the original source anchor.\nAlt+click sets a new source anchor."),
        ToolOptionsButton::Kind::Toggle, this, ToolOptionsButton::Presentation::Icon);
    alignedButton_->setIcon(toolGlyph(ToolGlyph::CloneAligned));

    adaptationControl_ = valueControl(QStringLiteral("CloneAdaptationControl"),
        QStringLiteral("Adaptation"), 0.0, 100.0, QStringLiteral("%"), 0, 148, this);
    adaptationControl_->setToolTip(QStringLiteral(
        "Heal adaptation\nHigher values fit transferred texture more strongly to surrounding "
        "destination tone and illumination. This does not change brush edge softness."));
    row->addWidget(adaptationControl_);
    row->addWidget(alignedButton_);
    row->addStretch();

    progressRow_=new QWidget(this);progressRow_->setObjectName(QStringLiteral("SpotHealProgressRow"));
    auto* progressLayout=new QHBoxLayout(progressRow_);progressLayout->setContentsMargins(0,0,0,0);
    progressLayout->setSpacing(8);
    // Match the centered tool controls: spare width belongs outside the group,
    // not inside the status label and Cancel button.
    progressLayout->addStretch();
    progressLabel_=new QLabel;progressLabel_->setTextFormat(Qt::PlainText);
    progressLabel_->setObjectName(QStringLiteral("SpotHealProgressLabel"));progressLayout->addWidget(progressLabel_);
    progress_=new QProgressBar;progress_->setRange(0,1000);progress_->setFixedWidth(180);
    progress_->setTextVisible(false);progressLayout->addWidget(progress_);
    auto* cancel=new QPushButton(tr("Cancel"));cancel->setObjectName(QStringLiteral("SpotHealCancelProcessing"));
    progressLayout->addWidget(cancel);progressLayout->addStretch();
    outer->addWidget(progressRow_);progressRow_->hide();
    connect(cancel,&QPushButton::clicked,this,[this]{if(onCancelProcessing)onCancelProcessing();});

    connect(modeGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        if (updating_) return;
        const auto mode = static_cast<core::CloneMode>(id);
        if (cloneSettings_.mode == mode) return;
        setMode(mode);
        if (onModeChanged) onModeChanged(mode);
    });
    connect(sourceGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        if (updating_) return;
        const auto source = static_cast<core::CloneSampleSource>(id);
        if (cloneSettings_.source == source) return;
        setSource(source);
        if (onSourceChanged) onSourceChanged(source);
    });
    connect(alignedButton_, &QToolButton::toggled, this, [this](bool aligned) {
        if (updating_) return;
        cloneSettings_.aligned = aligned;
        if (onAlignedChanged) onAlignedChanged(aligned);
    });
    connect(adaptationControl_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (updating_) return;
        cloneSettings_.adaptation = value / 100.0;
        const QScopedValueRollback publishing(publishingControl_, adaptationControl_);
        if (onAdaptationChanged) onAdaptationChanged(cloneSettings_.adaptation);
    });
    setBrushSettings(brushSettings_);
    setCloneSettings(cloneSettings_);
    setSourceLayerLabel({}, false);
}

void CloningOptionsPage::setBrushSettings(const core::BrushSettings& settings)
{
    brushSettings_ = settings;
    const QScopedValueRollback guard(updating_, true);
    const auto synchronize = [this](CompactValueControl* control, double value) {
        const QSignalBlocker blocker(control);
        if (control != publishingControl_ || !control->isManualEntryActive())
            control->setValue(value);
    };
    synchronize(sizeControl_, settings.sizePixels);
    synchronize(opacityControl_, settings.opacity * 100.0);
    synchronize(hardnessControl_, settings.hardness * 100.0);
    synchronize(flowControl_, settings.flow * 100.0);
    synchronize(spacingControl_, settings.spacingPercent);
}

void CloningOptionsPage::setCloneSettings(const core::CloneSettings& settings)
{
    cloneSettings_ = settings;
    const QScopedValueRollback guard(updating_, true);
    const QSignalBlocker sourceBlocker(sourceGroup_);
    const QSignalBlocker alignedBlocker(alignedButton_);
    const QSignalBlocker adaptationBlocker(adaptationControl_);
    if (auto* button = modeGroup_->button(static_cast<int>(settings.mode))) {
        const QSignalBlocker modeBlocker(modeGroup_);
        const QSignalBlocker buttonBlocker(button);
        button->setChecked(true);
    }
    if (auto* button = sourceGroup_->button(static_cast<int>(settings.source))) {
        const QSignalBlocker buttonBlocker(button);
        button->setChecked(true);
    }
    alignedButton_->setChecked(settings.aligned);
    if (adaptationControl_ != publishingControl_ || !adaptationControl_->isManualEntryActive())
        adaptationControl_->setValue(settings.adaptation * 100.0);
    // Keep the slot visible so switching modes never shifts the other controls.
    adaptationControl_->setEnabled(settings.mode != core::CloneMode::Stamp);
    alignedButton_->setEnabled(settings.mode != core::CloneMode::SpotHeal);
}

void CloningOptionsPage::setMode(core::CloneMode mode)
{
    auto settings = cloneSettings_;
    settings.mode = mode;
    setCloneSettings(settings);
}

void CloningOptionsPage::setSource(core::CloneSampleSource source)
{
    auto settings = cloneSettings_;
    settings.source = source;
    setCloneSettings(settings);
}

void CloningOptionsPage::setAligned(bool aligned)
{
    auto settings = cloneSettings_;
    settings.aligned = aligned;
    setCloneSettings(settings);
}

void CloningOptionsPage::setAdaptation(double adaptation)
{
    auto settings = cloneSettings_;
    settings.adaptation = adaptation;
    setCloneSettings(settings);
}

void CloningOptionsPage::setSourceLayerLabel(const QString& label, bool valid)
{
    if(cloneSettings_.mode==core::CloneMode::SpotHeal){
        sourceModes_->setAccessibleDescription(tr("Automatic repair; no source anchor required"));
        if(auto* button=sourceGroup_->button(int(core::CloneSampleSource::SourceLayer)))
            button->setToolTip(tr("Source Layer (Raw)\nRepair the primary raster layer before its adjustments, filters, opacity and blending. No source anchor required."));
        return;
    }
    const auto fullText = valid
        ? QStringLiteral("Source: %1").arg(label)
        : QStringLiteral("Alt+click to set source");
    // The bottom information bar owns the visible source readout. Keep the
    // same identity available here without reserving a wide toolbar label.
    sourceModes_->setAccessibleDescription(fullText);
    if (auto* button = sourceGroup_->button(static_cast<int>(core::CloneSampleSource::SourceLayer))) {
        button->setAccessibleDescription(fullText);
        button->setToolTip(rawSourceHint_ + QStringLiteral("\n\n") + (valid
            ? fullText + QStringLiteral("\nAlt+click establishes a new source anchor.")
            : QStringLiteral("No valid source. Alt+click the image to set a source point, then paint to clone.")));
    }
}

void CloningOptionsPage::setProcessing(bool busy,double progress,const QString& message)
{
    controls_->setVisible(!busy);progressRow_->setVisible(busy);modes_->setEnabled(!busy);
    progressLabel_->setText(message);progress_->setValue(qRound(std::clamp(progress,0.0,1.0)*1000));
}

void CloningOptionsPage::publishBrushSettings(CompactValueControl* source)
{
    if (updating_) return;
    // Preserve the exact shared settings of untouched fields, including values
    // with more precision than their displayed percentages and all tip/grain
    // and pressure/smoothing settings owned by the existing brush controls.
    if (source == sizeControl_) brushSettings_.sizePixels = source->value();
    else if (source == opacityControl_) brushSettings_.opacity = source->value() / 100.0;
    else if (source == hardnessControl_) brushSettings_.hardness = source->value() / 100.0;
    else if (source == flowControl_) brushSettings_.flow = source->value() / 100.0;
    else if (source == spacingControl_) brushSettings_.spacingPercent = source->value();
    const QScopedValueRollback publishing(publishingControl_, source);
    if (onBrushSettingsChanged) onBrushSettingsChanged(brushSettings_);
}

} // namespace imageeditor::ui
