#pragma once
#include "imageeditor/platform/PerformanceSettings.hpp"
#include "imageeditor/render/GraphicsDeviceSelection.hpp"
#include <QWidget>
#include <QTimer>
#include <functional>
#include <optional>
class QComboBox;
class QLabel;
namespace imageeditor::ui {
struct PerformanceUsage {
    std::optional<std::uint64_t> residentBytes;
    std::uint64_t activeUndoBytes{}, allUndoBytes{}, documentCacheBytes{}, textureCacheBytes{};
    QString gpuName;
};
class PerformancePanel final : public QWidget {
public:
    explicit PerformancePanel(QWidget* parent=nullptr);
    void setSettings(platform::PerformanceSettings);
    platform::PerformanceSettings settings() const;
    void setGraphicsDevices(const std::vector<render::GraphicsDeviceInfo>&);
    std::function<void()> onChanged, onClearCaches;
    std::function<PerformanceUsage()> readUsage;
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void refreshUsage();
    void changed();
    QComboBox *gpu_{}, *undo_{}, *cpuCache_{}, *gpuCache_{}, *threads_{}, *objectThreads_{};
    QLabel* usage_{};
    QTimer timer_;
    bool updating_{};
};
}
