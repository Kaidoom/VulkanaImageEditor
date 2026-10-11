#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"

#include "imageeditor/core/Layer.hpp"
#include "imageeditor/ui/BrushAssetLibrary.hpp"
#include "imageeditor/ui/BrushComponentPicker.hpp"
#include "imageeditor/ui/BrushPresetGrid.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

namespace imageeditor::ui {
namespace {

constexpr int kPanelInset = 14;
constexpr int kPanelBottomInset = 18;
constexpr int kControlSpacing = 9;
constexpr int kTitleToSectionsGap = 6;
constexpr int kSectionBreak = 8;

class BrushSeedEdit final : public QLineEdit {
public:
    std::function<void()> restore;
protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape) {
            if (restore) restore();
            clearFocus(); event->accept(); return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

QLabel* sectionLabel(const i18n::Message& text)
{
    auto* label = i18n::make<QLabel>(text.toUpper());
    label->setObjectName(QStringLiteral("SectionLabel"));
    return label;
}

bool sameBrushBehavior(const core::BrushSettings& left,
    const core::BrushSettings& right) noexcept
{
    // Foreground is global editor state and deliberately not preset state.
    return left.tip == right.tip
        && left.grain == right.grain
        && left.sizePixels == right.sizePixels
        && left.hardness == right.hardness
        && left.opacity == right.opacity
        && left.flow == right.flow
        && left.spacingPercent == right.spacingPercent
        && left.pressureToSize == right.pressureToSize
        && left.pressureToFlow == right.pressureToFlow
        && left.smoothing == right.smoothing
        && left.smoothingTimeMilliseconds == right.smoothingTimeMilliseconds
        && left.deterministicSeed == right.deterministicSeed;
}

CompactValueControl* grainValueControl(const QString& objectName,
    const i18n::Message& label, double minimum, double maximum, double value,
    const QString& suffix)
{
    auto* control = new CompactValueControl;
    control->setObjectName(objectName);
    control->setRange(minimum, maximum);
    control->setDecimals(1);
    control->setValue(value);
    i18n::setPrefix(control, label + QStringLiteral(": "));
    control->setSuffix(suffix);
    control->setSingleStep(1.0);
    control->setFixedHeight(30);
    control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    i18n::setAccessibleName(control, label);
    return control;
}

QWidget* collapsibleSection(const i18n::Message& title,
    const QString& objectName, QVBoxLayout*& sectionLayout)
{
    auto* container = new QWidget;
    container->setObjectName(objectName + QStringLiteral("Section"));
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kControlSpacing);

    auto* header = new QToolButton(container);
    header->setObjectName(objectName + QStringLiteral("SectionHeader"));
    i18n::setText(header, title);
    header->setCheckable(true);
    header->setChecked(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(Qt::DownArrow);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(header);

    auto* body = new QWidget(container);
    body->setObjectName(objectName + QStringLiteral("SectionBody"));
    sectionLayout = new QVBoxLayout(body);
    sectionLayout->setContentsMargins(8, 0, 0, 0);
    sectionLayout->setSpacing(kControlSpacing);
    layout->addWidget(body);
    QObject::connect(header, &QToolButton::toggled, body, &QWidget::setVisible);
    QObject::connect(header, &QToolButton::toggled, header,
        [header](bool expanded) {
            header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        });
    return container;
}

} // namespace

PropertiesPanel::PropertiesPanel(
    std::shared_ptr<BrushAssetLibrary> brushAssets, QWidget* parent)
    : QWidget(parent)
    , brushAssets_(brushAssets ? std::move(brushAssets)
                               : BrushAssetLibrary::createPackaged())
    , stack_(new QStackedWidget(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);

    pages_[core::ToolId::Move] = createMovePage();
    pages_[core::ToolId::Transform] = createSelectionTransformPage();
    pages_[core::ToolId::Marquee] = createSelectionPage(core::ToolId::Marquee, i18n::text("properties.panel.select"), i18n::text("properties.panel.create.and.combine.selections.layertransformaction.transforms.the.selection.mask"));
    pages_[core::ToolId::Lasso] = createSelectionPage(core::ToolId::Lasso, i18n::text("properties.panel.lasso"), i18n::text("properties.panel.freehand.polygonal.and.magnetic.share.one.selection.mask.layertransformaction"));
    pages_[core::ToolId::Brush] = createBrushPage(i18n::text("properties.panel.brush"));
    pages_[core::ToolId::Fill] = createFillPage();
    pages_[core::ToolId::Eyedropper] = createEyedropperPage();
    pages_[core::ToolId::Text] = createTextPage();
    for (const auto& [tool, page] : pages_) {
        (void)tool;
        stack_->addWidget(page);
    }

    const auto builtins = core::builtinBrushPresets();
    setBrushPresets({builtins.begin(), builtins.end()});
    const auto* pressure = core::findBuiltinBrushPreset(
        "builtin.preset.pressure-round.v1");
    if (pressure) {
        selectedPresetId_ = pressure->id;
        selectedPresetBaseline_ = pressure->settings;
        setBrushSettings(pressure->settings);
    }
    setActiveTool(core::ToolId::Move);
    updateShortcutHints(this, defaultShortcutBindings());
}

void PropertiesPanel::setActiveTool(core::ToolId tool)
{
    if(cloningHelp_)cloningHelp_->setVisible(tool==core::ToolId::Cloning);
    if(localBlurHelp_)localBlurHelp_->setVisible(tool==core::ToolId::LocalBlur);
    if (tool == core::ToolId::Eraser || tool==core::ToolId::Cloning || tool==core::ToolId::LocalBlur) {
        tool = core::ToolId::Brush;
    }
    const auto found = pages_.find(tool);
    if (found != pages_.end()) {
        stack_->setCurrentWidget(found->second);
    }
}

void PropertiesPanel::setSelectedLayer(const core::Layer* layer, const core::LayerContainer* container)
{
    if (!selectedLayerName_ || !selectedLayerType_) {
        return;
    }
    if (!layer) {
        if (container) {
            selectedLayerName_->setText(QString::fromUtf8(container->name));
            i18n::setText(selectedLayerType_, container->kind == core::ContainerKind::ClippingMaskGroup
                ? i18n::text("properties.panel.clipping.mask.group.the.bottommost.child.is.the.base")
                : container->kind == core::ContainerKind::Group
                ? i18n::text("properties.panel.pass.through.group.move.transform.affects.every.member.including")
                : i18n::text("properties.panel.organizational.folder.select.its.layers.to.move.or.transform"));
            return;
        }
        i18n::setText(selectedLayerName_, i18n::text("properties.panel.no.layer.selected"));
        selectedLayerType_->clear();
        return;
    }
    selectedLayerName_->setText(QString::fromUtf8(layer->name));
    i18n::setText(selectedLayerType_, std::holds_alternative<core::RasterLayer>(layer->payload)
        ? i18n::text("properties.panel.raster.layer") : std::holds_alternative<core::ShapeLayer>(layer->payload)
        ? i18n::text("properties.panel.editable.shape.layer") : std::holds_alternative<core::AdjustmentLayer>(layer->payload)
        ? i18n::text("properties.panel.adjustment.layer.edits.the.lower.composite.select.its.mask") : i18n::text("properties.panel.editable.text.layer"));
}

void PropertiesPanel::setBrushSettings(const core::BrushSettings& settings)
{
    brushSettings_ = settings;
    if (!brushPresetGrid_) {
        return;
    }
    updatingBrushUi_ = true;
    const QSignalBlocker strengthBlocker(brushGrainStrengthControl_);
    const QSignalBlocker scaleBlocker(brushGrainScaleControl_);
    const QSignalBlocker angleBlocker(brushGrainAngleControl_);
    brushSmoothingCombo_->setCurrentIndex(
        settings.smoothing == core::BrushSmoothingMode::Weighted ? 1 : 0);
    const auto synchronize = [this](CompactValueControl* control,
                                 double value) {
        if (control == publishingControl_
            && control->isManualEntryActive()) {
            return;
        }
        control->setValue(value);
    };
    synchronize(brushGrainStrengthControl_, settings.grain.strength * 100.0);
    synchronize(brushGrainScaleControl_, settings.grain.scalePixels);
    synchronize(brushGrainAngleControl_, settings.grain.angleDegrees);
    brushPressureSize_->setChecked(settings.pressureToSize);
    brushPressureFlow_->setChecked(settings.pressureToFlow);
    brushSeed_->setText(QString::number(settings.deterministicSeed));
    brushGrainPicker_->setCurrentAssetId(settings.grain.assetId);
    updatingBrushUi_ = false;
    updateComponentControlState();
    updatePresetPresentation();
}

void PropertiesPanel::setBrushPresets(
    std::vector<core::BrushPresetRecord> presets)
{
    brushPresets_ = std::move(presets);
    if (selectedPresetId_.empty() && !brushPresets_.empty()) {
        const auto found = std::find_if(brushPresets_.begin(), brushPresets_.end(),
            [](const core::BrushPresetRecord& preset) {
                return preset.id == "builtin.preset.pressure-round.v1";
            });
        const auto& initial = found == brushPresets_.end()
            ? brushPresets_.front() : *found;
        selectedPresetId_ = initial.id;
        selectedPresetBaseline_ = initial.settings;
    }
    rebuildPresetGrid();
}

QWidget* PropertiesPanel::createSelectionTransformPage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(i18n::text("properties.panel.transform"), i18n::text("properties.panel.selection.boundary.selected.pixels.or.layer.geometry.according.to"), content);
    auto* help = i18n::make<QLabel>(i18n::text("properties.panel.layertransformaction.transform.the.selection.mask.transformselectedpixelsaction.cut.and.transform"));
    help->setObjectName(QStringLiteral("MutedLabel"));
    help->setWordWrap(true);
    content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createMovePage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(i18n::text("properties.panel.move"), i18n::text("properties.panel.under.mouse.picks.a.layer.active.layer.keeps.the"), content);
    content->addWidget(sectionLabel(i18n::text("properties.panel.target")));
    selectedLayerName_ = i18n::make<QLabel>(i18n::text("properties.panel.no.layer.selected"));
    selectedLayerName_->setObjectName(QStringLiteral("PropertiesTargetName"));
    selectedLayerName_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    selectedLayerType_ = new QLabel;
    selectedLayerType_->setObjectName(QStringLiteral("MutedLabel"));
    selectedLayerType_->setWordWrap(true);
    content->addWidget(selectedLayerName_);
    content->addWidget(selectedLayerType_);
    content->addSpacing(kSectionBreak);
    content->addWidget(sectionLabel(i18n::text("properties.panel.controls.shortcuts")));
    auto* help = i18n::make<QLabel>(i18n::text("properties.panel.toolaction.move.move.click.select.layer.drag.move.selected"));
    help->setObjectName(QStringLiteral("MutedLabel"));
    help->setWordWrap(true);
    content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createSelectionPage(
    core::ToolId tool, const i18n::Message& title, const i18n::Message& description)
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(title, description, content);
    const auto intro = tool == core::ToolId::Lasso
        ? i18n::text("properties.panel.toolaction.lasso.lasso.freehand.drag.to.trace.release.to")
        : i18n::text("properties.panel.toolaction.marquee.select.choose.rectangle.or.ellipse.beside.the");
    auto* hints = i18n::make<QLabel>(intro + i18n::text("properties.panel.at.press.shift.add.alt.subtract.shift.alt.intersect"));
    hints->setWordWrap(true);
    content->addWidget(hints);
    return page;
}

QWidget* PropertiesPanel::createBrushPage(const i18n::Message& title)
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(title, QString {}, content);

    cloningHelp_=i18n::make<QLabel>(i18n::text("properties.panel.toolaction.cloning.cloning.alt.click.set.stamp.heal.source"));
    cloningHelp_->setObjectName(QStringLiteral("CloningHelp"));
    cloningHelp_->setWordWrap(true);cloningHelp_->hide();content->addWidget(cloningHelp_);
    localBlurHelp_=i18n::make<QLabel>(i18n::text("properties.panel.toolaction.local.blur.local.blur.drag.soften.raster.colors"));
    localBlurHelp_->setObjectName(QStringLiteral("LocalBlurHelp"));
    localBlurHelp_->setWordWrap(true);localBlurHelp_->hide();content->addWidget(localBlurHelp_);

    brushPresetGrid_ = new BrushPresetGrid;
    content->addWidget(brushPresetGrid_);

    auto* presetActions = new QWidget;
    auto* presetActionLayout = new QHBoxLayout(presetActions);
    presetActionLayout->setContentsMargins(0, 0, 0, 0);
    presetActionLayout->setSpacing(6);
    brushPresetResetButton_ = i18n::make<QPushButton>(i18n::text("properties.panel.reset"));
    brushPresetResetButton_->setObjectName(
        QStringLiteral("BrushPresetResetButton"));
    brushPresetSaveCopyButton_ = i18n::make<QPushButton>(i18n::text("properties.panel.save.copy"));
    brushPresetSaveCopyButton_->setObjectName(
        QStringLiteral("BrushPresetSaveCopyButton"));
    presetActionLayout->addStretch(1);
    presetActionLayout->addWidget(brushPresetResetButton_);
    presetActionLayout->addWidget(brushPresetSaveCopyButton_);
    content->addWidget(presetActions);

    content->addWidget(sectionLabel(i18n::text("properties.panel.smoothing")));
    brushSmoothingCombo_ = new QComboBox;
    brushSmoothingCombo_->setObjectName(QStringLiteral("BrushSmoothingCombo"));
    i18n::addItems(brushSmoothingCombo_, {i18n::text("properties.panel.none"),
        i18n::text("properties.panel.light.weighted")});
    content->addWidget(brushSmoothingCombo_);

    content->addWidget(sectionLabel(i18n::text("properties.panel.pressure")));
    brushPressureSize_ = i18n::make<QCheckBox>(i18n::text("properties.panel.pressure.controls.size"));
    brushPressureFlow_ = i18n::make<QCheckBox>(i18n::text("properties.panel.pressure.controls.flow"));
    brushPressureSize_->setObjectName(QStringLiteral("BrushPressureSize"));
    brushPressureFlow_->setObjectName(QStringLiteral("BrushPressureFlow"));
    brushPressureSize_->setChecked(true);
    brushPressureFlow_->setChecked(true);
    content->addWidget(brushPressureSize_);
    content->addWidget(brushPressureFlow_);

    auto* variation = new QHBoxLayout;
    variation->setSpacing(6);
    auto* seed = new BrushSeedEdit;
    brushSeed_ = seed;
    seed->setObjectName(QStringLiteral("BrushVariationSeed"));
    seed->setMaxLength(20); // full uint64, without lossy floating-point spinbox conversion
    seed->setText(QString::number(brushSettings_.deterministicSeed));
    i18n::setToolTip(seed, i18n::text("properties.panel.repeatable.stamp.variation.and.grain.placement.saved.by.save"));
    i18n::setAccessibleName(seed, i18n::text("properties.panel.variation.seed"));
    seed->restore = [this] { brushSeed_->setText(QString::number(brushSettings_.deterministicSeed)); };
    auto* reseed = i18n::make<QPushButton>(i18n::text("properties.panel.reseed"));
    reseed->setObjectName(QStringLiteral("BrushReseedButton"));
    variation->addWidget(i18n::make<QLabel>(i18n::text("properties.panel.seed")));
    variation->addWidget(seed, 1); variation->addWidget(reseed);
    content->addLayout(variation);
    connect(seed, &QLineEdit::editingFinished, this, [this] {
        if (updatingBrushUi_) return;
        bool valid = false;
        const auto value = brushSeed_->text().toULongLong(&valid);
        if (valid && value != brushSettings_.deterministicSeed) {
            brushSettings_.deterministicSeed = value;
            publishBrushSettings();
        }
        brushSeed_->setText(QString::number(brushSettings_.deterministicSeed));
    });
    connect(reseed, &QPushButton::clicked, this, [this] {
        brushSettings_.deterministicSeed = QRandomGenerator::global()->generate64();
        brushSeed_->setText(QString::number(brushSettings_.deterministicSeed));
        publishBrushSettings();
    });

    QVBoxLayout* grainLayout = nullptr;
    auto* grainSection = collapsibleSection(i18n::text("properties.panel.texture.grain"), QStringLiteral("BrushGrain"), grainLayout);
    brushGrainPicker_ = new BrushComponentPicker(core::BrushAssetType::Grain);
    brushGrainPicker_->setItems(
        brushAssets_->components(core::BrushAssetType::Grain));
    grainLayout->addWidget(brushGrainPicker_);
    brushGrainStrengthControl_ = grainValueControl(QStringLiteral("BrushGrainStrengthControl"), i18n::text("properties.panel.strength"), 0.0, 100.0, 0.0, QStringLiteral("%"));
    brushGrainScaleControl_ = grainValueControl(QStringLiteral("BrushGrainScaleControl"), i18n::text("properties.panel.scale"), 8.0, 512.0, 96.0, QStringLiteral(" px"));
    brushGrainAngleControl_ = grainValueControl(QStringLiteral("BrushGrainAngleControl"), i18n::text("properties.panel.rotation"), -180.0, 180.0, 0.0, QStringLiteral("°"));
    grainLayout->addWidget(brushGrainStrengthControl_);
    grainLayout->addWidget(brushGrainScaleControl_);
    grainLayout->addWidget(brushGrainAngleControl_);
    content->addWidget(grainSection);
    QVBoxLayout* shortcutLayout=nullptr;
    auto* shortcuts=collapsibleSection(i18n::text("properties.panel.shortcuts"), QStringLiteral("BrushShortcuts"), shortcutLayout);
    auto* help=i18n::make<QLabel>(i18n::text("properties.panel.toolaction.brush.selects.brush.toolaction.eraser.toggles.erase.decreasebrushsizeaction"));
    help->setWordWrap(true);shortcutLayout->addWidget(help);content->addWidget(shortcuts);
    shortcuts->findChild<QToolButton*>(QStringLiteral("BrushShortcutsSectionHeader"))->setChecked(false);

    const auto changed = [this] { publishBrushSettings(); };
    connect(brushGrainStrengthControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(brushGrainStrengthControl_); });
    connect(brushGrainScaleControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(brushGrainScaleControl_); });
    connect(brushGrainAngleControl_, &QDoubleSpinBox::valueChanged, this,
        [this] { publishBrushSettings(brushGrainAngleControl_); });
    connect(brushSmoothingCombo_, &QComboBox::currentIndexChanged,
        this, changed);
    connect(brushPressureSize_, &QCheckBox::toggled, this, changed);
    connect(brushPressureFlow_, &QCheckBox::toggled, this, changed);
    brushPresetGrid_->onPresetSelected = [this](const std::string& id) {
        if (!updatingBrushUi_) {
            applyPresetById(id);
        }
    };
    connect(brushPresetResetButton_, &QPushButton::clicked,
        this, [this] { (void)resetBrushPreset(); });
    connect(brushPresetSaveCopyButton_, &QPushButton::clicked,
        this, [this] {
            const auto* preset = selectedPreset();
            const auto suggested = preset
                ? QString::fromStdString(preset->displayName)
                    + QStringLiteral(" Copy")
                : i18n::text("properties.panel.custom.brush");
            bool accepted = false;
            const auto name = QInputDialog::getText(this,
                i18n::text("properties.panel.save.brush.preset.copy"),
                i18n::text("properties.panel.preset.name"), QLineEdit::Normal,
                suggested, &accepted);
            if (!accepted) {
                return;
            }
            const auto result = saveBrushCopy(name);
            if (!result.preset) {
                QMessageBox::warning(this,
                    i18n::text("properties.panel.unable.to.save.brush.preset"),
                    result.error);
            }
        });
    brushGrainPicker_->onAssetSelected = [this](const std::string& id) {
        QString error;
        if (!selectBrushComponent(core::BrushAssetType::Grain, id, &error)) {
            QMessageBox::warning(this, i18n::text("properties.panel.brush.grain.unavailable"),
                error);
        }
    };
    return page;
}

void PropertiesPanel::publishBrushSettings(CompactValueControl* source)
{
    if (updatingBrushUi_ || !brushPresetGrid_) {
        return;
    }
    brushSettings_.grain.strength
        = brushGrainStrengthControl_->value() / 100.0;
    brushSettings_.grain.scalePixels = brushGrainScaleControl_->value();
    brushSettings_.grain.angleDegrees = brushGrainAngleControl_->value();
    brushSettings_.smoothing = brushSmoothingCombo_->currentIndex() == 1
        ? core::BrushSmoothingMode::Weighted
        : core::BrushSmoothingMode::None;
    brushSettings_.pressureToSize = brushPressureSize_->isChecked();
    brushSettings_.pressureToFlow = brushPressureFlow_->isChecked();
    updatePresetPresentation();
    QScopedValueRollback publishing(publishingControl_, source);
    if (onBrushSettingsChanged) {
        onBrushSettingsChanged(brushSettings_);
    }
}

bool PropertiesPanel::selectBrushComponent(core::BrushAssetType type,
    std::string_view assetId, QString* error)
{
    if (!brushAssets_->prepareAsset(assetId, type, error)) {
        return false;
    }
    const auto* item = brushAssets_->component(assetId);
    if (!item) {
        if (error) {
            *error = i18n::text("properties.panel.unknown.brush.asset.1")
                .arg(QString::fromUtf8(assetId));
        }
        return false;
    }

    if (type == core::BrushAssetType::Tip) {
        const auto wasRound = brushSettings_.tip.assetId
            == core::BrushAssetIds::ProceduralRoundTip;
        brushSettings_.tip.assetId = assetId;
        if (assetId == core::BrushAssetIds::ProceduralRoundTip) {
            brushSettings_.tip.aspectRatio = 1.0;
            brushSettings_.tip.angleDegrees = 0.0;
            brushSettings_.tip.rotationMode = core::BrushTipRotationMode::FollowStrokeDirection;
        } else if (assetId == core::BrushAssetIds::ProceduralEllipseTip
            && wasRound) {
            brushSettings_.tip.aspectRatio = 0.35;
        }
        if (item->defaultRotationDegrees) {
            brushSettings_.tip.angleDegrees = *item->defaultRotationDegrees;
        }
    } else {
        const auto wasNone = brushSettings_.grain.assetId
            == core::BrushAssetIds::NoGrain;
        brushSettings_.grain.assetId = assetId;
        brushSettings_.grain.invert = false;
        if (assetId == core::BrushAssetIds::NoGrain) {
            brushSettings_.grain.strength = 0.0;
        } else {
            if (wasNone || brushSettings_.grain.strength <= 0.0) {
                brushSettings_.grain.strength = 0.55;
            }
            if (item->defaultScalePixels) {
                brushSettings_.grain.scalePixels = *item->defaultScalePixels;
            }
            if (item->defaultRotationDegrees) {
                brushSettings_.grain.angleDegrees
                    = *item->defaultRotationDegrees;
            }
        }
    }
    setBrushSettings(brushSettings_);
    if (onBrushSettingsChanged) {
        onBrushSettingsChanged(brushSettings_);
    }
    return true;
}

void PropertiesPanel::applyPresetById(std::string_view id)
{
    const auto found = std::find_if(brushPresets_.begin(), brushPresets_.end(),
        [&id](const core::BrushPresetRecord& preset) { return preset.id == id; });
    if (found == brushPresets_.end()) {
        return;
    }
    QString error;
    if (!brushAssets_->prepareSettings(found->settings, &error)) {
        QMessageBox::warning(this, i18n::text("properties.panel.brush.preset.unavailable"),
            error);
        rebuildPresetGrid();
        return;
    }
    auto settings = found->settings;
    settings.foreground = brushSettings_.foreground;
    selectedPresetId_ = found->id;
    selectedPresetBaseline_ = found->settings;
    setBrushSettings(settings);
    if (onBrushSettingsChanged) {
        onBrushSettingsChanged(brushSettings_);
    }
}

void PropertiesPanel::rebuildPresetGrid()
{
    if (!brushPresetGrid_) {
        return;
    }
    updatingBrushUi_ = true;
    std::vector<BrushPresetGridItem> items;
    items.reserve(brushPresets_.size());
    for (const auto& preset : brushPresets_) {
        const auto available = brushAssets_->supports(
                preset.settings.tip.assetId, core::BrushAssetType::Tip)
            && brushAssets_->supports(
                preset.settings.grain.assetId, core::BrushAssetType::Grain);
        QImage thumbnail;
        if (const auto* component = brushAssets_->component(
                preset.settings.tip.assetId)) {
            thumbnail = component->thumbnail;
        }
        auto displayName = QString::fromStdString(preset.displayName);
        if (!preset.id.starts_with("builtin.") && std::any_of(brushPresets_.begin(), brushPresets_.end(),
                [&displayName](const core::BrushPresetRecord& other) {
                    return other.id.starts_with("builtin.")
                        && QString::fromStdString(other.displayName).compare(displayName, Qt::CaseInsensitive) == 0;
                })) displayName += QStringLiteral(" (User)");
        items.push_back({preset.id,
            std::move(displayName), std::move(thumbnail),
            available, available ? QString {}
                                 : brushAssets_->unavailableMessage(
                                       preset.settings)});
    }
    // Owner-provided bitmap presets stay at the bottom, even after Save Copy
    // inserts a user preset. IDs, not names or the user's chosen tip, classify them.
    std::stable_partition(items.begin(), items.end(), [](const BrushPresetGridItem& item) {
        return !item.id.starts_with("builtin.preset.asset.");
    });
    brushPresetGrid_->setItems(std::move(items));
    updatingBrushUi_ = false;
    updatePresetPresentation();
}

const core::BrushPresetRecord* PropertiesPanel::selectedPreset() const noexcept
{
    const auto found = std::find_if(brushPresets_.begin(), brushPresets_.end(),
        [this](const core::BrushPresetRecord& preset) {
            return preset.id == selectedPresetId_;
        });
    return found == brushPresets_.end() ? nullptr : &*found;
}

bool PropertiesPanel::brushPresetModified() const noexcept
{
    return selectedPreset() == nullptr
        || !sameBrushBehavior(brushSettings_, selectedPresetBaseline_);
}

void PropertiesPanel::updatePresetPresentation()
{
    if (!brushPresetGrid_) {
        return;
    }
    const auto modified = brushPresetModified();
    brushPresetGrid_->setCurrentPresetId(selectedPresetId_);
    brushPresetGrid_->setCurrentPresetModified(modified);
    if (brushPresetResetButton_) {
        brushPresetResetButton_->setEnabled(modified && selectedPreset());
    }
    if (brushPresetSaveCopyButton_) {
        brushPresetSaveCopyButton_->setEnabled(true);
    }
}

bool PropertiesPanel::resetBrushPreset()
{
    if (!selectedPreset()) {
        return false;
    }
    auto settings = selectedPresetBaseline_;
    settings.foreground = brushSettings_.foreground;
    QString error;
    if (!brushAssets_->prepareSettings(settings, &error)) {
        return false;
    }
    setBrushSettings(settings);
    if (onBrushSettingsChanged) {
        onBrushSettingsChanged(brushSettings_);
    }
    return true;
}

BrushPresetSaveResult PropertiesPanel::saveBrushCopy(const QString& name)
{
    if (!onSaveBrushCopyRequested) {
        return {{}, i18n::text("properties.panel.brush.preset.storage.is.unavailable")};
    }
    auto result = onSaveBrushCopyRequested(name, brushSettings_);
    if (!result.preset) {
        return result;
    }
    selectedPresetId_ = result.preset->id;
    selectedPresetBaseline_ = result.preset->settings;
    brushPresets_.push_back(*result.preset);
    rebuildPresetGrid();
    setBrushSettings(brushSettings_);
    return result;
}

void PropertiesPanel::updateComponentControlState()
{
    const auto noGrain = brushSettings_.grain.assetId
        == core::BrushAssetIds::NoGrain;
    if (brushGrainStrengthControl_) {
        brushGrainStrengthControl_->setEnabled(!noGrain);
    }
    if (brushGrainScaleControl_) {
        brushGrainScaleControl_->setEnabled(!noGrain);
    }
    if (brushGrainAngleControl_) {
        brushGrainAngleControl_->setEnabled(!noGrain);
    }
}

QWidget* PropertiesPanel::createFillPage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(i18n::text("properties.panel.fill"), i18n::text("properties.panel.fill.the.active.layer.or.current.selection"), content);
    auto* help = i18n::make<QLabel>(i18n::text("properties.panel.toolaction.fill.fill.click.apply.the.highlighted.color.alt"));
    help->setWordWrap(true); content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createEyedropperPage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(i18n::text("properties.panel.eyedropper"), i18n::text("properties.panel.pick.the.document.color.including.alpha.hold.alt.with"), content);
    auto* help = i18n::make<QLabel>(i18n::text("properties.panel.toolaction.eyedropper.eyedropper.click.drag.sample.alt.in.brush"));
    help->setWordWrap(true);
    content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createTextPage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(i18n::text("properties.panel.text"), i18n::text("properties.panel.create.editable.text.without.baking.it.into.pixels"), content);
    auto* help=i18n::make<QLabel>(i18n::text("properties.panel.toolaction.text.click.to.create.edit.move.double.click"));
    help->setWordWrap(true);help->setObjectName(QStringLiteral("MutedLabel"));content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::addToolPage(core::ToolId tool, const i18n::Message& title,
    const i18n::Message& description, QVBoxLayout*& content)
{
    auto* page = createPageShell(title, description, content);
    pages_[tool] = page;
    stack_->addWidget(page);
    return page;
}

QWidget* PropertiesPanel::createPageShell(const i18n::Message& title,
    const i18n::Message& description, QVBoxLayout*& contentLayout)
{
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("PropertiesPageScroll"));
    scroll->verticalScrollBar()->setObjectName(QStringLiteral("PropertiesPageScrollBar"));
    scroll->verticalScrollBar()->setProperty("editorScrollBar", true);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("PropertiesPageBody"));
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(kPanelInset, kPanelInset,
        kPanelInset, kPanelBottomInset);
    bodyLayout->setSpacing(0);

    auto* controls = new QWidget(body);
    controls->setObjectName(QStringLiteral("PropertiesPageControls"));
    controls->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    contentLayout = new QVBoxLayout(controls);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(kControlSpacing);
    contentLayout->setSizeConstraint(QLayout::SetMinimumSize);
    bodyLayout->addWidget(controls, 0, Qt::AlignTop);
    bodyLayout->addStretch(1);

    auto* titleLabel = i18n::make<QLabel>(title);
    titleLabel->setObjectName(QStringLiteral("ToolTitle"));
    contentLayout->addWidget(titleLabel);
    if (!description.isEmpty()) {
        auto* descriptionLabel = i18n::make<QLabel>(description);
        descriptionLabel->setObjectName(QStringLiteral("MutedLabel"));
        descriptionLabel->setWordWrap(true);
        contentLayout->addWidget(descriptionLabel);
        contentLayout->addSpacing(kTitleToSectionsGap);
    }
    scroll->setWidget(body);
    return scroll;
}

} // namespace imageeditor::ui
