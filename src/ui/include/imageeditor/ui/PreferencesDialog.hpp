#pragma once
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include "imageeditor/platform/PerformanceSettings.hpp"
#include "imageeditor/platform/McpSettings.hpp"
#include "imageeditor/ui/LayerSelectionFocus.hpp"
#include <QDialog>
#include <functional>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QDoubleSpinBox;
namespace imageeditor::ui {
class ShortcutsPanel;
class PerformancePanel;
class McpPanel;
// Dialog-local snapshot: preview and rollback cover all preferences together.
struct PreferencesState {
    ThemeSettings theme;
    bool advancedMeasurementReadout = false;
    bool layerOutlinesVisible = true;
    bool collapseLayerSelectionOnEmptyClick = true;
    int shiftNudgePixels = 10;
    ShortcutBindings shortcuts = defaultShortcutBindings();
    int toolHintPosition = 1; // Left, Center (default), Disabled; UI-only preference.
    QString language = QStringLiteral("system");
    platform::PerformanceSettings performance {};
    bool layerInfoVisible = true;
    platform::McpSettings mcp {};
    LayerSelectionFocus layerSelectionFocus = LayerSelectionFocus::Focus;
    bool operator==(const PreferencesState&) const = default;
};
class PreferencesDialog final : public QDialog {
public:
    static constexpr int ProjectFilesRequested = 100;
    explicit PreferencesDialog(PreferencesState, QWidget* parent = nullptr);
    [[nodiscard]] const PreferencesState& draft() const { return draft_; }
    std::function<void(const PreferencesState&)> onPreview;
    std::function<bool(const PreferencesState&)> onApply;
    std::function<bool()> confirmUndoReduction;
    std::function<std::uint64_t()> currentUndoBudget;
    PerformancePanel* performancePanel() const { return performance_; }
    McpPanel* mcpPanel() const { return mcp_; }
    void reject() override;
private:
    void refresh();
    void preview();
    bool apply();
    void pickColor(ThemeColor);
    PreferencesState draft_, applied_;
    QComboBox* preset_ {};
    QComboBox* language_ {};
    QCheckBox* advancedMeasurementReadout_ {};
    QCheckBox* layerOutlinesVisible_ {};
    QCheckBox* layerInfoVisible_ {};
    QComboBox* layerSelectionFocus_ {};
    QCheckBox* collapseLayerSelectionOnEmptyClick_ {};
    QDoubleSpinBox* shiftNudgePixels_ {};
    QComboBox* toolHintPosition_ {};
    QLabel* status_ {};
    QPushButton* customize_ {};
    ShortcutsPanel* shortcuts_ {};
    PerformancePanel* performance_ {};
    McpPanel* mcp_ {};
    std::array<QPushButton*, static_cast<std::size_t>(ThemeColor::Count)> swatches_ {};
};
}
