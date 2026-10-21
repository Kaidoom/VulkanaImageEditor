#pragma once

#include "imageeditor/ui/BackgroundRemoval.hpp"
#include "imageeditor/ui/Localization.hpp"

#include <QElapsedTimer>
#include <QFrame>
#include <QString>

#include <functional>
#include <vector>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace imageeditor::ui {

// The editor canvas owns the actual image preview;
// this panel never changes the document or starts inference itself.
class BackgroundRemovalPanel final : public QFrame {
public:
    enum class View { Transparency, Black, White, Mask };

    explicit BackgroundRemovalPanel(QWidget* parent = nullptr);

    void setModels(const std::vector<BackgroundRemovalModel>& models);
    void setTarget(const QString& layerName, bool existingMask);
    void setBusy(bool busy, i18n::Message status = {});
    void setStatus(i18n::Message status);
    void setError(const QString& error);
    void setResult(const BackgroundRemovalResult& result, bool canApply = true);

    [[nodiscard]] QString selectedModel() const;
    [[nodiscard]] View view() const;
    [[nodiscard]] QSize sizeHint() const override;

    std::function<void(QString)> onModelChanged;
    std::function<void(View)> onViewChanged;
    std::function<void(bool)> onOriginalChanged;
    std::function<void()> onApply;
    std::function<void()> onCancel;
    std::function<void()> onRegenerate;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void setOriginalHeld(bool held);
    void updateElapsed();
    void updateStatusText();
    void updateControls();
    void clearResult();

    QComboBox* model_{};
    QComboBox* view_{};
    QLabel* target_{};
    QLabel* status_{};
    QLabel* timing_{};
    QLabel* maskHint_{};
    QProgressBar* progress_{};
    QPushButton* original_{};
    QPushButton* regenerate_{};
    QPushButton* apply_{};
    QTimer* elapsedTimer_{};
    QElapsedTimer elapsed_;
    i18n::Message busyStatus_;
    i18n::Message fullStatus_;
    bool busy_{};
    bool validResult_{};
    bool failed_{};
    bool originalHeld_{};
    int originalKey_{};
};

} // namespace imageeditor::ui
