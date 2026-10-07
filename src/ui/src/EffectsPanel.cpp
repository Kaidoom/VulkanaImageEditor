#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/BlendModeCombo.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/CurrentPageStack.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>
namespace imageeditor::ui {
void EffectsPanel::selectType(core::LayerEffectType type) {
  const auto index = static_cast<int>(type);
  if (index >= 0 && index < navigation_->count()) navigation_->setCurrentIndex(index);
}
EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent) {
  setObjectName("EffectsPanelContent");
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(8);
  auto *row = new QHBoxLayout;
  navigation_ = new QComboBox;
  navigation_->setObjectName("EffectNavigation");
  navigation_->setMaxVisibleItems(int(core::layerEffectCount));
  row->addWidget(navigation_, 1);
  auto *reset = new QPushButton(tr("Reset"));
  reset->setObjectName("EffectResetCurrent");
  reset->setToolTip(tr("Reset the current effect"));
  row->addWidget(reset);
  auto *enabled = new QStackedWidget;
  enabled->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  row->addWidget(enabled);
  outer->addLayout(row);
  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stack_ = new CurrentPageStack;
  stack_->setObjectName("EffectParameterPages");
  stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  scroll->setWidget(stack_);
  outer->addWidget(scroll, 1);
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    navigation_->addItem(
        QString::fromUtf8(core::layerEffectName(core::LayerEffectType(i))));
    auto *navItem =
        static_cast<QStandardItemModel *>(navigation_->model())->item(int(i));
    navItem->setCheckable(true);
    auto &page = pages_[i];
    auto *widget = new QWidget;
    page.layout = new QVBoxLayout(widget);
    page.layout->setContentsMargins(0, 0, 0, 8);
    page.layout->setSpacing(8);
    stack_->addWidget(widget);
    page.enabled = new QCheckBox(tr("Enabled"));
    page.enabled->setObjectName(QString("EffectEnabled%1").arg(i));
    enabled->addWidget(page.enabled);
    connect(page.enabled, &QCheckBox::toggled, this, [this, i](bool v) {
      change([&] { working_.items[i].enabled = v; });
    });
    if(i==7) {bevelPage();page.layout->addStretch();continue;}
    auto *blendRow = blendChoices(
        i, tr("Blend"),
        [this, i] { return working_.items[i].blendMode; },
        [this, i](core::BlendMode v) { working_.items[i].blendMode = v; });
    color(i, false, blendRow);
    if (i == 6)
      color(i, true, blendRow);
    if (i == 0)
      choices(
          i, tr("Position"), {tr("Inside"), tr("Center"), tr("Outside")},
          [this, i] { return int(working_.items[i].position); },
          [this, i](int v) {
            working_.items[i].position = core::StrokePosition(v);
          });
    if (i == 6) {
      auto *typeRow = choices(
          i, tr("Type"), {tr("Linear"), tr("Radial")},
          [this, i] { return int(working_.items[i].gradient); },
          [this, i](int v) {
            working_.items[i].gradient = core::GradientType(v);
          });
      auto *reverse = new QCheckBox(tr("Reverse"));
      reverse->setObjectName("Effect6Reverse");
      typeRow->addWidget(reverse);
      connect(reverse, &QCheckBox::toggled, this, [this](bool v) {
        change([&] { working_.items[6].reverse = v; });
      });
      const auto previous = page.refresh;
      page.refresh = [this, previous, reverse] {
        if (previous)
          previous();
        reverse->setChecked(working_.items[6].reverse);
      };
    }
    number(i, tr("Opacity"), 0, 100, &core::LayerEffect::opacity, 100);
    if (i < 5)
      number(i, tr("Size"), 0, core::maximumEffectSize,
             &core::LayerEffect::size);
    if (i > 0 && i < 5)
      number(i, i == 2 || i == 4 ? tr("Choke") : tr("Spread"), 0, 100,
             &core::LayerEffect::spread, 100);
    if (i == 1 || i == 2 || i == 6)
      number(i, tr("Angle"), -180, 180, &core::LayerEffect::angle);
    if (i == 1 || i == 2)
      number(i, tr("Distance"), 0, core::maximumEffectDistance,
             &core::LayerEffect::distance);
    if (i == 6)
      number(i, tr("Scale"), 1, 1000, &core::LayerEffect::scale, 100);
    page.layout->addStretch();
  }
  connect(static_cast<QStandardItemModel *>(navigation_->model()),
          &QStandardItemModel::itemChanged, this, [this](QStandardItem *item) {
            const auto row=std::size_t(item->row());
            const bool enabled=item->checkState()==Qt::Checked;
            if (!updating_)change([this,row,enabled] {working_.items[row].enabled=enabled;});
          });
  connect(navigation_, &QComboBox::currentIndexChanged, this,
          [this, enabled](int index) {
            finishEditing();
            stack_->setCurrentIndex(index);
            enabled->setCurrentIndex(index);
          });
  connect(reset, &QPushButton::clicked, this, [this] {
    change([&] {
      const auto i = std::size_t(navigation_->currentIndex());
      working_.items[i] = core::defaultLayerEffect(core::LayerEffectType(i));
    });
  });
  // Reserve the same footer space for both pages, including a wrapped hint.
  footer_ = new QStackedWidget;
  footer_->setObjectName("EffectFooter");
  footer_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto *note =
      new QLabel(tr("Local pixels · effects stay independent. Enable each "
                    "effect separately."));
  note->setObjectName("MutedLabel");
  note->setWordWrap(true);
  note->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  footer_->addWidget(note);
  status_ = new QLabel;
  status_->setObjectName("EffectProcessingStatus");
  status_->setWordWrap(true);
  status_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  footer_->addWidget(status_);
  outer->addWidget(footer_);
  setTarget(nullptr);
}
void EffectsPanel::number(std::size_t i, const QString &label, double lo,
                          double hi, double core::LayerEffect::*member,
                          double factor) {
  number(i,label,lo,hi,[this,i,member,factor]{return working_.items[i].*member*factor;},
    [this,i,member,factor](double v){working_.items[i].*member=v/factor;});
}
CompactValueControl* EffectsPanel::number(std::size_t i,const QString& label,double lo,double hi,
    std::function<double()> read,std::function<void(double)> write) {
  auto *n = new CompactValueControl;
  n->setObjectName(QString("Effect%1%2").arg(i).arg(label));
  n->setAccessibleName(label);
  n->setPrefix(label + ": ");
  n->setDecimals(1);
  n->setRange(lo, hi);
  n->setSingleStep(1);
  n->setFixedHeight(30);
  pages_[i].layout->addWidget(n);
  pages_[i].numbers.push_back(n);
  const auto previous = pages_[i].refresh;
  pages_[i].refresh = [n, read, previous] {
    if (previous)
      previous();
    if (!n->interactionActive())
      n->setValue(read());
  };
  n->onInteractionStarted = [this] {
    if (!updating_)
      begin();
  };
  n->onInteractionFinished = [this] {
    if (!updating_)
      finishEditing();
  };
  connect(n, &QDoubleSpinBox::valueChanged, this,
          [this, n, write](double v) {
            if (updating_)
              return;
            if (!begin()) {
              refresh();
              return;
            }
            write(v);
            if (onPreview)
              onPreview(
                  std::make_shared<const core::LayerEffectStack>(working_));
            if (!n->interactionActive())
              finishEditing();
          });
  return n;
}
QHBoxLayout *EffectsPanel::choices(std::size_t i, const QString &label,
                           const QStringList &options,
                           std::function<int()> read,
                           std::function<void(int)> write,
                           QHBoxLayout *existingRow) {
  auto *row = existingRow ? existingRow : new QHBoxLayout;
  if (!existingRow)
    row->addWidget(new QLabel(label));
  auto *box = new QComboBox;
  box->setAccessibleName(label);
  box->addItems(options);
  box->setMaxVisibleItems(int(options.size()));
  row->addWidget(box, 1);
  if (!existingRow)
    pages_[i].layout->addLayout(row);
  const auto previous = pages_[i].refresh;
  pages_[i].refresh = [previous, box, read] {
    if (previous)
      previous();
    box->setCurrentIndex(read());
  };
  connect(box, &QComboBox::currentIndexChanged, this,
          [this, write](int v) { change([&] { write(v); }); });
  return row;
}
QHBoxLayout *EffectsPanel::blendChoices(std::size_t i, const QString &label,
    std::function<core::BlendMode()> read, std::function<void(core::BlendMode)> write) {
  auto *row = new QHBoxLayout;
  row->addWidget(new QLabel(label));
  auto *box = new QComboBox;
  box->setObjectName(QString("Effect%1%2").arg(i).arg(label));
  box->setAccessibleName(label);
  populateBlendModeCombo(*box);
  row->addWidget(box, 1);
  pages_[i].layout->addLayout(row);
  const auto previous = pages_[i].refresh;
  pages_[i].refresh = [previous, box, read] {
    if (previous) previous();
    box->setCurrentIndex(box->findData(int(read())));
  };
  connect(box, &QComboBox::currentIndexChanged, this, [this, box, write](int index) {
    if (const auto mode = blendModeAt(*box, index)) change([&] { write(*mode); });
  });
  return row;
}
void EffectsPanel::color(std::size_t i, bool second, QHBoxLayout *row) {
  auto *button = new QPushButton;
  button->setObjectName(QString("EffectColor%1%2").arg(i).arg(second));
  const auto label = i==7 ? (second?tr("Shadow"):tr("Highlight")) :
      second ? tr("End") : i == 6 ? tr("Start") : tr("Color");
  button->setText(label);
  button->setAccessibleName(label);
  row->addWidget(button);
  const auto previous = pages_[i].refresh;
  pages_[i].refresh = [this, i, second, button, label, previous] {
    if (previous)
      previous();
    const auto c =
        second ? working_.items[i].secondColor : working_.items[i].color;
    const QColor color(c.red, c.green, c.blue, c.alpha);
    button->setToolTip(tr("%1: %2").arg(label, color.name(QColor::HexArgb)));
    QPixmap swatch(20, 16);
    swatch.fill(color);
    button->setIcon(QIcon(swatch));
  };
  connect(button, &QPushButton::clicked, this, [this, i, second] {
    finishEditing();
    if (onColorRequested)
      onColorRequested(core::LayerEffectType(i), second);
  });
}
bool EffectsPanel::begin() {
  if (updating_ || finishing_ || !target_)
    return false;
  if (editing_)
    return true;
  if (onInteractionStarted && !onInteractionStarted())
    return false;
  before_ = working_;
  editing_ = true;
  return true;
}
void EffectsPanel::change(const std::function<void()> &f) {
  if (updating_)
    return;
  finishEditing();
  if (!begin())
    return;
  f();
  if (onPreview)
    onPreview(std::make_shared<const core::LayerEffectStack>(working_));
  finishEditing();
  refresh();
}
void EffectsPanel::finishEditing(bool commit) {
  if (finishing_)
    return;
  const QScopedValueRollback guard(finishing_, true);
  const bool had = editing_;
  editing_ = false;
  if(contourEditor_)contourEditor_->finishInteraction(commit);
  for (auto &p : pages_)
    for (auto *n : p.numbers)
      n->finishEditing(commit, true);
  if (had && !commit)
    working_ = before_;
  if (had && onInteractionFinished)
    onInteractionFinished(commit);
}
void EffectsPanel::resetAll() {
  change([this] { working_ = core::LayerEffectStack{}; });
}
void EffectsPanel::setProcessing(bool busy, double progress,
                                 const QString &message) {
  status_->setText(
      busy ? tr("Preparing effects · %1%").arg(qRound(progress * 100))
           : message);
  footer_->setCurrentIndex(busy || !message.isEmpty() ? 1 : 0);
}
void EffectsPanel::setTarget(const core::Layer *l) {
  const auto next = l ? std::optional(l->id) : std::nullopt;
  if (next != target_) {
    finishEditing(false);
    if (onComparison)
      onComparison(false);
  }
  target_ = next;
  working_ = l && l->effects ? *l->effects : core::LayerEffectStack{};
  setEnabled(l);
  refresh();
}
void EffectsPanel::refresh() {
  const QScopedValueRollback guard(updating_, true);
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    pages_[i].enabled->setChecked(working_.items[i].enabled);
    static_cast<QStandardItemModel *>(navigation_->model())
        ->item(int(i))
        ->setCheckState(working_.items[i].enabled ? Qt::Checked
                                                  : Qt::Unchecked);
    if (pages_[i].refresh)
      pages_[i].refresh();
  }
}
void EffectsPanel::hideEvent(QHideEvent *e) {
  finishEditing();
  if (onComparison)
    onComparison(false);
  QWidget::hideEvent(e);
}
} // namespace imageeditor::ui
