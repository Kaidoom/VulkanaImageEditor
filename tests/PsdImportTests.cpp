#include "../src/ui/src/PsdReader.hpp"
#include "PsdFixture.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/PsdImportDialog.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QLabel>
#include <iostream>
#include <zlib.h>
namespace u = imageeditor::ui;
namespace c = imageeditor::core;
int failures{};
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__)) {                                                      \
      ++failures;                                                              \
      std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n";                         \
    }                                                                          \
  } while (false)
void be16(QByteArray &b, int v) {
  b += char((v >> 8) & 255);
  b += char(v & 255);
}
void be32(QByteArray &b, int v) {
  be16(b, (v >> 16) & 65535);
  be16(b, v & 65535);
}
QByteArray encoded(const QByteArray &data, int width, int compression) {
  QByteArray result;
  be16(result, compression);
  if (compression == 0)
    return result + data;
  if (compression == 1) {
    QByteArray rows;
    for (int row = 0; row < data.size() / width; ++row) {
      QByteArray r;
      for (int x = 0; x < width;) {
        auto n = std::min(128, width - x);
        r += char(n - 1);
        r += data.mid(row * width + x, n);
        x += n;
      }
      be16(result, int(r.size()));
      rows += r;
    }
    return result + rows;
  }
  auto input = data;
  if (compression == 3)
    for (int row = 0; row < data.size() / width; ++row)
      for (int x = width - 1; x > 0; --x)
        input[row * width + x] = char(quint8(data[row * width + x]) -
                                      quint8(data[row * width + x - 1]));
  uLongf size = compressBound(uLong(data.size()));
  QByteArray zip(qsizetype(size), Qt::Uninitialized);
  CHECK(compress2(reinterpret_cast<Bytef *>(zip.data()), &size,
                  reinterpret_cast<const Bytef *>(input.constData()),
                  uLong(input.size()), 9) == Z_OK);
  zip.resize(qsizetype(size));
  return result + zip;
}
QByteArray fixture(int compression, bool mask = false) {
  QByteArray file = "8BPS";
  be16(file, 1);
  file += QByteArray(6, 0);
  be16(file, 3);
  be32(file, 2);
  be32(file, 3);
  be16(file, 8);
  be16(file, 3);
  be32(file, 0);
  be32(file, 0);
  QByteArray info;
  be16(info, 1);
  be32(info, -1);
  be32(info, 2);
  be32(info, 1);
  be32(info, 5);
  be16(info, mask ? 5 : 4);
  std::vector<QByteArray> channels;
  for (int id : {-1, 0, 1, 2}) {
    auto data = id == -1 ? QByteArray::fromHex("00ff8040ff01")
                         : QByteArray::fromHex(id == 0   ? "010203040506"
                                               : id == 1 ? "ff0000408060"
                                                         : "c0c1c2c3c4c5");
    channels.push_back(encoded(data, 3, compression));
    be16(info, id);
    be32(info, int(channels.back().size()));
  }
  if (mask) {
    channels.push_back(encoded(QByteArray::fromHex("0080"), 2, compression));
    be16(info, -2);
    be32(info, int(channels.back().size()));
  }
  info += "8BIMnorm";
  info += char(128);
  info += QByteArray(3, 0);
  QByteArray extra;
  if (mask) {
    be32(extra, 20);
    be32(extra, 0);
    be32(extra, 3);
    be32(extra, 1);
    be32(extra, 5);
    extra += char(255);
    extra += char(2);
    be16(extra, 0);
  } else
    be32(extra, 0);
  be32(extra, 0);
  extra += char(1);
  extra += 'A';
  extra += QByteArray(2, 0);
  be32(info, int(extra.size()));
  info += extra;
  for (auto &channel : channels)
    info += channel;
  if (info.size() % 2)
    info += char(0);
  QByteArray section;
  be32(section, int(info.size()));
  section += info;
  be32(section, 0);
  be32(file, int(section.size()));
  file += section;
  be16(file, 0);
  file += QByteArray(18, char(127));
  return file;
}
void unit() {
  constexpr std::uint64_t GiB = 1024ULL * 1024 * 1024;
  CHECK(u::psdWorkingMemoryLimit(8 * GiB, 2 * GiB) == 8 * GiB);
  CHECK(u::psdWorkingMemoryLimit(0, 2 * GiB) == 2 * GiB);
  CHECK(u::psdWorkingMemoryLimit(GiB, UINT64_MAX - 1) == UINT64_MAX);
  CHECK(u::PsdLimits{}.rasterBytes > 256ULL * 1024 * 1024);
  auto job = std::make_shared<u::PsdJob>();
  const auto source = QByteArray::fromHex("000102ffff03aabbcc108077");
  for (int compression = 0; compression < 4; ++compression)
    CHECK(u::psd::decodeChannel(encoded(source, 6, compression), 6, 2, job) ==
          source);
  for (auto bad :
       {QByteArray::fromHex("0001000200040080"),
        QByteArray::fromHex("00010002ff"), QByteArray::fromHex("0000ff"),
        QByteArray::fromHex("0002789c")}) {
    bool failed = false;
    try {
      (void)u::psd::decodeChannel(bad, 3, 1, job);
    } catch (const std::exception &) {
      failed = true;
    }
    CHECK(failed);
  }
  QTemporaryDir dir;
  CHECK(dir.isValid());
  for (int compression = 0; compression < 4; ++compression) {
    QString path = dir.filePath(QString::number(compression) + ".psd");
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly));
    auto bytes = fixture(compression, true);
    CHECK(f.write(bytes) == bytes.size());
    f.close();
    auto inspection = u::inspectPsd(path, job);
    std::cerr << inspection.error.toStdString();
    CHECK(inspection.error.isEmpty());
    CHECK(inspection.layers.size() == 1);
    CHECK(inspection.savedComposite);
    auto options = u::defaultPsdOptions(inspection);
    auto output = u::convertPsd(inspection, options, job, {}, false);
    std::cerr << output.error.toStdString();
    CHECK(output.document);
    if (!output.document)
      continue;
    const auto &layer = output.document->layers()[0];
    CHECK(layer.opacity == float(128) / 255);
    CHECK(layer.localToDocument.m02 == 2 && layer.localToDocument.m12 == -1);
    CHECK(layer.mask && !layer.mask->enabled && layer.mask->outside == 255);
    CHECK(layer.mask->localToMask.m02 == -1 &&
          layer.mask->localToMask.m12 == -1);
    auto surface = std::get<c::RasterLayer>(layer.payload).surface;
    CHECK(surface->extent() == c::Extent2u{3, 2});
    CHECK(u::saveProject(dir.filePath("copy.vulkana"), *output.document));
    auto saved = u::loadProject(dir.filePath("copy.vulkana"));
    CHECK(saved.document && saved.document->layers()[0].mask &&
          !saved.document->layers()[0].mask->enabled);
    f.remove();
    auto independent = u::convertPsd(inspection, options, job, {}, false);
    CHECK(independent.document);
    if (independent.document)
      CHECK(independent.document->layers()[0].id != layer.id);
    options.composite = true;
    auto comp = u::convertPsd(inspection, options, job, {}, false);
    CHECK(comp.document && comp.document->layers().size() == 1);
    CHECK(!job->renderingPreview && job->previewPercent == 0);
    auto preview = u::convertPsd(inspection, options, job, {}, true);
    CHECK(preview.document && !preview.preview.isNull());
    CHECK(job->renderingPreview && job->previewPercent == 100);
    auto withoutPreview = u::convertPsd(inspection, options, job, {}, false);
    CHECK(withoutPreview.document && !job->renderingPreview && job->previewPercent == 0);
    job->cancelled = true;
    auto cancelled = u::convertPsd(inspection, options, job, {}, false);
    CHECK(cancelled.cancelled && !cancelled.document);
    job->cancelled = false;
  }
  CHECK(u::psd::utf16(QByteArray::fromHex("0041d83dde000042")) ==
        QString::fromUtf8("A😀B"));
  bool failed = false;
  try {
    (void)u::psd::utf16(QByteArray::fromHex("d83d0041"));
  } catch (...) {
    failed = true;
  }
  CHECK(failed);
  auto full = fixture(0);
  for (int n = 0; n < full.size(); n += 7) {
    try {
      (void)u::psd::parse(full.left(n), job, {});
    } catch (const std::exception &) {
      continue;
    }
    CHECK(n >= full.size() - 20);
  }
}
void semanticFixtures() {
  namespace f = psdfixture;
  QTemporaryDir dir;
  auto job = std::make_shared<u::PsdJob>();
  auto inspect = [&](QByteArray bytes) {
    QFile file(dir.filePath("test.psd"));
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(bytes) == bytes.size());
    file.close();
    return u::inspectPsd(file.fileName(), job);
  };
  auto convert = [&](const u::PsdInspection &in) {
    auto out = u::convertPsd(in, u::defaultPsdOptions(in), job, {}, false);
    CHECK(out.error.isEmpty());
    return out;
  };
  const QString words = QString::fromUtf8("A😀 café\r第二行\r");
  f::Layer text;
  text.tags["TySh"] = f::textTag(words, "Unavailable-fixture-face",
                                 {3., double(words.size() - 3)});
  auto in = inspect(f::file({text}));
  CHECK(in.error.isEmpty() && in.layers[0].editable && in.layers[0].raster &&
        !in.layers[0].fonts.empty());
  auto out = convert(in);
  CHECK(out.document);
  if (out.document) {
    auto &layer = out.document->layers()[0];
    auto *t = std::get_if<c::TextLayer>(&layer.payload);
    CHECK(t);
    if (t) {
      CHECK(t->utf8 ==
            QString::fromUtf8("A😀 café\n第二行").toUtf8().toStdString());
      CHECK(t->runs.size() == 2 && t->runs[0].length == 5 &&
            t->runs[1].start == 5);
      CHECK(t->runs[0].style.sizePixels == 16 &&
            t->runs[1].style.sizePixels == 24);
      CHECK(t->runs[0].style.color.red == 255 &&
            t->runs[1].style.color.blue == 255);
      CHECK(t->defaultStyle.font.originalFace == "Unavailable-fixture-face");
      CHECK(t->paragraphs.size() == 2);
      CHECK(u::saveProject(dir.filePath("unicode.vulkana"), *out.document));
      auto reopened = u::loadProject(dir.filePath("unicode.vulkana"));
      CHECK(reopened.document &&
            std::get<c::TextLayer>(reopened.document->layers()[0].payload) ==
                *t);
    }
  }
  auto highPpi = inspect(f::file({text}, {96, 96}, 300));
  auto high = convert(highPpi);
  CHECK(high.document);
  if (high.document && out.document) {
    CHECK(std::get<c::TextLayer>(high.document->layers()[0].payload) ==
          std::get<c::TextLayer>(out.document->layers()[0].payload));
    CHECK(high.document->layers()[0].localToDocument ==
          out.document->layers()[0].localToDocument);
  }
  const auto installed =
      QFontInfo(QFontDatabase::systemFont(QFontDatabase::GeneralFont)).family();
  text.tags["TySh"] = f::textTag("Present face\r", installed);
  in = inspect(f::file({text}));
  CHECK(in.layers[0].editable && in.layers[0].fonts.empty());
  text.tags["TySh"] =
      f::textTag("Invalid index\r", installed, {}, false, 1e100);
  in = inspect(f::file({text}));
  CHECK(in.error.isEmpty() && !in.layers[0].editable && in.layers[0].raster);
  CHECK(in.layers[0].issues.join(' ').contains("font index"));
  text.tags["TySh"] =
      f::textTag(words, "Missing-face", {2., double(words.size() - 2)});
  in = inspect(f::file({text}));
  CHECK(!in.layers[0].editable && in.layers[0].raster);
  CHECK(in.layers[0].issues.join(' ').contains("surrogate"));
  text.tags["TySh"] = f::textTag("Warp\r", installed, {}, true);
  in = inspect(f::file({text}));
  CHECK(!in.layers[0].editable && in.layers[0].raster);
  text.pixels = false;
  in = inspect(f::file({text}, {96, 96}, 72, {}, false));
  CHECK(!in.layers[0].editable && !in.layers[0].raster &&
        !in.layers[0].basePixels && !in.savedComposite);
  f::Layer shape;
  shape.tags["vmsk"] = f::polygon({{10, 10}, {50, 10}, {30, 40}});
  shape.tags["SoCo"] = f::descriptor(
      {{"Clr ", QVariantMap{{"Rd  ", 10.}, {"Grn ", 20.}, {"Bl  ", 200.}}}});
  in = inspect(f::file({shape}));
  CHECK(in.layers[0].editable);
  out = convert(in);
  CHECK(out.document && std::holds_alternative<c::ShapeLayer>(
                            out.document->layers()[0].payload));
  shape.tags["vmsk"] =
      f::polygon({{10, 10}, {50, 10}, {30, 40}}, {96, 96}, true);
  in = inspect(f::file({shape}));
  CHECK(!in.layers[0].editable && in.layers[0].raster);
  f::Layer stroked;
  QByteArray style;
  f::u32(style, 0);
  style += f::descriptor(
      {{"Scl ", f::unit(150., "#Prc")},
       {"masterFXSwitch", true},
       {"FrFX",
        QVariantMap{
            {"enab", true},
            {"Md  ", f::enumeration("Nrml")},
            {"Opct", f::unit(40., "#Prc")},
            {"Clr ", QVariantMap{{"Rd  ", 255.}, {"Grn ", 40.}, {"Bl  ", 10.}}},
            {"Sz  ", f::unit(4.)},
            {"Styl", f::enumeration("OutF")},
            {"PntT", f::enumeration("SClr")}}}});
  stroked.tags["lfx2"] = style;
  in = inspect(f::file({stroked}));
  CHECK(in.layers[0].editable);
  out = convert(in);
  CHECK(out.document);
  if (out.document) {
    auto effect = out.document->layers()[0].effects;
    CHECK(effect && effect->items[0].enabled && effect->items[0].size == 6 &&
          std::abs(effect->items[0].opacity - .4) < .00001);
  }
  f::Layer leaf;
  leaf.name = "leaf";
  f::Layer start;
  start.pixels = false;
  start.name = "parent";
  start.tags["lsct"] = f::group(1);
  start.blend = "pass";
  f::Layer end = start;
  end.tags["lsct"] = f::group(3);
  in = inspect(f::file({end, leaf, start}));
  CHECK(in.error.isEmpty() && in.layers.size() == 2 &&
        in.layers[0].parent == 2);
  out = convert(in);
  CHECK(out.document && out.document->tree().roots.size() == 1 &&
        out.document->tree().containers.size() == 1);
  auto options = u::defaultPsdOptions(in);
  options.layers[1].route = u::PsdRoute::Skip;
  auto skip = u::convertPsd(in, options, job, {}, false);
  CHECK(!skip.document);
  start.tags["lsct"] = f::group(1, "norm");
  in = inspect(f::file({end, leaf, start}));
  CHECK(!in.layers[0].editable && !in.layers[1].editable);
  leaf.clipping = true;
  in = inspect(f::file({leaf}));
  CHECK(!in.layers[0].editable && !in.layers[0].raster &&
        in.layers[0].basePixels);
  options = u::defaultPsdOptions(in);
  options.layers[0].route = u::PsdRoute::BasePixels;
  auto lossy = u::convertPsd(in, options, job, {}, false);
  CHECK(lossy.document && lossy.report.contains("omitted"));
  leaf.clipping = false;
  {
    f::Layer base;base.name="Base";base.tags["clbl"]=QByteArray::fromHex("01000000");
    f::Layer clipped;clipped.name="Clipped";clipped.clipping=true;
    auto chain=inspect(f::file({base,clipped}));CHECK(chain.error.isEmpty());
    CHECK(chain.layers[1].editable&&chain.layers[1].clippingBase==0);
    auto result=convert(chain);CHECK(result.document&&result.document->layers().size()==2);
    CHECK(result.document&&result.document->tree().containers.front().kind==c::ContainerKind::ClippingMaskGroup);
    auto choices=u::defaultPsdOptions(chain);choices.layers[0].route=u::PsdRoute::Skip;
    auto missing=u::convertPsd(chain,choices,job,{},false);CHECK(!missing.document&&missing.error.contains("requires clipping base"));
    choices.layers[1].route=u::PsdRoute::BasePixels;auto detached=u::convertPsd(chain,choices,job,{},false);
    CHECK(detached.document&&detached.document->tree().containers.empty());
    base.tags["clbl"]=QByteArray::fromHex("00000000");chain=inspect(f::file({base,clipped}));
    CHECK(!chain.layers[1].editable&&chain.layers[1].issues.join(' ').contains("disabled"));
    base.hidden=true;base.tags["clbl"]=QByteArray::fromHex("01000000");chain=inspect(f::file({base,clipped}));result=convert(chain);
    CHECK(result.document&&c::PinnedDocumentSampler(*result.document,{},c::ColorSampleSource::MergedVisible).sample({1.5,1.5}).alpha==0);
    f::Layer folder;folder.name="Folder";folder.tags["lsct"]=f::group(1,"pass");
    f::Layer boundary;boundary.tags["lsct"]=f::group(3);
    chain=inspect(f::file({base,boundary,clipped,folder}));
    CHECK(chain.error.isEmpty());
    const auto orphan=std::ranges::find_if(chain.layers,[](const auto& item){return item.name=="Clipped";});
    CHECK(orphan!=chain.layers.end()&&!orphan->editable&&orphan->clippingBase<0);
    CHECK(orphan!=chain.layers.end()&&orphan->issues.join(' ').contains("no base in this group"));
  }
  leaf.tags["iOpa"] = QByteArray(1, char(120));
  in = inspect(f::file({leaf}));
  CHECK(!in.layers[0].editable &&
        in.layers[0].issues.join(' ').contains("Fill opacity"));
  leaf.tags.clear();
  leaf.hidden = true;
  leaf.opacity = 83;
  in = inspect(f::file({leaf}));
  out = convert(in);
  CHECK(out.document && !out.document->layers()[0].visible &&
        out.document->layers()[0].opacity == float(83) / 255);
  leaf.hidden = false;
  leaf.opacity = 255;
  const QColorSpace source(QColorSpace::DisplayP3);
  in = inspect(f::file({leaf}, {96, 96}, 72, source.iccProfile()));
  CHECK(in.error.isEmpty());
  out = convert(in);
  if (out.document) {
    auto image = u::flattenDocument(*out.document);
    CHECK(image);
    QImage reference(1, 1, QImage::Format_RGBA8888);
    reference.fill(QColor(180, 85, 25));
    reference.setColorSpace(source);
    reference = reference.convertedToColorSpace(QColorSpace::SRgb);
    CHECK(image.image.pixelColor(0, 0) == reference.pixelColor(0, 0));
  }
  auto malformed = f::file({leaf});
  malformed[23] = char(16);
  in = inspect(malformed);
  CHECK(in.error.contains("8-bit"));
  malformed = f::file({leaf});
  malformed[5] = char(2);
  in = inspect(malformed);
  CHECK(in.error.contains("PSB"));
  auto limit = u::PsdLimits{};
  limit.rasterBytes = 1;
  in = inspect(f::file({leaf}));
  auto bounded = u::convertPsd(in, u::defaultPsdOptions(in), job, limit, false);
  CHECK(!bounded.document && bounded.error.contains("memory"));
  CHECK(bounded.rasterBytes == std::uint64_t(leaf.rect.width()) *
                                   std::uint64_t(leaf.rect.height()) * 4 &&
        bounded.estimatedWorkingBytes > 0);
  CHECK(bounded.error.contains("Caller-specified raster ceiling"));

  // Charge only selected retained representations, not all saved layer caches.
  auto retained = bounded.rasterBytes;
  in = inspect(f::file({leaf, leaf}));
  auto subset = u::defaultPsdOptions(in);
  limit.rasterBytes = retained;
  bounded = u::convertPsd(in, subset, job, limit, false);
  CHECK(!bounded.document && bounded.rasterBytes == retained * 2);
  subset.layers[1].route = u::PsdRoute::Skip;
  bounded = u::convertPsd(in, subset, job, limit, false);
  CHECK(bounded.document && bounded.document->layers().size() == 1 &&
        bounded.rasterBytes == retained);
  limit.rasterBytes = UINT64_MAX;
  limit.existingBytes = 1024 * 1024;
  limit.workingBytes = bounded.estimatedWorkingBytes + limit.existingBytes;
  CHECK(u::convertPsd(in, subset, job, limit, false).document);
  --limit.workingBytes;
  auto refused = u::convertPsd(in, subset, job, limit, false);
  CHECK(!refused.document && refused.error.contains("available import allowance"));

  f::Layer editableText;
  editableText.rect = {0, 0, 4096, 4096};
  editableText.pixels = false;
  editableText.tags["TySh"] = f::textTag("No retained display cache\r");
  in = inspect(f::file({editableText}));
  limit = {};
  limit.rasterBytes = 1;
  auto typed = u::convertPsd(in, u::defaultPsdOptions(in), job, limit, false);
  CHECK(typed.document && typed.rasterBytes == 0);
  in = inspect(f::file({leaf}));
  // The immutable inspection owns its data; deleting/replacing the input cannot
  // redirect an asynchronous conversion to a different revision or document.
  auto future =
      u::convertPsdAsync(in, u::defaultPsdOptions(in), job, {}, false);
  QFile::remove(dir.filePath("test.psd"));
  auto async = future.get();
  CHECK(async.document);
  auto *preparedDocument = async.document.get();
  const auto preparedSurface = std::get<c::RasterLayer>(async.document->layers()[0].payload).surface;
  CHECK(async.preview.isNull());
  auto previewJob = std::make_shared<u::PsdJob>();
  previewJob->cancelled = true;
  async = u::previewPsdImportAsync(std::move(async), previewJob).get();
  CHECK(async.cancelled && async.document.get() == preparedDocument && async.preview.isNull());
  previewJob = std::make_shared<u::PsdJob>();
  async = u::previewPsdImportAsync(std::move(async), previewJob).get();
  CHECK(!async.cancelled && async.error.isEmpty() && !async.preview.isNull());
  CHECK(async.document.get() == preparedDocument);
  CHECK(std::get<c::RasterLayer>(async.document->layers()[0].payload).surface == preparedSurface);
  const auto previewKey = async.preview.cacheKey();
  previewJob = std::make_shared<u::PsdJob>();
  async = u::previewPsdImportAsync(std::move(async), previewJob).get();
  CHECK(async.preview.cacheKey() == previewKey && !previewJob->renderingPreview);
  inspect(f::file({text, leaf}));
  u::PsdImportDialog dialog(dir.filePath("test.psd"), true, false, {});
  dialog.show();
  auto *submit = dialog.findChild<QPushButton *>("PsdImport");
  QElapsedTimer timer;
  timer.start();
  auto *tree = dialog.findChild<QTreeWidget *>("PsdLayers");
  while (!tree->topLevelItemCount() && timer.elapsed() < 10000)
    QTest::qWait(20);
  CHECK(!submit->isEnabled());
  auto *undecided = dialog.findChild<QComboBox *>("PsdImportAction");
  CHECK(undecided && undecided->currentData().toInt() == -1);
  if (undecided)
    undecided->setCurrentIndex(undecided->findData(int(u::PsdRoute::Skip)));
  timer.restart();
  while (!submit->isEnabled() && timer.elapsed() < 10000)
    QTest::qWait(20);
  CHECK(submit->isEnabled());
  CHECK(submit->text() == "Validate Import");
  QTest::qWait(200);
  CHECK(!dialog.takeResult().document); // Choices alone never construct a document.
  CHECK(tree && tree->topLevelItemCount() == 2);
  CHECK(tree->columnCount() == 2 && tree->findChildren<QComboBox *>().empty());
  CHECK(dialog.findChild<QCheckBox *>("PsdAttention")->isChecked());
  CHECK(dialog.findChild<QCheckBox *>("PsdHideGroups")->isChecked());
  dialog.findChild<QCheckBox *>("PsdAttention")->setChecked(true);
  CHECK(tree->topLevelItem(0)->isHidden());
  dialog.findChild<QCheckBox *>("PsdAttention")->setChecked(false);
  dialog.findChild<QComboBox *>("PsdMode")->setCurrentIndex(1);
  CHECK(!tree->isEnabled() && submit->isEnabled());
  auto *enablePreview = dialog.findChild<QCheckBox *>("PsdImportPreview");
  auto *preview = dialog.findChild<QLabel *>("PsdPreview");
  CHECK(enablePreview && enablePreview->isChecked());
  enablePreview->setChecked(false);
  QTest::qWait(200);
  CHECK(!dialog.takeResult().document);
  submit->click();
  CHECK(!submit->isEnabled());
  timer.restart();
  while (!dialog.findChild<QPushButton *>("PsdConfirmImport") && timer.elapsed() < 10000)
    QTest::qWait(20);
  auto *confirm = dialog.findChild<QPushButton *>("PsdConfirmImport");
  auto *overlay = dialog.findChild<QWidget *>("PsdConfirmationOverlay");
  CHECK(confirm && overlay && overlay->isVisible() && !overlay->isWindow());
  CHECK(overlay && overlay->parentWidget() == &dialog);
  CHECK(!submit->isEnabled());
  CHECK(preview->pixmap().isNull());
  dialog.reject(); // Escape returns to settings, not out of the import dialog.
  CHECK(dialog.isVisible() && overlay && !overlay->isVisible());
  CHECK(submit->isEnabled());
  // Enabling computes the missing image from already prepared content, without
  // another confirmation; toggling again reuses that image synchronously.
  enablePreview->setChecked(true);
  timer.restart();
  while ((!submit->isEnabled() || preview->pixmap().isNull()) && timer.elapsed() < 10000)
    QTest::qWait(20);
  CHECK(submit->isEnabled() && !preview->pixmap().isNull());
  CHECK(!dialog.findChild<QWidget *>("PsdConfirmationOverlay"));
  enablePreview->setChecked(false);
  CHECK(preview->pixmap().isNull() && submit->isEnabled());
  enablePreview->setChecked(true);
  CHECK(!preview->pixmap().isNull() && submit->isEnabled());
  // An unchanged validation reuses prepared pixels, immediately showing the card.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  submit->click();
  CHECK(dialog.findChild<QWidget *>("PsdConfirmationOverlay")->isVisible());
  dialog.findChild<QPushButton *>("PsdConfirmationCancel")->click();
  CHECK(dialog.isVisible());
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  dialog.findChild<QComboBox *>("PsdMode")->setCurrentIndex(0);
  CHECK(!dialog.takeResult().document); // A changed choice invalidates prepared data.
  submit->click();
  timer.restart();
  while (!dialog.findChild<QPushButton *>("PsdConfirmImport") && timer.elapsed() < 10000)
    QTest::qWait(20);
  confirm = dialog.findChild<QPushButton *>("PsdConfirmImport");
  CHECK(confirm);
  if (confirm) confirm->click();
  CHECK(dialog.result() == QDialog::Accepted);
  CHECK(dialog.takeResult().document);

  u::PsdImportDialog cancelled(dir.filePath("test.psd"), false, false, {});
  cancelled.show();
  auto *validate = cancelled.findChild<QPushButton *>("PsdImport");
  timer.restart();
  while (!cancelled.findChild<QTreeWidget *>("PsdLayers")->topLevelItemCount() && timer.elapsed() < 10000)
    QTest::qWait(20);
  cancelled.findChild<QComboBox *>("PsdMode")->setCurrentIndex(1);
  validate->click();
  cancelled.reject();
  QTest::qWait(200);
  CHECK(!cancelled.isVisible() && !cancelled.takeResult().document);
  CHECK(!cancelled.findChild<QWidget *>("PsdConfirmationOverlay"));
}
void sample(const QString &file, const QString &directory) {
  auto job = std::make_shared<u::PsdJob>();
  QElapsedTimer timer;
  timer.start();
  auto in = u::inspectPsd(file, job);
  std::cout << "Inspection ms: " << timer.elapsed() << "\n"
            << in.summary.toStdString() << "\n"
            << in.report.toStdString();
  CHECK(in.error.isEmpty());
  if (!in.error.isEmpty()) {
    std::cerr << in.error.toStdString();
    return;
  }
  auto options = u::defaultPsdOptions(in);
  auto output = u::convertPsd(in, options, job, {}, false);
  std::cout << output.report.toStdString()
            << "Conversion ms: " << output.milliseconds
            << " raster bytes: " << output.rasterBytes << "\n";
  CHECK(output.document);
  if (!output.document) {
    std::cerr << output.error.toStdString();
    return;
  }
  QDir().mkpath(directory);
  auto native = u::flattenDocument(*output.document);
  CHECK(native);
  if (native)
    CHECK(native.image.save(directory + "/native.png"));
  CHECK(u::saveProject(directory + "/converted.vulkana", *output.document));
  auto reopen = u::loadProject(directory + "/converted.vulkana");
  CHECK(reopen.document);
  if (reopen.document) {
    auto second = u::flattenDocument(*reopen.document);
    CHECK(second && native && second.image == native.image);
  }
  for (const auto &l : output.document->layers()) {
    std::cout << l.name << " kind " << l.payload.index() << " mask "
              << bool(l.mask) << "\n";
    if (auto *text = std::get_if<c::TextLayer>(&l.payload)) {
      std::cout << text->utf8 << "\n";
      for (auto &r : text->runs)
        std::cout << r.start << "+" << r.length << " " << r.style.font.family
                  << " / " << r.style.font.originalFace << " "
                  << r.style.sizePixels << "\n";
      c::TextStylePatch patch;
      patch.sizePixels = 17;
      auto edited = c::formatTextRange(
          *text, 0, std::min<size_t>(1, text->utf8.size()), patch);
      CHECK(output.document->setLayerText(l.id, edited));
      if (text->utf8.find(' ') != std::string::npos) {
        const auto start = text->utf8.find(' ') + 1;
        patch.sizePixels = 33;
        auto secondWord =
            c::formatTextRange(edited, start, edited.utf8.size(), patch);
        CHECK(output.document->setLayerText(l.id, secondWord));
      }
    } else if (auto *raster = std::get_if<c::RasterLayer>(&l.payload)) {
      const auto extent = raster->surface->extent();
      QImage raw(int(extent.width), int(extent.height),
                 QImage::Format_RGBA8888);
      raster->surface->copyRgba8(
          {0, 0, int(extent.width), int(extent.height)},
          std::span<std::byte>(reinterpret_cast<std::byte *>(raw.bits()),
                               size_t(raw.sizeInBytes())),
          size_t(raw.bytesPerLine()));
      CHECK(raw.save(directory +
                     QStringLiteral("/native-layer-%1.png")
                         .arg(&l - output.document->layers().data())));
    }
  }
  CHECK(u::saveProject(directory + "/edited.vulkana", *output.document));
  auto editedReopen = u::loadProject(directory + "/edited.vulkana");
  CHECK(editedReopen.document);
  if (editedReopen.document) {
    auto changed = u::flattenDocument(*editedReopen.document);
    CHECK(changed && native && changed.image != native.image);
  }
  for (size_t i = 0; i < in.layers.size(); ++i)
    if (in.layers[i].type == "Text" && in.layers[i].raster)
      options.layers[i].route = u::PsdRoute::Raster;
  auto raster = u::convertPsd(in, options, job, {}, false);
  CHECK(raster.document);
  if (raster.document) {
    auto flat = u::flattenDocument(*raster.document);
    CHECK(flat);
    if (flat)
      CHECK(flat.image.save(directory + "/saved-text.png"));
  }
  options.composite = true;
  auto composite = u::convertPsd(in, options, job, {}, false);
  CHECK(composite.document);
  if (composite.document) {
    auto flat = u::flattenDocument(*composite.document);
    CHECK(flat);
    if (flat)
      CHECK(flat.image.save(directory + "/compatibility.png"));
  }
}
void benchmark(const QString &path) {
  namespace f = psdfixture;
  f::Layer a;
  a.name = "4K raster";
  a.rect = {-128, -64, 4096, 3072};
  f::Layer b = a;
  b.name = "Second raster";
  b.rect = {1024, 768, 2048, 1536};
  f::Layer text;
  text.name = "Title";
  text.tags["TySh"] = f::textTag("Large document\r");
  {
    QFile output(path);
    CHECK(output.open(QIODevice::WriteOnly));
    auto data = f::file({a, b, text}, {4096, 3072}, 300);
    CHECK(output.write(data) == data.size());
  }
  auto job = std::make_shared<u::PsdJob>();
  QElapsedTimer timer;
  timer.start();
  auto in = u::inspectPsd(path, job);
  CHECK(in.error.isEmpty());
  auto inspection = timer.elapsed();
  timer.restart();
  auto out = u::convertPsd(in, u::defaultPsdOptions(in), job, {}, true);
  CHECK(out.document);
  std::cout << "4K inspection_ms " << inspection
            << " conversion_and_preview_ms " << timer.elapsed()
            << " retained_estimate " << out.rasterBytes << "\n";
}
void profile(const QString &path, bool chooseBasePixels = false, const QString &roundTripPath = {}) {
  auto job = std::make_shared<u::PsdJob>();
  QElapsedTimer timer;
  timer.start();
  auto in = u::inspectPsd(path, job);
  CHECK(in.error.isEmpty());
  const auto inspection = timer.elapsed();
  timer.restart();
  auto options = u::defaultPsdOptions(in);
  int lossyChoices = 0;
  if (chooseBasePixels)
    for (std::size_t i = 0; i < in.layers.size(); ++i)
      if (options.layers[i].route == u::PsdRoute::Skip && in.layers[i].basePixels) {
        options.layers[i].route = u::PsdRoute::BasePixels;
        ++lossyChoices;
      }
  auto out = u::convertPsd(in, options, job, {}, true);
  CHECK(out.document);
  std::cout << "inspection_ms " << inspection << " conversion_and_preview_ms "
            << timer.elapsed() << " preparation_ms " << out.preparationMilliseconds
            << " preview_ms " << out.previewMilliseconds
            << " preview_rgba_sha256 " << QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char*>(out.preview.constBits()),
                               out.preview.sizeInBytes()), QCryptographicHash::Sha256).toHex().constData()
            << " raster_estimate_bytes " << out.rasterBytes
            << " working_estimate_bytes " << out.estimatedWorkingBytes
            << " layers " << (out.document ? out.document->layers().size() : 0)
            << " containers " << (out.document ? out.document->tree().containers.size() : 0)
            << " explicit_lossy_base_choices " << lossyChoices
            << "\n";
  if (!out.error.isEmpty())
    std::cerr << out.error.toStdString() << '\n';
  if (!out.document || roundTripPath.isEmpty())
    return;
  // Reconstruct each original channel rectangle from the compact surface.
  // Zero padding must be the ONLY bytes discarded, including hidden RGB.
  std::size_t importedIndex=0,verified=0;
  std::uint64_t originalRasterBytes=0;
  for(std::size_t i=0;i<in.layers.size();++i) {
    if(in.layers[i].container||options.layers[i].route==u::PsdRoute::Skip)continue;
    const auto& layer=out.document->layers().at(importedIndex++);
    const auto* raster=std::get_if<c::RasterLayer>(&layer.payload);
    if(!raster)continue;
    auto raw=u::psd::pixels(*in.source,in.source->records[std::size_t(in.layers[i].sourceIndex)],job);
    if(raw.colorSpace().isValid())raw.convertToColorSpace(QColorSpace::SRgb);
    originalRasterBytes+=std::uint64_t(raw.width())*uint(raw.height())*4;
    const auto e=raster->surface->extent();std::vector<std::byte> stored(e.width*4),row(std::size_t(raw.width())*4);
    for(int y=0;y<raw.height();++y) {
      std::fill(row.begin(),row.end(),std::byte{});
      const int ry=y-int(layer.rasterOrigin.y);
      if(ry>=0&&ry<int(e.height)) {
        raster->surface->copyRgba8({0,ry,int(e.width),1},stored,e.width*4);
        std::copy(stored.begin(),stored.end(),row.begin()+std::ptrdiff_t(layer.rasterOrigin.x)*4);
      }
      CHECK(std::memcmp(row.data(),raw.constScanLine(y),row.size())==0);
    }
    ++verified;
  }
  std::cout<<"exact_original_raster_rectangles "<<verified<<" original_raster_bytes "<<originalRasterBytes<<'\n';
  // Optional large-file probe: hash in row chunks, release the original document
  // before reopening, then verify all raw bytes without another full-image copy.
  const auto hashes = [](const c::Document &doc) {
    std::vector<QByteArray> result;
    for (const auto &layer : doc.layers()) {
      const auto *raster = std::get_if<c::RasterLayer>(&layer.payload);
      CHECK(raster);
      if (!raster) continue;
      QCryptographicHash hash(QCryptographicHash::Sha256);
      const auto e = raster->surface->extent();
      const auto stride = size_t(e.width) * 4;
      std::vector<std::byte> row(stride);
      for (uint32_t y = 0; y < e.height; ++y) {
        raster->surface->copyRgba8({0, int32_t(y), int32_t(e.width), 1}, row, stride);
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(row.data()), qsizetype(row.size())));
      }
      result.push_back(hash.result());
    }
    return result;
  };
  const auto before = hashes(*out.document);
  const auto hierarchy = out.document->tree();
  const auto canvas = out.document->canvas();
  timer.restart();
  const auto saved = u::saveProject(roundTripPath, *out.document);
  CHECK(saved);
  const auto saveMs = timer.elapsed();
  if (!saved) {
    std::cerr << saved.error.toStdString() << '\n';
    return;
  }
  out.document.reset();
  timer.restart();
  auto reopened = u::loadProject(roundTripPath);
  CHECK(reopened);
  const auto loadMs = timer.elapsed();
  if (reopened) {
    CHECK(reopened.document->tree() == hierarchy);
    CHECK(reopened.document->canvas() == canvas);
    CHECK(hashes(*reopened.document) == before);
  } else
    std::cerr << reopened.error.toStdString() << '\n';
  std::cout << "save_ms " << saveMs << " reopen_ms " << loadMs
            << " archive_bytes " << QFileInfo(roundTripPath).size()
            << " exact_raster_hashes " << before.size() << '\n';
}
void compactBounds() {
  namespace f=psdfixture;
  QTemporaryDir dir;
  auto job=std::make_shared<u::PsdJob>();
  for(bool hiddenRgb:{false,true}) {
    f::Layer input;input.rect={-4,5,64,60};
    for(int channel:{-1,0,1,2}) input.channelData[channel]=QByteArray(64*60,0);
    for(int y=20;y<25;++y)for(int x=18;x<24;++x) {
      input.channelData[-1][y*64+x]=char(x==18?1:200);
      input.channelData[0][y*64+x]=char(220);
    }
    if(hiddenRgb)input.channelData[1][8*64+9]=char(80);
    input.maskRect={0,0,96,96};input.mask=QByteArray(96*96,char(128));
    const auto path=dir.filePath("compact.psd");
    QFile file(path);CHECK(file.open(QIODevice::WriteOnly));file.write(f::file({input}));file.close();
    auto inspected=u::inspectPsd(path,job);CHECK(inspected.error.isEmpty());
    auto converted=u::convertPsd(inspected,u::defaultPsdOptions(inspected),job,{},false);
    CHECK(converted.document);if(!converted.document)continue;
    auto& doc=*converted.document;
    auto* layer=doc.layer(doc.layers().front().id);
    CHECK(c::layerInteractionBounds(*layer)==c::RectD{18,20,6,5});
    CHECK(c::layerEffectReferenceFrame(*layer)==c::RectD{0,0,64,60});
    CHECK(layer->rasterOrigin==(hiddenRgb?c::Vec2d{8,7}:c::Vec2d{17,19}));
    const auto surface=std::get<c::RasterLayer>(layer->payload).surface;
    const auto e=surface->extent();
    CHECK(converted.rasterBytes==std::uint64_t(e.width)*e.height*4+96*96);
    // Reconstruct the original coordinate grid, including hidden RGB and alpha=1.
    std::vector<std::byte> row(e.width*4);
    for(int y=0;y<60;++y) {
      const int ry=y-int(layer->rasterOrigin.y);
      if(ry>=0&&ry<int(e.height))surface->copyRgba8({0,ry,int(e.width),1},row,e.width*4);
      for(int x=0;x<64;++x)for(int ch=0;ch<4;++ch) {
        const int rx=x-int(layer->rasterOrigin.x);
        const auto value=ry>=0&&ry<int(e.height)&&rx>=0&&rx<int(e.width)?row[std::size_t(rx)*4+ch]:std::byte{};
        CHECK(value==std::byte(quint8(input.channelData[ch==3?-1:ch][y*64+x])));
      }
    }
    const auto mask=layer->mask;
    const auto before=u::flattenDocument(doc);CHECK(before);
    c::Document reference(doc.canvas());
    auto baseline=*layer;
    auto full=u::rasterLayerFromImage(u::psd::pixels(*inspected.source,inspected.source->records[0],job),"Reference");
    baseline.payload=full.layer->payload;baseline.rasterOrigin={};baseline.rasterEffectFrame.reset();
    CHECK(reference.insertLayer(0,std::move(baseline)));
    const auto originalTransform=layer->localToDocument;
    // Compaction cannot move gradients or clip shadows, including when the
    // content is later transformed. Compare against the untrimmed source.
    auto effects=std::make_shared<c::LayerEffectStack>();
    effects->items[std::size_t(c::LayerEffectType::DropShadow)].enabled=true;
    effects->items[std::size_t(c::LayerEffectType::GradientOverlay)].enabled=true;
    for(bool styled:{false,true}) {
      doc.setLayerEffects(layer->id,styled?effects:nullptr);
      reference.setLayerEffects(layer->id,styled?effects:nullptr);
      for(const auto& transform:{originalTransform,c::AffineTransform{.6,-.2,28,.2,.6,12},c::AffineTransform{.1,0,20,0,.1,20}}) {
        doc.setLayerTransform(layer->id,transform);reference.setLayerTransform(layer->id,transform);
        const auto actual=u::flattenDocument(doc),expected=u::flattenDocument(reference);
        CHECK(actual&&expected&&actual.image==expected.image);
      }
    }
    doc.setLayerEffects(layer->id,{});doc.setLayerTransform(layer->id,originalTransform);
    c::History history;
    auto settings=c::proceduralBrushPreset(c::ProceduralBrushPreset::HardRound);settings.sizePixels=5;
    c::BasicPixelBrushStroke stroke(doc,layer->id,settings);
    c::NormalizedPointerSample pointer;pointer.documentPosition={80,80};pointer.pressure=1;pointer.buttons=c::PointerButtonPrimary;
    CHECK(stroke.begin(pointer));CHECK(stroke.end(pointer,history)==c::RasterEditCommitResult::Committed);
    CHECK(layer->mask==mask);CHECK(history.undo(doc));CHECK(std::get<c::RasterLayer>(layer->payload).surface==surface);
    CHECK(u::flattenDocument(doc).image==before.image);CHECK(history.redo(doc));
    const auto after=u::flattenDocument(doc);CHECK(after);
    const auto project=dir.filePath("compact.vulkana");CHECK(u::saveProject(project,doc));
    const auto reopened=u::loadProject(project);CHECK(reopened.document);
    if(reopened.document) {
      CHECK(u::flattenDocument(*reopened.document).image==after.image);
      CHECK(reopened.document->layers()[0].rasterOrigin==layer->rasterOrigin);
      CHECK(c::layerEffectReferenceFrame(reopened.document->layers()[0])==c::layerEffectReferenceFrame(*layer));
      CHECK(c::equivalentLayerMasks(reopened.document->layers()[0].mask,mask));
    }
  }
}
void mergedTransparency() {
  // Independent raw merged-image fixture: encoded RGB is white-matted, unlike
  // layer channels. Alpha 85 makes the expected unmatte exact, not a tolerance.
  u::PsdSource source;
  source.size = {3, 1}; source.channels = 4; source.depth = 8;
  source.mode = 3; source.compositeAlpha = true;
  source.colorSpace = QColorSpace(QColorSpace::SRgb);
  source.bytes = QByteArray::fromHex("0000c8ff14a0ff287fff3c55ff00");
  // RGB at pixel 0 after unmatting: (90, -30, -129) is clipped to (90,0,0).
  auto image = u::psd::composite(source, {});
  CHECK(image.pixelColor(0, 0) == QColor(90, 0, 0, 85));
  CHECK(image.pixelColor(1, 0) == QColor(255, 255, 255, 255));
  CHECK(image.pixelColor(2, 0).alpha() == 0);
  source.compositeAlpha = false;
  image = u::psd::composite(source, {});
  CHECK(image.pixelColor(0, 0) == QColor(200, 160, 127, 255));
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (argc >= 3 && (QString::fromLocal8Bit(argv[1]) == "--profile" ||
                   QString::fromLocal8Bit(argv[1]) == "--profile-base-pixels")) {
    profile(QString::fromLocal8Bit(argv[2]),
            QString::fromLocal8Bit(argv[1]) == "--profile-base-pixels",
            argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString{});
    return failures ? 1 : 0;
  }
  if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == "--benchmark") {
    benchmark(QString::fromLocal8Bit(argv[2]));
    return failures ? 1 : 0;
  }
  unit();
  mergedTransparency();
  semanticFixtures();
  compactBounds();
  if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == "--sample")
    sample(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
  std::cout << "PSD import failures: " << failures << "\n";
  return failures ? 1 : 0;
}
