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
#include <limits>
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
void gradientContracts() {
  auto state = effect(6);
  auto &gradient = state->items[6];
  gradient.angle = 0;
  gradient.color = {240, 30, 90, 64};
  gradient.secondColor = {10, 170, 220, 230};
  check(c::effectiveGradientStops(gradient) == std::vector<c::GradientStop>{
      {0, gradient.color}, {1, gradient.secondColor}}, "legacy gradients expose editable endpoints");
  const auto first = c::decodeColor(gradient.color), last = c::decodeColor(gradient.secondColor);
  for (int step = 0; step <= 256; ++step) {
    const float t = float(step) / 256;
    auto expected = first;
    for (std::size_t i = 0; i < 4; ++i) expected[i] = first[i] * (1 - t) + last[i] * t;
    check(c::sampleLayerEffectGradient(gradient, t) == expected,
          "legacy gradient interpolation remains bit-for-bit identical");
  }
  const auto legacy = c::compileLayerEffects(state, {0,0,100,100});
  check(legacy[23] == float(c::effectGradientStopOffset), "legacy GPU program needs no stop-table storage");
  gradient.gradientStops = c::effectiveGradientStops(gradient);
  const auto explicitEndpoints = c::compileLayerEffects(state, {0,0,100,100});
  check(explicitEndpoints[23] == float(c::effectGradientStopOffset + 2*c::effectGradientStopStride),
        "compiled program locates mask metadata after populated stop table");
  for (int step = 0; step <= 256; ++step) {
    const c::Vec2d local{100. * step / 256, 50};
    const auto base = c::decodeColor({0,0,0,255});
    check(c::compositeLayerEffects({},base,legacy,nullptr,local,1,c::BlendMode::Normal) ==
          c::compositeLayerEffects({},base,explicitEndpoints,nullptr,local,1,c::BlendMode::Normal),
          "explicit endpoint stops preserve legacy compiled output exactly");
  }
  gradient.gradientStops = {{.125,{255,0,0,255}}, {.5,{0,255,0,255}},
                            {.5,{0,0,255,255}}, {.875,{255,255,255,255}}};
  check(c::validLayerEffects(*state), "duplicate stop positions are valid hard transitions");
  check(c::sampleLayerEffectGradient(gradient,0) == c::decodeColor({255,0,0,255}),
        "gradient extends its first color before the first stop");
  check(c::sampleLayerEffectGradient(gradient,1) == c::decodeColor({255,255,255,255}),
        "gradient extends its final color after the last stop");
  check(c::sampleLayerEffectGradient(gradient,.5) == c::decodeColor({0,0,255,255}),
        "last coincident stop owns the exact hard transition");
  const auto multi = c::compileLayerEffects(state,{0,0,100,100});
  for (int step = 0; step <= 256; ++step) {
    const double t = double(step) / 256;
    const auto expected = c::sampleLayerEffectGradient(gradient,t);
    const auto actual = c::compositeLayerEffects({},c::decodeColor({0,0,0,255}),multi,
        nullptr,{t*100,50},1,c::BlendMode::Normal);
    for (std::size_t i = 0; i < 4; ++i)
      check(std::abs(actual[i]-expected[i]) < .000002F,
            "compiled multi-stop renderer agrees with the preview sampler");
  }
  gradient.gradientStops = {{0,{255,0,0,0}}, {.5,{0,0,255,255}}, {1,{0,255,0,128}}};
  const auto transparentMix = c::sampleLayerEffectGradient(gradient,.25);
  check(transparentMix == c::PremultipliedColor{0,0,.5F,.5F},
        "transparent stops do not leak hidden color through interpolation");
  auto layer = fixture(); layer.effects = state; layer = c::prepareSpatialFilterLayer(layer);
  const auto prepared = layer.effects;
  auto edited = std::make_shared<c::LayerEffectStack>(*state);
  edited->items[6].gradientStops[1] = {.7,{25,70,180,255}};
  layer.effects = edited;
  check(c::equivalentLayerEffectGeometry(prepared,edited) && c::layerEffectCacheValid(layer) &&
        c::prepareLayerEffects(layer,{}) == layer.effectCache,
        "gradient stop edits reuse all prepared effect geometry");
  auto noStops = std::make_shared<c::LayerEffectStack>();
  auto withStops = std::make_shared<c::LayerEffectStack>(*noStops);
  withStops->items[6].gradientStops.reserve(37);
  check(c::layerEffectMemoryCost(withStops)-c::layerEffectMemoryCost(noStops) ==
        withStops->items[6].gradientStops.capacity()*sizeof(c::GradientStop),
        "effect memory budgets include allocated gradient stops");
  for (auto stops : {std::vector<c::GradientStop>{{0,{}}},
                    std::vector<c::GradientStop>{{-.1,{}},{1,{}}},
                    std::vector<c::GradientStop>{{0,{}},{1.1,{}}},
                    std::vector<c::GradientStop>{{1,{}},{0,{}}},
                    std::vector<c::GradientStop>{{0,{}},{std::numeric_limits<double>::quiet_NaN(),{}}},
                    std::vector<c::GradientStop>{{0,{}},{std::numeric_limits<double>::infinity(),{}}}}) {
    gradient.gradientStops = std::move(stops);
    check(!c::validLayerEffects(*state), "invalid gradient stop data rejected");
  }
  gradient.gradientStops.clear();
  for (std::size_t i = 0; i < c::maximumGradientStops; ++i)
    gradient.gradientStops.push_back({double(i)/double(c::maximumGradientStops-1),{20,40,80,255}});
  check(c::validLayerEffects(*state), "maximum gradient stop count supported");
  const auto fullTableSample = c::sampleLayerEffectGradient(gradient,.5);
  const auto constantColor = c::decodeColor({20,40,80,255});
  for (std::size_t i = 0; i < 4; ++i)
    check(std::abs(fullTableSample[i]-constantColor[i]) < 1e-7F,
          "full stop table sampled correctly within float interpolation precision");
  gradient.gradientStops.push_back({1,{}});
  check(!c::validLayerEffects(*state), "excessive gradient stop count rejected");
  gradient.gradientStops.clear();
  check(c::validLayerEffects(*state), "empty stops retain valid legacy gradient");
  state->items[0].gradientStops = {{0,{0,0,0,255}},{1,{255,255,255,255}}};
  check(!c::validLayerEffects(*state), "gradient stops rejected on effects that do not render gradients");
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
  // Empty style geometry must preserve the floating-point backdrop exactly,
  // not just its eventual RGBA8 value. A later Hard Mix can amplify a single
  // transfer-function rounding difference into a visible color change.
  for (int code = 0; code <= 255; ++code) {
    const auto value = static_cast<std::uint8_t>(code);
    const auto backdrop = c::decodeColor({value, value, value, 190});
    for (float opacity : {.1F, .6F, .99F}) {
      const auto result = c::compositeLayerEffects(backdrop, {}, disabled,
          nullptr, {}, opacity, c::BlendMode::Normal, 1, false,
          c::CompositingSpace::EncodedSRgb);
      check(result == backdrop, "empty encoded style preserves exact backdrop");
    }
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
  s->items[6].enabled = true;
  s->items[6].opacity = .4;
  s->items[6].gradientStops = {{0,{25,60,180,255}}, {.3,{240,150,80,255}}, {1,{180,20,90,180}}};
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
void advancedBlending() {
  // An opaque rectangle with transparent padding and a separate mask hole.
  std::vector<std::byte> pixels(64 * 64 * 4);
  std::vector<std::uint8_t> maskPixels(64 * 64, 255);
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
    const auto i = std::size_t(y * 64 + x);
    pixels[i * 4] = std::byte{30}; pixels[i * 4 + 1] = std::byte{80}; pixels[i * 4 + 2] = std::byte{180};
    pixels[i * 4 + 3] = std::byte(x >= 12 && x < 52 && y >= 12 && y < 52 ? 255 : 0);
    if (x >= 25 && x < 39 && y >= 25 && y < 39) maskPixels[i] = 0;
  }
  auto layer = c::Layer::raster("Alpha and mask", std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{64,64}, pixels));
  auto mask = std::make_shared<c::LayerMask>();
  mask->coverage = c::SelectionMask::fromR8({64,64}, maskPixels, 64);
  layer.mask = mask;
  for (bool pixelAlpha : {true, false}) for (bool after : {true, false}) {
    auto state = effect(0); state->pixelAlphaShapesEffects = pixelAlpha; state->maskEffectsAfterRendering = after;
    state->items[0].color = {255,0,0,255}; state->items[0].size = 4;
    layer.effects = state; layer = c::prepareSpatialFilterLayer(layer);
    c::Document snapshotDocument({{64,64},96});
    auto unprepared = layer; unprepared.effectCache.reset();
    check(snapshotDocument.insertLayer(0,unprepared), "insert unprepared masked style");
    auto snapshot = c::prepareSpatialFilterLayer(snapshotDocument.snapshot().layersBottomToTop.front());
    check(c::layerEffectCacheValid(snapshot), "snapshot effect preparation retains shape mask");
    check(snapshot.effectCache->masks[0]->coverage->equivalent(*layer.effectCache->masks[0]->coverage),
          "snapshot and layer prepare identical mask-shaped geometry");
    const c::PreparedLayerSampler sampler(layer);
    check((sampler.sample({10.5,32.5})[3] > .9F) == pixelAlpha, "rectangle stroke depends only on pixel-alpha shaping");
    check((sampler.sample({26.5,32.5})[0] > .9F) == !after, "mask hole stroke depends only on mask geometry policy");
    check(sampler.sample({32.5,32.5})[3] == 0, "mask continues to hide content in all four combinations");
    const auto cache = layer.effectCache;
    auto moved = std::make_shared<c::LayerMask>(*mask); moved->localToMask.m02 = 2;
    layer.mask = moved;
    check(c::layerEffectCacheValid(layer) == after, "geometry mask mapping invalidates only shape-dependent caches");
    layer = c::prepareSpatialFilterLayer(layer);
    check((layer.effectCache == cache) == after, "final mask reuses geometry, shape mask rebuilds it");
    layer.mask = mask;
  }
  // Ignoring pixel alpha does not create an edge at the scratch-buffer border.
  for (int type : {0,1,2,3,4,7}) {
    auto state = effect(type); state->pixelAlphaShapesEffects = false;
    state->items[std::size_t(type)].size = 3;
    layer.effects = state; layer.mask.reset(); layer = c::prepareSpatialFilterLayer(layer);
    for (const auto& geometry : layer.effectCache->masks)
      if (geometry) check(geometry->coverage->bounds().empty(), "constant unbounded silhouette has no boundary effects");
  }
  // Geometry changes must reach every boundary effect, not only Stroke.
  for (int type : {0,1,2,3,4,7}) {
    auto state = effect(type); state->items[std::size_t(type)].size = 4;
    layer.effects = state; layer.mask = mask; layer = c::prepareSpatialFilterLayer(layer);
    const auto before = layer.effectCache;
    auto shifted = std::make_shared<c::LayerMask>(*mask); shifted->localToMask.m02 = 4;
    layer.mask = shifted; layer = c::prepareSpatialFilterLayer(layer);
    bool different = false;
    for (std::size_t i = 0; i < c::layerEffectMaskCount; ++i)
      if (before->masks[i] && layer.effectCache->masks[i])
        different |= !before->masks[i]->coverage->equivalent(*layer.effectCache->masks[i]->coverage);
    check(different, "moving mask regenerates shared boundary-effect geometry");
  }
  auto half = std::make_shared<c::LayerMask>(); half->coverage = c::SelectionMask::filled({64,64},128); half->outside = 128;
  for (bool pixelAlpha : {true,false}) for (bool after : {true,false}) {
    auto state = effect(5); state->pixelAlphaShapesEffects = pixelAlpha; state->maskEffectsAfterRendering = after;
    state->items[5].color = {255,0,0,255};
    layer.effects = state; layer.mask = half; layer = c::prepareSpatialFilterLayer(layer);
    const c::PreparedLayerSampler sampler(layer);
    check(std::abs(sampler.sample({20.5,20.5})[3] - 128.F/255) < .00001F, "soft mask applied once to interior paint");
    check((sampler.sample({3.5,3.5})[3] > .49F) == !pixelAlpha, "overlay can paint transparent source pixels only when alpha shaping is off");
    check(sampler.sample({-2.5,3.5})[3] == 0, "interior paint does not fill arbitrary effect padding");
  }
  // Geometry policy is not a source-alpha/Fill conversion.
  auto overlay = effect(5); overlay->pixelAlphaShapesEffects = false;
  layer.effects = overlay; layer.mask.reset(); layer.rasterEffectFrame = c::RectD{-10,-10,84,84};
  layer = c::prepareSpatialFilterLayer(layer);
  check(layer.effectCache->visualBounds.x <= -10 && layer.effectCache->visualBounds.right() >= 74,
        "alpha-independent interior paint retains its reference frame beyond compacted pixels");
  layer.rasterEffectFrame = c::RectD{-20,-20,104,104};
  check(!c::layerEffectCacheValid(layer), "changed alpha-independent paint frame invalidates visual bounds");
  for (auto mode : c::allBlendModes) {
    auto state = effect(0); state->pixelAlphaShapesEffects = false;
    const auto parameters = c::compileLayerEffects(state,{0,0,64,64});
    const auto source = c::decodeColor({140,80,170,100}), backdrop = c::decodeColor({90,150,60,255});
    const auto actual = c::compositeLayerEffects(backdrop,source,parameters,nullptr,{20.5,20.5},.8F,mode,1,false,c::CompositingSpace::LinearSRgb,.4F);
    const auto expected = c::compositeLayer(backdrop,source,.8F,mode,{20.5,20.5},c::defaultBlendSeed,false,c::CompositingSpace::LinearSRgb,.4F);
    // Existing styled group-opacity vs per-pixel-opacity semantics differ for
    // non-Normal modes; check full opacity to isolate alpha/Fill preservation.
    const auto actualFull = c::compositeLayerEffects(backdrop,source,parameters,nullptr,{20.5,20.5},1,mode,1,false,c::CompositingSpace::LinearSRgb,.4F);
    const auto expectedFull = c::compositeLayer(backdrop,source,1,mode,{20.5,20.5},c::defaultBlendSeed,false,c::CompositingSpace::LinearSRgb,.4F);
    for (std::size_t i = 0; i < 4; ++i) {
      check(std::abs(actualFull[i]-expectedFull[i]) < .00001F, "ignored alpha does not become Fill");
      if (mode == c::BlendMode::Normal) check(std::abs(actual[i]-expected[i]) < .00001F,"ordinary layer opacity remains separate");
    }
  }
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
      gradientContracts();
      morphologyReference();
      historyAndOutput();
      advancedBlending();
    }
  } catch (const std::exception &e) {
    check(false, e.what());
  }
  return failures ? 1 : 0;
}
