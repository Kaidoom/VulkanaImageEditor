#pragma once
#include <QLayout>
#include <QStackedWidget>

namespace imageeditor::ui {
// Persistent hidden pages must not force the visible page to scroll.
class CurrentPageStack final : public QStackedWidget {
public:
  CurrentPageStack() {
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    connect(this, &QStackedWidget::currentChanged, this,
            [this] { updateGeometry(); });
  }
  QSize sizeHint() const override {
    return currentWidget() ? currentWidget()->sizeHint() : QSize{};
  }
  QSize minimumSizeHint() const override {
    return currentWidget() ? currentWidget()->minimumSizeHint() : QSize{};
  }
  bool hasHeightForWidth() const override {
    return currentWidget() && currentWidget()->hasHeightForWidth();
  }
  int heightForWidth(int width) const override {
    return hasHeightForWidth() ? currentWidget()->heightForWidth(width)
                              : sizeHint().height();
  }
};
} // namespace imageeditor::ui
