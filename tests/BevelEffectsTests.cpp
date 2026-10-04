#include "../src/ui/src/LayerEffectCodec.hpp"
#include "../src/ui/src/PsdReader.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/PsdExport.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QTemporaryDir>
#include <QStackedWidget>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/resource.h>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int failures = 0;
void check(bool ok, const char *why) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL " << why << '\n';
  }
}
QImage artwork(int w = 180, int h = 120) {
  QImage image(w, h, QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(170, 105, 42));
  QFont font("DejaVu Sans");
  font.setPixelSize(50);
  font.setBold(true);
  p.setFont(font);
  p.setPen(QColor(170, 105, 42));
  p.drawText(6, 50, "OB");
  p.setPen(Qt::NoPen);
  p.drawRect(QRectF(95, 10, 34, 30));
  p.drawEllipse(QRectF(138, 10, 32, 35));
  p.drawPolygon(QPolygonF{{15, 100}, {35, 60}, {55, 100}});
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(QColor(170, 105, 42, 150), 5));
  p.drawEllipse(QRectF(65, 65, 35, 35));
  p.drawLine(QPointF(114, 74), QPointF(165, 84));
  p.setPen(QPen(QColor(170, 105, 42, 70), 2));
  p.drawLine(114, 92, 158, 96);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(170, 105, 42));
  p.drawEllipse(QPointF(160, 110), 2, 2);
  return image;
}
c::Layer layer(const QImage &image) {
  std::vector<std::byte> bytes(std::size_t(image.width()) *
                               std::size_t(image.height()) * 4);
  for (int y = 0; y < image.height(); ++y)
    std::memcpy(bytes.data() + std::size_t(y) * std::size_t(image.width()) * 4,
                image.constScanLine(y), std::size_t(image.width()) * 4);
  return c::Layer::raster(
      "Relief", std::make_shared<c::ContiguousRasterSurface>(
                    c::Extent2u{uint(image.width()), uint(image.height())},
                    std::move(bytes)));
}
std::shared_ptr<c::LayerEffectStack> settings() {
  auto s = std::make_shared<c::LayerEffectStack>();
  s->items[7].enabled = true;
  return s;
}
void contracts() {
  auto l = layer(artwork());
  auto state = settings();
  l.effects = state;
  l = c::prepareSpatialFilterLayer(l);
  const auto initial = l.effectCache;
  check(initial && initial->bevelDistance && initial->bevelNormals,
        "relief caches constructed");
  auto color = artwork();
  for (int y = 0; y < color.height(); ++y)
    for (int x = 0; x < color.width(); ++x) {
      auto c = color.pixelColor(x, y);
      c.setRed(10);
      c.setGreen(240);
      color.setPixelColor(x, y, c);
    }
  auto other = layer(color);
  other.effects = state;
  other = c::prepareSpatialFilterLayer(other);
  check(initial->bevelDistance->distance ==
                other.effectCache->bevelDistance->distance &&
            initial->bevelNormals->xy == other.effectCache->bevelNormals->xy,
        "equal alpha different RGB identical geometry");
  const auto im = artwork();
  c::PreparedLayerSampler sample(l);
  for (int y = 0; y < im.height(); ++y)
    for (int x = 0; x < im.width(); ++x)
      check(std::abs(sample.sample({x + .5, y + .5})[3] -
                     im.pixelColor(x, y).alphaF()) < 1e-6,
            "inner alpha preserved including soft edges/holes");
  for (int edit = 0; edit < 4; ++edit) {
    auto changed = std::make_shared<c::LayerEffectStack>(*state);
    auto &e = changed->items[7];
    if (edit == 0) {
      e.color = {220, 50, 180, 255};
      e.opacity = .3;
      e.bevel.shadowBlend = c::BlendMode::ColorDodge;
    }
    if (edit == 1) {
      e.angle = 44;
      e.bevel.altitude = 70;
    }
    if (edit == 2)
      e.bevel.gloss = c::effectContourPreset(true, 2);
    if (edit == 3)
      e.bevel.surface = c::effectContourPreset(false, 3);
    l.effects = changed;
    l.effectCache = initial;
    l = c::prepareSpatialFilterLayer(l);
    check(l.effectCache->bevelDistance == initial->bevelDistance,
          "parameter changes reuse distance field");
    if (edit < 3)
      check(l.effectCache->bevelNormals == initial->bevelNormals,
            "lighting/gloss/material reuse normals");
    if (edit == 0)
      check(l.effectCache == initial, "material edit no mask work");
    if (edit == 3)
      check(l.effectCache->bevelNormals != initial->bevelNormals,
            "surface rebuilds normals only");
  }
  for (int neutral = 0; neutral < 4; ++neutral) {
    auto s = settings();
    auto &e = s->items[7];
    if (neutral == 0)
      e.enabled = false;
    if (neutral == 1)
      e.size = 0;
    if (neutral == 2)
      e.bevel.depth = 0;
    if (neutral == 3) {
      e.opacity = 0;
      e.bevel.shadowOpacity = 0;
    }
    check(!c::hasActiveLayerEffects(s), "neutral bevel cheap bypass");
  }
  {
    c::Document document({{180, 120}, 96});
    auto source = layer(artwork());
    source.effects = settings();
    source = c::prepareSpatialFilterLayer(source);
    const auto cached = source.effectCache;
    document.insertLayer(0, source);
    auto zero = std::make_shared<c::LayerEffectStack>(*source.effects);
    zero->items[7].opacity = 0;
    zero->items[7].bevel.shadowOpacity = 0;
    document.setLayerEffects(source.id, zero);
    document.setLayerEffects(source.id, source.effects);
    check(document.layer(source.id)->effectCache == cached &&
              c::layerEffectCacheValid(*document.layer(source.id)),
          "zero-strength roundtrip retains geometry");
    zero->items[5].enabled = true;
    document.setLayerEffects(source.id, zero);
    *document.layer(source.id) =
        c::prepareSpatialFilterLayer(*document.layer(source.id));
    auto restored = std::make_shared<c::LayerEffectStack>(*zero);
    restored->items[7] = source.effects->items[7];
    document.setLayerEffects(source.id, restored);
    *document.layer(source.id) =
        c::prepareSpatialFilterLayer(*document.layer(source.id));
    check(document.layer(source.id)->effectCache->bevelDistance ==
                  cached->bevelDistance &&
              document.layer(source.id)->effectCache->masks[6] ==
                  cached->masks[6],
          "other styles and zero strength preserve lighting cache");
    auto cancelled = layer(artwork());
    cancelled.effects = settings();
    c::FilterPreparationOptions options;
    options.cancelled = [] { return true; };
    bool refused = false;
    try {
      (void)c::prepareLayerEffects(cancelled, options);
    } catch (const std::runtime_error &) {
      refused = true;
    }
    check(refused && !cancelled.effectCache,
          "cancelled work cannot publish a partial cache");
    options.cancelled = {};
    options.byteBudget = 1024;
    refused = false;
    try {
      (void)c::prepareLayerEffects(cancelled, options);
    } catch (const std::runtime_error &) {
      refused = true;
    }
    check(refused, "bevel preflights memory before allocations");
  }
  // Independent flat half-plane and symmetry checks, not a renderer golden.
  QImage rectangle(64, 64, QImage::Format_RGBA8888);
  rectangle.fill(Qt::transparent);
  for (int y = 8; y < 56; ++y)
    for (int x = 8; x < 56; ++x)
      rectangle.setPixelColor(x, y, QColor(120, 120, 120));
  l = layer(rectangle);
  state = settings();
  state->items[7].angle = 0;
  state->items[7].bevel.altitude = 0;
  l.effects = state;
  l = c::prepareSpatialFilterLayer(l);
  const auto &d = *l.effectCache->bevelDistance;
  const auto &n = *l.effectCache->bevelNormals;
  const auto index = [&](int x, int y) {
    return std::size_t(y - d.bounds.y) * std::size_t(d.bounds.width) +
           std::size_t(x - d.bounds.x);
  };
  check(n.xy[index(32, 32)] == std::array<std::int16_t, 2>{0, 0},
        "flat plateau no normal/tint");
  check(n.xy[index(10, 32)][0] == -n.xy[index(53, 32)][0] &&
            n.xy[index(10, 32)][1] == 0,
        "symmetric sides, no tangential normal");
  const auto normal = l.effectCache;
  state = std::make_shared<c::LayerEffectStack>(*state);
  state->items[7].bevel.down = true;
  l.effects = state;
  l = c::prepareSpatialFilterLayer(l);
  check(c::sampleEffectMask(*normal->masks[6], {53.5, 32.5}) ==
            c::sampleEffectMask(*l.effectCache->masks[7], {53.5, 32.5}),
        "Up/Down reverses horizontal light response");
  for (int style = 0; style < 3; ++style) {
    state = settings();
    state->items[7].bevel.style = c::BevelStyle(style);
    state->items[7].size = 64;
    l.effects = state;
    l = c::prepareSpatialFilterLayer(l);
    check(c::layerEffectCacheValid(l), "large widths valid");
    if (style)
      check(l.effectCache->visualBounds.x < 0,
            "outer support expands beyond source cache");
  }
  QImage feathered(40, 40, QImage::Format_RGBA8888);
  feathered.fill(Qt::transparent);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 40; ++x) {
      const auto alpha = int(std::lround(180 * std::clamp(
          (15 - std::hypot(x - 19.5, y - 19.5)) / 6., 0., 1.)));
      feathered.setPixelColor(x, y, QColor(90, 150, 20, alpha));
    }
  auto soft = layer(feathered);
  soft.effects = settings();
  const auto originalSurface = std::get<c::RasterLayer>(soft.payload).surface;
  soft = c::prepareSpatialFilterLayer(soft);
  c::PreparedLayerSampler softSampler(soft);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 40; ++x)
      check(std::abs(softSampler.sample({x + .5, y + .5})[3] -
                     feathered.pixelColor(x, y).alphaF()) < 1e-6,
            "feathered coverage retained without multiplying twice");
  check(std::get<c::RasterLayer>(soft.payload).surface == originalSurface,
        "relief never replaces source pixels");
  QImage empty(1, 1, QImage::Format_RGBA8888);
  empty.fill(QColor(220, 70, 30, 0));
  auto blank = layer(empty);
  auto extreme = settings();
  extreme->items[7].size = 256;
  extreme->items[7].bevel.style = c::BevelStyle::Emboss;
  blank.effects = extreme;
  blank = c::prepareSpatialFilterLayer(blank);
  c::PreparedLayerSampler blankSampler(blank);
  check(blankSampler.sample({.5, .5})[3] == 0 &&
            blankSampler.sample({-40.5, .5})[3] == 0,
        "empty hidden RGB cannot create relief even at maximum width");
  for (bool gloss : {false, true})
    for (int p = 0; p < 4; ++p) {
      auto curve = c::effectContourPreset(gloss, p);
      check(c::validEffectContour(curve), "generated preset valid");
      for (int n = 0; n <= 4096; ++n) {
        const auto v = c::evaluateEffectContour(curve, n / 4096.);
        check(std::isfinite(v) && v >= 0 && v <= 1,
              "bounded nonmonotonic curve");
      }
      curve.points[1].input = 0;
      check(!c::validEffectContour(curve), "coincident curve points rejected");
    }
  auto legacy = u::detail::encodeLayerEffects(c::LayerEffectStack{});
  check(legacy["items"].toArray().size() == 7,
        "default bevel does not add an essential capability");
  check(!u::detail::decodeLayerEffects(legacy)->items[7].enabled,
        "old seven-effect projects stay unstyled");
}
void persistenceAndUi() {
  c::Document doc({{180, 120}, 144});
  auto l = layer(artwork());
  auto id = l.id;
  doc.insertLayer(0, l);
  doc.markSaved();
  c::History history;
  auto s = settings();
  auto &b = s->items[7].bevel;
  b.surface = c::effectContourPreset(false, 3);
  b.gloss = c::effectContourPreset(true, 2);
  b.gloss.points[1].corner = true;
  {
    c::EffectEditTransaction edit(doc, id);
    check(edit.update(s) && edit.commit(history),
          "one bevel history transaction");
  }
  check(history.undo(doc) && history.redo(doc), "bevel undo redo");
  {
    c::EffectEditTransaction edit(doc, id);
    edit.update(settings());
    check(edit.cancel(), "bevel cancel");
  }
  check(*doc.layer(id)->effects == *s, "cancel retains both contours");
  QTemporaryDir files;
  const auto path = files.filePath("bevel.vulkana");
  check(bool(u::saveProject(path, doc)), "save bevel");
  auto loaded = u::loadProject(path);
  check(bool(loaded), "load bevel");
  if (loaded)
    check(*loaded.document->layer(id)->effects == *s,
          "versioned parameters and corners exact roundtrip");
  c::Document destination(doc.canvas());
  auto transfer = c::captureLayerTransfer(doc, std::array{id});
  c::History copied;
  auto insert =
      c::insertLayerTransfer(destination, std::move(transfer), {0, 0}, {});
  check(insert && copied.execute(destination, std::move(insert)),
        "cross-document bevel copy");
  const auto copiedId = destination.layers().front().id;
  auto changed = std::make_shared<c::LayerEffectStack>(*s);
  changed->items[7].bevel.surface.points[1].output = .1;
  destination.setLayerEffects(copiedId, changed);
  check(copiedId != id && *doc.layer(id)->effects == *s,
        "independent contour edits after transfer");
  u::EffectsPanel panel;
  panel.setTarget(doc.layer(id));
  int edits = 0;
  panel.onPreview = [&](c::LayerEffectState) { ++edits; };
  auto *nav = panel.findChild<QComboBox *>("EffectNavigation");
  check(nav && nav->count() == 8, "eighth independent effect page");
  nav->setCurrentIndex(7);
  auto *selector = panel.findChild<QComboBox *>("BevelContourSelector");
  selector->setCurrentIndex(1);
  selector->setCurrentIndex(0);
  nav->setCurrentIndex(0);
  check(edits == 0, "curve/page navigation does not edit");
  auto *pages = panel.findChild<QStackedWidget *>("EffectParameterPages");
  check(pages && pages->minimumSizeHint() == pages->currentWidget()->minimumSizeHint(),
        "hidden bevel controls do not enlarge other effect pages");
  nav->setCurrentIndex(7);
  panel.resize(540, 1200);
  panel.show();
  QApplication::processEvents();
  const auto choice = [&](const QString &name) -> QComboBox * {
    for (auto *box : pages->currentWidget()->findChildren<QComboBox *>())
      if (box->accessibleName() == name)
        return box;
    return nullptr;
  };
  const auto sameRow = [](QWidget *a, QWidget *b) {
    return a && b && a->geometry().center().y() == b->geometry().center().y();
  };
  check(sameRow(choice("Style"), choice("Direction")),
        "bevel style and direction share a compact row");
  check(choice("Highlight blend") && choice("Shadow blend"),
        "bevel material blends have distinct labels");
  auto *presets = panel.findChild<QComboBox *>("BevelContourPreset");
  auto *interpolation = panel.findChild<QComboBox *>("BevelContourInterpolation");
  check(sameRow(presets, interpolation), "contour preset and interpolation share a row");
  auto *graph = panel.findChild<QWidget *>("BevelContourEditor");
  auto *corner = panel.findChild<QCheckBox *>("BevelContourCorner");
  auto *layout = pages->currentWidget()->layout();
  check(layout->indexOf(corner) == layout->indexOf(graph) + 1 &&
            std::abs(corner->geometry().center().x() - graph->geometry().center().x()) <= 1,
        "corner point is centered immediately below the graph");
  for (auto *label : panel.findChildren<QLabel *>())
    check(!label->text().contains("Escape cancels"), "effects hint omits unreliable Escape instruction");
  if (const auto path = qEnvironmentVariable("IMAGEEDITOR_BEVEL_UI_REVIEW"); !path.isEmpty())
    check(panel.grab().save(path), "save compact bevel UI review");
  const auto above = [](QWidget *a, QWidget *b) {
    return a && b && a->geometry().bottom() < b->geometry().top();
  };
  nav->setCurrentIndex(0);
  QApplication::processEvents();
  check(above(choice("Blend"), choice("Position")) &&
            above(choice("Position"), panel.findChild<QWidget *>("Effect0Opacity")) &&
            above(panel.findChild<QWidget *>("Effect0Opacity"), panel.findChild<QWidget *>("Effect0Size")),
        "stroke position follows Blend with both sliders below");
  nav->setCurrentIndex(6);
  QApplication::processEvents();
  auto *reverse = panel.findChild<QCheckBox *>("Effect6Reverse");
  const auto *typeRow = pages->currentWidget()->layout()->itemAt(1)->layout();
  // Qt's checkbox and combobox style margins can offset widget centers by a
  // pixel even within the same row. Verify actual row membership and order.
  check(typeRow && typeRow->indexOf(choice("Type")) >= 0 &&
            typeRow->indexOf(reverse) > typeRow->indexOf(choice("Type")) &&
            above(choice("Blend"), choice("Type")) &&
            above(choice("Type"), panel.findChild<QWidget *>("Effect6Opacity")),
        "gradient Type and Reverse share a row below Blend and above sliders");
  check(edits == 0, "layout and page switching do not alter settings");
  c::LayerEffectState preview;
  panel.onPreview = [&](c::LayerEffectState state) { preview = std::move(state); };
  reverse->setChecked(true);
  check(preview && preview->items[6].reverse && preview->items[7] == s->items[7],
        "inline Reverse changes only its gradient setting");
  nav->setCurrentIndex(0);
  nav->setCurrentIndex(6);
  check(reverse->isChecked(), "inline Reverse persists across page navigation");
  panel.hide();
}
void psd(const QString &directory) {
  c::Document d({{180, 120}, 144});
  auto l = layer(artwork());
  auto s = settings();
  s->items[7].bevel.surface = c::effectContourPreset(false, 2);
  s->items[7].bevel.gloss = c::effectContourPreset(true, 2);
  l.effects = s;
  d.insertLayer(0, l);
  auto snapshot = u::capturePsdExport(d, 42);
  std::atomic_bool cancel = false;
  auto plan = u::planPsdExport(snapshot, {}, cancel);
  check(bool(plan), "bevel PSD editable plan");
  const auto path = directory + "/bevel.psd";
  const auto written = u::writePsdExport(snapshot, plan, path, cancel);
  check(bool(written), "write native bevel PSD");
  auto inspected = u::inspectPsd(path, std::make_shared<u::PsdJob>());
  check(bool(inspected.source), "reinspect native bevel");
  if (inspected.source) {
    const auto &record = inspected.source->records.front();
    u::psd::Reader r(record.tags["lfx2"]);
    r.skip(4);
    auto descriptor = u::psd::versionedDescriptor(r);
    const auto bevel = descriptor["ebbl"].toMap();
    check(bevel.contains("TrnS") && bevel.contains("MpgS"),
          "separate modern contour descriptors");
    auto choices = u::defaultPsdOptions(inspected);
    auto result =
        u::convertPsd(inspected, choices, std::make_shared<u::PsdJob>());
    check(bool(result.document), "editable bevel imported");
    if (result.document) {
      const auto &e = result.document->layers().front().effects;
      check(e && e->items[7].enabled &&
                e->items[7].bevel.surface.points ==
                    s->items[7].bevel.surface.points &&
                e->items[7].bevel.gloss.points ==
                    s->items[7].bevel.gloss.points,
            "PSD actual independent contour values");
    }
  }
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "read PSD for unsupported fixture");
  auto bytes = file.readAll();
  file.close();
  check(bytes.contains("SfBL"), "fixture has Smooth enum");
  bytes.replace("SfBL", "PrBL");
  const auto unsupported = directory + "/bevel-chisel.psd";
  file.setFileName(unsupported);
  check(file.open(QIODevice::WriteOnly), "write unsupported fixture");
  file.write(bytes);
  file.close();
  const auto analysis =
      u::inspectPsd(unsupported, std::make_shared<u::PsdJob>());
  check(analysis.error.isEmpty() && !analysis.layers.front().editable &&
            analysis.layers.front().basePixels,
        "unsupported chisel honest fallback, not Smooth substitution");
  for (int type = 0; type < 2; ++type) {
    c::Document typed({{180, 120}, 144});
    c::Layer content;
    if (type == 0) {
      c::TextLayer text;
      text.utf8 = "OB";
      text.defaultStyle.font.family = "DejaVu Sans";
      text.defaultStyle.sizePixels = 54;
      text.defaultStyle.color = {180, 100, 40, 255};
      content = c::Layer::text("Editable bevel text", c::normalizedText(text));
    } else {
      c::ShapeLayer shape;
      shape.kind = c::ShapeKind::Triangle;
      shape.size = {80, 70};
      shape.fillEnabled = false;
      shape.strokeEnabled = true;
      shape.strokeWidth = 10;
      shape.strokeColor = {180, 100, 40, 255};
      content = c::Layer::shape("Editable hollow triangle", shape);
    }
    content.effects = settings();
    content.localToDocument = {1.2, 0, 25, 0, 1.2, 20};
    typed.insertLayer(0, content);
    auto capture = u::capturePsdExport(typed, 43);
    auto nativePlan = u::planPsdExport(capture, {}, cancel);
    const auto typedPath =
        directory + (type == 0 ? "/bevel-text.psd" : "/bevel-shape.psd");
    check(bool(nativePlan) && nativePlan.entries.front().editable,
          "typed bevel remains editable export");
    check(bool(u::writePsdExport(capture, nativePlan, typedPath, cancel)),
          "typed bevel PSD written");
    check(bool(u::saveProject(typedPath.chopped(4)+".vulkana",typed)),"editable typed manual fixture");
    auto input = u::inspectPsd(typedPath, std::make_shared<u::PsdJob>());
    auto options = u::defaultPsdOptions(input);
    auto converted =
        u::convertPsd(input, options, std::make_shared<u::PsdJob>());
    check(bool(converted.document), "typed bevel import works");
    if (converted.document) {
      const auto &actual = converted.document->layers().front();
      check(type == 0 ? std::holds_alternative<c::TextLayer>(actual.payload)
                      : std::holds_alternative<c::ShapeLayer>(actual.payload),
            "typed editability retained, not cached raster disguise");
      check(actual.effects && actual.effects->items[7].enabled,
            "typed bevel live");
    }
  }
}
void sheet(const QString &path) {
  QImage sheet(1200, 1040, QImage::Format_RGB32);
  sheet.fill(QColor(30, 31, 34));
  QPainter painter(&sheet);
  for (int style = 0; style < 3; ++style)
    for (int option = 0; option < 8; ++option) {
      auto l = layer(artwork());
      auto s = settings();
      auto &e = s->items[7];
      e.bevel.style = c::BevelStyle(style);
      if (option == 1)
        e.bevel.down = true;
      if (option == 2)
        e.angle = 45;
      if (option == 3)
        e.bevel.altitude = 80;
      if (option == 4) {
        e.size = 16;
        e.bevel.depth = 3;
      }
      if (option == 5)
        e.bevel.soften = 3;
      if (option == 6)
        e.bevel.surface = c::effectContourPreset(false, 3);
      if (option == 7)
        e.bevel.gloss = c::effectContourPreset(true, 2);
      l.effects = s;
      l.localToDocument.m02 = 25;
      l.localToDocument.m12 = 27;
      c::Document d({{240, 175}, 96});
      const auto background=option%4==0?QColor(220,220,220):option%4==1?QColor(45,65,90):option%4==2?QColor(30,31,34):QColor(Qt::transparent);
      QImage backdrop(240,175,QImage::Format_RGBA8888);backdrop.fill(background);
      d.insertLayer(0,layer(backdrop));d.insertLayer(1, l);
      const auto rendered = u::flattenDocument(d);
      check(bool(rendered), "sheet native rendering");
      const int at = style * 8 + option, x = (at % 5) * 240, y = (at / 5) * 208;
      for(int cy=0;cy<175;cy+=8)for(int cx=0;cx<240;cx+=8)
        painter.fillRect(x+cx,y+cy,8,std::min(8,175-cy),(cx/8+cy/8)%2?QColor(90,90,90):QColor(120,120,120));
      painter.drawImage(x, y, rendered.image);
      painter.setPen(Qt::white);
      painter.drawText(x + 8, y + 193,
                       QString("%1 · %2").arg(
                           QStringList{"Inner", "Outer", "Emboss"}[style],
                           QStringList{"Default", "Down", "Light 45°",
                                       "Altitude 80°", "Large / deep", "Soften",
                                       "Surface ridge", "Gloss ring"}[option]));
    }
  painter.end();
  check(sheet.save(path), "write generated sheet");
}
void benchmark(int width, int height) {
  QImage image(width, height, QImage::Format_RGBA8888);
  image.fill(Qt::transparent);
  {
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(180, 100, 40));
    p.drawEllipse(QRectF(40, 40, width - 80, height - 80));
  }
  auto l = layer(image);
  auto s = settings();
  s->items[7].size = 16;
  l.effects = s;
  const auto time = [&](const char *label) {
    auto start = std::chrono::steady_clock::now();
    const auto before = l.effectCache;
    l = c::prepareSpatialFilterLayer(l);
    std::cout << label << '='
              << std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count()
              << "ms";
    if (before != l.effectCache)
      std::cout << " distance=" << l.effectCache->bevelDistanceMilliseconds
                << " normals=" << l.effectCache->bevelNormalMilliseconds
                << " lighting=" << l.effectCache->bevelLightingMilliseconds;
    std::cout << '\n';
  };
  time("cold");
  const auto distance = l.effectCache->bevelDistance;
  const auto normals = l.effectCache->bevelNormals;
  s = std::make_shared<c::LayerEffectStack>(*s);
  s->items[7].angle = 10;
  l.effects = s;
  time("lighting");
  check(l.effectCache->bevelDistance == distance &&
            l.effectCache->bevelNormals == normals,
        "warm benchmark geometry reused");
  s = std::make_shared<c::LayerEffectStack>(*s);
  s->items[7].bevel.gloss = c::effectContourPreset(true, 2);
  l.effects = s;
  time("gloss");
  s = std::make_shared<c::LayerEffectStack>(*s);
  s->items[7].color = {255, 120, 20, 255};
  l.effects = s;
  time("color");
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::cout << width << 'x' << height
            << " retained geometry=" << l.effectCache->bevelMemoryCost()
            << " peakRSS=" << usage.ru_maxrss << "KiB\n";
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--sheet")
      sheet(QString::fromLocal8Bit(argv[2]));
    else if (argc == 3 && std::string_view(argv[1]) == "--psd")
      psd(QString::fromLocal8Bit(argv[2]));
    else if (argc == 3)
      benchmark(std::stoi(argv[1]), std::stoi(argv[2]));
    else {
      contracts();
      persistenceAndUi();
      QTemporaryDir files;
      psd(files.path());
    }
  } catch (const std::exception &e) {
    check(false, e.what());
  }
  return failures ? 1 : 0;
}
