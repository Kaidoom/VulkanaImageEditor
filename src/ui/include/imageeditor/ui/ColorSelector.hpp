#pragma once
#include "imageeditor/core/EditorColors.hpp"
#include <QWidget>
#include <QPointer>
#include <functional>

class QPushButton;
class QToolButton;
class QVariantAnimation;
class QHideEvent;

namespace imageeditor::ui {
class ColorDialog;
class ColorSelector final : public QWidget {
public:
    enum class Presentation { Detailed, Compact };
    explicit ColorSelector(Presentation presentation, QWidget* parent = nullptr);
    void setColors(core::EditorColors colors);
    [[nodiscard]] core::EditorColors colors() const noexcept { return colors_; }
    std::function<void(core::EditorColors)> onColorsChanged;
protected:
    void hideEvent(QHideEvent* event) override;
private:
    void activateOrEdit(core::ColorSlot slot);
    void refresh();
    void publish();
    void updateSelection();
    void settleSelection();
    void paintSelection();
    core::EditorColors colors_;
    Presentation presentation_;
    core::ColorSlot displayedActive_ {core::ColorSlot::Primary};
    core::ColorSlot animationTarget_ {core::ColorSlot::Primary};
    QPoint animationPrimaryStart_;
    QPoint animationSecondaryStart_;
    QVariantAnimation* selectionAnimation_ {nullptr};
    QPointer<ColorDialog> colorDialog_;
    QPushButton* primary_;
    QPushButton* secondary_;
    QToolButton* swap_;
};
} // namespace imageeditor::ui
