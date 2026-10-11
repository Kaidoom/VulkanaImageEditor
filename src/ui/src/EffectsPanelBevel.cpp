#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
namespace imageeditor::ui {
core::EffectContour &EffectsPanel::selectedContour() {
  return glossSelected_ ? working_.items[7].bevel.gloss
                        : working_.items[7].bevel.surface;
}
void EffectsPanel::bevelPage() {
  constexpr std::size_t i = 7;
  auto &page = pages_[i];
  const auto section = [&](const i18n::Message& text) {
    auto *label = i18n::make<QLabel>(text);
    label->setObjectName("MutedLabel");
    page.layout->addWidget(label);
  };
  section(i18n::text("effects.panel.bevel.structure.smooth"));
  auto *structure = choices(
      i, i18n::text("effects.panel.bevel.style"), {i18n::text("effects.panel.bevel.inner.bevel"), i18n::text("effects.panel.bevel.outer.bevel"), i18n::text("effects.panel.bevel.emboss")},
      [this] { return int(working_.items[7].bevel.style); },
      [this](int v) { working_.items[7].bevel.style = core::BevelStyle(v); });
  choices(
      i, i18n::text("effects.panel.bevel.direction"), {i18n::text("effects.panel.bevel.up"), i18n::text("effects.panel.bevel.down")},
      [this] { return int(working_.items[7].bevel.down); },
      [this](int v) { working_.items[7].bevel.down = v != 0; }, structure);
  structure->setStretch(structure->count() - 1, 0);
  const auto value = [this](const char* identity, const i18n::Message& label, double lo, double hi,
                            double core::BevelParameters::*member,
                            double scale = 1.) {
    number(
        7, identity, label, lo, hi,
        [this, member, scale] {
          return working_.items[7].bevel.*member * scale;
        },
        [this, member, scale](double v) {
          working_.items[7].bevel.*member = v / scale;
        });
  };
  value("Depth", i18n::text("effects.panel.bevel.depth"), 0, 1000, &core::BevelParameters::depth, 100);
  number(i, "Size", i18n::text("effects.panel.bevel.size"), 0, core::maximumEffectSize, &core::LayerEffect::size);
  value("Soften", i18n::text("effects.panel.bevel.soften"), 0, 64, &core::BevelParameters::soften);
  section(i18n::text("effects.panel.bevel.lighting"));
  number(i, "Angle", i18n::text("effects.panel.bevel.angle"), -180, 180, &core::LayerEffect::angle);
  value("Altitude", i18n::text("effects.panel.bevel.altitude"), 0, 90, &core::BevelParameters::altitude);
  auto *highlight = blendChoices(
      i, "Highlight blend", i18n::text("effects.panel.bevel.highlight.blend"),
      [this] { return working_.items[7].blendMode; },
      [this](core::BlendMode v) { working_.items[7].blendMode = v; });
  color(i, false, highlight);
  number(i, "Highlight opacity", i18n::text("effects.panel.bevel.highlight.opacity"), 0, 100, &core::LayerEffect::opacity, 100);
  auto *shadow = blendChoices(
      i, "Shadow blend", i18n::text("effects.panel.bevel.shadow.blend"),
      [this] { return working_.items[7].bevel.shadowBlend; },
      [this](core::BlendMode v) {
        working_.items[7].bevel.shadowBlend = v;
      });
  color(i, true, shadow);
  value("Shadow opacity", i18n::text("effects.panel.bevel.shadow.opacity"), 0, 100, &core::BevelParameters::shadowOpacity,
        100);
  section(i18n::text("effects.panel.bevel.contour"));
  // Navigation is deliberately not a parameter/history operation.
  auto *row = new QHBoxLayout;
  auto *selector = new QComboBox;
  selector->setObjectName("BevelContourSelector");
  i18n::addItems(selector, {i18n::text("effects.panel.bevel.surface.profile"), i18n::text("effects.panel.bevel.gloss.contour")});
  row->addWidget(selector, 1);
  auto *enabled = i18n::make<QCheckBox>(i18n::text("effects.panel.bevel.enabled"));
  enabled->setObjectName("BevelContourEnabled");
  row->addWidget(enabled);
  page.layout->addLayout(row);
  auto *presets = new QComboBox;
  presets->setObjectName("BevelContourPreset");
  auto *curveOptions = new QHBoxLayout;
  curveOptions->addWidget(presets, 1);
  auto *interpolation = new QComboBox;
  i18n::addItems(interpolation, {i18n::text("effects.panel.bevel.linear.segments"), i18n::text("effects.panel.bevel.smooth.curve")});
  interpolation->setObjectName("BevelContourInterpolation");
  curveOptions->addWidget(interpolation, 1);
  page.layout->addLayout(curveOptions);
  contourEditor_ = new AdjustmentCurveEditor;
  contourEditor_->setObjectName("BevelContourEditor");
  page.layout->addWidget(contourEditor_);
  auto *corner = i18n::make<QCheckBox>(i18n::text("effects.panel.bevel.corner.point"));
  corner->setObjectName("BevelContourCorner");
  page.layout->addWidget(corner, 0, Qt::AlignHCenter);
  contourEditor_->evaluate = [this](double x) {
    auto curve = selectedContour();
    curve.enabled = true;
    return core::evaluateEffectContour(curve, x);
  };
  contourEditor_->onInteractionStarted = [this] { return begin(); };
  contourEditor_->onPointsChanged =
      [this](const std::vector<core::Vec2d> &points) {
        if (updating_)
          return;
        auto &c = selectedContour();
        const auto old = c.points;
        c.points.clear();
        for (std::size_t n = 0; n < points.size(); ++n) {
          bool corner = false;
          if (old.size() == points.size())
            corner = old[n].corner;
          else
            for (const auto &p : old)
              if (p.input == points[n].x && p.output == points[n].y)
                corner = p.corner;
          c.points.push_back({points[n].x, points[n].y, corner});
        }
        if (onPreview)
          onPreview(std::make_shared<const core::LayerEffectStack>(working_));
        refresh();
      };
  contourEditor_->onInteractionFinished = [this](bool commit) {
    finishEditing(commit);
    refresh();
  };
  auto *input = number(
      i, "Input", i18n::text("effects.panel.bevel.input"), 0, 100,
      [this] {
        return selectedContour()
                   .points[std::size_t(contourEditor_->selectedIndex())]
                   .input *
               100;
      },
      [this](double v) {
        auto p = contourEditor_
                     ->points()[std::size_t(contourEditor_->selectedIndex())];
        p.x = v / 100;
        contourEditor_->setSelectedPoint(p);
      });
  auto *output = number(
      i, "Output", i18n::text("effects.panel.bevel.output"), 0, 100,
      [this] {
        return selectedContour()
                   .points[std::size_t(contourEditor_->selectedIndex())]
                   .output *
               100;
      },
      [this](double v) {
        auto p = contourEditor_
                     ->points()[std::size_t(contourEditor_->selectedIndex())];
        p.y = v / 100;
        contourEditor_->setSelectedPoint(p);
      });
  (void)input;
  (void)output;
  auto *remove = i18n::make<QPushButton>(i18n::text("effects.panel.bevel.remove.point"));
  page.layout->addWidget(remove);
  connect(remove, &QPushButton::clicked, contourEditor_,
          &AdjustmentCurveEditor::removeSelectedPoint);
  contourEditor_->onPointSelected = [this](int) { refresh(); };
  connect(selector, &QComboBox::currentIndexChanged, this, [this](int n) {
    finishEditing();
    glossSelected_ = n != 0;
    refresh();
  });
  connect(enabled, &QCheckBox::toggled, this,
          [this](bool v) { change([&] { selectedContour().enabled = v; }); });
  connect(interpolation, &QComboBox::currentIndexChanged, this, [this](int n) {
    change([&] {
      selectedContour().interpolation = core::EffectContourInterpolation(n);
    });
  });
  connect(presets, &QComboBox::activated, this, [this](int n) {
    if (n < 4)
      change([&] {
        selectedContour() = core::effectContourPreset(glossSelected_, n);
      });
  });
  connect(corner, &QCheckBox::toggled, this, [this](bool v) {
    change([&] {
      selectedContour()
          .points[std::size_t(contourEditor_->selectedIndex())]
          .corner = v;
    });
  });
  const auto previous = page.refresh;
  page.refresh = [this, previous, presets, enabled, interpolation, corner,
                  remove] {
    auto &c = selectedContour();
    if (!contourEditor_->interactionActive()) {
      std::vector<core::Vec2d> points;
      for (const auto &p : c.points)
        points.push_back({p.input, p.output});
      contourEditor_->setPoints(points);
    }
    if (previous)
      previous();
    enabled->setChecked(c.enabled);
    interpolation->setCurrentIndex(int(c.interpolation));
    presets->clear();
    presets->addItems(
        glossSelected_ ? QStringList{i18n::text("effects.panel.bevel.identity"), i18n::text("effects.panel.bevel.glossy"), i18n::text("effects.panel.bevel.ring"),
                                     i18n::text("effects.panel.bevel.double.ring"), i18n::text("effects.panel.bevel.custom")}
                       : QStringList{i18n::text("effects.panel.bevel.linear"), i18n::text("effects.panel.bevel.rounded"), i18n::text("effects.panel.bevel.concave"),
                                     i18n::text("effects.panel.bevel.ridged"), i18n::text("effects.panel.bevel.custom")});
    int preset = 4;
    for (int n = 0; n < 4; ++n) {
      auto p = core::effectContourPreset(glossSelected_, n);
      if (c.points == p.points && c.interpolation == p.interpolation)
        preset = n;
    }
    presets->setCurrentIndex(preset);
    const auto selected = std::size_t(contourEditor_->selectedIndex());
    corner->setChecked(c.points[selected].corner);
    remove->setEnabled(selected > 0 && selected + 1 < c.points.size());
  };
}
} // namespace imageeditor::ui
