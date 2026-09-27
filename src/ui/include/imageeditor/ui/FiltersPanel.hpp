#pragma once
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/SpatialFilters.hpp"
#include <QWidget>
#include <array>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QStackedWidget;
class QVBoxLayout;
class QHBoxLayout;
namespace imageeditor::ui {
class CompactValueControl;
class FiltersPanel final : public QWidget {
public:
    explicit FiltersPanel(QWidget* parent = nullptr);
    void setTarget(const core::Layer*, bool hasSelection);
    void finishEditing(bool commit = true);
    void selectType(core::SpatialFilterType);
    void setEmbedded();
    void resetAll();
    [[nodiscard]] core::SpatialFilterType currentType() const;
    [[nodiscard]] std::optional<core::LayerId> target() const { return target_; }
    [[nodiscard]] bool interactionActive() const { return editing_; }
    [[nodiscard]] bool capturedRegionVisible() const { return showCaptured_ && isVisible(); }
    void setProcessing(bool busy, double progress = 0, const QString& message = {});
    std::function<bool(core::SpatialFilterType)> onInteractionStarted;
    std::function<void(core::SpatialFilterState)> onPreview;
    std::function<void(bool)> onInteractionFinished;
    std::function<void(core::SpatialFilterType)> onCaptureSelection;
    std::function<void(bool)> onComparison;
    std::function<void()> onCapturedRegionChanged;
    std::function<void()> onCancelProcessing;
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void hideEvent(QHideEvent*) override;
    void showEvent(QShowEvent*) override;
private:
    struct Page {
        QWidget* widget {};
        QVBoxLayout* layout {};
        QHBoxLayout* optionsRow {};
        QCheckBox* enabled {};
        QCheckBox* preserveAlpha {};
        QComboBox* scope {};
        QPushButton* capture {};
        QLabel* maskStatus {};
        QCheckBox* showRegion {};
        std::vector<CompactValueControl*> numbers;
        std::function<void()> refresh;
    };
    Page& makePage(core::SpatialFilterType);
    void number(Page&, const char*, const QString&, double maximum, int decimals,
        const QString& suffix, std::function<double()>, std::function<void(double)>, double minimum = 0);
    bool begin(core::SpatialFilterType);
    void change(core::SpatialFilterType, const std::function<void(core::LayerSpatialFilter&)>&, bool enable = false);
    void refresh();
    core::LayerSpatialFilter& item(core::SpatialFilterType);
    core::SpatialFilterStack working_, before_;
    std::optional<core::LayerId> target_;
    std::array<Page, core::spatialFilterCount> pages_;
    core::SpatialFilterType editingType_ {core::SpatialFilterType::Gaussian};
    bool updating_ {}, editing_ {}, finishing_ {}, autoEnabled_ {}, hasSelection_ {};
    bool linked_ {true}, showCaptured_ {true}; // UI state, not layer content.
    QLabel* targetLabel_ {};
    QComboBox* navigation_ {};
    QStackedWidget* stack_ {};
    QPushButton* compare_ {};
    QCheckBox* linkedControl_ {};
    QComboBox* aperture_ {};
    QWidget* progressRow_ {};
    QProgressBar* progress_ {};
    QLabel* progressText_ {};
};
}
