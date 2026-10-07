#pragma once

#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include <QWidget>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace imageeditor::ui {
class CompactValueControl;
class FiltersPanel;
class EffectsPanel;

class AdjustmentsPanel final : public QWidget {
public:
    explicit AdjustmentsPanel(QWidget* parent = nullptr);
    ~AdjustmentsPanel() override;
    void setTarget(const core::Layer*, bool hasSelection, const core::Document* = nullptr, std::uint64_t instance = 0);
    void finishEditing(bool commit = true);
    void requestHistogram(const core::Layer*);
    [[nodiscard]] FiltersPanel* filtersPanel() const { return filtersPanel_; }
    [[nodiscard]] EffectsPanel* effectsPanel() const { return effectsPanel_; }
    [[nodiscard]] bool effectsCategoryActive() const;
    [[nodiscard]] bool filtersCategoryActive() const;
    void showFilter(core::SpatialFilterType);
    void showEffect(core::LayerEffectType);
    [[nodiscard]] bool interactionActive() const { return editing_; }
    [[nodiscard]] core::AdjustmentType currentType() const;
    [[nodiscard]] std::optional<core::LayerId> target() const { return target_; }
    [[nodiscard]] bool capturedRegionVisible() const { return showCapturedRegion_ && isVisible() && !filtersCategoryActive() && !effectsCategoryActive(); }
    [[nodiscard]] std::uint64_t completedHistogramJobs() const { return completedHistogramJobs_; }
    std::function<bool(core::AdjustmentType)> onInteractionStarted;
    std::function<void(core::AdjustmentState)> onPreview;
    std::function<void(bool)> onInteractionFinished;
    std::function<void(core::AdjustmentType)> onCaptureSelection;
    std::function<void(bool)> onComparison;
    std::function<void()> onHistogramRequested;
    std::function<void()> onCapturedRegionChanged;
    std::function<void(bool)> onFiltersCategoryChanged;

protected:
    bool eventFilter(QObject*, QEvent*) override;
    void hideEvent(QHideEvent*) override;
    void showEvent(QShowEvent*) override;

private:
    struct Page {
        QWidget* widget {nullptr};
        QVBoxLayout* layout {nullptr};
        QCheckBox* enabled {nullptr};
        QComboBox* scope {nullptr};
        QPushButton* capture {nullptr};
        QLabel* maskStatus {nullptr};
        QCheckBox* showRegion {nullptr};
        std::vector<CompactValueControl*> numbers;
        std::function<void()> refresh;
    };
    struct HistogramCache {
        core::LayerId target {0};
        std::shared_ptr<const core::RasterSurface> surface;
        core::Revision revision {0};
        core::Extent2u extent{};
        const core::Document* document{};
        std::vector<std::uint64_t> inputKey;
        core::AffineTransform pixelsToLocal;
        core::AdjustmentState upstream;
        AdjustmentHistogram bins {};
        std::size_t sampled {0};
    };
    struct HistogramJob {
        HistogramCache result;
        core::CompiledAdjustmentStack program;
        std::shared_ptr<core::PinnedDocumentSampler> composite;
        std::size_t stage {0}, cursor {0}, columns {0}, rows {0}, step {1};
    };
    Page& makePage(core::AdjustmentType, QStackedWidget*);
    CompactValueControl* number(Page&, const char* objectName, const QString& label,
        double min, double max, int decimals,
        std::function<double()> read, std::function<void(double)> write,
        const QString& suffix = {});
    QComboBox* choices(Page&, const char* name, const QStringList&, std::function<void(int)>);
    void change(core::AdjustmentType, const std::function<void(core::Adjustment&)>&, bool autoEnable = true);
    bool begin(core::AdjustmentType);
    void refresh();
    void refreshCurvePoint();
    void navigate();
    void advanceHistogram();
    void updateHistogramDisplay();
    void chooseTint();
    core::Adjustment& item(core::AdjustmentType);
    const core::Adjustment& item(core::AdjustmentType) const;
    core::AdjustmentStack working_;
    const core::Document* document_{};
    std::uint64_t documentInstance_{};
    QLabel* scopeLabel_{};
    core::AdjustmentStack editingBefore_;
    bool automaticallyEnabled_ {false};
    bool showCapturedRegion_ {true}; // Panel/view state only, shared by all pages.
    std::optional<core::LayerId> target_;
    std::array<Page, core::adjustmentCount> pages_;
    bool updating_ {false}, editing_ {false}, hasSelection_ {false}, finishing_ {false};
    core::AdjustmentType editingType_ {core::AdjustmentType::Exposure};
    QLabel* targetLabel_ {nullptr};
    QPushButton* resetAll_ {nullptr};
    QPushButton* compare_ {nullptr};
    QTabWidget* tabs_ {nullptr};
    FiltersPanel* filtersPanel_ {nullptr};
    EffectsPanel* effectsPanel_ {nullptr};
    std::array<QComboBox*,3> navigation_ {};
    std::array<QStackedWidget*,3> stacks_ {};
    QComboBox* levelsChannel_ {nullptr};
    QComboBox* curvesChannel_ {nullptr};
    QComboBox* hueRange_ {nullptr};
    QComboBox* balanceRange_ {nullptr};
    QCheckBox* colorize_ {nullptr};
    QCheckBox* preserveLuminosity_ {nullptr};
    CompactValueControl* curveInput_ {nullptr};
    CompactValueControl* curveOutput_ {nullptr};
    QPushButton* removePoint_ {nullptr};
    QPushButton* tint_ {nullptr};
    AdjustmentCurveEditor* curve_ {nullptr};
    AdjustmentCurveEditor* levelsHistogram_ {nullptr};
    std::array<QLabel*,2> histogramLabels_ {};
    std::array<std::optional<HistogramCache>,2> histogramCaches_;
    std::optional<HistogramJob> histogramJob_;
    QTimer* histogramTimer_ {nullptr};
    std::uint64_t completedHistogramJobs_ {0};
};
} // namespace imageeditor::ui
