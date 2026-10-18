#pragma once
#include "imageeditor/ui/Localization.hpp"

#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/BrushAssetRegistry.hpp"
#include "imageeditor/ui/BrushPresetStore.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include "imageeditor/core/BoundsSnapping.hpp"
#include "imageeditor/core/RichText.hpp"

#include <QWidget>

#include <map>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

class QLabel;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace imageeditor::ui {

class BrushAssetLibrary;
class BrushComponentPicker;
class BrushPresetGrid;
class CompactValueControl;
class LayerInfoPanel;
class ToolShortcutHints;

class PropertiesPanel final : public QWidget {
public:
    explicit PropertiesPanel(
        std::shared_ptr<BrushAssetLibrary> brushAssets = {},
        QWidget* parent = nullptr);

    void setActiveTool(core::ToolId tool);
    void setSelectedLayer(const core::Layer* layer, const core::LayerContainer* container = nullptr,
        std::optional<core::DocumentBounds> groupBounds = {});
    void setShortcutBindings(const ShortcutBindings&);
    // Cached contextual pages use the same scrolling/top-aligned shell.
    QWidget* addToolPage(core::ToolId tool, const i18n::Message& title,
        const i18n::Message& description, QVBoxLayout*& content);
    void setBrushSettings(const core::BrushSettings& settings);
    void setBrushPresets(std::vector<core::BrushPresetRecord> presets);
    [[nodiscard]] bool selectBrushComponent(core::BrushAssetType type,
        std::string_view assetId, QString* error = nullptr);
    [[nodiscard]] bool resetBrushPreset();
    [[nodiscard]] BrushPresetSaveResult saveBrushCopy(const QString& name);
    [[nodiscard]] bool brushPresetModified() const noexcept;

    std::function<void(const core::BrushSettings&)> onBrushSettingsChanged;
    std::function<void(core::LayerId, core::TextStylePatch)> onTextFormat;
    std::function<void(core::LayerId)> onTextColor;
    std::function<BrushPresetSaveResult(const QString&,
        const core::BrushSettings&)> onSaveBrushCopyRequested;

private:
    QWidget* createMovePage();
    QWidget* createSelectionTransformPage();
    QWidget* createSelectionPage(core::ToolId tool, const i18n::Message& title, const i18n::Message& description);
    QWidget* createBrushPage(const i18n::Message& title);
    QWidget* createFillPage();
    QWidget* createEyedropperPage();
    QWidget* createTextPage();
    QWidget* createPageShell(const i18n::Message& title, const i18n::Message& description,
        QVBoxLayout*& contentLayout);
    void publishBrushSettings(CompactValueControl* source = nullptr);
    void applyPresetById(std::string_view id);
    void rebuildPresetGrid();
    void updatePresetPresentation();
    void updateComponentControlState();
    [[nodiscard]] const core::BrushPresetRecord* selectedPreset() const noexcept;

    std::shared_ptr<BrushAssetLibrary> brushAssets_;
    QStackedWidget* stack_ {nullptr};
    LayerInfoPanel* layerInfo_{};
    ToolShortcutHints* shortcutHints_{};
    struct PageSections {QVBoxLayout *root{}, *info{}, *shortcuts{};};
    std::map<QWidget*, PageSections> sections_;
    BrushPresetGrid* brushPresetGrid_ {nullptr};
    QComboBox* brushSmoothingCombo_ {nullptr};
    QLineEdit* brushSeed_ {nullptr};
    CompactValueControl* brushGrainStrengthControl_ {nullptr};
    CompactValueControl* brushGrainScaleControl_ {nullptr};
    CompactValueControl* brushGrainAngleControl_ {nullptr};
    CompactValueControl* publishingControl_ {nullptr};
    QCheckBox* brushPressureSize_ {nullptr};
    QCheckBox* brushPressureFlow_ {nullptr};
    BrushComponentPicker* brushGrainPicker_ {nullptr};
    QPushButton* brushPresetResetButton_ {nullptr};
    QPushButton* brushPresetSaveCopyButton_ {nullptr};
    core::BrushSettings brushSettings_;
    core::BrushSettings selectedPresetBaseline_;
    std::string selectedPresetId_;
    std::vector<core::BrushPresetRecord> brushPresets_;
    bool updatingBrushUi_ {false};
    std::map<core::ToolId, QWidget*> pages_;
};

} // namespace imageeditor::ui
