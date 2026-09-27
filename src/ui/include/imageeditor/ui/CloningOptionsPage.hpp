#pragma once

#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/CloneSettings.hpp"

#include <QWidget>

#include <functional>

class QButtonGroup;
class QToolButton;
class QLabel;
class QProgressBar;

namespace imageeditor::ui {

class CompactValueControl;

// A cached options page and separate leading mode group. Source selection is
// owned by the session; this widget only presents its stable layer identity.
class CloningOptionsPage final : public QWidget {
public:
    explicit CloningOptionsPage(QWidget* parent = nullptr);

    [[nodiscard]] QWidget* modeWidget() const noexcept { return modes_; }
    void setBrushSettings(const core::BrushSettings& settings);
    [[nodiscard]] const core::BrushSettings& brushSettings() const noexcept
    {
        return brushSettings_;
    }
    void setCloneSettings(const core::CloneSettings& settings);
    [[nodiscard]] const core::CloneSettings& cloneSettings() const noexcept
    {
        return cloneSettings_;
    }
    void setMode(core::CloneMode mode);
    void setSource(core::CloneSampleSource source);
    void setAligned(bool aligned);
    void setAdaptation(double adaptation);
    void setSourceLayerLabel(const QString& label, bool valid = true);
    void setProcessing(bool busy, double progress = 0, const QString& message = {});

    std::function<void(const core::BrushSettings&)> onBrushSettingsChanged;
    std::function<void(core::CloneMode)> onModeChanged;
    std::function<void(core::CloneSampleSource)> onSourceChanged;
    std::function<void(bool)> onAlignedChanged;
    std::function<void(double)> onAdaptationChanged;
    std::function<void()> onCancelProcessing;

private:
    void publishBrushSettings(CompactValueControl* source);

    QWidget* modes_ {nullptr};
    QWidget* controls_ {nullptr};
    QWidget* progressRow_ {nullptr};
    QLabel* progressLabel_ {nullptr};
    QProgressBar* progress_ {nullptr};
    QButtonGroup* modeGroup_ {nullptr};
    QWidget* sourceModes_ {nullptr};
    QButtonGroup* sourceGroup_ {nullptr};
    CompactValueControl* sizeControl_ {nullptr};
    CompactValueControl* opacityControl_ {nullptr};
    CompactValueControl* hardnessControl_ {nullptr};
    CompactValueControl* flowControl_ {nullptr};
    CompactValueControl* spacingControl_ {nullptr};
    QToolButton* alignedButton_ {nullptr};
    QString rawSourceHint_;
    CompactValueControl* adaptationControl_ {nullptr};
    CompactValueControl* publishingControl_ {nullptr};
    core::BrushSettings brushSettings_;
    core::CloneSettings cloneSettings_;
    bool updating_ {false};
};

} // namespace imageeditor::ui
