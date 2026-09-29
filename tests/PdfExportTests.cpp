#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/PdfExport.hpp"
#include "imageeditor/ui/PdfImport.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QPdfDocument>
#include <QPdfSearchModel>
#include <QPdfSelection>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstring>
#include <iostream>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <set>
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
c::Layer raster(QString name, c::Extent2u size, c::Rgba8 color,
                c::Vec2d origin = {}) {
  c::Layer layer;
  layer.id = c::makeLayerId();
  layer.name = name.toStdString();
  layer.payload =
      c::RasterLayer{std::make_shared<c::ContiguousRasterSurface>(size, color)};
  layer.localToDocument.m02 = origin.x;
  layer.localToDocument.m12 = origin.y;
  return layer;
}
QByteArray bytes(const QString &path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}
// Compare decoded RGBA bytes, not QImage's DPI/color-space metadata. The PDF
// boundary explicitly tags sRGB; flattenDocument is an untagged core buffer.
bool samePixels(const QImage &a, const QImage &b) {
  if (a.isNull() || b.isNull() || a.size() != b.size())
    return false;
  auto x = a.convertToFormat(QImage::Format_RGBA8888),
       y = b.convertToFormat(QImage::Format_RGBA8888);
  for (int row = 0; row < x.height(); ++row)
    if (std::memcmp(x.constScanLine(row), y.constScanLine(row),
                    std::size_t(x.width()) * 4) != 0)
      return false;
  return true;
}
void independentText(const QString &path) {
  const auto tool = QStandardPaths::findExecutable("pdftotext");
  if (tool.isEmpty()) {
    std::cout << "Independent text extraction unavailable (pdftotext not "
                 "installed)\n";
    return;
  }
  QProcess process;
  process.start(tool, {"-enc", "UTF-8", path, "-"});
  CHECK(process.waitForFinished(10000));
  CHECK(process.exitCode() == 0);
  const auto extracted = QString::fromUtf8(process.readAllStandardOutput());
  CHECK(extracted.contains("Office"));
  CHECK(extracted.contains(QString::fromUtf8("café")));
  // Poppler may use visual-order RTL text inside an explicit RLE, which is a
  // display-equivalent Unicode representation, not missing/substituted letters.
  CHECK(extracted.contains(QString::fromUtf8("مرحبا")) ||
        (extracted.contains(QChar(0x202b)) &&
         extracted.contains(QString::fromUtf8("ابحرم"))));
  CHECK(!extracted.contains(QChar(0xfffd)));
}
void images(QPDFObjectHandle resources, std::vector<QPDFObjectHandle> &result) {
  auto objects = resources.getKey("/XObject");
  for (const auto &key : objects.getKeys()) {
    auto object = objects.getKey(key);
    auto dict = object.getDict();
    if (dict.getKey("/Subtype").isNameAndEquals("/Image"))
      result.push_back(object);
    else if (dict.getKey("/Subtype").isNameAndEquals("/Form"))
      images(dict.getKey("/Resources"), result);
  }
}
void exactImage(const QString &path, const QImage &expected) {
  QPDF pdf;
  pdf.processFile(path.toUtf8().constData());
  std::vector<QPDFObjectHandle> found;
  images(QPDFPageDocumentHelper(pdf).getAllPages().front().getAttribute(
             "/Resources", false),
         found);
  CHECK(found.size() == 1);
  if (found.size() != 1)
    return;
  auto image = found[0];
  auto dict = image.getDict();
  CHECK(dict.getKey("/Filter").unparse().find("DCTDecode") ==
        std::string::npos);
  CHECK(dict.getKey("/Width").getIntValue() == expected.width());
  CHECK(dict.getKey("/Height").getIntValue() == expected.height());
  auto data = image.getStreamData();
  CHECK(data->getSize() ==
        std::size_t(expected.width() * expected.height() * 3));
  std::shared_ptr<Buffer> alpha;
  if (dict.getKey("/SMask").isStream())
    alpha = dict.getKey("/SMask").getStreamData();
  CHECK(!dict.hasKey("/Mask"));
  if (data->getSize() != std::size_t(expected.width() * expected.height() * 3))
    return;
  bool equal = true;
  for (int y = 0; y < expected.height(); ++y)
    for (int x = 0; x < expected.width(); ++x) {
      const auto color = expected.pixelColor(x, y);
      const auto i = std::size_t(y * expected.width() + x);
      const auto *p = data->getBuffer() + i * 3;
      const auto a = alpha ? alpha->getBuffer()[i] : 255;
      equal &= a == color.alpha() && p[0] == color.red() &&
               p[1] == color.green() && p[2] == color.blue();
    }
  CHECK(equal);
}
void hierarchyTests(const QDir &directory) {
  std::atomic_bool cancel{false};
  c::Document doc({{48, 40}, 144});
  auto a = raster("Duplicate", {12, 15}, {255, 0, 0, 255}, {-3, 5});
  auto b = raster("Duplicate", {14, 10}, {0, 255, 0, 150}, {5, 5});
  auto d = raster("hidden child", {10, 10}, {0, 0, 255, 255});
  d.visible = false;
  auto zero = raster("zero", {8, 8}, {255, 255, 255, 255});
  zero.opacity = 0;
  for (auto layer : {a, b, d, zero})
    CHECK(doc.insertLayer(doc.layers().size(), layer));
  auto folder = c::makeLayerId(), group = c::makeLayerId(),
       empty = c::makeLayerId();
  auto tree = doc.tree();
  tree.roots = {zero.id, group, folder, empty};
  tree.containers = {{folder,
                      "folder",
                      c::ContainerKind::Folder,
                      c::ColorLabel::None,
                      {d.id},
                      false},
                     {group,
                      "group",
                      c::ContainerKind::Group,
                      c::ColorLabel::None,
                      {a.id, b.id}},
                     {empty,
                      "empty folder",
                      c::ContainerKind::Folder,
                      c::ColorLabel::None,
                      {}}};
  CHECK(doc.replaceStructure(doc.tree(), tree));
  auto snapshot =
      u::capturePdfExport(doc, 501, std::array{folder, group, a.id});
  CHECK(snapshot.entries.size() == 3);
  CHECK(snapshot.entries[0].id == d.id);
  CHECK(snapshot.entries[1].id == group);
  CHECK(snapshot.current == std::vector<c::LayerId>({d.id, group}));
  u::PdfExportOptions options;
  options.mode = u::PdfExportMode::Pages;
  options.selection = u::PdfExportSelection::Current;
  auto plan = u::planPdfExport(snapshot, options, cancel);
  CHECK(plan);
  CHECK(plan.pages.size() == 1);
  CHECK(plan.pages[0].leaves == std::vector<c::LayerId>({a.id, b.id}));
  options.ignoreHidden = false;
  plan = u::planPdfExport(snapshot, options, cancel);
  CHECK(plan.pages.size() == 2);
  auto hidden = u::renderPdfExportPage(snapshot, plan, 0, cancel);
  CHECK(hidden.image.pixelColor(2, 2) == QColor(0, 0, 255, 255));
  auto reference = u::flattenLayerItems(doc, std::array{group});
  CHECK(reference);
  options.pageSize = u::PdfExportPageSize::Fit;
  auto fit = u::planPdfExport(snapshot, options, cancel);
  CHECK(fit);
  auto fitted = u::renderPdfExportPage(snapshot, fit, 1, cancel);
  CHECK(fitted);
  CHECK(fit.pages[1].rect.x == -3);
  CHECK(fit.pages[1].pixels == reference.image.size());
  CHECK(samePixels(fitted.image, reference.image));
  options.reverse = true;
  auto reversed = u::planPdfExport(snapshot, options, cancel);
  CHECK(reversed.pages[0].leaves == fit.pages[1].leaves);
  CHECK(u::renderPdfExportPage(snapshot, reversed, 0, cancel).image ==
        fitted.image);
  options.selection = u::PdfExportSelection::Custom;
  options.chosen = {zero.id};
  auto blank = u::planPdfExport(snapshot, options, cancel);
  CHECK(blank && blank.pages[0].blankFallback);
  CHECK(blank.pages[0].pixels == QSize(48, 40));
  options.chosen = {d.id};
  options.ignoreHidden = true;
  CHECK(!u::planPdfExport(snapshot, options, cancel));
  options.chosen = {group, group};
  options.ignoreHidden = false;
  plan = u::planPdfExport(snapshot, options, cancel);
  CHECK(plan.pages.size() == 1);
  options.instanceId = 999;
  CHECK(!u::planPdfExport(snapshot, options, cancel));
  options.instanceId = 501;
  options.chosen = {99999999};
  CHECK(!u::planPdfExport(snapshot, options, cancel));
  options.selection = u::PdfExportSelection::All;
  CHECK(u::planPdfExport(snapshot, options,
                         cancel)); // Old custom IDs are inactive.
  // All visible content matches full canonical export, including pass-through
  // groups.
  options = {};
  auto full = u::planPdfExport(snapshot, options, cancel);
  auto native = u::flattenDocument(doc);
  CHECK(native);
  CHECK(samePixels(u::renderPdfExportPage(snapshot, full, 0, cancel).image,
                   native.image));
  const auto project = directory.filePath("hierarchy.vulkana");
  CHECK(u::saveProject(project, doc));
  auto loaded = u::loadProject(project);
  CHECK(loaded);
  if (loaded) {
    auto reopened = u::capturePdfExport(*loaded.document, 502, {});
    auto p = u::planPdfExport(reopened, {}, cancel);
    CHECK(samePixels(u::renderPdfExportPage(reopened, p, 0, cancel).image,
                     native.image));
  }
  // The captured layer IDs/revision can coincide in another document, but the
  // owner cannot.
  auto wrong = full;
  wrong.instanceId = 502;
  CHECK(!u::renderPdfExportPage(snapshot, wrong, 0, cancel));
  const std::array pixel{std::byte(0), std::byte(0), std::byte(0),
                         std::byte(0)};
  std::get<c::RasterLayer>(doc.layer(a.id)->payload)
      .surface->replaceRgba8({0, 0, 1, 1}, pixel, 4);
  CHECK(samePixels(u::renderPdfExportPage(snapshot, full, 0, cancel).image,
                   native.image));
}
void alphaBlendTests(const QDir &directory) {
  std::atomic_bool cancel{false};
  c::Document doc({{64, 48}, 96});
  auto base = raster("Base", {64, 48}, {100, 140, 200, 96});
  auto top = raster("Top", {25, 30}, {220, 40, 90, 137}, {7, 9});
  CHECK(doc.insertLayer(0, base));
  CHECK(doc.insertLayer(1, top));
  const auto original = doc.contentState();
  for (auto mode : c::allBlendModes) {
    doc.layer(top.id)->blendMode = mode;
    auto styles = std::make_shared<c::LayerEffectStack>();
    auto &shadow = styles->items[std::size_t(c::LayerEffectType::DropShadow)];
    shadow.enabled = true;
    shadow.size = 3;
    shadow.distance = 4;
    doc.layer(top.id)->effects = styles;
    const auto source = u::capturePdfExport(doc, 600 + unsigned(mode), {});
    const auto plan = u::planPdfExport(source, {}, cancel);
    CHECK(plan);
    const auto result = u::renderPdfExportPage(source, plan, 0, cancel);
    const auto reference = u::flattenDocument(doc);
    CHECK(result && reference);
    CHECK(samePixels(result.image, reference.image));
    auto isolated = u::PdfExportOptions{};
    isolated.mode = u::PdfExportMode::Pages;
    isolated.selection = u::PdfExportSelection::Custom;
    isolated.chosen = {top.id};
    auto p = u::planPdfExport(source, isolated, cancel);
    auto rendered = u::renderPdfExportPage(source, p, 0, cancel);
    CHECK(rendered);
    c::Document independent(doc.canvas());
    CHECK(independent.insertLayer(0, *doc.layer(top.id)));
    CHECK(samePixels(rendered.image, u::flattenDocument(independent).image));
    if (mode == c::BlendMode::ColorDodge) {
      CHECK(u::writePdfExport(source, p, directory.filePath("alpha.pdf"),
                              cancel));
      exactImage(directory.filePath("alpha.pdf"), rendered.image);
      CHECK(rendered.image.save(directory.filePath("alpha-reference.png")));
      isolated.matte = true;
      isolated.matteColor = QColor(60, 90, 170);
      auto matte = u::planPdfExport(source, isolated, cancel);
      u::ExportSettings flat;
      flat.size = {64, 48};
      flat.pngMatte = true;
      flat.pngMatteColor = isolated.matteColor;
      CHECK(u::renderPdfExportPage(source, matte, 0, cancel).image ==
            u::renderExport(independent, flat).image);
      CHECK(u::renderPdfExportPage(source, p, 0, cancel).image ==
            rendered.image);
    }
  }
  CHECK(doc.contentState() ==
        original); // Export itself never advances history/content identity.
}
void textGeometryTests(const QDir &directory) {
  std::atomic_bool cancel{false};
  for (int variant = 0; variant < 5; ++variant) {
    c::Document doc({{900, 500}, 120});
    auto backdrop = raster("Photo", {900, 500}, {45, 105, 170, 255});
    CHECK(doc.insertLayer(0, backdrop));
    c::Layer title;
    title.id = c::makeLayerId();
    title.name = "Editable title";
    c::TextLayer text;
    text.utf8 = "Office café / Ångström\nمرحبا بالعالم 123\nMultiple formats";
    text.defaultStyle.sizePixels = 28;
    auto emphasis = text.defaultStyle;
    emphasis.font.family = "Serif";
    emphasis.sizePixels = 30;
    emphasis.color = {255, 180, 90, 255};
    text.runs = {{0, 6, emphasis}};
    title.payload = text;
    title.localToDocument.m02 = 70;
    title.localToDocument.m12 = 30;
    if (variant == 1) {
      const double a = .2;
      title.localToDocument.m00 = std::cos(a);
      title.localToDocument.m01 = -std::sin(a);
      title.localToDocument.m10 = std::sin(a);
      title.localToDocument.m11 = std::cos(a);
    }
    if (variant == 2) {
      title.localToDocument.m00 = -1;
      title.localToDocument.m02 = 700;
    }
    if (variant == 3) {
      title.localToDocument.m00 = 1.3;
      title.localToDocument.m11 = .8;
    }
    if (variant == 4) {
      title.localToDocument.m11 = -1;
      title.localToDocument.m12 = 350;
    }
    CHECK(doc.insertLayer(1, title));
    const auto group = c::makeLayerId();
    auto tree = doc.tree();
    tree.roots = {group};
    tree.containers = {{group,
                        "Page group",
                        c::ContainerKind::Group,
                        c::ColorLabel::None,
                        {backdrop.id, title.id}}};
    CHECK(doc.replaceStructure(doc.tree(), tree));
    const auto source =
        u::capturePdfExport(doc, 800 + std::uint64_t(variant), {});
    u::PdfExportOptions options;
    options.mode = u::PdfExportMode::Pages;
    auto plan = u::planPdfExport(source, options, cancel);
    CHECK(plan && plan.pages.size() == 1 && plan.preserved == 1);
    if (!plan)
      continue;
    const auto name =
        directory.filePath(QString("text-transform-%1").arg(variant));
    auto result = u::writePdfExport(source, plan, name + ".pdf", cancel);
    CHECK(result);
    CHECK(result.textWarnings.empty());
    for (const auto &warning : result.textWarnings)
      std::cerr << warning.toStdString() << '\n';
    if (result)
      independentText(name + ".pdf");
    auto reference = u::renderPdfExportPage(source, plan, 0, cancel);
    CHECK(reference);
    CHECK(reference.image.save(name + "-reference.png"));
    {
      auto highOptions = options;
      highOptions.ppi = 480;
      auto highPlan = u::planPdfExport(source, highOptions, cancel);
      auto high = u::renderPdfExportPage(source, highPlan, 0, cancel);
      CHECK(high);
      CHECK(high.image.save(name + "-high-reference.png"));
      CHECK(u::writePdfExport(source, highPlan, name + "-high.pdf", cancel));
    }
    QPdfDocument pdf;
    CHECK(pdf.load(name + ".pdf") == QPdfDocument::Error::None);
    CHECK(pdf.render(0, {900, 500}).save(name + "-qt.png"));
    auto extracted = pdf.getAllText(0).text();
    std::cout << "Transform " << variant
              << " extracted: " << extracted.toStdString() << '\n';
    // PDFium ignores a form's enclosing ActualText and geometrically sorts
    // rotated/mirrored words. The independent Poppler check below validates
    // logical extraction in these cases; inspect real fonts and raster here.
    if (variant == 0 || variant == 3) {
      CHECK(extracted.contains("Office"));
      CHECK(extracted.contains(QString::fromUtf8("مرحبا")));
    }
    auto rasterOptions = options;
    rasterOptions.text = u::PdfExportText::Rasterize;
    auto rasterPlan = u::planPdfExport(source, rasterOptions, cancel);
    CHECK(rasterPlan.preserved == 0 && rasterPlan.rasterized == 1);
    CHECK(u::writePdfExport(source, rasterPlan, name + "-raster.pdf", cancel));
    exactImage(name + "-raster.pdf", reference.image);
  }
}
void fittedGeometryTests(const QDir &directory) {
  std::atomic_bool cancel{false};
  c::Document doc({{80, 64}, 144});
  c::ShapeLayer shape;
  shape.kind = c::ShapeKind::Ellipse;
  shape.size = {42, 31};
  shape.fillColor = {220, 80, 40, 170};
  shape.strokeEnabled = true;
  shape.strokeWidth = 3;
  auto layer = c::Layer::shape("Distorted shape", shape);
  layer.localToDocument = {.9, -.25, -12, .2, 1.1, -8, .0015, .0007, 1};
  layer.crop = c::LayerCrop{1, 2, 39, 27};
  layer.crop->corners = {2, 4, 1, 3};
  auto effects = std::make_shared<c::LayerEffectStack>();
  auto &shadow = effects->items[std::size_t(c::LayerEffectType::DropShadow)];
  shadow.enabled = true;
  shadow.size = 4;
  shadow.distance = 9;
  layer.effects = effects;
  CHECK(doc.insertLayer(0, layer));
  const auto source = u::capturePdfExport(doc, 888, {});
  u::PdfExportOptions options;
  options.mode = u::PdfExportMode::Pages;
  options.pageSize = u::PdfExportPageSize::Fit;
  const auto plan = u::planPdfExport(source, options, cancel);
  CHECK(plan);
  const auto canonical = u::flattenLayerItems(doc, std::array{layer.id});
  CHECK(canonical);
  const auto image = u::renderPdfExportPage(source, plan, 0, cancel);
  CHECK(image);
  CHECK(samePixels(image.image, canonical.image));
  CHECK(plan.pages[0].rect.x < 0 && plan.pages[0].rect.y < 0);
  CHECK(plan.pages[0].points ==
        QSizeF(plan.pages[0].rect.width * .5, plan.pages[0].rect.height * .5));
  CHECK(u::writePdfExport(source, plan, directory.filePath("fitted-shape.pdf"),
                          cancel));
  exactImage(directory.filePath("fitted-shape.pdf"), image.image);
  options.ppi = 288;
  const auto dense = u::planPdfExport(source, options, cancel);
  CHECK(dense);
  CHECK(dense.pages[0].points == plan.pages[0].points);
  CHECK(dense.pages[0].pixels == plan.pages[0].pixels * 2);
  CHECK(std::holds_alternative<c::ShapeLayer>(doc.layer(layer.id)->payload));
  CHECK(doc.layer(layer.id)->crop == layer.crop &&
        doc.layer(layer.id)->effects == effects);
}
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QTemporaryDir temporary;
  QDir directory(
      qEnvironmentVariable("VULKANA_PDF_EXPORT_OUTPUT", temporary.path()));
  CHECK(directory.mkpath("."));
  std::atomic_bool cancel{false};
  if (app.arguments().contains("--benchmark")) {
    const auto at = app.arguments().indexOf("--benchmark");
    const auto size = app.arguments().value(at + 1, "4096").toUInt();
    const auto count = app.arguments().value(at + 2, "5").toInt();
    c::Document doc({{size, size * 9 / 16}, 300});
    for (int i = 0; i < count; ++i)
      CHECK(doc.insertLayer(doc.layers().size(),
                            raster(QString("Page %1").arg(i + 1),
                                   doc.canvas().extent,
                                   {std::uint8_t(20 + i * 15), 70, 120, 200})));
    QElapsedTimer timer;
    timer.start();
    const auto source = u::capturePdfExport(doc, 1000, {});
    const auto capture = timer.elapsed();
    u::PdfExportOptions options;
    options.mode = u::PdfExportMode::Pages;
    const auto plan = u::planPdfExport(source, options, cancel);
    CHECK(plan);
    const auto planning = timer.elapsed() - capture;
    const auto written = u::writePdfExport(
        source, plan, directory.filePath("benchmark.pdf"), cancel);
    CHECK(written);
    std::cout << size << " x " << size * 9 / 16 << ", " << count
              << " pages: freeze " << capture << " ms, planning " << planning
              << " ms, render/write " << written.milliseconds
              << " ms, finalization " << written.finalizationMilliseconds
              << " ms, " << written.bytes << " bytes\nPer-page ms:";
    for (auto ms : written.pageMilliseconds)
      std::cout << ' ' << ms;
    std::cout << '\n';
    return failures ? 1 : 0;
  }
  hierarchyTests(directory);
  alphaBlendTests(directory);
  textGeometryTests(directory);
  fittedGeometryTests(directory);
  c::Document doc({{501, 301}, 96});
  for (int i = 0; i < 15; ++i)
    CHECK(doc.insertLayer(doc.layers().size(),
                          raster(QString::number(i + 1), {20, 30},
                                 {std::uint8_t(i * 10), 0, 0, 255},
                                 {double(i * 25), 10})));
  const auto snapshot = u::capturePdfExport(doc, 42, {});
  CHECK(snapshot.entries.size() == 15);
  CHECK(snapshot.entries[0].id == doc.layers().back().id);
  u::PdfExportOptions options;
  options.mode = u::PdfExportMode::Pages;
  options.selection = u::PdfExportSelection::Custom;
  options.chosen = {snapshot.entries[0].id, snapshot.entries[1].id,
                    snapshot.entries[2].id};
  auto plan = u::planPdfExport(snapshot, options, cancel);
  CHECK(plan);
  CHECK(plan.pages.size() == 3);
  const auto filename = directory.filePath("three-of-fifteen.pdf");
  const auto written = u::writePdfExport(snapshot, plan, filename, cancel);
  CHECK(written);
  if (!written)
    std::cerr << written.error.toStdString() << '\n';
  if (written) {
    QPDF pdf;
    pdf.processFile(filename.toUtf8().constData());
    auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
    CHECK(pages.size() == 3);
    const auto box = pages.front().getMediaBox().getArrayAsRectangle();
    CHECK(std::abs(box.urx - 375.75) < 1e-8);
    CHECK(std::abs(box.ury - 225.75) < 1e-8);
  }
  c::Document textDoc({{700, 350}, 96});
  CHECK(
      textDoc.insertLayer(0, raster("Photo", {700, 350}, {60, 120, 180, 255})));
  c::Layer text;
  text.id = c::makeLayerId();
  text.name = "Title";
  c::TextLayer model;
  model.utf8 = "Vulkana office café\nمرحبا بالعالم";
  model.defaultStyle.sizePixels = 36;
  model.defaultStyle.font.family = "DejaVu Sans";
  model.defaultStyle.color = {255, 255, 255, 255};
  text.payload = model;
  text.localToDocument.m02 = 30;
  text.localToDocument.m12 = 30;
  CHECK(textDoc.insertLayer(1, text));
  const auto textSnapshot = u::capturePdfExport(textDoc, 43, {});
  auto textPlan = u::planPdfExport(textSnapshot, {}, cancel);
  CHECK(textPlan);
  CHECK(textPlan.preserved == 1);
  std::cout << "Text preserved " << textPlan.preserved << " rasterized "
            << textPlan.rasterized << '\n';
  for (const auto &p : textPlan.pages)
    for (const auto &reason : p.textReasons)
      std::cout << reason.toStdString() << '\n';
  const auto textName = directory.filePath("hybrid.pdf");
  auto result = u::writePdfExport(textSnapshot, textPlan, textName, cancel);
  CHECK(result);
  if (!result)
    std::cerr << result.error.toStdString() << '\n';
  auto reference = u::renderPdfExportPage(textSnapshot, textPlan, 0, cancel);
  CHECK(reference);
  if (reference)
    CHECK(reference.image.save(directory.filePath("hybrid-reference.png")));
  if (result) {
    QPdfDocument pdf;
    CHECK(pdf.load(textName) == QPdfDocument::Error::None);
    CHECK(pdf.pageCount() == 1);
    CHECK(pdf.render(0, {700, 350}).save(directory.filePath("hybrid-qt.png")));
    std::cout << "Qt extracted: " << pdf.getAllText(0).text().toStdString()
              << '\n';
    QPdfSearchModel search;
    search.setDocument(&pdf);
    for (const auto &term :
         QStringList{"office", "café", QString::fromUtf8("مرحبا")}) {
      search.setSearchString(term);
      QElapsedTimer wait;
      wait.start();
      while (search.rowCount({}) == 0 && wait.elapsed() < 1000)
        QCoreApplication::processEvents();
      std::cout << "Search " << term.toStdString() << ": "
                << search.rowCount({}) << '\n';
      CHECK(search.rowCount({}) > 0);
    }
  }
  // Raster PPI cannot change physical text layout or page dimensions.
  for (double ppi : {72., 150., 300.}) {
    auto higher = textPlan.options;
    higher.ppi = ppi;
    auto p = u::planPdfExport(textSnapshot, higher, cancel);
    CHECK(p.preserved == 1);
    CHECK(p.pages[0].points == textPlan.pages[0].points);
    CHECK(u::writePdfExport(
        textSnapshot, p, directory.filePath(QString("hybrid-%1.pdf").arg(ppi)),
        cancel));
  }
  for (int kind = 0; kind < 5; ++kind) {
    auto modified = textDoc;
    auto *layer = modified.layer(text.id);
    if (kind == 0)
      layer->opacity = .5;
    if (kind == 1)
      layer->localToDocument.m20 = .001;
    if (kind == 2) {
      auto style = std::make_shared<c::LayerEffectStack>();
      style->items[0].enabled = true;
      layer->effects = style;
    }
    if (kind == 3)
      CHECK(
          modified.insertLayer(2, raster("Above", {40, 40}, {255, 0, 0, 128})));
    // Synthetic bold/variable-font weight can be painted with Qt's PDF
    // overstrike rather than the canonical font outline. Do not silently
    // preserve different glyph geometry; allow either an actual bold face or
    // an explicit canonical fallback, depending on installed font resolution.
    if (kind == 4) {
      auto &styled = std::get<c::TextLayer>(layer->payload);
      styled.defaultStyle.font.family = "Serif";
      styled.defaultStyle.font.weight = 700;
    }
    auto source = u::capturePdfExport(modified, 700 + std::uint64_t(kind), {});
    const auto sourceReference = u::flattenDocument(modified).image;
    auto p = u::planPdfExport(source, {}, cancel);
    CHECK(p);
    if (kind != 4)
      CHECK(p.preserved == 0 && p.rasterized == 1);
    else {
      const auto path = directory.filePath("bold.pdf");
      const auto writtenBold = u::writePdfExport(source, p, path, cancel);
      CHECK(writtenBold);
      if (p.preserved == 0 || !writtenBold.textWarnings.empty()) {
        CHECK(!p.pages[0].textReasons.empty() ||
              !writtenBold.textWarnings.empty());
        exactImage(path, u::renderPdfExportPage(source, p, 0, cancel).image);
      }
      auto regular = text;
      regular.id = c::makeLayerId();
      CHECK(modified.insertLayer(2, regular));
      const auto mixed = u::capturePdfExport(modified, 799, {});
      u::PdfExportOptions pages;
      pages.mode = u::PdfExportMode::Pages;
      const auto mixedPlan = u::planPdfExport(mixed, pages, cancel);
      CHECK(mixedPlan);
      const auto mixedResult = u::writePdfExport(
          mixed, mixedPlan, directory.filePath("font-fallback-pages.pdf"),
          cancel);
      CHECK(mixedResult && mixedResult.preserved >= 1 &&
            mixedResult.rasterized <= 1);
      QPdfDocument mixedPdf;
      CHECK(mixedPdf.load(directory.filePath("font-fallback-pages.pdf")) ==
            QPdfDocument::Error::None);
      CHECK(mixedPdf.getAllText(0).text().contains("Vulkana office"));
    }
    if (kind != 4)
      CHECK(!p.pages[0].textReasons.empty());
    CHECK(samePixels(u::renderPdfExportPage(source, p, 0, cancel).image,
                     sourceReference));
  }
  // Unfinished/failed output cannot damage an existing file.
  const auto before = bytes(filename);
  CHECK(u::writePdfExport(snapshot, plan, filename, cancel,
                          [](int, int, const QString &) { return false; })
            .cancelled);
  CHECK(bytes(filename) == before);
  CHECK(!u::writePdfExport(
      snapshot, plan, directory.filePath("missing/sub/output.pdf"), cancel));
  CHECK(!u::writePdfExport(snapshot, plan, directory.absolutePath(), cancel));
  auto bad = plan.options;
  bad.ppi = 2401;
  CHECK(!u::planPdfExport(snapshot, bad, cancel));
  bad = plan.options;
  bad.mode = static_cast<u::PdfExportMode>(99);
  CHECK(!u::planPdfExport(snapshot, bad, cancel));
  auto limited = snapshot;
  limited.limits.workingBytes = limited.sourceBytes;
  CHECK(!u::planPdfExport(limited, plan.options, cancel));
  bool refused = false;
  try {
    (void)u::capturePdfExport(doc, 42, {}, u::PdfExportLimits{1024, 512});
  } catch (const std::exception &) {
    refused = true;
  }
  CHECK(refused);
  options.reverse = true;
  auto reversed = u::planPdfExport(snapshot, options, cancel);
  CHECK(reversed);
  CHECK(reversed.pages.front().entries == plan.pages.back().entries);
  options.pageSize = u::PdfExportPageSize::Fit;
  auto fitted = u::planPdfExport(snapshot, options, cancel);
  CHECK(fitted);
  CHECK(fitted.pages.front().pixels == QSize(20, 30));
  cancel = true;
  const auto cancelled = u::writePdfExport(
      snapshot, plan, directory.filePath("cancel.pdf"), cancel);
  CHECK(cancelled.cancelled);
  cancel = false;
  // Optional private import -> edit -> project round-trip -> PDF export checks.
  for (int arg = 1; arg < argc; ++arg) {
    QString path = QString::fromLocal8Bit(argv[arg]);
    if (!path.endsWith(".pdf", Qt::CaseInsensitive))
      continue;
    auto state = std::make_shared<u::PdfJobState>();
    auto metadata = u::readPdfMetadata(path, {}, state).get();
    CHECK(metadata.source);
    if (!metadata.source)
      continue;
    u::PdfOptions import;
    for (std::size_t i = 0; i < metadata.pages.size(); ++i)
      import.pages.push_back(int(i));
    auto dimensions = u::planPdfImport(metadata, import);
    CHECK(dimensions);
    auto pages = u::renderPdfPages(metadata, import, {}, state).get();
    CHECK(pages.error.isEmpty());
    c::Document imported({{std::uint32_t(dimensions.canvas.width()),
                           std::uint32_t(dimensions.canvas.height())},
                          300});
    for (auto &page : pages.pages)
      CHECK(imported.insertLayer(0, std::move(page.layer)));
    // A real source-pixel edit, without leaving imported PDF provenance
    // dependencies.
    auto surface =
        std::get<c::RasterLayer>(imported.layers().back().payload).surface;
    const std::array pixel{std::byte(255), std::byte(0), std::byte(255),
                           std::byte(255)};
    surface->replaceRgba8({0, 0, 1, 1}, pixel, 4);
    auto name = QFileInfo(path).completeBaseName();
    auto project = directory.filePath(name + ".vulkana");
    CHECK(u::saveProject(project, imported));
    auto loaded = u::loadProject(project);
    CHECK(loaded);
    if (!loaded)
      continue;
    auto source =
        u::capturePdfExport(*loaded.document, 900 + std::uint64_t(arg), {});
    u::PdfExportOptions opt;
    opt.mode = u::PdfExportMode::Pages;
    opt.pageSize = u::PdfExportPageSize::Fit;
    auto p = u::planPdfExport(source, opt, cancel);
    CHECK(p);
    auto result = u::writePdfExport(
        source, p, directory.filePath(name + "-export.pdf"), cancel);
    CHECK(result);
    std::cout << name.toStdString() << ": " << result.pages << " pages, "
              << result.milliseconds << " ms, " << result.bytes
              << " bytes; estimated peak " << p.peakWorkingBytes << '\n';
    std::cout << "Per-page ms:";
    for (auto ms : result.pageMilliseconds)
      std::cout << ' ' << ms;
    std::cout << "; finalization " << result.finalizationMilliseconds
              << " ms\n";
    for (std::size_t i = 0; i < p.pages.size(); ++i) {
      auto image = u::renderPdfExportPage(source, p, i, cancel);
      CHECK(image);
      CHECK(image.image.save(
          directory.filePath(name + QString("-reference-%1.png").arg(i + 1))));
    }
    auto reopened = u::readPdfMetadata(directory.filePath(name + "-export.pdf"),
                                       {}, std::make_shared<u::PdfJobState>())
                        .get();
    CHECK(reopened.source);
    CHECK(reopened.pages.size() == metadata.pages.size());
    for (std::size_t i = 0; i < reopened.pages.size(); ++i) {
      CHECK(std::abs(reopened.pages[i].points.width() -
                     metadata.pages[i].points.width()) < .25);
      CHECK(std::abs(reopened.pages[i].points.height() -
                     metadata.pages[i].points.height()) < .25);
    }
  }
  std::cout << "PDF export tests " << (failures ? "FAILED" : "passed") << '\n';
  return failures ? 1 : 0;
}
