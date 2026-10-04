#include "../src/ui/src/PsdNativeRecords.hpp"
#include "../src/ui/src/PsdReader.hpp"
#include "../src/ui/src/PsdWriter.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/PsdExport.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstring>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int failures = 0;
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__)) {                                                      \
      ++failures;                                                              \
      std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n";              \
    }                                                                          \
  } while (false)
c::Layer raster(const char *name, c::Extent2u size, c::Rgba8 color,
                c::Vec2d origin = {}) {
  auto l = c::Layer::raster(
      name, std::make_shared<c::ContiguousRasterSurface>(size, color));
  l.localToDocument.m02 = origin.x;
  l.localToDocument.m12 = origin.y;
  return l;
}
QByteArray data(QString path) {
  QFile f(path);
  CHECK(f.open(QIODevice::ReadOnly));
  return f.readAll();
}
bool same(const QImage &x, const QImage &y) {
  if (x.isNull() || y.isNull() || x.size() != y.size())
    return false;
  auto a = x.convertToFormat(QImage::Format_RGBA8888),
       b = y.convertToFormat(QImage::Format_RGBA8888);
  for (int row = 0; row < a.height(); ++row)
    if (std::memcmp(a.constScanLine(row), b.constScanLine(row),
                    size_t(a.width()) * 4))
      return false;
  return true;
}
std::shared_ptr<const u::PsdSource> parse(QString path) {
  auto i = u::inspectPsd(path, std::make_shared<u::PsdJob>());
  CHECK(i.error.isEmpty());
  if (!i.error.isEmpty())
    std::cerr << i.error.toStdString() << "\n";
  return i.source;
}
u::PsdExportResult write(c::Document &d, QString path,
                         u::PsdExportOptions options = {}) {
  auto snapshot = u::capturePsdExport(d, 19);
  std::atomic_bool cancel = false;
  auto plan = u::planPsdExport(snapshot, options, cancel);
  CHECK(plan);
  if (!plan) {
    std::cerr << plan.error.toStdString() << "\n";
    return {};
  }
  auto result = u::writePsdExport(snapshot, plan, path, cancel);
  CHECK(result);
  if (!result)
    std::cerr << result.error.toStdString() << "\n";
  return result;
}
void exact(QString dir) {
  c::Document doc({{260, 12}, 300});
  auto l = raster("Samples α", {260, 5}, {0, 0, 0, 0}, {-4, 3});
  auto &surface = *std::get<c::RasterLayer>(l.payload).surface;
  std::vector<std::byte> bytes(260 * 5 * 4);
  for (size_t n = 0; n < bytes.size(); ++n)
    bytes[n] = std::byte((n * 73 + n / 101) % 256);
  // Runs exercise literal/run boundaries, maximum 128, and transparent hidden
  // RGB.
  for (size_t n = 128 * 4; n < 259 * 4; ++n)
    bytes[n] = std::byte(37);
  auto pixels =
      std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{260, 5}, bytes);
  l.payload = c::RasterLayer{pixels};
  (void)surface;
  l.opacity = .6F;
  l.blendMode = c::BlendMode::Multiply;
  l.visible = false;
  l.colorLabel = 3;
  auto id = l.id;
  CHECK(doc.insertLayer(0, l));
  auto bottom = raster("Bottom", {2, 2}, {31, 63, 191, 255});
  CHECK(doc.insertLayer(0, bottom));
  doc.setSelection(c::SelectionMask::rectangle({260, 12}, {0, 0, 1, 1}));
  auto revision = doc.revision(), state = doc.contentState();
  auto tree = doc.tree();
  auto path = dir + "/exact.psd";
  write(doc, path);
  auto s = parse(path);
  if (!s)
    return;
  CHECK(s->size == QSize(260, 12));
  CHECK(s->ppi == 300);
  CHECK(s->records.size() == 2);
  auto &record = s->records.back();
  CHECK(record.name == QString::fromUtf8("Samples α"));
  CHECK(record.bounds == QRect(-4, 3, 260, 5));
  CHECK(!record.visible);
  CHECK(record.opacity == 153);
  CHECK(record.blend == "mul ");
  auto image = u::psd::pixels(*s, record, {});
  for (int y = 0; y < 5; ++y)
    CHECK(std::memcmp(image.constScanLine(y),
                      bytes.data() + size_t(y) * 260 * 4, 260 * 4) == 0);
  CHECK(doc.revision() == revision && doc.contentState() == state &&
        doc.tree() == tree && doc.layer(id)->visible == false);
  auto composite = u::psd::composite(*s, {});
  CHECK(composite.pixelColor(0, 0) == QColor(31, 63, 191, 255));
  CHECK(composite.pixelColor(5, 5).alpha() == 0);
}
void masks(QString dir) {
  c::Document d({{32, 24}, 144});
  auto l = raster("Mask source", {16, 12}, {200, 100, 20, 255}, {5, 3});
  auto mask = std::make_shared<c::LayerMask>();
  mask->coverage = c::SelectionMask::filled({6, 7}, 128);
  mask->outside = 255;
  mask->localToMask = {.m02 = -2, .m12 = 1};
  mask->enabled = false;
  l.mask = mask;
  auto id = l.id;
  CHECK(d.insertLayer(0, l));
  auto path = dir + "/mask.psd";
  write(d, path);
  auto s = parse(path);
  if (!s)
    return;
  auto r = s->records[0];
  CHECK(r.mask.present);
  CHECK(r.mask.bounds == QRect(7, 2, 6, 7));
  CHECK(r.mask.outside == 255);
  CHECK((r.mask.flags & 2) != 0);
  auto raw = u::psd::pixels(*s, r, {});
  CHECK(raw.pixelColor(0, 0).alpha() == 255);
  auto inspection = u::inspectPsd(path, std::make_shared<u::PsdJob>());
  auto converted = u::convertPsd(inspection, u::defaultPsdOptions(inspection),
                                 std::make_shared<u::PsdJob>(), {}, false);
  CHECK(converted.document);
  if (converted.document) {
    auto &m = converted.document->layers()[0];
    CHECK(m.mask && !m.mask->enabled);
    CHECK(m.mask->outside == 255);
    CHECK(same(u::flattenDocument(d).image,
               u::flattenDocument(*converted.document).image));
  }
  CHECK(d.layer(id)->mask == mask);
  auto fractionalOutside = std::make_shared<c::LayerMask>(*mask);
  fractionalOutside->outside = 128;
  d.layer(id)->mask = fractionalOutside;
  std::atomic_bool cancel = false;
  auto p = u::planPsdExport(u::capturePsdExport(d, 88), {}, cancel);
  CHECK(p && p.needsReview &&
        p.entries.front().action == u::PsdExportAction::Pixels);
  write(d, dir + "/fractional-outside.psd");
  auto baked = parse(dir + "/fractional-outside.psd");
  CHECK(baked && !baked->records[0].mask.present);
  c::ShapeLayer shape;
  shape.size = {16, 12};
  d.layer(id)->payload = shape;
  d.layer(id)->mask = mask;
  p = u::planPsdExport(u::capturePsdExport(d, 89), {}, cancel);
  CHECK(p && p.needsReview &&
        p.entries.front().action == u::PsdExportAction::Pixels);
}
c::Document editableFixture() {
  c::Document d({{480, 360}, 144});
  auto bg = raster("Background (ordinary)", {480, 360}, {21, 26, 35, 255});
  CHECK(d.insertLayer(0, bg));
  c::TextLayer text;
  text.utf8 = "Ember café";
  text.defaultStyle.font = {"DejaVu Sans", "Book", 400, false};
  text.defaultStyle.sizePixels = 28;
  text.defaultStyle.color = {245, 115, 20, 255};
  text = c::normalizedText(text);
  auto ember = c::Layer::text("Editable ember", text);
  ember.localToDocument = {.m02 = 20, .m12 = 20};
  CHECK(d.insertLayer(d.layers().size(), ember));
  text.utf8 = "Mixed words";
  text.runs = {
      {0, 6, text.defaultStyle},
      {6, 5, {{"DejaVu Serif", "Book", 400, false}, 21, {115, 205, 250, 255}}}};
  auto mixed = c::Layer::text("Mixed words", text);
  mixed.localToDocument = {
      .m00 = .95, .m01 = -.1, .m02 = 30, .m10 = .1, .m11 = .95, .m12 = 75};
  CHECK(d.insertLayer(d.layers().size(), mixed));
  c::ShapeLayer rectangle;
  rectangle.size = {100, 55};
  rectangle.fillColor = {70, 180, 110, 255};
  auto rect = c::Layer::shape("Rectangle", rectangle);
  rect.localToDocument = {.m02 = 20, .m12 = 130};
  CHECK(d.insertLayer(d.layers().size(), rect));
  c::ShapeLayer triangle;
  triangle.kind = c::ShapeKind::Triangle;
  triangle.size = {90, 80};
  triangle.fillEnabled = false;
  triangle.strokeEnabled = true;
  triangle.strokeColor = {255, 30, 40, 255};
  triangle.strokeWidth = 5;
  auto tri = c::Layer::shape("Hollow triangle", triangle);
  tri.localToDocument = {.m02 = 150, .m12 = 135};
  CHECK(d.insertLayer(d.layers().size(), tri));
  auto ellipse = rectangle;
  ellipse.kind = c::ShapeKind::Ellipse;
  ellipse.size = {75, 60};
  auto oval = c::Layer::shape("Ellipse", ellipse);
  oval.localToDocument = {.m02 = 280, .m12 = 140};
  CHECK(d.insertLayer(d.layers().size(), oval));
  auto style = raster("Live Stroke", {40, 40}, {90, 55, 175, 255}, {340, 35});
  auto fx = std::make_shared<c::LayerEffectStack>();
  auto &stroke = fx->items[0];
  stroke.enabled = true;
  stroke.color = {250, 200, 30, 255};
  stroke.size = 5;
  style.effects = fx;
  CHECK(d.insertLayer(d.layers().size(), style));
  auto masked =
      raster("Independent mask", {60, 60}, {60, 180, 200, 255}, {395, 30});
  auto mask = std::make_shared<c::LayerMask>();
  mask->coverage = c::SelectionMask::rectangle({60, 60}, {10, 5, 30, 50});
  mask->outside = 255;
  masked.mask = mask;
  CHECK(d.insertLayer(d.layers().size(), masked));
  auto base =
      raster("Half alpha base", {160, 80}, {30, 70, 110, 128}, {40, 250});
  base.opacity = .8F;
  auto upper =
      raster("Upper Multiply", {150, 65}, {220, 100, 30, 255}, {25, 235});
  upper.blendMode = c::BlendMode::Multiply;
  auto highlight =
      raster("Upper Normal", {65, 50}, {250, 180, 80, 255}, {155, 280});
  CHECK(d.insertLayer(d.layers().size(), base));
  CHECK(d.insertLayer(d.layers().size(), upper));
  CHECK(d.insertLayer(d.layers().size(), highlight));
  auto tree = d.tree();
  c::LayerContainer clip{c::makeLayerId(),
                         "Clipping stack",
                         c::ContainerKind::ClippingMaskGroup,
                         c::ColorLabel::Blue,
                         {base.id, upper.id, highlight.id},
                         true};
  for (auto id : clip.children)
    std::erase(tree.roots, id);
  tree.roots.push_back(clip.id);
  tree.containers.push_back(clip);
  c::LayerContainer folder{c::makeLayerId(),
                           "Editable types",
                           c::ContainerKind::Folder,
                           c::ColorLabel::None,
                           {ember.id, mixed.id, rect.id, tri.id, oval.id},
                           true};
  for (auto id : folder.children)
    std::erase(tree.roots, id);
  tree.roots.insert(tree.roots.begin() + 1, folder.id);
  tree.containers.push_back(folder);
  CHECK(d.replaceStructure(d.tree(), tree));
  return d;
}
void editable(QString dir) {
  auto d = editableFixture();
  auto path = dir + "/editable.psd";
  write(d, path);
  auto source = parse(path);
  if (!source)
    return;
  int texts = 0, shapes = 0, effects = 0, clipped = 0;
  for (auto &r : source->records) {
    texts += r.tags.contains("TySh");
    shapes += r.tags.contains("vmsk");
    effects += r.tags.contains("lfx2");
    clipped += r.clipping;
  }
  CHECK(texts == 2);
  CHECK(shapes == 3);
  CHECK(effects == 1);
  CHECK(clipped == 2);
  auto job = std::make_shared<u::PsdJob>();
  auto inspection = u::inspectPsd(path, job);
  auto options = u::defaultPsdOptions(inspection);
  for (auto &e : inspection.layers)
    std::cout << e.name.toStdString() << ": " << e.type.toStdString() << " "
              << e.status.toStdString() << "\n";
  auto result = u::convertPsd(inspection, options, job, {}, false);
  CHECK(result.document);
  if (!result.document)
    std::cerr << result.error.toStdString() << "\n";
  if (result.document) {
    int count = 0;
    for (auto &l : result.document->layers())
      if (auto t = std::get_if<c::TextLayer>(&l.payload)) {
        ++count;
        if (t->utf8 == "Mixed words")
          CHECK(t->runs.size() == 2);
      }
    CHECK(count == 2);
    CHECK(std::ranges::count_if(result.document->layers(), [](auto &l) {
            return std::holds_alternative<c::ShapeLayer>(l.payload);
          }) == 3);
    int clip = 0;
    for (auto &c : result.document->tree().containers)
      clip += c.kind == c::ContainerKind::ClippingMaskGroup;
    CHECK(clip == 1);
  }
  CHECK(u::flattenDocument(d).image.save(dir + "/editable-reference.png"));
  auto project = dir + "/editable.vulkana";
  CHECK(u::saveProject(project, d));
  auto reopened = u::loadProject(project);
  CHECK(reopened.document);
  if (reopened.document)
    write(*reopened.document, dir + "/editable-reopened.psd");
  u::PsdExportOptions flat;
  flat.mode = u::PsdExportMode::Flattened;
  write(d, dir + "/flattened.psd", flat);
  auto f = parse(dir + "/flattened.psd");
  CHECK(f && f->records.size() == 1);
  if (f)
    CHECK(same(u::psd::pixels(*f, f->records[0], {}),
               u::flattenDocument(d).image));
}
void safety(QString dir) {
  c::Document d({{16, 16}, 96});
  auto l = raster("Source", {16, 16}, {40, 100, 170, 255});
  CHECK(d.insertLayer(0, l));
  std::atomic_bool cancel = false;
  auto s = u::capturePsdExport(d, 77);
  u::PsdExportOptions o;
  auto p = u::planPsdExport(s, o, cancel);
  auto path = dir + "/atomic.psd";
  QFile old(path);
  CHECK(old.open(QIODevice::WriteOnly));
  old.write("old contents");
  old.close();
  cancel = true;
  CHECK(u::writePsdExport(s, p, path, cancel).cancelled);
  CHECK(data(path) == "old contents");
  cancel = false;
  s.limits.fileBytes = 100;
  CHECK(!u::writePsdExport(s, p, path, cancel));
  CHECK(data(path) == "old contents");
  s.limits.fileBytes = 2ULL * 1024 * 1024 * 1024 - 1;
  CHECK(u::writePsdExport(s, p, path, cancel, [](int, int, const QString &) {
          return false;
        }).cancelled);
  CHECK(data(path) == "old contents");
  CHECK(!u::writePsdExport(s, p, dir + "/missing/file.psd", cancel));
  o.instanceId = 99;
  CHECK(!u::planPsdExport(s, o, cancel));
  o = {};
  // Snapshot remains coherent after original pixels, names and selection
  // change.
  auto *original = d.layer(l.id);
  original->name = "Changed live";
  original->payload =
      c::RasterLayer{std::make_shared<c::ContiguousRasterSurface>(
          c::Extent2u{16, 16}, c::Rgba8{200, 0, 0, 255})};
  CHECK(u::writePsdExport(s, p, dir + "/snapshot.psd", cancel));
  auto source = parse(dir + "/snapshot.psd");
  CHECK(source && source->records[0].name == "Source");
  // Honest fallback: an exterior Multiply effect cannot become Normal pixels.
  auto fx = std::make_shared<c::LayerEffectStack>();
  fx->items[1].enabled = true;
  original->effects = fx;
  auto styled = u::capturePsdExport(d, 78);
  auto liveStyle = u::planPsdExport(styled, {}, cancel);
  CHECK(liveStyle && liveStyle.needsReview);
  u::PsdExportOptions bakeStyle;
  bakeStyle.actions[l.id] = u::PsdExportAction::Pixels;
  auto invalid = u::planPsdExport(styled, bakeStyle, cancel);
  CHECK(!invalid);
  CHECK(invalid.error.contains("backdrop"));
  o.mode = u::PsdExportMode::Flattened;
  CHECK(u::planPsdExport(styled, o, cancel));
  c::Document empty({{8, 8}, 72});
  write(empty, dir + "/empty.psd");
  bool rejected = false;
  try {
    c::Document huge({{30001, 2}, 96});
    (void)u::capturePsdExport(huge, 1);
  } catch (const std::exception &) {
    rejected = true;
  }
  CHECK(rejected);
}
void omission(QString dir) {
  c::Document d({{8, 8}, 72});
  auto background = raster("Background", {8, 8}, {20, 40, 200, 255});
  auto foreground = raster("Foreground", {8, 8}, {210, 30, 20, 255});
  CHECK(d.insertLayer(0, background));
  CHECK(d.insertLayer(1, foreground));
  c::LayerContainer inner{c::makeLayerId(),         "Inner",
                          c::ContainerKind::Folder, c::ColorLabel::None,
                          {foreground.id},          true};
  c::LayerContainer empty{
      c::makeLayerId(),    "Empty", c::ContainerKind::Folder,
      c::ColorLabel::None, {},      true};
  c::LayerContainer outer{c::makeLayerId(),        "Outer",
                          c::ContainerKind::Group, c::ColorLabel::None,
                          {inner.id, empty.id},    true};
  auto tree = d.tree();
  tree.roots = {background.id, outer.id};
  tree.containers = {inner, empty, outer};
  CHECK(d.replaceStructure(d.tree(), tree));
  const auto revision = d.revision();
  const auto source = u::capturePsdExport(d, 210);
  std::atomic_bool cancel = false;
  for (const auto id : {foreground.id, inner.id, outer.id, background.id}) {
    u::PsdExportOptions options;
    options.actions[id] = u::PsdExportAction::Omit;
    auto plan = u::planPsdExport(source, options, cancel);
    CHECK(plan);
    auto preview = u::previewPsdExport(source, plan, cancel);
    CHECK(preview);
    if (preview)
      CHECK(preview.image.pixelColor(2, 2) ==
            (id == background.id ? QColor(210, 30, 20) : QColor(20, 40, 200)));
    else
      std::cerr << "Omit preview: " << preview.error.toStdString() << '\n';
    const auto path = dir + QString("/omit-%1.psd").arg(id);
    auto result = u::writePsdExport(source, plan, path, cancel);
    CHECK(result);
    if (result) {
      auto file = parse(path);
      CHECK(file);
      if (file)
        CHECK(same(u::psd::composite(*file, {}), preview.image));
    }
  }
  u::PsdExportOptions options;
  options.actions[background.id] = options.actions[outer.id] =
      u::PsdExportAction::Omit;
  auto plan = u::planPsdExport(source, options, cancel);
  auto emptyPreview = u::previewPsdExport(source, plan, cancel);
  CHECK(emptyPreview && emptyPreview.image.pixelColor(2, 2).alpha() == 0);
  CHECK(d.revision() == revision && d.tree() == tree && d.layers().size() == 2);
}
void variants(QString dir) {
  auto d = editableFixture();
  std::atomic_bool cancel = false;
  auto s = u::capturePsdExport(d, 901);
  auto clip = std::ranges::find_if(d.tree().containers, [](auto &item) {
    return item.kind == c::ContainerKind::ClippingMaskGroup;
  });
  CHECK(clip != d.tree().containers.end());
  u::PsdExportOptions choices;
  choices.actions[clip->children.front()] = u::PsdExportAction::Omit;
  CHECK(!u::planPsdExport(s, choices, cancel));
  choices.actions[clip->id] = u::PsdExportAction::Pixels;
  CHECK(!u::planPsdExport(s, choices, cancel));
  auto nested = d.tree();
  c::LayerContainer enclosing{
      c::makeLayerId(),    "Enclosing folder", c::ContainerKind::Folder,
      c::ColorLabel::None, {clip->id},         true};
  std::erase(nested.roots, clip->id);
  nested.roots.push_back(enclosing.id);
  nested.containers.push_back(enclosing);
  CHECK(d.replaceStructure(d.tree(), nested));
  auto nestedSnapshot = u::capturePsdExport(d, 903);
  choices.actions[enclosing.id] = u::PsdExportAction::Pixels;
  CHECK(!u::planPsdExport(nestedSnapshot, choices, cancel));
  CHECK(d.replaceStructure(d.tree(), s.document->tree()));
  clip = std::ranges::find_if(d.tree().containers, [](auto &item) {
    return item.kind == c::ContainerKind::ClippingMaskGroup;
  });
  choices.actions.clear();
  choices.actions[clip->id] = u::PsdExportAction::Pixels;
  write(d, dir + "/consolidated.psd", choices);
  auto consolidated = parse(dir + "/consolidated.psd");
  CHECK(consolidated);
  if (consolidated)
    CHECK(std::ranges::none_of(consolidated->records,
                               [](auto &r) { return r.clipping; }));
  choices.actions[clip->id] = u::PsdExportAction::Omit;
  write(d, dir + "/omitted.psd", choices);
  auto omitted = parse(dir + "/omitted.psd");
  CHECK(omitted);
  if (omitted) {
    CHECK(std::ranges::none_of(
        omitted->records, [](auto &r) { return r.name == "Half alpha base"; }));
    CHECK(u::psd::composite(*omitted, {}).pixelColor(45, 260) ==
          QColor(21, 26, 35));
  }
  choices = {};
  choices.preserveText = false;
  write(d, dir + "/raster-text.psd", choices);
  auto rasterized = parse(dir + "/raster-text.psd");
  if (rasterized)
    CHECK(std::ranges::none_of(
        rasterized->records, [](auto &r) { return r.tags.contains("TySh"); }));

  c::Document styles({{260, 100}, 72});
  for (size_t i = 0; i < c::layerEffectCount; ++i) {
    auto l = raster(("Style " + std::to_string(i)).c_str(), {12, 12},
                    {160, 80, 30, 128}, {10. + double(i) * 32, 35});
    auto fx = std::make_shared<c::LayerEffectStack>();
    auto &e = fx->items[i];
    e.enabled = true;
    e.size = 4;
    e.distance = 5;
    e.spread = .3;
    e.color = {40, 80, 120, 160};
    e.secondColor = {210, 180, 20, 70};
    e.opacity = .65;
    e.angle = 37;
    l.effects = fx;
    CHECK(styles.insertLayer(styles.layers().size(), l));
  }
  write(styles, dir + "/styles.psd");
  auto styled = parse(dir + "/styles.psd");
  CHECK(styled && styled->records.size() == c::layerEffectCount);
  if (styled) {
    const QByteArray keys[]{"FrFX", "DrSh", "IrSh", "OrGl",
                            "IrGl", "SoFi", "GrFl", "ebbl"};
    for (size_t i = 0; i < c::layerEffectCount; ++i) {
      u::psd::Reader r(styled->records[i].tags["lfx2"]);
      r.skip(4);
      auto effects = u::psd::versionedDescriptor(r);
      CHECK(effects.contains(QString::fromLatin1(keys[i])));
      auto effect = effects[QString::fromLatin1(keys[i])].toMap();
      CHECK(effect["enab"].toBool());
      CHECK(std::abs(u::psd::number(effect[i==7?"hglO":"Opct"]) -
                     .65 * 100 * (i == 6 ? 1 : 160. / 255)) < 1e-4);
    }
  }
  CHECK(u::flattenDocument(styles).image.save(dir + "/styles-reference.png"));
  // Explicit projective typed content has an honest, reviewed pixel fallback.
  for (auto &item : d.layers())
    if (std::holds_alternative<c::TextLayer>(item.payload)) {
      d.layer(item.id)->localToDocument.m20 = .001;
      break;
    }
  auto distorted = u::capturePsdExport(d, 902);
  auto plan = u::planPsdExport(distorted, {}, cancel);
  CHECK(plan && plan.needsReview);
  CHECK(std::ranges::any_of(plan.entries, [](auto &e) {
    return e.type == "Text" && e.action == u::PsdExportAction::Pixels;
  }));
  write(d, dir + "/projective-text.psd");

  c::Document empty({{8, 8}, 72});
  auto tree = empty.tree();
  c::LayerContainer folder{c::makeLayerId(),
                           "Empty folder",
                           c::ContainerKind::Folder,
                           c::ColorLabel::None,
                           {},
                           true};
  tree.roots.push_back(folder.id);
  tree.containers.push_back(folder);
  CHECK(empty.replaceStructure(empty.tree(), tree));
  write(empty, dir + "/empty-folder.psd");
  choices = {};
  choices.actions[folder.id] = u::PsdExportAction::Pixels;
  write(empty, dir + "/empty-folder-consolidated.psd", choices);
  c::TextLayer text;
  text.utf8 = "Café\nSecond line";
  text.defaultStyle.font = {"Noto Sans", "Regular", 400, false};
  auto l = c::Layer::text("Unicode and multiline", c::normalizedText(text));
  c::Document words({{240, 120}, 300});
  CHECK(words.insertLayer(0, l));
  write(words, dir + "/multiline.psd");
  c::Document emptyText({{24, 24}, 96});
  CHECK(emptyText.insertLayer(
      0, c::Layer::text("Empty text", c::normalizedText(c::TextLayer{}))));
  write(emptyText, dir + "/empty-text.psd");
  auto disabled = raster("Disabled style", {8, 8}, {40, 80, 90, 255});
  auto disabledEffects = std::make_shared<c::LayerEffectStack>();
  disabledEffects->items[0].size = 11;
  disabled.effects = disabledEffects;
  c::Document inactive({{12, 12}, 72});
  CHECK(inactive.insertLayer(0, disabled));
  write(inactive, dir + "/disabled-style.psd");
  // Engine strings are UTF-16 with escaping, never UTF-8 byte run lengths.
  const auto unicode = QString::fromUtf8("A😀(é)\\\r");
  const auto encoded = u::psdwrite::engine(QVariantMap{{"Text", unicode}});
  CHECK(u::psd::engineData(encoded)["Text"].toString() == unicode);
  if (QFontDatabase::families().contains("Noto Sans Symbols 2")) {
    c::TextLayer astral;
    astral.utf8 = "🞀🞁";
    astral.defaultStyle.font = {"Noto Sans Symbols 2", "Regular", 400, false};
    auto red = astral.defaultStyle;
    red.color = {210, 20, 30, 255};
    astral.runs = {{0, 4, astral.defaultStyle}, {4, 4, red}};
    c::Document symbols({{100, 80}, 144});
    CHECK(symbols.insertLayer(
        0, c::Layer::text("Astral runs", c::normalizedText(astral))));
    write(symbols, dir + "/astral.psd");
    auto file = parse(dir + "/astral.psd");
    CHECK(file && file->records[0].tags.contains("TySh"));
  } else
    std::cout << "Astral native-font case unavailable: Noto Sans Symbols 2 is "
                 "not installed\n";
}
void review(QString dir) {
  auto d = editableFixture();
  std::atomic_bool cancel = false;
  auto source = u::capturePsdExport(d, 123);
  auto p = u::planPsdExport(source, {}, cancel);
  CHECK(p && p.needsReview);
  u::ExportSettings settings;
  settings.format = u::ExportFormat::Psd;
  settings.size = {480, 360};
  settings.destination = dir + "/ui.psd";
  u::ExportDialog dialog(settings, {480, 360});
  dialog.show();
  dialog.setPsdPlan(p, QImage(48, 36, QImage::Format_RGBA8888));
  QCoreApplication::processEvents();
  auto *list = dialog.findChild<QTreeWidget *>("PsdExportItems");
  auto *button = dialog.findChild<QPushButton *>("ExportWrite");
  auto *action = dialog.findChild<QComboBox *>("PsdExportAction");
  auto *attention = dialog.findChild<QCheckBox *>("PsdExportAttention");
  auto *hideGroups = dialog.findChild<QCheckBox *>("PsdExportHideGroups");
  auto *description = dialog.findChild<QLabel *>("PsdExportActionDescription");
  CHECK(list && button && action);
  CHECK(attention && attention->isChecked());
  CHECK(hideGroups && hideGroups->isChecked());
  CHECK(description);
  CHECK(list->currentItem() &&
        list->currentItem()->data(0, Qt::UserRole + 1).toBool());
  // Hidden group rows must not hide their children or alter export choices.
  int leaves = 0;
  for (QTreeWidgetItemIterator row(list); *row; ++row) {
    CHECK(!d.tree().container((*row)->data(0, Qt::UserRole).toULongLong()));
    ++leaves;
  }
  CHECK(leaves == int(d.layers().size()));
  CHECK(!dialog.findChild<QCheckBox *>("PsdExportReviewed"));
  CHECK(!dialog.findChild<QPushButton *>("PsdExportDetails"));
  CHECK(list->topLevelItemCount() > 1);
  CHECK(button->isEnabled()); // Export itself accepts valid conversions.
  CHECK(dialog.settings().psd.instanceId == 123);
  auto *items = dialog.findChild<QWidget *>("PsdExportConversions");
  auto *preview = dialog.findChild<QWidget *>("ExportPreviewScroll");
  auto *formats = dialog.findChild<QWidget *>("PsdExportOptions");
  const auto bounds = [&](QWidget *w) {
    return QRect(w->mapTo(&dialog, QPoint{}), w->size());
  };
  CHECK(bounds(items).right() < bounds(preview).left());
  CHECK(bounds(items).top() <= bounds(preview).top());
  CHECK(bounds(items).bottom() < bounds(formats).top());
  CHECK(bounds(preview).bottom() < bounds(formats).top());
  const auto selectedId = list->currentItem()->data(0, Qt::UserRole);
  const auto choices = dialog.settings().psd;
  int settingsChanges = 0;
  dialog.onSettingsChanged = [&] { ++settingsChanges; };
  attention->setChecked(false);
  hideGroups->setChecked(false);
  CHECK(dialog.settings().psd == choices && settingsChanges == 0);
  CHECK(button->isEnabled());
  CHECK(list->currentItem()->data(0, Qt::UserRole) == selectedId);
  int groups = 0;
  for (QTreeWidgetItemIterator row(list); *row; ++row) {
    const auto id = (*row)->data(0, Qt::UserRole).toULongLong();
    CHECK(!(*row)->isHidden());
    if (d.tree().container(id)) {
      ++groups;
      CHECK((*row)->foreground(0).color() ==
            u::themeColor(u::ThemeColor::SecondaryText));
      CHECK((*row)->foreground(1).color() ==
            u::themeColor(u::ThemeColor::SecondaryText));
      list->setCurrentItem(*row);
      CHECK(action->isHidden()); // Ordinary and supported clipping groups.
    }
  }
  CHECK(groups == int(d.tree().containers.size()));
  attention->setChecked(true);
  hideGroups->setChecked(true);
  CHECK(dialog.settings().psd == choices && settingsChanges == 0);
  CHECK(list->currentItem() &&
        list->currentItem()->data(0, Qt::UserRole + 1).toBool());
  // Descriptions track the selected option immediately, not a stale plan's
  // technical reasons. The editable label has no parenthetical qualifier.
  CHECK(action->findText("Keep editable") >= 0);
  CHECK(action->findText("Keep editable (review differences)") < 0);
  action->setCurrentIndex(action->findData(int(u::PsdExportAction::Editable)));
  CHECK(description->text().startsWith("Keep this layer editable"));
  CHECK(description->text().contains("appearance may differ outside Vulkana"));
  CHECK(!description->text().contains("Photoshop"));
  action->setCurrentIndex(action->findData(int(u::PsdExportAction::Pixels)));
  CHECK(description->text() == "Export this layer’s appearance as pixels.");
  action->setCurrentIndex(action->findData(int(u::PsdExportAction::Omit)));
  CHECK(description->text() == "Leave this layer out of the PSD.");
  action->setCurrentIndex(action->findData(int(u::PsdExportAction::Automatic)));
  CHECK(description->text().startsWith(
      "Keep this layer editable where possible"));
  dialog.onSettingsChanged = {};
  // A missing clipping base is a real blocker, not an acceptance checkbox.
  auto clip = std::ranges::find_if(d.tree().containers, [](auto &c) {
    return c.kind == c::ContainerKind::ClippingMaskGroup;
  });
  u::PsdExportOptions invalid;
  invalid.actions[clip->children.front()] = u::PsdExportAction::Omit;
  const auto blocked = u::planPsdExport(source, invalid, cancel);
  CHECK(!blocked);
  dialog.setPsdPlan(blocked, {});
  CHECK(!button->isEnabled());
  auto *previewToggle = dialog.findChild<QCheckBox *>("PsdExportPreview");
  CHECK(previewToggle && previewToggle->isChecked());
  previewToggle->setChecked(false);
  dialog.setPsdPlan(blocked, {});
  CHECK(!button->isEnabled()); // No preview does not bypass plan validation.
  dialog.setPsdPlan(p, {});
  CHECK(button->isEnabled());
  previewToggle->setChecked(true);
  dialog.setPsdPlan(p, {});
  CHECK(!button->isEnabled()); // Enabled previews must be current and ready.
  CHECK(!u::writePsdExport(source, blocked, dir + "/blocked.psd", cancel));
  dialog.setPsdPlan(p, QImage(48, 36, QImage::Format_RGBA8888));
  CHECK(button->isEnabled());
  bool requested = false;
  dialog.onExportRequested = [&] { requested = true; };
  button->click();
  CHECK(requested);
  auto *format = dialog.findChild<QComboBox *>("ExportFormat");
  format->setCurrentIndex(int(u::ExportFormat::Png));
  CHECK(items->isHidden() && !formats->isVisible());
  CHECK(dialog.findChild<QWidget *>("ExportSettingsScroll")->isVisible());
  format->setCurrentIndex(int(u::ExportFormat::Psd));
  CHECK(items->isVisible() && formats->isVisible());
  CHECK(!dialog.findChild<QWidget *>("ExportSettingsScroll")->isVisible());
}
void reference(QString path, QString output) {
  QElapsedTimer clock;
  clock.start();
  auto job = std::make_shared<u::PsdJob>();
  auto i = u::inspectPsd(path, job);
  CHECK(i.error.isEmpty());
  auto native = u::convertPsd(i, u::defaultPsdOptions(i), job, {}, false);
  CHECK(native.document);
  if (!native.document) {
    std::cerr << native.error.toStdString() << "\n";
    return;
  }
  auto &d = *native.document;
  for (auto &l : d.layers())
    if (auto t = std::get_if<c::TextLayer>(&d.layer(l.id)->payload)) {
      t->utf8 += " edited";
      *t = c::normalizedText(*t);
    }
  std::cout << "IMPORT_MS " << clock.elapsed() << " LAYERS "
            << d.layers().size() << " SOURCE_BYTES " << native.rasterBytes
            << "\n";
  std::atomic_bool cancel = false;
  auto source = u::capturePsdExport(d, 345);
  u::PsdExportOptions options;
  auto plan = u::planPsdExport(source, options, cancel);
  for (auto &e : plan.entries)
    std::cout << e.name.toStdString() << " -> " << e.type.toStdString()
              << " action=" << int(e.action) << " "
              << e.reasons.join("; ").toStdString() << "\n";
  CHECK(plan);
  if (!plan) {
    std::cerr << plan.error.toStdString() << "\n";
    return;
  }
  auto result = u::writePsdExport(source, plan, output, cancel);
  CHECK(result);
  std::cout << "EXPORT_MS " << result.milliseconds << " BYTES " << result.bytes
            << " ERROR " << result.error.toStdString() << "\n";
  auto render = u::flattenDocument(d);
  CHECK(render);
  if (render)
    CHECK(render.image.save(output + ".reference.png"));
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTemporaryDir temporary;
  if (argc > 3 && QString::fromLocal8Bit(argv[1]) == "--reference") {
    reference(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    return failures ? 1 : 0;
  }
  QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : temporary.path();
  QDir().mkpath(dir);
  exact(dir);
  masks(dir);
  editable(dir);
  safety(dir);
  variants(dir);
  review(dir);
  omission(dir);
  std::cout << "Failures: " << failures << "\n";
  return failures ? 1 : 0;
}
