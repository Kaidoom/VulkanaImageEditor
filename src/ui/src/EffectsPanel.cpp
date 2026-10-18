#include "imageeditor/ui/LocalizedNames.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/BlendModeCombo.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/GradientStopEditor.hpp"
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
#include <QStyle>
#include <QVBoxLayout>
namespace imageeditor::ui {
namespace {
// Keep layer-wide options side by side without clipping their localized labels.
// The checkbox still owns keyboard input, focus, accessibility, and all clicks.
class WrappedEffectOption final : public QCheckBox {
public:
  explicit WrappedEffectOption(const i18n::Message &message) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, this) +
                               style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, this),
                           2, 0, 2);
    auto *label = i18n::make<QLabel>(message);
    label->setWordWrap(true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(label);
    i18n::setAccessibleName(this, message);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  }
  QSize sizeHint() const override { return layout()->sizeHint(); }
  QSize minimumSizeHint() const override { return layout()->minimumSize(); }
protected:
  bool hitButton(const QPoint &point) const override { return rect().contains(point); }
};
} // namespace
void EffectsPanel::selectType(core::LayerEffectType type) {
  const auto index = static_cast<int>(type);
  if (index >= 0 && index < navigation_->count()) navigation_->setCurrentIndex(index);
}
EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent) {
  setObjectName("EffectsPanelContent");
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(8);
  auto *blending = new QHBoxLayout;
  blending->setSpacing(12);
  pixelAlphaShapesEffects_ = new WrappedEffectOption(
      i18n::text("effects.panel.pixel.alpha.shapes.effects"));
  pixelAlphaShapesEffects_->setObjectName("EffectPixelAlphaShapesEffects");
  i18n::setToolTip(pixelAlphaShapesEffects_,
      i18n::text("effects.panel.pixel.alpha.shapes.effects.hint"));
  maskEffectsAfterRendering_ = new WrappedEffectOption(
      i18n::text("effects.panel.mask.effects.after.rendering"));
  maskEffectsAfterRendering_->setObjectName("EffectMaskEffectsAfterRendering");
  i18n::setToolTip(maskEffectsAfterRendering_,
      i18n::text("effects.panel.mask.effects.after.rendering.hint"));
  blending->addWidget(pixelAlphaShapesEffects_, 1);
  blending->addWidget(maskEffectsAfterRendering_, 1);
  outer->addLayout(blending);
  connect(pixelAlphaShapesEffects_, &QCheckBox::toggled, this, [this](bool value) {
    change([&] { working_.pixelAlphaShapesEffects = value; });
  });
  connect(maskEffectsAfterRendering_, &QCheckBox::toggled, this, [this](bool value) {
    change([&] { working_.maskEffectsAfterRendering = value; });
  });
  auto *row = new QHBoxLayout;
  navigation_ = new QComboBox;
  navigation_->setObjectName("EffectNavigation");
  navigation_->setMaxVisibleItems(int(core::layerEffectCount));
  row->addWidget(navigation_, 1);
  auto *reset = i18n::make<QPushButton>(i18n::text("effects.panel.reset"));
  reset->setObjectName("EffectResetCurrent");
  i18n::setToolTip(reset, i18n::text("effects.panel.reset.the.current.effect"));
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
    i18n::addItem(navigation_, i18n::effectName(core::LayerEffectType(i)));
    auto *navItem =
        static_cast<QStandardItemModel *>(navigation_->model())->item(int(i));
    navItem->setCheckable(true);
    auto &page = pages_[i];
    auto *widget = new QWidget;
    page.layout = new QVBoxLayout(widget);
    page.layout->setContentsMargins(0, 0, 0, 8);
    page.layout->setSpacing(8);
    stack_->addWidget(widget);
    page.enabled = i18n::make<QCheckBox>(i18n::text("effects.panel.enabled"));
    page.enabled->setObjectName(QString("EffectEnabled%1").arg(i));
    enabled->addWidget(page.enabled);
    connect(page.enabled, &QCheckBox::toggled, this, [this, i](bool v) {
      change([&] { working_.items[i].enabled = v; });
    });
    if(i==7) {bevelPage();page.layout->addStretch();continue;}
    auto *blendRow = blendChoices(
        i, "Blend", i18n::text("effects.panel.blend"),
        [this, i] { return working_.items[i].blendMode; },
        [this, i](core::BlendMode v) { working_.items[i].blendMode = v; });
    if(i!=6)color(i, false, blendRow);
    else {
      gradientEditor_=new GradientStopEditor;
      page.layout->addWidget(gradientEditor_);
      gradientEditor_->onInteractionStarted=[this]{return begin();};
      gradientEditor_->onStopsChanged=[this](const auto& stops) {
        auto& effect=working_.items[6];
        effect.gradientStops=stops;
        effect.color=stops.front().color;effect.secondColor=stops.back().color;
        if(onPreview)onPreview(std::make_shared<const core::LayerEffectStack>(working_));
      };
      gradientEditor_->onInteractionFinished=[this](bool commit){finishEditing(commit);};
      gradientEditor_->onColorRequested=[this](std::size_t stop) {
        finishEditing();
        if(onGradientColorRequested)onGradientColorRequested(stop);
      };
      const auto previous=page.refresh;
      page.refresh=[this,previous] {
        if(previous)previous();
        gradientEditor_->setStops(core::effectiveGradientStops(working_.items[6]));
      };
    }
    if (i == 0)
      choices(
          i, i18n::text("effects.panel.position"), {i18n::text("effects.panel.inside"), i18n::text("effects.panel.center"), i18n::text("effects.panel.outside")},
          [this, i] { return int(working_.items[i].position); },
          [this, i](int v) {
            working_.items[i].position = core::StrokePosition(v);
          });
    if (i == 6) {
      auto *typeRow = choices(
          i, i18n::text("effects.panel.type"), {i18n::text("effects.panel.linear"), i18n::text("effects.panel.radial")},
          [this, i] { return int(working_.items[i].gradient); },
          [this, i](int v) {
            working_.items[i].gradient = core::GradientType(v);
          });
      auto *reverse = i18n::make<QCheckBox>(i18n::text("effects.panel.reverse"));
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
    number(i, "Opacity", i18n::text("effects.panel.opacity"), 0, 100, &core::LayerEffect::opacity, 100);
    if (i < 5)
      number(i, "Size", i18n::text("effects.panel.size"), 0, core::maximumEffectSize,
             &core::LayerEffect::size);
    if (i > 0 && i < 5)
      number(i, i == 2 || i == 4 ? "Choke" : "Spread", i == 2 || i == 4 ? i18n::text("effects.panel.choke") : i18n::text("effects.panel.spread"), 0, 100,
             &core::LayerEffect::spread, 100);
    if (i == 1 || i == 2 || i == 6)
      number(i, "Angle", i18n::text("effects.panel.angle"), -180, 180, &core::LayerEffect::angle);
    if (i == 1 || i == 2)
      number(i, "Distance", i18n::text("effects.panel.distance"), 0, core::maximumEffectDistance,
             &core::LayerEffect::distance);
    if (i == 6)
      number(i, "Scale", i18n::text("effects.panel.scale"), 1, 1000, &core::LayerEffect::scale, 100);
    page.layout->addStretch();
  }
  connect(static_cast<QStandardItemModel *>(navigation_->model()),
          &QStandardItemModel::itemChanged, this, [this](QStandardItem *item) {
            const auto row=std::size_t(item->row());
            const bool enabled=item->checkState()==Qt::Checked;
            // DisplayRole changes during translation are not effect edits.
            if (!updating_ && enabled != working_.items[row].enabled)
              change([this,row,enabled] {working_.items[row].enabled=enabled;});
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
      i18n::make<QLabel>(i18n::text("effects.panel.local.pixels.effects.stay.independent.enable.each.effect.separately"));
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
void EffectsPanel::number(std::size_t i, const char* identity, const i18n::Message& label, double lo,
                          double hi, double core::LayerEffect::*member,
                          double factor) {
  number(i,identity,label,lo,hi,[this,i,member,factor]{return working_.items[i].*member*factor;},
    [this,i,member,factor](double v){working_.items[i].*member=v/factor;});
}
CompactValueControl* EffectsPanel::number(std::size_t i,const char* identity,const i18n::Message& label,double lo,double hi,
    std::function<double()> read,std::function<void(double)> write) {
  auto *n = new CompactValueControl;
  n->setObjectName(QString("Effect%1%2").arg(i).arg(QString::fromLatin1(identity)));
  i18n::setAccessibleName(n, label);
  i18n::setPrefix(n, label + ": ");
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
QHBoxLayout *EffectsPanel::choices(std::size_t i, const i18n::Message& label,
                           const QList<i18n::Message> &options,
                           std::function<int()> read,
                           std::function<void(int)> write,
                           QHBoxLayout *existingRow) {
  auto *row = existingRow ? existingRow : new QHBoxLayout;
  if (!existingRow)
    row->addWidget(i18n::make<QLabel>(label));
  auto *box = new QComboBox;
  i18n::setAccessibleName(box, label);
  i18n::addItems(box, options);
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
QHBoxLayout *EffectsPanel::blendChoices(std::size_t i, const char* identity, const i18n::Message& label,
    std::function<core::BlendMode()> read, std::function<void(core::BlendMode)> write) {
  auto *row = new QHBoxLayout;
  row->addWidget(i18n::make<QLabel>(label));
  auto *box = new QComboBox;
  box->setObjectName(QString("Effect%1%2").arg(i).arg(QString::fromLatin1(identity)));
  i18n::setAccessibleName(box, label);
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
  const auto label = i==7 ? (second?i18n::text("effects.panel.shadow"):i18n::text("effects.panel.highlight")) :
      second ? i18n::text("effects.panel.end") : i == 6 ? i18n::text("effects.panel.start") : i18n::text("effects.panel.color");
  button->setText(label);
  i18n::setAccessibleName(button, label);
  row->addWidget(button);
  const auto previous = pages_[i].refresh;
  pages_[i].refresh = [this, i, second, button, label, previous] {
    if (previous)
      previous();
    const auto c =
        second ? working_.items[i].secondColor : working_.items[i].color;
    const QColor color(c.red, c.green, c.blue, c.alpha);
    i18n::setToolTip(button, i18n::text("effects.panel.1.2").arg(label, color.name(QColor::HexArgb)));
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
  if (!begin()) {
    refresh();
    return;
  }
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
  if(gradientEditor_)gradientEditor_->finishInteraction(commit);
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
  i18n::setText(status_, busy ? i18n::text("effects.panel.preparing.effects.1").arg(qRound(progress * 100))
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
  pixelAlphaShapesEffects_->setChecked(working_.pixelAlphaShapesEffects);
  maskEffectsAfterRendering_->setChecked(working_.maskEffectsAfterRendering);
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
