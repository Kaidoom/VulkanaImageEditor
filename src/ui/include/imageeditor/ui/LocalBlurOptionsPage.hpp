#pragma once
#include "imageeditor/core/BlurSettings.hpp"
#include "imageeditor/core/BrushEngine.hpp"
#include <QWidget>
#include <functional>

namespace imageeditor::ui {
class CompactValueControl;
class LocalBlurOptionsPage final : public QWidget {
public:
    explicit LocalBlurOptionsPage(QWidget* parent = nullptr);
    void setBrushSettings(const core::BrushSettings&);
    void setBlurSettings(const core::BlurSettings&);
    [[nodiscard]] core::BlurSettings blurSettings() const noexcept { return settings_; }
    std::function<void(const core::BrushSettings&)> onBrushSettingsChanged;
    std::function<void(const core::BlurSettings&)> onBlurSettingsChanged;
private:
    core::BrushSettings brush_;
    core::BlurSettings settings_;
    CompactValueControl* size_;
    CompactValueControl* hardness_;
    CompactValueControl* strength_;
    CompactValueControl* radius_;
    CompactValueControl* publishing_ {nullptr};
    bool updating_ {false};
};
}
