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

QLabel* sectionLabel(const QString& text)
{
    auto* label = new QLabel(text.toUpper());
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
    const QString& label, double minimum, double maximum, double value,
    const QString& suffix)
{
    auto* control = new CompactValueControl;
    control->setObjectName(objectName);
    control->setRange(minimum, maximum);
    control->setDecimals(1);
    control->setValue(value);
    control->setPrefix(label + QStringLiteral(": "));
    control->setSuffix(suffix);
    control->setSingleStep(1.0);
    control->setFixedHeight(30);
    control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    control->setAccessibleName(label);
    return control;
}

QWidget* collapsibleSection(const QString& title,
    const QString& objectName, QVBoxLayout*& sectionLayout)
{
    auto* container = new QWidget;
    container->setObjectName(objectName + QStringLiteral("Section"));
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kControlSpacing);

    auto* header = new QToolButton(container);
    header->setObjectName(objectName + QStringLiteral("SectionHeader"));
    header->setText(title);
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
    pages_[core::ToolId::Marquee] = createSelectionPage(
        QStringLiteral("Select"),
        QStringLiteral("Create and combine selections. {{LayerTransformAction}} transforms the selection mask, not the layer."));
    pages_[core::ToolId::Lasso] = createSelectionPage(
        QStringLiteral("Lasso"),
        QStringLiteral("Freehand, Polygonal and Magnetic share one selection mask. {{LayerTransformAction}} transforms the mask, not the layer."));
    pages_[core::ToolId::Brush] = createBrushPage(QStringLiteral("Brush"));
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
            selectedLayerType_->setText(container->kind == core::ContainerKind::ClippingMaskGroup
                ? tr("Clipping mask group · The bottommost child is the base. Upper members share its coverage.")
                : container->kind == core::ContainerKind::Group
                ? tr("Pass-through group · Move/Transform affects every member, including hidden layers.")
                : tr("Organizational folder · select its layers to move or transform content."));
            return;
        }
        selectedLayerName_->setText(QStringLiteral("No layer selected"));
        selectedLayerType_->clear();
        return;
    }
    selectedLayerName_->setText(QString::fromUtf8(layer->name));
    selectedLayerType_->setText(std::holds_alternative<core::RasterLayer>(layer->payload)
        ? QStringLiteral("Raster layer") : std::holds_alternative<core::ShapeLayer>(layer->payload)
        ? QStringLiteral("Editable shape layer") : std::holds_alternative<core::AdjustmentLayer>(layer->payload)
        ? tr("Adjustment layer · edits the lower composite. Select its mask thumbnail to paint coverage.") : QStringLiteral("Editable text layer"));
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
    auto* page = createPageShell(QStringLiteral("Transform"),
        QStringLiteral("Selection boundary, selected pixels, or layer geometry — according to the command used."), content);
    auto* help = new QLabel(QStringLiteral(
        "{{LayerTransformAction}} · Transform the selection mask\n"
        "{{TransformSelectedPixelsAction}} · Cut and transform primary-layer pixels\n"
        "{{NudgeLeftAction}} / {{NudgeRightAction}} / {{NudgeUpAction}} / {{NudgeDownAction}} · Move 1 px; Shift + nudge · Larger step (Preferences)\n"
        "Drag inside · Move; handles · Scale; outside corners · Rotate\n"
        "Ctrl + corner · Distort; Ctrl while moving · Bypass snapping\n"
        "Shift · Toggle ratio lock / snap rotation to 15°\nAlt · Scale from center\n"
        "{{UndoAction}} / {{RedoAction}} · Undo / redo preview changes\n"
        "{{FinishOperationAction}} / right-click · Apply\nEscape · Cancel session"));
    help->setObjectName(QStringLiteral("MutedLabel"));
    help->setWordWrap(true);
    content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createMovePage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(QStringLiteral("Move"),
        QStringLiteral("Under mouse picks a layer; Active layer keeps the current target."), content);
    content->addWidget(sectionLabel(QStringLiteral("Target")));
    selectedLayerName_ = new QLabel(QStringLiteral("No layer selected"));
    selectedLayerName_->setObjectName(QStringLiteral("PropertiesTargetName"));
    selectedLayerName_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    selectedLayerType_ = new QLabel;
    selectedLayerType_->setObjectName(QStringLiteral("MutedLabel"));
    selectedLayerType_->setWordWrap(true);
    content->addWidget(selectedLayerName_);
    content->addWidget(selectedLayerType_);
    content->addSpacing(kSectionBreak);
    content->addWidget(sectionLabel(QStringLiteral("Controls & shortcuts")));
    auto* help = new QLabel(QStringLiteral(
        "{{ToolAction_move}} · Move\nClick · Select layer; drag · Move selected layers\n"
        "Alt+drag · Duplicate and move selected layers\n"
        "Ctrl while dragging · Bypass snapping (View → Snapping / Snap To)\n"
        "{{NudgeLeftAction}} / {{NudgeRightAction}} / {{NudgeUpAction}} / {{NudgeDownAction}} · Move 1 px; Shift + nudge · Larger step (Preferences)\n"
        "Shift-click canvas · Add/remove layer\n"
        "Layers panel: Ctrl-click · Toggle; Shift-click · Range; arrows · Navigate\n"
        "Click canvas · Return arrows to movement\n"
        "{{RenameLayerItemAction}} · Rename\nH / {{ShowSelectedLayersAction}} · Hide / show selected\n"
        "{{DuplicateLayersAction}} · Duplicate selected; {{DeleteSelectedLayersAction}} · Delete selected layers\n"
        "{{IsolateSelectedLayersAction}} · Isolate; {{ShowAllLayersAction}} · Show all\nO · Toggle selected-layer outlines\n\n"
        "{{LayerTransformAction}} · Transform selected layers\nHandles · Scale; outside corners · Rotate\n"
        "Ctrl + corner · Distort (keeps text/shapes editable)\n"
        "Shift · Toggle ratio lock / snap rotation to 15°\nAlt · Scale from center\n"
        "{{FinishOperationAction}} / right-click · Apply transform\nEscape · Cancel drag or transform session\n"
        "{{UndoAction}} / {{RedoAction}} · Undo / redo\n\n"
        "{{PanCanvasAction}} + left-drag / middle-drag · Pan\nWheel · Zoom; {{FitCanvasAction}} · Fit; {{ActualSizeAction}} · 100%"));
    help->setObjectName(QStringLiteral("MutedLabel"));
    help->setWordWrap(true);
    content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createSelectionPage(
    const QString& title, const QString& description)
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(title, description, content);
    const auto intro = title == QStringLiteral("Lasso")
        ? QStringLiteral("{{ToolAction_lasso}} · Lasso\nFreehand · Drag to trace; release to close\n"
            "Polygonal · Click vertices\nMagnetic · Follow edges; click to anchor\n"
            "Ctrl-click (Magnetic) · Straight segment\n"
            "Polygonal / Magnetic: {{FinishOperationAction}}, right-click, double-click or click start · Close\n"
            "{{RemovePointAction}} · Remove last anchor\n\n")
        : QStringLiteral("{{ToolAction_marquee}} · Select; choose Rectangle or Ellipse beside the tool name\n"
            "Drag · Draw selection\nShift · Constrain square/circle\n"
            "With an existing selection, starting with Shift adds instead. Release and press Shift again while dragging to constrain.\n\n");
    auto* hints = new QLabel(intro + QStringLiteral(
        "At press: Shift · Add; Alt · Subtract; Shift+Alt · Intersect\n"
        "Replace mode: drag selected coverage · Move mask\nEscape · Cancel\n"
        "{{NudgeLeftAction}} / {{NudgeRightAction}} / {{NudgeUpAction}} / {{NudgeDownAction}} · Move mask 1 px; Shift + nudge · Larger step (Preferences)\n"
        "{{ErasePixelsAction}} · Erase selected pixels; without a selection, clear the primary raster layer\n"
        "{{ReselectAction}} · Reselect last selection (also saved in projects)\n"
        "{{SelectAllAction}} · Select all; {{DeselectAction}} · Deselect\n{{InvertSelectionAction}} · Invert; {{LayerViaCopyAction}} · Layer via Copy\n"
        "{{LayerTransformAction}} · Transform mask; {{FinishOperationAction}} / right-click · Apply\n"
        "Adjustment icon · Grow/shrink per side or rotate mask\n"
        "{{UndoAction}} / {{RedoAction}} · Undo / redo\n"
        "{{FillForegroundAction}} / {{FillBackgroundAction}} · Foreground / background fill\n\n"
        "{{PanCanvasAction}} + left-drag / middle-drag · Pan\nWheel · Zoom; {{FitCanvasAction}} · Fit; {{ActualSizeAction}} · 100%"));
    hints->setWordWrap(true);
    content->addWidget(hints);
    return page;
}

QWidget* PropertiesPanel::createBrushPage(const QString& title)
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(title, QString {}, content);

    cloningHelp_=new QLabel(QStringLiteral(
        "{{ToolAction_cloning}} · Cloning\nAlt-click · Set Stamp/Heal source; drag · Paint\n"
        "Stamp copies; Heal blends texture with surrounding tone.\n"
        "Spot Heal · Mark the whole defect; release to repair (no source anchor)\n"
        "{{DecreaseBrushSizeAction}} / {{IncreaseBrushSizeAction}} · Size; Shift · Constrain direction\nEscape / Cancel · Cancel repair\n"
        "Aligned keeps the source offset; off restarts at the anchor.\n"
        "Raw preserves intrinsic colors/alpha. Rendered repair: use a clean Normal, 100%-opacity retouch layer over opaque content.\n"
        "Adaptation adjusts tone, not coverage. Brush controls below are shared."));
    cloningHelp_->setObjectName(QStringLiteral("CloningHelp"));
    cloningHelp_->setWordWrap(true);cloningHelp_->hide();content->addWidget(cloningHelp_);
    localBlurHelp_=new QLabel(QStringLiteral(
        "{{ToolAction_local_blur}} · Local Blur\nDrag · Soften raster colors; alpha stays unchanged\n"
        "{{DecreaseBrushSizeAction}} / {{IncreaseBrushSizeAction}} · Size; Shift · Constrain direction\nEscape · Cancel stroke\n"
        "Strength limits the effect; Flow controls buildup; Radius sets the blur neighborhood in document pixels.\n"
        "Displayed adjustments/filters are not baked in. Brush controls below are shared."));
    localBlurHelp_->setObjectName(QStringLiteral("LocalBlurHelp"));
    localBlurHelp_->setWordWrap(true);localBlurHelp_->hide();content->addWidget(localBlurHelp_);

    brushPresetGrid_ = new BrushPresetGrid;
    content->addWidget(brushPresetGrid_);

    auto* presetActions = new QWidget;
    auto* presetActionLayout = new QHBoxLayout(presetActions);
    presetActionLayout->setContentsMargins(0, 0, 0, 0);
    presetActionLayout->setSpacing(6);
    brushPresetResetButton_ = new QPushButton(QStringLiteral("Reset"));
    brushPresetResetButton_->setObjectName(
        QStringLiteral("BrushPresetResetButton"));
    brushPresetSaveCopyButton_ = new QPushButton(QStringLiteral("Save Copy"));
    brushPresetSaveCopyButton_->setObjectName(
        QStringLiteral("BrushPresetSaveCopyButton"));
    presetActionLayout->addStretch(1);
    presetActionLayout->addWidget(brushPresetResetButton_);
    presetActionLayout->addWidget(brushPresetSaveCopyButton_);
    content->addWidget(presetActions);

    content->addWidget(sectionLabel(QStringLiteral("Smoothing")));
    brushSmoothingCombo_ = new QComboBox;
    brushSmoothingCombo_->setObjectName(QStringLiteral("BrushSmoothingCombo"));
    brushSmoothingCombo_->addItems({QStringLiteral("None"),
        QStringLiteral("Light weighted")});
    content->addWidget(brushSmoothingCombo_);

    content->addWidget(sectionLabel(QStringLiteral("Pressure")));
    brushPressureSize_ = new QCheckBox(QStringLiteral("Pressure controls size"));
    brushPressureFlow_ = new QCheckBox(QStringLiteral("Pressure controls flow"));
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
    seed->setToolTip(QStringLiteral("Repeatable stamp variation and grain placement. Saved by Save Copy."));
    seed->setAccessibleName(QStringLiteral("Variation seed"));
    seed->restore = [this] { brushSeed_->setText(QString::number(brushSettings_.deterministicSeed)); };
    auto* reseed = new QPushButton(QStringLiteral("Reseed"));
    reseed->setObjectName(QStringLiteral("BrushReseedButton"));
    variation->addWidget(new QLabel(QStringLiteral("Seed")));
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
    auto* grainSection = collapsibleSection(
        QStringLiteral("Texture / Grain"), QStringLiteral("BrushGrain"),
        grainLayout);
    brushGrainPicker_ = new BrushComponentPicker(core::BrushAssetType::Grain);
    brushGrainPicker_->setItems(
        brushAssets_->components(core::BrushAssetType::Grain));
    grainLayout->addWidget(brushGrainPicker_);
    brushGrainStrengthControl_ = grainValueControl(
        QStringLiteral("BrushGrainStrengthControl"), QStringLiteral("Strength"),
        0.0, 100.0, 0.0, QStringLiteral("%"));
    brushGrainScaleControl_ = grainValueControl(
        QStringLiteral("BrushGrainScaleControl"), QStringLiteral("Scale"),
        8.0, 512.0, 96.0, QStringLiteral(" px"));
    brushGrainAngleControl_ = grainValueControl(
        QStringLiteral("BrushGrainAngleControl"), QStringLiteral("Rotation"),
        -180.0, 180.0, 0.0, QStringLiteral("°"));
    grainLayout->addWidget(brushGrainStrengthControl_);
    grainLayout->addWidget(brushGrainScaleControl_);
    grainLayout->addWidget(brushGrainAngleControl_);
    content->addWidget(grainSection);
    QVBoxLayout* shortcutLayout=nullptr;
    auto* shortcuts=collapsibleSection(QStringLiteral("Shortcuts"),QStringLiteral("BrushShortcuts"),shortcutLayout);
    auto* help=new QLabel(QStringLiteral("{{ToolAction_brush}} selects Brush · {{ToolAction_eraser}} toggles Erase\n{{DecreaseBrushSizeAction}} / {{IncreaseBrushSizeAction}} change size\n"
        "Hold Shift to lock a straight 45°-increment direction; release for freehand\n"
        "Alt temporarily samples color · {{SwapColorsAction}} switches the active swatch\nEscape cancels a stroke\n"
        "{{UndoAction}} / {{RedoAction}} undo / redo\n{{PanCanvasAction}}-drag or middle-drag pans; wheel zooms\n{{FitCanvasAction}} fits · {{ActualSizeAction}} shows 100%"));
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
                : QStringLiteral("Custom Brush");
            bool accepted = false;
            const auto name = QInputDialog::getText(this,
                QStringLiteral("Save Brush Preset Copy"),
                QStringLiteral("Preset name"), QLineEdit::Normal,
                suggested, &accepted);
            if (!accepted) {
                return;
            }
            const auto result = saveBrushCopy(name);
            if (!result.preset) {
                QMessageBox::warning(this,
                    QStringLiteral("Unable to Save Brush Preset"),
                    result.error);
            }
        });
    brushGrainPicker_->onAssetSelected = [this](const std::string& id) {
        QString error;
        if (!selectBrushComponent(core::BrushAssetType::Grain, id, &error)) {
            QMessageBox::warning(this, QStringLiteral("Brush Grain Unavailable"),
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
            *error = QStringLiteral("Unknown brush asset: %1")
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
        QMessageBox::warning(this, QStringLiteral("Brush Preset Unavailable"),
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
        return {{}, QStringLiteral("Brush preset storage is unavailable.")};
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
    auto* page = createPageShell(QStringLiteral("Fill"),
        QStringLiteral("Fill the active layer or current selection."), content);
    auto* help = new QLabel(QStringLiteral(
        "{{ToolAction_fill}} · Fill; click · Apply the highlighted color\nAlt · Temporary eyedropper\n"
        "{{SwapColorsAction}} · Switch color swatch\n"
        "{{FillForegroundAction}} / {{FillBackgroundAction}} · Fill with foreground / background\n"
        "Escape · Cancel pending fill\n{{UndoAction}} / {{RedoAction}} · Undo / redo\n\n"
        "Selection / Layer fills every selected region; no selection fills the layer inside the canvas. "
        "An empty selection does nothing. Contiguous fills connected matching pixels; tolerance 0 is exact, 255 accepts all colors. "
        "Both respect selection coverage and use the top-bar opacity.\n\n"
        "{{PanCanvasAction}} + left-drag / middle-drag · Pan\nWheel · Zoom; {{FitCanvasAction}} · Fit; {{ActualSizeAction}} · 100%"));
    help->setWordWrap(true); content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createEyedropperPage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(QStringLiteral("Eyedropper"),
        QStringLiteral("Pick the document color, including alpha. Hold Alt with Brush, Eraser or Fill for temporary picking."), content);
    auto* help = new QLabel(QStringLiteral(
        "{{ToolAction_eyedropper}} · Eyedropper; click / drag · Sample\nAlt in Brush / Eraser / Fill · Temporary picker\n"
        "{{SwapColorsAction}} · Switch color swatch\nEscape · End picking\n\n"
        "Merged Visible samples the visible document. Active Layer ignores its visibility and opacity. "
        "Samples retain alpha. The ring shows hovered color above, current color below.\n\n"
        "{{PanCanvasAction}} + left-drag / middle-drag · Pan\nWheel · Zoom; {{FitCanvasAction}} · Fit; {{ActualSizeAction}} · 100%"));
    help->setWordWrap(true);
    content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::createTextPage()
{
    QVBoxLayout* content = nullptr;
    auto* page = createPageShell(QStringLiteral("Text"),
        QStringLiteral("Create editable text without baking it into pixels."),
        content);
    auto* help=new QLabel(QStringLiteral("{{ToolAction_text}} · Click to create / edit; Move double-click · Edit text\n"
        "Edit text button · Reopen selected text, including empty layers\n\n"
        "Select characters to format them. With no selection, controls change only what you type next. Font sizes are document pixels.\n\n"
        "Enter · New line\n{{FinishTextAction}} / Done · Finish\nEscape · Cancel composition first, otherwise finish (keep edits)\n"
        "Ctrl+A/C/X/V · Select all / Copy / Cut / Paste\n{{UndoAction}} / {{RedoAction}} · Undo / Redo\n"
        "Shift+arrows · Select\nCtrl+arrows · Navigate words\n\n"
        "Click outside the edit area · Finish, without creating new text\n\n"
        "After editing: {{ToolAction_move}} · Move; {{LayerTransformAction}} · Transform\nText color stays independent of the paint colors."));
    help->setWordWrap(true);help->setObjectName(QStringLiteral("MutedLabel"));content->addWidget(help);
    return page;
}

QWidget* PropertiesPanel::addToolPage(core::ToolId tool, const QString& title,
    const QString& description, QVBoxLayout*& content)
{
    auto* page = createPageShell(title, description, content);
    pages_[tool] = page;
    stack_->addWidget(page);
    return page;
}

QWidget* PropertiesPanel::createPageShell(const QString& title,
    const QString& description, QVBoxLayout*& contentLayout)
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

    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("ToolTitle"));
    contentLayout->addWidget(titleLabel);
    if (!description.isEmpty()) {
        auto* descriptionLabel = new QLabel(description);
        descriptionLabel->setObjectName(QStringLiteral("MutedLabel"));
        descriptionLabel->setWordWrap(true);
        contentLayout->addWidget(descriptionLabel);
        contentLayout->addSpacing(kTitleToSectionsGap);
    }
    scroll->setWidget(body);
    return scroll;
}

} // namespace imageeditor::ui
