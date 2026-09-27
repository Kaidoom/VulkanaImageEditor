#pragma once
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include <QDialog>
#include <functional>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QDoubleSpinBox;
namespace imageeditor::ui {
class ShortcutsPanel;
// Dialog-local snapshot: preview and rollback cover all preferences together.
struct PreferencesState {
    ThemeSettings theme;
    bool advancedMeasurementReadout = false;
    bool layerOutlinesVisible = true;
    bool collapseLayerSelectionOnEmptyClick = true;
    int shiftNudgePixels = 10;
    ShortcutBindings shortcuts = defaultShortcutBindings();
    int toolHintPosition = 1; // Left, Center (default), Disabled; UI-only preference.
    bool operator==(const PreferencesState&) const = default;
};
class PreferencesDialog final : public QDialog {
public:
    explicit PreferencesDialog(PreferencesState, QWidget* parent = nullptr);
    [[nodiscard]] const PreferencesState& draft() const { return draft_; }
    std::function<void(const PreferencesState&)> onPreview;
    std::function<bool(const PreferencesState&)> onApply;
    void reject() override;
private:
    void refresh();
    void preview();
    bool apply();
    void pickColor(ThemeColor);
    PreferencesState draft_, applied_;
    QComboBox* preset_ {};
    QCheckBox* advancedMeasurementReadout_ {};
    QCheckBox* layerOutlinesVisible_ {};
    QCheckBox* collapseLayerSelectionOnEmptyClick_ {};
    QDoubleSpinBox* shiftNudgePixels_ {};
    QComboBox* toolHintPosition_ {};
    QLabel* status_ {};
    QPushButton* customize_ {};
    ShortcutsPanel* shortcuts_ {};
    std::array<QPushButton*, static_cast<std::size_t>(ThemeColor::Count)> swatches_ {};
};
}
