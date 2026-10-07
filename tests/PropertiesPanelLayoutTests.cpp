#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/BrushPresetGrid.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QPushButton>
#include <QWidget>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

QScrollArea* currentPage(imageeditor::ui::PropertiesPanel& panel)
{
    auto* stack = panel.findChild<QStackedWidget*>();
    CHECK(stack != nullptr);
    return stack ? qobject_cast<QScrollArea*>(stack->currentWidget()) : nullptr;
}

void everyPageUsesSharedTopAlignedShell()
{
    imageeditor::ui::PropertiesPanel panel;
    const std::array tools {
        imageeditor::core::ToolId::Move,
        imageeditor::core::ToolId::Marquee,
        imageeditor::core::ToolId::Lasso,
        imageeditor::core::ToolId::Brush,
        imageeditor::core::ToolId::Eraser,
        imageeditor::core::ToolId::Fill,
        imageeditor::core::ToolId::Eyedropper,
        imageeditor::core::ToolId::Text,
    };

    for (const auto tool : tools) {
        panel.setActiveTool(tool);
        auto* scroll = currentPage(panel);
        CHECK(scroll != nullptr);
        if (!scroll) {
            continue;
        }
        auto* body = scroll->widget();
        auto* controls = body
            ? body->findChild<QWidget*>(QStringLiteral("PropertiesPageControls"),
                Qt::FindDirectChildrenOnly)
            : nullptr;
        CHECK(body != nullptr);
        CHECK(controls != nullptr);
        if (!body || !controls || !body->layout()) {
            continue;
        }
        CHECK(body->layout()->count() == 2);
        CHECK(body->layout()->itemAt(0)->widget() == controls);
        CHECK(body->layout()->itemAt(1)->spacerItem() != nullptr);
        CHECK(controls->sizePolicy().verticalPolicy() == QSizePolicy::Maximum);
    }
}

void resizingLeavesControlsAtNaturalTopPositionAndEnablesScrolling()
{
    imageeditor::ui::PropertiesPanel panel;
    panel.setActiveTool(imageeditor::core::ToolId::Brush);
    panel.resize(340, 800);
    panel.show();
    QCoreApplication::processEvents();

    auto* scroll = currentPage(panel);
    auto* controls = scroll && scroll->widget()
        ? scroll->widget()->findChild<QWidget*>(QStringLiteral("PropertiesPageControls"),
            Qt::FindDirectChildrenOnly)
        : nullptr;
    CHECK(scroll != nullptr);
    CHECK(controls != nullptr);
    if (!scroll || !controls) {
        return;
    }
    const auto tallGeometry = controls->geometry();
    CHECK(tallGeometry.top() > 0);
    CHECK(tallGeometry.height() > 0);

    panel.resize(340, 620);
    QCoreApplication::processEvents();
    CHECK(controls->geometry().top() == tallGeometry.top());
    CHECK(controls->geometry().height() == tallGeometry.height());

    panel.resize(340, 150);
    QCoreApplication::processEvents();
    CHECK(controls->geometry().top() == tallGeometry.top());
    CHECK(controls->geometry().height() == tallGeometry.height());
    CHECK(scroll->verticalScrollBar()->maximum() > 0);
}

void brushControlsExposeTheProductionSettings()
{
    imageeditor::ui::PropertiesPanel panel;
    panel.setActiveTool(imageeditor::core::ToolId::Brush);
    auto* grid = dynamic_cast<imageeditor::ui::BrushPresetGrid*>(
        panel.findChild<QListWidget*>(QStringLiteral("BrushPresetGrid")));
    auto* grainStrength = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        panel.findChild<QDoubleSpinBox*>(QStringLiteral("BrushGrainStrengthControl")));
    auto* grainScale = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        panel.findChild<QDoubleSpinBox*>(QStringLiteral("BrushGrainScaleControl")));
    auto* grainAngle = dynamic_cast<imageeditor::ui::CompactValueControl*>(
        panel.findChild<QDoubleSpinBox*>(QStringLiteral("BrushGrainAngleControl")));
    auto* smoothing = panel.findChild<QComboBox*>(QStringLiteral("BrushSmoothingCombo"));
    auto* pressureSize = panel.findChild<QCheckBox*>(QStringLiteral("BrushPressureSize"));
    auto* pressureFlow = panel.findChild<QCheckBox*>(QStringLiteral("BrushPressureFlow"));
    CHECK(grid != nullptr);
    // The base preset registry intentionally grew beyond the original five
    // round/ink tips. The standalone panel must expose the complete registry.
    CHECK(grid && grid->count() == int(imageeditor::core::builtinBrushPresets().size()));
    CHECK(panel.findChild<QSlider*>(QStringLiteral("BrushVerticalScaleSlider")) == nullptr);
    CHECK(panel.findChild<QSlider*>(QStringLiteral("BrushFlowSlider")) == nullptr);
    CHECK(panel.findChild<QSlider*>(QStringLiteral("BrushSpacingSlider")) == nullptr);
    CHECK(panel.findChild<QWidget*>(QStringLiteral("BrushFlowControl")) == nullptr);
    CHECK(panel.findChild<QWidget*>(QStringLiteral("BrushScaleControl")) == nullptr);
    CHECK(panel.findChild<QWidget*>(QStringLiteral("BrushSpacingControl")) == nullptr);
    CHECK(grainStrength != nullptr);
    CHECK(grainScale != nullptr);
    CHECK(grainAngle != nullptr);
    CHECK(smoothing != nullptr);
    CHECK(pressureSize != nullptr);
    CHECK(pressureFlow != nullptr);
    CHECK(panel.findChild<QSlider*>(QStringLiteral("BrushSizeSlider")) == nullptr);
    CHECK(panel.findChild<QSlider*>(QStringLiteral("BrushHardnessSlider")) == nullptr);
    CHECK(panel.findChild<QSlider*>(QStringLiteral("BrushOpacitySlider")) == nullptr);
    CHECK(panel.findChild<QSpinBox*>(QStringLiteral("BrushAngleSpin")) == nullptr);
    CHECK(panel.findChild<QComboBox*>(QStringLiteral("BrushRotationCombo")) == nullptr);
    CHECK(panel.findChild<QComboBox*>(QStringLiteral("BrushPresetCombo")) == nullptr);
    CHECK(panel.findChild<QWidget*>(QStringLiteral("BrushTipPicker")) == nullptr);
    if (!grid || !grainStrength
        || !grainScale || !grainAngle || !smoothing || !pressureSize
        || !pressureFlow) {
        return;
    }

    bool hasOldDescription = false;
    for (auto* label : panel.findChildren<QLabel*>()) {
        hasOldDescription = hasOldDescription
            || label->text().contains(QStringLiteral("smooth freehand"));
    }
    CHECK(!hasOldDescription);
    CHECK(grainStrength->minimum() == 0.0 && grainStrength->maximum() == 100.0);
    CHECK(grainScale->minimum() == 8.0 && grainScale->maximum() == 512.0);
    CHECK(grainAngle->minimum() == -180.0 && grainAngle->maximum() == 180.0);
    CHECK(grainStrength->prefix() == QStringLiteral("Strength: "));
    CHECK(grainScale->prefix() == QStringLiteral("Scale: "));
    CHECK(grainAngle->prefix() == QStringLiteral("Rotation: "));
    CHECK(grainStrength->suffix() == QStringLiteral("%"));
    CHECK(grainScale->suffix() == QStringLiteral(" px"));
    CHECK(grainAngle->suffix() == QStringLiteral("°"));
    for (auto* control : {grainStrength, grainScale, grainAngle}) {
        CHECK(control->singleStep() == 1.0);
        CHECK(control->decimals() == 1);
        CHECK(!control->isEnabled());
        CHECK(control->buttonSymbols() == QAbstractSpinBox::UpDownArrows);
    }
    auto* grainBody = panel.findChild<QWidget*>(QStringLiteral("BrushGrainSectionBody"));
    CHECK(grainBody != nullptr);
    if (grainBody) {
        CHECK(grainBody->findChildren<QSlider*>().isEmpty());
        CHECK(grainBody->findChildren<QSpinBox*>().isEmpty());
        CHECK(grainBody->findChildren<QLabel*>(QStringLiteral("SectionLabel")).isEmpty());
        panel.resize(340, 800);
        panel.show();
        QCoreApplication::processEvents();
        const auto contentWidth = grainBody->contentsRect().width()
            - grainBody->layout()->contentsMargins().left()
            - grainBody->layout()->contentsMargins().right();
        for (auto* control : {grainStrength, grainScale, grainAngle}) {
            CHECK(control->width() == contentWidth);
            CHECK(control->height() == 30);
        }
    }

    auto pressurePreset = imageeditor::core::proceduralBrushPreset(
        imageeditor::core::ProceduralBrushPreset::PressureRound);
    const imageeditor::core::Rgba8 globalColor {12, 34, 56, 78};
    pressurePreset.foreground = globalColor;
    panel.setBrushSettings(pressurePreset);
    CHECK(grid->currentPresetId() == "builtin.preset.pressure-round.v1");

    // These settings now arrive from the top options page. Changing a
    // Properties-only setting must carry them through unchanged.
    pressurePreset.tip.aspectRatio = 0.36;
    pressurePreset.flow = 0.31;
    pressurePreset.spacingPercent = 18.0;
    panel.setBrushSettings(pressurePreset);

    int changes = 0;
    imageeditor::core::BrushSettings published;
    panel.onBrushSettingsChanged = [&](const imageeditor::core::BrushSettings& settings) {
        ++changes;
        published = settings;
    };
    QString error;
    CHECK(panel.selectBrushComponent(
        imageeditor::core::BrushAssetType::Grain,
        imageeditor::core::BrushAssetIds::DryInkPaperGrain, &error));
    CHECK(grainStrength->isEnabled());
    CHECK(grainScale->isEnabled());
    CHECK(grainAngle->isEnabled());
    grainStrength->setValue(54);
    grainScale->setValue(81);
    grainAngle->setValue(23);
    smoothing->setCurrentIndex(1);
    pressureSize->setChecked(false);
    pressureFlow->setChecked(false);
    CHECK(grid->currentPresetId() == "builtin.preset.pressure-round.v1");
    CHECK(grid->currentItem()
        && grid->currentItem()->text().isEmpty()
        && grid->currentItem()->toolTip().contains(QStringLiteral("Modified")));
    CHECK(changes >= 7);
    CHECK(published.sizePixels == pressurePreset.sizePixels);
    CHECK(std::abs(published.hardness - pressurePreset.hardness) < 1.0e-9);
    CHECK(std::abs(published.tip.aspectRatio - 0.36) < 1.0e-9);
    CHECK(published.tip.angleDegrees == pressurePreset.tip.angleDegrees);
    CHECK(published.tip.rotationMode == pressurePreset.tip.rotationMode);
    CHECK(published.tip.assetId == pressurePreset.tip.assetId);
    CHECK(std::abs(published.opacity - pressurePreset.opacity) < 1.0e-9);
    CHECK(std::abs(published.flow - 0.31) < 1.0e-9);
    CHECK(published.spacingPercent == 18.0);
    CHECK(std::abs(published.grain.strength - 0.54) < 1.0e-9);
    CHECK(published.grain.scalePixels == 81.0);
    CHECK(published.grain.angleDegrees == 23.0);
    CHECK(published.grain.assetId
        == imageeditor::core::BrushAssetIds::DryInkPaperGrain);
    CHECK(published.smoothing == imageeditor::core::BrushSmoothingMode::Weighted);
    CHECK(!published.pressureToSize);
    CHECK(!published.pressureToFlow);

    const auto selectPreset = [grid](const QString& id) {
        for (int row = 0; row < grid->count(); ++row) {
            auto* item = grid->item(row);
            if (item->data(Qt::UserRole).toString() == id) {
                grid->setCurrentItem(item);
                return true;
            }
        }
        return false;
    };
    CHECK(selectPreset(QStringLiteral("builtin.preset.hard-round.v1")));
    CHECK(published.foreground == globalColor);
    CHECK(grid->currentPresetId() == "builtin.preset.hard-round.v1");

    CHECK(selectPreset(QStringLiteral("builtin.preset.dry-ink.v1")));
    CHECK(published.tip.assetId
        == imageeditor::core::BrushAssetIds::DryInkMaskTip);
    CHECK(published.grain.assetId
        == imageeditor::core::BrushAssetIds::DryInkPaperGrain);
    CHECK(published.foreground == globalColor);
}

void grainManualDecimalEntrySurvivesSettingsSynchronization()
{
    imageeditor::ui::PropertiesPanel panel;
    panel.setActiveTool(imageeditor::core::ToolId::Brush);
    panel.resize(340, 800);
    panel.show();
    QCoreApplication::processEvents();
    QString error;
    CHECK(panel.selectBrushComponent(imageeditor::core::BrushAssetType::Grain,
        imageeditor::core::BrushAssetIds::DryInkPaperGrain, &error));

    imageeditor::core::BrushSettings published;
    int changes = 0;
    panel.onBrushSettingsChanged = [&](const imageeditor::core::BrushSettings& settings) {
        ++changes;
        published = settings;
        // MainWindow immediately echoes the session's brush settings to both
        // settings pages. This must preserve the active editor's partial text.
        panel.setBrushSettings(settings);
    };
    const auto sendKey = [](QWidget* target, int key, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(target, &release);
    };
    for (const auto& name : {QStringLiteral("BrushGrainStrengthControl"),
             QStringLiteral("BrushGrainScaleControl"),
             QStringLiteral("BrushGrainAngleControl")}) {
        auto* control = dynamic_cast<imageeditor::ui::CompactValueControl*>(
            panel.findChild<QDoubleSpinBox*>(name));
        auto* editor = control ? control->findChild<QLineEdit*>() : nullptr;
        CHECK(control != nullptr && editor != nullptr);
        if (!control || !editor) {
            continue;
        }
        control->setFocus(Qt::OtherFocusReason);
        sendKey(control, Qt::Key_8, QStringLiteral("8"));
        CHECK(control->isManualEntryActive());
        CHECK(editor->text() == control->prefix() + QStringLiteral("8") + control->suffix());
        sendKey(control, Qt::Key_Period, QStringLiteral("."));
        CHECK(editor->text() == control->prefix() + QStringLiteral("8.") + control->suffix());
        sendKey(control, Qt::Key_5, QStringLiteral("5"));
        CHECK(editor->text() == control->prefix() + QStringLiteral("8.5") + control->suffix());
        CHECK(control->value() == 8.5);
        sendKey(control, Qt::Key_Return);
        CHECK(!control->isManualEntryActive());
        CHECK(control->value() == 8.5);
    }
    CHECK(changes >= 6);
    CHECK(std::abs(published.grain.strength - 0.085) < 1.0e-9);
    CHECK(published.grain.scalePixels == 8.5);
    CHECK(published.grain.angleDegrees == 8.5);
    CHECK(panel.brushPresetModified());
    CHECK(panel.resetBrushPreset());
    CHECK(!panel.brushPresetModified());
    CHECK(published.grain.assetId == imageeditor::core::BrushAssetIds::NoGrain);
    CHECK(published.grain.strength == 0.0);
    for (auto* control : panel.findChildren<QDoubleSpinBox*>()) {
        CHECK(!control->isEnabled());
    }
}

void brushPresetGridReflowsAtFixedCellSize()
{
    imageeditor::ui::PropertiesPanel panel;
    panel.setActiveTool(imageeditor::core::ToolId::Brush);
    auto* grid = dynamic_cast<imageeditor::ui::BrushPresetGrid*>(
        panel.findChild<QListWidget*>(QStringLiteral("BrushPresetGrid")));
    CHECK(grid != nullptr);
    if (!grid) {
        return;
    }
    const auto fixedCell = grid->gridSize();
    CHECK(fixedCell == QSize(66, 66));
    CHECK(grid->iconSize() == QSize(56, 56));
    CHECK(grid->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    CHECK(grid->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    CHECK(!grid->wordWrap());
    CHECK(grid->textElideMode() == Qt::ElideRight);
    for (int row = 0; row < grid->count(); ++row) {
        CHECK(grid->item(row)->text().isEmpty());
        CHECK(!grid->item(row)->toolTip().isEmpty());
        CHECK(!grid->item(row)->data(Qt::AccessibleTextRole).toString().isEmpty());
    }

    panel.resize(230, 720);
    panel.show();
    QCoreApplication::processEvents();
    const auto narrowHeight = grid->height();
    const auto narrowFourth = grid->visualItemRect(grid->item(3));
    CHECK(narrowFourth.top() >= grid->gridSize().height());

    panel.resize(380, 720);
    QCoreApplication::processEvents();
    CHECK(grid->gridSize() == fixedCell);
    CHECK(grid->height() < narrowHeight);
    CHECK(grid->visualItemRect(grid->item(3)).top()
        < grid->gridSize().height());
    CHECK(grid->currentPresetId() == "builtin.preset.pressure-round.v1");
    if (const auto path = qEnvironmentVariable("IMAGEEDITOR_BRUSH_GRID_REVIEW"); !path.isEmpty())
        CHECK(grid->grab().save(path));
}

void toolPagesHaveNoDedicatedColorControls()
{
    imageeditor::ui::PropertiesPanel panel;
    CHECK(panel.findChild<QPushButton*>(
        QStringLiteral("BrushColorButton")) == nullptr);
    for (const auto tool : {imageeditor::core::ToolId::Brush,
             imageeditor::core::ToolId::Fill,
             imageeditor::core::ToolId::Text}) {
        panel.setActiveTool(tool);
        auto* page = currentPage(panel);
        CHECK(page != nullptr);
    }
}

void colorPanelPresentsOneTopAlignedGlobalControl()
{
    imageeditor::ui::ColorPanel panel;
    panel.resize(320, 160);
    panel.show();
    QCoreApplication::processEvents();
    auto* button = panel.findChild<QPushButton*>(
        QStringLiteral("ForegroundColorButton"));
    CHECK(button != nullptr);
    CHECK(button && button->text().contains(QStringLiteral("#4F73FF")));
    CHECK(button && button->geometry().top() <= 16);

    int notifications = 0;
    panel.onColorsChanged = [&](imageeditor::core::EditorColors) {
        ++notifications;
    };
    const imageeditor::core::Rgba8 color {12, 34, 56, 78};
    panel.setForegroundColor(color);
    CHECK(panel.foregroundColor() == color);
    CHECK(button && button->text().contains(QStringLiteral("#0C2238")));
    CHECK(button && button->accessibleName().contains(QStringLiteral("#0C2238")));
    CHECK(notifications == 0);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    imageeditor::ui::applyEditorTheme(application);
    everyPageUsesSharedTopAlignedShell();
    resizingLeavesControlsAtNaturalTopPositionAndEnablesScrolling();
    brushControlsExposeTheProductionSettings();
    grainManualDecimalEntrySurvivesSettingsSynchronization();
    brushPresetGridReflowsAtFixedCellSize();
    toolPagesHaveNoDedicatedColorControls();
    colorPanelPresentsOneTopAlignedGlobalControl();

    if (failures != 0) {
        std::cerr << failures << " properties layout assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All properties layout tests passed\n";
    return EXIT_SUCCESS;
}
