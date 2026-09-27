#pragma once
#include "imageeditor/core/Layer.hpp"
#include <QWidget>
#include <functional>
class QComboBox;
class QStackedWidget;
class QCheckBox;
class QVBoxLayout;
class QHBoxLayout;
class QLabel;
namespace imageeditor::ui {
class CompactValueControl;
class EffectsPanel final : public QWidget {
public:
  explicit EffectsPanel(QWidget *parent = nullptr);
  void setTarget(const core::Layer *);
  void finishEditing(bool commit = true);
  void resetAll();
  void setProcessing(bool busy, double progress = 0,
                     const QString &message = {});
  bool interactionActive() const { return editing_; }
  std::optional<core::LayerId> target() const { return target_; }
  std::function<bool()> onInteractionStarted;
  std::function<void(core::LayerEffectState)> onPreview;
  std::function<void(bool)> onInteractionFinished, onComparison;
  std::function<void(core::LayerEffectType, bool)> onColorRequested;

protected:
  void hideEvent(QHideEvent *) override;

private:
  struct Page {
    QVBoxLayout *layout{};
    QCheckBox *enabled{};
    std::vector<CompactValueControl *> numbers;
    std::function<void()> refresh;
  };
  bool begin();
  void refresh();
  void change(const std::function<void()> &);
  void number(std::size_t, const QString &, double, double,
              double core::LayerEffect::*, double factor = 1);
  QHBoxLayout *choices(std::size_t, const QString &, const QStringList &,
                       std::function<int()>, std::function<void(int)>);
  void color(std::size_t, bool, QHBoxLayout *);
  core::LayerEffectStack working_, before_;
  std::optional<core::LayerId> target_;
  std::array<Page, core::layerEffectCount> pages_;
  QComboBox *navigation_{};
  QStackedWidget *stack_{};
  QLabel *status_{};
  bool updating_{}, editing_{}, finishing_{};
};
} // namespace imageeditor::ui
