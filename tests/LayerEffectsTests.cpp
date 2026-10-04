#include "../src/ui/src/LayerEffectCodec.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QApplication>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QPainter>
#include <QTemporaryDir>
#include <chrono>
#include <iostream>
#include <sys/resource.h>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool ok, const char *why) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL " << why << '\n';
  }
}
c::Layer fixture(std::uint32_t width = 24, std::uint32_t height = 24) {
  std::vector<std::byte> p(std::size_t(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto at = (std::size_t(y) * width + x) * 4;
      p[at] = std::byte{180};
      p[at + 1] = std::byte{85};
      p[at + 2] = std::byte{210};
      const bool inside = x > width / 8 && y > height / 8 &&
                          x < 7 * width / 8 && y < 7 * height / 8;
      const bool hole = x > width * 3 / 8 && x < width * 5 / 8 &&
                        y > height * 3 / 8 && y < height * 5 / 8;
      p[at + 3] =
          std::byte(inside && !hole ? x == width / 8 + 1 ? 128 : 255 : 0);
    }
  return c::Layer::raster("Cutout",
                          std::make_shared<c::ContiguousRasterSurface>(
                              c::Extent2u{width, height}, std::move(p)));
}
std::shared_ptr<c::LayerEffectStack> effect(int i) {
  auto s = std::make_shared<c::LayerEffectStack>();
  s->items[std::size_t(i)].enabled = true;
  return s;
}
void contracts() {
  auto l = fixture();
  const auto source = c::intrinsicSurface(l);
  const auto revision = source->revision();
  const auto base = c::decodeColor({90, 140, 210, 128}),
             bg = c::decodeColor({180, 90, 35, 190});
  const auto disabled = c::compileLayerEffects({}, {});
  for (auto mode : c::allBlendModes) {
    auto result =
        c::compositeLayerEffects(bg, base, disabled, nullptr, {}, .7F, mode);
    auto reference = c::compositeLayer(bg, base, .7F, mode);
    for (int i = 0; i < 4; ++i)
      check(std::abs(result[std::size_t(i)] - reference[std::size_t(i)]) < 1e-6,
            "disabled identity all blend modes");
  }
  l.effects = effect(5);
  auto state = std::make_shared<c::LayerEffectStack>(*l.effects);
  state->items[5].color = {10, 230, 70, 255};
  l.effects = state;
  l = c::prepareSpatialFilterLayer(l);
  c::PreparedLayerSampler sampler(l);
  check(c::encodeColor(sampler.sample({4.5, 6.5})) ==
            c::Rgba8{10, 230, 70, 128},
        "normal overlay preserves partial alpha exactly");
  check(sampler.sample({12.5, 12.5})[3] == 0,
        "overlay preserves transparent hole");
  for (int type = 0; type < int(c::layerEffectCount); ++type) {
    l.effects = effect(type);
    l = c::prepareSpatialFilterLayer(l);
    check(c::layerEffectCacheValid(l), "prepared cache valid");
    const auto jobSettings = l.effects;
    auto changed = std::make_shared<c::LayerEffectStack>(*l.effects);
    changed->items[std::size_t(type)].color = {1, 2, 3, 140};
    changed->items[std::size_t(type)].opacity = .4;
    changed->items[std::size_t(type)].blendMode = c::BlendMode::ColorDodge;
    l.effects = changed;
    const auto cache = l.effectCache;
    check(c::equivalentLayerEffectGeometry(jobSettings, l.effects),
          "material changes keep an in-flight geometry job applicable");
    if (type < 5) {
      auto resized = std::make_shared<c::LayerEffectStack>(*l.effects);
      resized->items[std::size_t(type)].size += 1;
      check(!c::equivalentLayerEffectGeometry(jobSettings, resized),
            "geometry changes reject an in-flight geometry job");
    }
    check(c::layerEffectCacheValid(l),
          "color/opacity/mode do not invalidate geometry");
    check(c::prepareLayerEffects(l, {}) == cache,
          "reuses masks on material edits");
    const auto p = c::compileLayerEffects(l.effects, {0, 0, 24, 24});
    check(c::compositeLayerEffects(bg, base, p, l.effectCache.get(), {5, 5}, 0,
                                   c::BlendMode::Normal) == bg,
          "zero layer opacity removes all effects");
  }
  for (auto position : {c::StrokePosition::Inside, c::StrokePosition::Center,
                        c::StrokePosition::Outside}) {
    state = effect(0);
    state->items[0].position = position;
    state->items[0].size = 3;
    l.effects = state;
    l = c::prepareSpatialFilterLayer(l);
    check(c::sampleEffectMask(*l.effectCache->masks[0], {10.5, 12.5}) ==
              (position == c::StrokePosition::Inside ? 0 : 1),
          "stroke outlines hole, not bounding rectangle");
    if (position == c::StrokePosition::Inside)
      check(l.effectCache->masks[0]->coverage->bounds().empty(),
            "inside stroke has no exterior");
  }
  state = effect(6);
  state->items[6].angle = 0;
  state->items[6].color = {255, 0, 0, 0};
  state->items[6].secondColor = {0, 0, 255, 255};
  l.effects = state;
  l = c::prepareSpatialFilterLayer(l);
  const auto first = c::PreparedLayerSampler(l).sample({8.5, 6.5});
  state = std::make_shared<c::LayerEffectStack>(*state);
  state->items[1].enabled = true;
  state->items[1].distance = 100;
  state->items[1].size = 40;
  l.effects = state;
  l = c::prepareSpatialFilterLayer(l);
  check(c::PreparedLayerSampler(l).sample({8.5, 6.5}) == first,
        "gradient frame stable across effect padding");
  check(source->revision() == revision, "source never edited");
  auto empty = c::Layer::raster(
      "Empty", std::make_shared<c::ContiguousRasterSurface>(
                   c::Extent2u{1, 1}, c::Rgba8{255, 70, 80, 0}));
  auto all = std::make_shared<c::LayerEffectStack>();
  for (auto &e : all->items)
    e.enabled = true;
  empty.effects = all;
  empty = c::prepareSpatialFilterLayer(empty);
  check(c::PreparedLayerSampler(empty).sample({.5, .5}) ==
            c::PremultipliedColor{},
        "empty/hidden RGB remains empty");
}
void historyAndOutput() {
  c::Document doc({{48, 48}, 96});
  auto layer = fixture();
  layer.localToDocument.m02 = 12;
  layer.localToDocument.m12 = 12;
  const auto id = layer.id;
  doc.insertLayer(0, layer);
  doc.markSaved();
  c::History history;
  auto s = effect(0);
  s->items[0].size = 4;
  s->items[1].enabled = true;
  s->items[5].enabled = true;
  s->items[5].color = {20, 130, 230, 200};
  {
    c::EffectEditTransaction edit(doc, id);
    check(edit.update(s), "start preview");
    s = std::make_shared<c::LayerEffectStack>(*s);
    s->items[0].size = 5;
    check(edit.update(s), "refine preview");
    check(!doc.isModified(), "preview not persistent checkpoint");
    check(edit.commit(history), "commit");
  }
  check(history.undoDepth() == 1, "one scrub one history");
  check(history.undo(doc), "undo effect");
  check(!c::hasActiveLayerEffects(doc.layer(id)->effects),
        "undo restores unstyled");
  {
    c::EffectEditTransaction edit(doc, id);
    check(!edit.commit(history) && !edit.active(),
          "no-op finishes without command");
  }
  check(history.redoDepth() == 1, "no-op preserves redo");
  check(history.redo(doc), "redo");
  {
    c::EffectEditTransaction edit(doc, id);
    edit.update(effect(6));
    check(edit.cancel(), "cancel preview");
  }
  check(c::equivalentLayerEffects(doc.layer(id)->effects, s),
        "cancel restores values");
  *doc.layer(id) = c::prepareSpatialFilterLayer(*doc.layer(id));
  const auto frame = u::flattenDocument(doc);
  check(bool(frame), "native output");
  QTemporaryDir dir;
  check(bool(u::saveProject(dir.filePath("effects.vulkana"), doc)),
        "save effects");
  const auto reopened = u::loadProject(dir.filePath("effects.vulkana"));
  check(bool(reopened), "load effects");
  if (reopened) {
    check(c::equivalentLayerEffects(reopened.document->layer(id)->effects, s),
          "all editable parameters survive");
    check(u::flattenDocument(*reopened.document).image == frame.image,
          "roundtrip native pixels exact");
  }
  auto descriptor = u::detail::encodeLayerEffects(*s);
  descriptor["version"] = 99;
  bool rejected = false;
  try {
    (void)u::detail::decodeLayerEffects(descriptor);
  } catch (...) {
    rejected = true;
  }
  check(rejected, "unknown essential version rejected");
  auto raster =
      u::prepareRasterizeLayers(doc, {{id}, id, id}, 256ULL * 1024 * 1024);
  check(bool(raster.command), "rasterize styles");
  if (raster.command) {
    check(raster.command->apply(doc), "apply bake");
    check(!c::hasActiveLayerEffects(doc.layer(id)->effects),
          "no double-applied styles");
    check(u::flattenDocument(doc).image == frame.image,
          "identity rasterize native output exact");
    check(raster.command->undo(doc), "undo rasterize");
    check(c::equivalentLayerEffects(doc.layer(id)->effects, s),
          "undo editable styles");
  }
  for (auto mode : c::allBlendModes) {
    auto test = fixture();
    auto state = effect(1);
    state->items[1].blendMode = mode;
    test.effects = state;
    test.opacity = .55F;
    test.blendMode = c::BlendMode::Screen;
    c::Document isolated({{48, 48}, 96});
    test.localToDocument.m02 = 8;
    test.localToDocument.m12 = 8;
    isolated.insertLayer(0, test);
    const auto before = u::flattenDocument(isolated);
    const auto baked = u::rasterizeLayerContent(isolated, test.id);
    check(bool(before) && bool(baked),
          "all exterior blend modes bake isolated safely");
  }
}
void morphologyReference() {
  auto l = fixture();
  const auto s = c::intrinsicSurface(l);
  const auto extent = s->extent();
  std::vector<std::byte> pixels(std::size_t(extent.width) * extent.height * 4);
  s->copyRgba8({0, 0, 24, 24}, pixels, 96);
  // Independent exhaustive grayscale Euclidean-disk oracle (no pyramid).
  for (double radius : {.3, 1.3, 3.0, 5.5}) {
    auto state = effect(0);
    state->items[0].size = radius;
    l.effects = state;
    l = c::prepareSpatialFilterLayer(l);
    for (int y = -6; y < 30; ++y)
      for (int x = -6; x < 30; ++x) {
        double dilated = 0, base = 0;
        for (int sy = 0; sy < 24; ++sy)
          for (int sx = 0; sx < 24; ++sx) {
            const double a =
                std::to_integer<unsigned>(
                    pixels[(std::size_t(sy) * 24 + std::size_t(sx)) * 4 + 3]) /
                255.0;
            if (x == sx && y == sy)
              base = a;
            dilated = std::max(
                dilated, std::min(a, std::clamp(radius + 1.0 -
                                                    std::hypot(double(x - sx),
                                                               double(y - sy)),
                                                0.0, 1.0)));
          }
        const auto expected = std::max(0.0, dilated - base);
        const auto actual = c::sampleEffectMask(*l.effectCache->masks[0], {x + .5, y + .5});
        // Compare to continuous double coverage, not a second quantization:
        // 0.3 lies exactly on an R8 half-step, where float and double can
        // round to opposite neighbors. Only the one R8 storage error is allowed.
        check(std::abs(double(actual)-expected)<=.5/255+2e-7,
              "circular morphology agrees with continuous double oracle within one mask quantization");
      }
  }
}
c::Document comparisonDocument(c::LayerEffectState effects, int backdrop,
                               bool transformed) {
  c::Document doc({{224, 128}, 96});
  const std::array<c::Rgba8, 4> colors{{{0, 0, 0, 0},
                                        {240, 240, 235, 255},
                                        {28, 31, 37, 255},
                                        {60, 95, 160, 255}}};
  if (backdrop)
    doc.insertLayer(
        0, c::Layer::raster(
               "Backdrop",
               std::make_shared<c::ContiguousRasterSurface>(
                   c::Extent2u{224, 128}, colors[std::size_t(backdrop)])));
  auto add = [&](c::Layer l, double x, double y) {
    l.effects = effects;
    l.localToDocument = transformed ? c::AffineTransform{.9, -.15, x, .1, .9, y}
                                    : c::AffineTransform{1, 0, x, 0, 1, y};
    doc.insertLayer(doc.layers().size(), std::move(l));
  };
  c::TextLayer text;
  text.utf8 = "O8";
  text.defaultStyle.font.family = "Noto Sans";
  text.defaultStyle.sizePixels = 35;
  text.defaultStyle.color = {190, 125, 75, 255};
  add(c::Layer::text("Text holes", c::normalizedText(text)), 12, 10);
  c::ShapeLayer shape;
  shape.size = {42, 36};
  shape.kind = c::ShapeKind::Triangle;
  shape.fillColor = {70, 170, 125, 255};
  add(c::Layer::shape("Triangle", shape), 82, 14);
  shape.kind = c::ShapeKind::Ellipse;
  shape.fillColor = {125, 85, 190, 150};
  add(c::Layer::shape("Semitransparent ellipse", shape), 151, 17);
  shape.kind = c::ShapeKind::RoundedRectangle;
  shape.fillEnabled = false;
  shape.strokeEnabled = true;
  shape.strokeColor = {200, 130, 70, 255};
  shape.strokeWidth = 3;
  add(c::Layer::shape("Hollow shape", shape), 21, 78);
  add(fixture(40, 35), 98, 77);
  add(fixture(30, 30), 165, 79);
  return doc;
}
void comparisonSheet(const QString &path) {
  const auto font = QFileInfo(QString::fromUtf8(__FILE__))
                        .dir()
                        .filePath("assets/fonts/NotoSans-Regular.ttf");
  check(QFontDatabase::addApplicationFont(font) >= 0,
        "load pinned review font");
  struct Case {
    QString label;
    c::LayerEffectState state;
    bool transformed = false;
  };
  std::vector<Case> cases{{"Disabled", {}}};
  for (int i = 0; i < 7; ++i)
    cases.push_back(
        {QString::fromUtf8(c::layerEffectName(c::LayerEffectType(i))),
         effect(i)});
  for (auto position : {c::StrokePosition::Inside, c::StrokePosition::Center}) {
    auto s = effect(0);
    s->items[0].position = position;
    cases.push_back({position == c::StrokePosition::Inside ? "Stroke inside"
                                                           : "Stroke center",
                     s});
  }
  for (int angle : {-90, 0, 90, 180}) {
    auto s = effect(1);
    s->items[1].angle = angle;
    s->items[1].distance = 9;
    cases.push_back({QString("Shadow %1°").arg(angle), s});
  }
  for (int i : {1, 2, 3, 4}) {
    auto s = effect(i);
    s->items[std::size_t(i)].spread = 1;
    cases.push_back(
        {QString::fromUtf8(c::layerEffectName(c::LayerEffectType(i))) +
             " · solid",
         s});
  }
  auto gradient = effect(6);
  gradient->items[6].color = {255, 0, 0, 0};
  gradient->items[6].secondColor = {50, 180, 250, 255};
  cases.push_back({"Gradient transparent endpoint", gradient});
  gradient = std::make_shared<c::LayerEffectStack>(*gradient);
  gradient->items[6].gradient = c::GradientType::Radial;
  gradient->items[6].reverse = true;
  gradient->items[6].opacity = .65;
  cases.push_back({"Radial reverse · 65%", gradient, true});
  auto all = std::make_shared<c::LayerEffectStack>();
  for (auto &e : all->items) {
    e.enabled = true;
    e.size = 4;
    e.opacity = .7;
  }
  all->items[6].secondColor = {30, 210, 170, 255};
  cases.push_back({"All seven · transformed", all, true});
  QImage sheet(4 * 240, int(cases.size()) * 159, QImage::Format_RGB32);
  sheet.fill(QColor(30, 33, 39));
  QPainter painter(&sheet);
  painter.setFont(QFont("Noto Sans", 9));
  for (std::size_t row = 0; row < cases.size(); ++row)
    for (int bg = 0; bg < 4; ++bg) {
      auto doc =
          comparisonDocument(cases[row].state, bg, cases[row].transformed);
      const auto flat = u::flattenDocument(doc);
      check(bool(flat), "typed comparison native output");
      const int x = bg * 240 + 8, y = int(row) * 159 + 24;
      painter.setPen(Qt::white);
      painter.drawText(x, y - 7, cases[row].label);
      if (!bg)
        for (int iy = 0; iy < 128; iy += 8)
          for (int ix = 0; ix < 224; ix += 8)
            painter.fillRect(x + ix, y + iy, 8, 8,
                             ((ix + iy) / 8) % 2 ? QColor(110, 110, 115)
                                                 : QColor(145, 145, 150));
      painter.drawImage(x, y, flat.image);
      if (row == cases.size() - 1 && bg == 3)
        check(bool(u::saveProject(path + ".vulkana", doc)),
              "editable comparison project");
    }
  painter.end();
  check(sheet.save(path), "save comparison sheet");
}
void benchmark(int width, int height) {
  auto l = fixture(std::uint32_t(width), std::uint32_t(height));
  auto s = effect(0);
  s->items[0].size = 12;
  s->items[1].enabled = true;
  s->items[1].size = 24;
  s->items[1].distance = 30;
  l.effects = s;
  auto t = std::chrono::steady_clock::now();
  l = c::prepareSpatialFilterLayer(l);
  double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
          .count();
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::size_t retained = 0;
  for (auto &m : l.effectCache->masks)
    if (m)
      retained += m->coverage->memoryCost();
  std::cout << width << 'x' << height << " preparation=" << seconds
            << "s peakRSS=" << usage.ru_maxrss << "KiB masks=" << retained
            << " bytes\n";
  auto state = std::make_shared<c::LayerEffectStack>(*s);
  state->items[1].color = {80, 160, 240, 255};
  l.effects = state;
  t = std::chrono::steady_clock::now();
  auto cache = c::prepareLayerEffects(l, {});
  std::cout << "material update="
            << std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t)
                   .count()
            << "ms reused=" << (cache == l.effectCache) << '\n';
}
} // namespace
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--sheet")
      comparisonSheet(QString::fromLocal8Bit(argv[2]));
    else if (argc == 3)
      benchmark(std::stoi(argv[1]), std::stoi(argv[2]));
    else {
      contracts();
      morphologyReference();
      historyAndOutput();
    }
  } catch (const std::exception &e) {
    check(false, e.what());
  }
  return failures ? 1 : 0;
}
