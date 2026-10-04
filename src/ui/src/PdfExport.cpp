#include "imageeditor/ui/PdfExport.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QAbstractTextDocumentLayout>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QPdfOutputIntent>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <map>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <set>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
constexpr std::uint64_t sourceLimit = 512ULL * 1024 * 1024,
                        pagePixels = 32ULL * 1024 * 1024;
constexpr qint64 fileLimit = 1024LL * 1024 * 1024;
struct Cancelled {};
struct NativeTextUnavailable : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void check(const std::atomic_bool &cancel) {
  if (cancel.load())
    throw Cancelled{};
}
bool contains(std::span<const core::LayerId> ids, core::LayerId id) {
  return std::ranges::find(ids, id) != ids.end();
}
struct TextPiece {
  QGlyphRun glyphs;
  QFont requestedFont;
  QPointF origin;
  QColor color;
  QString text;
  int run;
};
std::vector<TextPiece> textPieces(QtTextLayout &layout) {
  std::vector<TextPiece> pieces;
  int runNumber = 0;
  for (auto block = layout.document().begin(); block.isValid();
       block = block.next()) {
    const auto *shaped = block.layout();
    for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
      const auto format = fragment.fragment();
      if (!format.isValid())
        continue;
      const auto first = format.position() - block.position(),
                 last = first + format.length();
      const auto runs =
          shaped->glyphRuns(first, format.length(), QTextLayout::RetrieveAll);
      std::set<qsizetype> boundaries{last}, used;
      for (const auto &run : runs)
        for (auto index : run.stringIndexes())
          boundaries.insert(index);
      for (const auto &run : runs) {
        ++runNumber;
        require(!run.flags().testFlag(QGlyphRun::SplitLigature),
                "Split-ligature formatting needs raster text");
        const auto indexes = run.stringIndexes();
        const auto glyphs = run.glyphIndexes();
        const auto positions = run.positions();
        require(indexes.size() == glyphs.size() &&
                    positions.size() == glyphs.size(),
                "Font has no reliable glyph-to-text clusters");
        std::map<qsizetype, std::vector<qsizetype>> clusters;
        for (qsizetype i = 0; i < indexes.size(); ++i)
          clusters[indexes[i]].push_back(i);
        for (auto it = clusters.begin(); it != clusters.end(); ++it) {
          require(it->first >= first && it->first < last,
                  "Invalid shaped cluster index");
          const auto end = *boundaries.upper_bound(it->first);
          require(it->first >= first && end > it->first && end <= last,
                  "Invalid shaped text cluster");
          require(used.insert(it->first).second,
                  "Multi-font glyph cluster needs raster text");
          TextPiece piece;
          piece.run = runNumber;
          piece.glyphs = run;
          piece.requestedFont = format.charFormat().font();
          piece.origin = shaped->position();
          piece.color = format.charFormat().foreground().color();
          piece.text = block.text().mid(it->first, end - it->first);
          require(!piece.text.isEmpty(),
                  "Missing source text for shaped cluster");
          QList<quint32> g;
          QList<QPointF> p;
          for (auto i : it->second) {
            g.push_back(glyphs[i]);
            p.push_back(positions[i]);
          }
          piece.glyphs.setGlyphIndexes(g);
          piece.glyphs.setPositions(p);
          pieces.push_back(std::move(piece));
        }
      }
    }
  }
  return pieces;
}
core::Document pageDocument(const PdfExportSnapshot &snapshot,
                            std::span<const core::LayerId> ids) {
  core::Document result(snapshot.document->canvas());
  for (const auto &source : snapshot.document->layers())
    if (contains(ids, source.id)) {
      core::Layer layer;
      layer = source;
      layer.visible = true;
      require(result.insertLayer(result.layers().size(), std::move(layer)),
              "Invalid PDF export layer");
    }
  // Retain structural clipping, including hidden bases (as hidden leaves).
  // A filtered-out base must never promote one of its upper members.
  auto tree=snapshot.document->tree();
  for(const auto& source:snapshot.document->layers())if(!contains(ids,source.id)) {
    bool requiredBase=false;
    for(const auto& c:tree.containers)if(c.kind==core::ContainerKind::ClippingMaskGroup && !c.children.empty()
        && (c.children.front()==source.id || tree.isAncestor(c.children.front(),source.id)))
      for(auto id:ids)if(tree.isAncestor(c.id,id))requiredBase=true;
    const auto* adjustment=std::get_if<core::AdjustmentLayer>(&source.payload);
    const bool scopeBoundary=adjustment && adjustment->scope==core::AdjustmentScope::ThisGroup;
    if(requiredBase || scopeBoundary) {auto hidden=source;hidden.visible=false;require(result.insertLayer(result.layers().size(),std::move(hidden)),"Invalid hidden composition member");}
    else if(auto p=tree.placement(source.id))std::erase(*tree.children(p->parent),source.id);
  }
  for(auto& c:tree.containers)c.visible=true;
  if(result.tree()!=tree)require(result.replaceStructure(result.tree(),std::move(tree)),"Invalid PDF clipping hierarchy");
  return result;
}
std::vector<core::LayerId> chosenEntries(const PdfExportSnapshot &source,
                                         const PdfExportOptions &options) {
  std::vector<core::LayerId> result;
  for (const auto &entry : source.entries) {
    const auto &selection = options.selection == PdfExportSelection::Current
                                ? source.current
                                : options.chosen;
    const bool chosen = options.selection == PdfExportSelection::All ||
                        contains(selection, entry.id);
    if (chosen && (!options.ignoreHidden || entry.visible))
      result.push_back(entry.id);
  }
  return result;
}
QString nativeTextReason(const core::Layer &layer, const core::RectI &page) {
  if (!layer.localToDocument.isAffine())
    return QStringLiteral("Projective text transform");
  if (layer.mask && layer.mask->enabled)
    return QStringLiteral("Text layer mask");
  if (layer.crop)
    return QStringLiteral("Text crop/chamfer");
  if (core::compileAdjustmentStack(layer.adjustments).active ||
      core::hasActiveSpatialFilters(layer.filters) ||
      core::hasActiveLayerEffects(layer.effects))
    return QStringLiteral("Text adjustments, filters or effects");
  if (layer.opacity != 1)
    return QStringLiteral(
        "Translucent text needs linear-light raster composition");
  const auto &text = std::get<core::TextLayer>(layer.payload);
  if (text.defaultStyle.color.alpha != 255 ||
      std::ranges::any_of(
          text.runs, [](const auto &r) { return r.style.color.alpha != 255; }))
    return QStringLiteral(
        "Translucent text color needs linear-light raster composition");
  QtTextLayout layout(text);
  try {
    const auto pieces = textPieces(layout);
    if (pieces.empty())
      return QStringLiteral("No painted text glyphs");
    for (const auto &piece : pieces) {
      const auto os2 = piece.glyphs.rawFont().fontTable("OS/2");
      if (os2.size() >= 6 && piece.requestedFont.weight() >= 600 &&
          ((unsigned(std::uint8_t(os2[4])) << 8) | std::uint8_t(os2[5])) < 600)
        return QStringLiteral(
            "Synthetic bold or a non-default font variation needs raster text");
    }
  } catch (const std::exception &e) {
    return QString::fromUtf8(e.what());
  }
  const auto bounds = layout.documentBounds(layer.localToDocument);
  if (bounds.x < page.x || bounds.y < page.y ||
      bounds.right() > double(page.x) + page.width ||
      bounds.bottom() > double(page.y) + page.height)
    return QStringLiteral("Text clipped by the page boundary");
  for (auto block = layout.document().begin(); block.isValid();
       block = block.next()) {
    for (const auto &run : block.layout()->glyphRuns()) {
      const auto font = run.rawFont();
      const auto os2 = font.fontTable("OS/2");
      if (!font.isValid() || os2.size() < 10 ||
          font.fontTable("head").isEmpty())
        return QStringLiteral("Font embedding information unavailable");
      const auto flags =
          (unsigned(std::uint8_t(os2[8])) << 8) | std::uint8_t(os2[9]);
      if (flags & (0x0002 | 0x0100 | 0x0200))
        return QStringLiteral(
            "Font disallows embedding, subsetting or outline embedding");
      const auto faceWeight =
          (unsigned(std::uint8_t(os2[4])) << 8) | std::uint8_t(os2[5]);
      if (font.weight() >= 600 && faceWeight < 600)
        return QStringLiteral(
            "Synthetic bold or a non-default font variation needs raster text");
      if (font.style() != QFont::StyleNormal && os2.size() >= 64) {
        const auto selection =
            (unsigned(std::uint8_t(os2[62])) << 8) | std::uint8_t(os2[63]);
        if (!(selection & 0x0201))
          return QStringLiteral("Synthetic italic needs raster text");
      }
      if (!font.fontTable("COLR").isEmpty() ||
          !font.fontTable("CBDT").isEmpty() ||
          !font.fontTable("sbix").isEmpty())
        return QStringLiteral("Color/bitmap font");
      const auto glyphs = run.glyphIndexes();
      if (std::ranges::find(glyphs, quint32(0)) != glyphs.end())
        return QStringLiteral("Unresolved glyph");
    }
  }
  return {};
}
void classifyText(const PdfExportSnapshot &source, PdfExportPlan &plan,
                  PdfExportPage &page) {
  bool sawNative = false, requiresCombined = false;
  for(const auto& c:source.document->tree().containers)
    if(c.kind==core::ContainerKind::ClippingMaskGroup && c.children.size()>1)
      for(auto id:page.leaves)if(source.document->tree().isAncestor(c.id,id)) {
        requiresCombined=true;
        page.textReasons.append(QStringLiteral("Clipping group requires canonical raster composition"));
        break;
      }
  for (auto id : page.leaves) {
    const auto &layer = *source.document->layer(id);
    if(std::holds_alternative<core::AdjustmentLayer>(layer.payload)&&core::compileAdjustmentStack(layer.adjustments).active) {
      requiresCombined=true;
      page.textReasons.append(QStringLiteral("Adjustment layers require canonical page composition"));
    }
    const bool text = std::holds_alternative<core::TextLayer>(layer.payload);
    if (layer.blendMode != core::BlendMode::Normal ||
        core::hasActiveLayerEffects(layer.effects))
      requiresCombined = true;
    QString reason;
    if (text) {
      if (plan.options.text == PdfExportText::Rasterize)
        reason = QStringLiteral("Rasterize all text selected");
      else if (plan.options.matte)
        reason = QStringLiteral("Final linear-light matte composition");
      else
        reason = nativeTextReason(layer, page.rect);
      if (reason.isEmpty()) {
        page.preservedText.push_back(id);
        sawNative = true;
      } else
        page.textReasons.append(QString::fromStdString(layer.name) +
                                QStringLiteral(": ") + reason);
    }
    // A raster span over native text would blend in the PDF viewer's space,
    // not Vulkana's linear space. Preserve the canonical interacting page.
    if (sawNative && !contains(page.preservedText, id))
      requiresCombined = true;
  }
  if (requiresCombined && !page.preservedText.empty()) {
    page.textReasons.append(
        QStringLiteral("Interacting blend/styles or raster content above text "
                       "require a combined raster page"));
    page.preservedText.clear();
  }
  plan.preserved += int(page.preservedText.size());
  for (auto id : page.leaves)
    if (std::holds_alternative<core::TextLayer>(
            source.document->layer(id)->payload) &&
        !contains(page.preservedText, id))
      ++plan.rasterized;
}
FlattenedDocumentResult render(const PdfExportSnapshot &source,
                               const PdfExportPage &page,
                               std::span<const core::LayerId> leaves,
                               const PdfExportOptions &options,
                               const std::atomic_bool &cancel, QSize size) {
  auto document = pageDocument(source, leaves);
  std::optional<core::Rgba8> matte;
  if (options.matte)
    matte = core::Rgba8{std::uint8_t(options.matteColor.red()),
                        std::uint8_t(options.matteColor.green()),
                        std::uint8_t(options.matteColor.blue()), 255};
  FlattenedDocumentLimits limits;
  limits.outputPixels = pagePixels;
  auto result = flattenDocumentRegion(
      document, page.rect,
      {std::uint32_t(size.width()), std::uint32_t(size.height())},
      [&](auto, auto) { return !cancel.load(); }, limits, matte);
  if (result)
    result.image.setColorSpace(QColorSpace::SRgb);
  return result;
}
QTransform transform(const core::AffineTransform &t) {
  return {t.m00, t.m10, t.m20, t.m01, t.m11, t.m21, t.m02, t.m12, t.m22};
}
using Object = QPDFObjectHandle;
Object dict(std::map<std::string, Object> values) {
  return Object::newDictionary(values);
}
bool embeddedTextFonts(Object resources) {
  auto fonts = resources.getKey("/Font");
  if (!fonts.isDictionary() || fonts.getKeys().empty())
    return false;
  for (const auto &key : fonts.getKeys()) {
    auto font = fonts.getKey(key);
    if (!font.getKey("/ToUnicode").isStream())
      return false;
    if (font.getKey("/Subtype").isNameAndEquals("/Type0"))
      font = font.getKey("/DescendantFonts").getArrayItem(0);
    auto descriptor = font.getKey("/FontDescriptor");
    if (!descriptor.getKey("/FontFile").isStream() &&
        !descriptor.getKey("/FontFile2").isStream() &&
        !descriptor.getKey("/FontFile3").isStream())
      return false;
  }
  return true;
}
// Qt subsets the correct resolved font, but reverse-cmap lookup cannot recover
// Unicode after shaping (ligatures, contextual Arabic forms, fallback
// clusters). Give each (subset glyph, source cluster) pair a CID, keeping its
// original outline/advance. Auxiliary outlines of that cluster are emitted
// separately. This repairs real PDF text; there is no invisible or duplicate
// text layer.
class TextFontMapping {
public:
  explicit TextFontMapping(Object font)
      : font_(font),
        descendant_(font.getKey("/DescendantFonts").getArrayItem(0)) {
    if (!font.getKey("/Encoding").isNameAndEquals("/Identity-H") ||
        !descendant_.getKey("/Subtype").isNameAndEquals("/CIDFontType2"))
      throw NativeTextUnavailable(
          "PDF font encoding cannot retain shaped Unicode");
    auto map = descendant_.getKey("/CIDToGIDMap");
    if (map.isStream())
      glyphMap_ = map.getStreamData();
    else if (!map.isNameAndEquals("/Identity"))
      throw NativeTextUnavailable("Unknown PDF font glyph mapping");
    auto dw = descendant_.getKey("/DW");
    defaultWidth_ = dw.isNumber() ? dw.getNumericValue() : 1000.;
    auto widths = descendant_.getKey("/W");
    for (int i = 0; i < widths.getArrayNItems();) {
      const int first = widths.getArrayItem(i++).getIntValueAsInt();
      auto values = widths.getArrayItem(i++);
      if (values.isArray()) {
        for (int j = 0; j < values.getArrayNItems(); ++j)
          widths_[first + j] = values.getArrayItem(j).getNumericValue();
      } else {
        const int end = values.getIntValueAsInt();
        const auto width = widths.getArrayItem(i++).getNumericValue();
        if (end - first > 65535)
          throw NativeTextUnavailable("Invalid PDF font widths");
        for (int j = first; j <= end; ++j)
          widths_[j] = width;
      }
    }
  }
  int add(int cid, const QString &text) {
    const auto key = std::make_pair(cid, text);
    const auto found = ids_.find(key);
    if (found != ids_.end())
      return found->second;
    if (items_.size() >= 65534)
      throw NativeTextUnavailable("Too many distinct shaped text mappings");
    int gid = cid;
    if (glyphMap_) {
      const auto pos = std::size_t(cid) * 2;
      if (pos + 1 >= glyphMap_->getSize())
        throw NativeTextUnavailable("Missing embedded font glyph");
      gid =
          (glyphMap_->getBuffer()[pos] << 8) | glyphMap_->getBuffer()[pos + 1];
    }
    items_.push_back(
        {gid, widths_.contains(cid) ? widths_.at(cid) : defaultWidth_, text});
    const int id = int(items_.size());
    ids_[key] = id;
    return id;
  }
  Object finish(QPDF &output) {
    std::string glyphs(2 * (items_.size() + 1), '\0');
    std::vector<Object> widths{Object::newInteger(0)};
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict "
        "begin\nbegincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) "
        "/Supplement 0 >> def\n/CMapName /VulkanaUnicode def\n/CMapType 2 "
        "def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    for (std::size_t i = 0; i < items_.size(); ++i) {
      const auto &item = items_[i];
      glyphs[(i + 1) * 2] = char(item.gid >> 8);
      glyphs[(i + 1) * 2 + 1] = char(item.gid & 255);
      widths.push_back(Object::newReal(item.width, 6));
      if (i % 100 == 0)
        cmap += std::to_string(std::min<std::size_t>(100, items_.size() - i)) +
                " beginbfchar\n";
      cmap += "<" + hex(int(i + 1)) + "> <";
      for (auto unit : item.text)
        cmap += hex(unit.unicode());
      cmap += ">\n";
      if (i % 100 == 99 || i + 1 == items_.size())
        cmap += "endbfchar\n";
    }
    cmap +=
        "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    auto descendant = Object::newDictionary(descendant_.getDictAsMap());
    descendant.replaceKey("/CIDToGIDMap", output.newStream(glyphs));
    descendant.replaceKey(
        "/W", Object::newArray(std::vector<Object>{Object::newInteger(0),
                                                   Object::newArray(widths)}));
    auto font = Object::newDictionary(font_.getDictAsMap());
    font.replaceKey("/DescendantFonts",
                    Object::newArray(std::vector<Object>{
                        output.makeIndirectObject(descendant)}));
    font.replaceKey("/ToUnicode", output.newStream(cmap));
    return output.makeIndirectObject(font);
  }
  static std::string hex(int value) {
    return QStringLiteral("%1")
        .arg(value, 4, 16, QLatin1Char('0'))
        .toStdString();
  }

private:
  struct Item {
    int gid;
    double width;
    QString text;
  };
  Object font_, descendant_;
  std::shared_ptr<Buffer> glyphMap_;
  double defaultWidth_{};
  std::map<int, double> widths_;
  std::map<std::pair<int, QString>, int> ids_;
  std::vector<Item> items_;
};
std::string number(double v) {
  return QByteArray::number(v, 'f', 9).toStdString();
}
class SavePipeline final : public Pipeline {
public:
  SavePipeline(QSaveFile &file, const std::atomic_bool &cancel)
      : Pipeline("Vulkana atomic PDF", nullptr), file_(file), cancel_(cancel) {}
  void write(unsigned char const *data, size_t size) override {
    check(cancel_);
    require(size <= std::size_t(fileLimit - file_.pos()),
            "PDF exceeds the 1 GiB output limit");
    require(file_.write(reinterpret_cast<const char *>(data), qint64(size)) ==
                qint64(size),
            "Could not write the PDF destination");
  }
  void finish() override { check(cancel_); }

private:
  QSaveFile &file_;
  const std::atomic_bool &cancel_;
};
} // namespace
PdfExportSnapshot capturePdfExport(const core::Document &document,
                                   std::uint64_t instanceId,
                                   std::span<const core::LayerId> selection,
                                   PdfExportLimits limits) {
  PdfExportSnapshot result;
  result.limits = limits;
  require(limits.existingBytes <= limits.workingBytes,
          "Open documents already exceed the PDF working budget. Close other "
          "documents first");
  result.instanceId = instanceId;
  result.revision = document.revision();
  require(document.layers().size() <= 1024,
          "PDF export exceeds the 1024-layer limit");
  std::map<core::SurfaceId, std::shared_ptr<core::RasterSurface>> frozen;
  std::uint64_t metadata = 0;
  for (const auto &layer : document.layers()) {
    metadata += sizeof(layer) + layer.name.size();
    if (const auto *text = std::get_if<core::TextLayer>(&layer.payload))
      metadata += core::textMemoryCost(*text);
    if (const auto *shape = std::get_if<core::ShapeLayer>(&layer.payload))
      metadata += core::shapeMemoryCost(*shape);
    if (const auto *raster = std::get_if<core::RasterLayer>(&layer.payload)) {
      require(bool(raster->surface), "PDF source has missing raster data");
      if (frozen.emplace(raster->surface->id(), nullptr).second) {
        const auto size = raster->surface->extent();
        result.sourceBytes += std::uint64_t(size.width) * size.height * 4;
      }
    }
  }
  require(metadata <= 16ULL * 1024 * 1024,
          "PDF export exceeds the text/geometry metadata budget");
  require(result.sourceBytes + metadata + 320ULL * 1024 * 1024 <=
              limits.workingBytes - limits.existingBytes,
          "Not enough working memory for a PDF snapshot. Close other documents "
          "first");
  require(result.sourceBytes <= sourceLimit,
          "PDF export exceeds the 512 MiB frozen-source budget");
  auto copy = std::make_shared<core::Document>(document.canvas());
  for (const auto &original : document.layers()) {
    core::Layer frozenLayer;
    frozenLayer = original;
    auto *layer = &frozenLayer;
    layer->renderCache.reset();
    layer->filterCache.reset();
    layer->effectCache.reset();
    if (auto *raster = std::get_if<core::RasterLayer>(&layer->payload)) {
      auto &pixels = frozen.at(raster->surface->id());
      if (!pixels) {
        const auto extent = raster->surface->extent();
        const auto stride = std::size_t(extent.width) * 4;
        std::vector<std::byte> bytes(stride * extent.height);
        raster->surface->copyRgba8(
            {0, 0, int(extent.width), int(extent.height)}, bytes, stride);
        pixels = std::make_shared<core::ContiguousRasterSurface>(
            extent, std::move(bytes));
      }
      raster->surface = pixels;
    }
    require(copy->insertLayer(copy->layers().size(), std::move(frozenLayer)),
            "Cannot stage PDF source layer");
  }
  if (copy->tree() != document.tree())
    require(copy->replaceStructure(copy->tree(), document.tree()),
            "Cannot stage PDF source hierarchy");
  result.document = std::move(copy);
  const auto visit = [&](auto &&self, const std::vector<core::LayerId> &items,
                         const QString &folder) -> void {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      const auto *container = document.tree().container(*it);
      if(!container&&std::holds_alternative<core::AdjustmentLayer>(document.layer(*it)->payload))continue;
      const auto name = QString::fromStdString(
          container ? container->name : document.layer(*it)->name);
      if (container && container->kind == core::ContainerKind::Folder) {
        self(self, container->children,
             folder.isEmpty() ? name : folder + QStringLiteral(" / ") + name);
        continue;
      }
      const std::array root{*it};
      const auto leaves = document.expandedLayers(root);
      // Empty groups are intentional entries with a previewed blank page.
      result.entries.push_back({int(result.entries.size() + 1), *it, name,
                                folder, document.isEffectivelyVisible(*it),
                                leaves});
      if (std::ranges::any_of(selection, [&](auto selected) {
            return selected == *it ||
                   document.tree().isAncestor(selected, *it) ||
                   (container && document.tree().isAncestor(*it, selected));
          }))
        result.current.push_back(*it);
    }
  };
  visit(visit, document.tree().roots, {});
  return result;
}
PdfExportPlan planPdfExport(const PdfExportSnapshot &source,
                            PdfExportOptions options,
                            const std::atomic_bool &cancel) {
  PdfExportPlan plan;
  plan.instanceId = source.instanceId;
  plan.revision = source.revision;
  try {
    check(cancel);
    require(bool(source.document), "Missing PDF export snapshot");
    require((options.mode == PdfExportMode::Composite ||
             options.mode == PdfExportMode::Pages) &&
                (options.selection == PdfExportSelection::All ||
                 options.selection == PdfExportSelection::Current ||
                 options.selection == PdfExportSelection::Custom) &&
                (options.pageSize == PdfExportPageSize::Canvas ||
                 options.pageSize == PdfExportPageSize::Fit) &&
                (options.text == PdfExportText::Preserve ||
                 options.text == PdfExportText::Rasterize),
            "Invalid PDF export option");
    require(options.instanceId == 0 || options.instanceId == source.instanceId,
            "PDF selection belongs to a different document");
    require(options.matteColor.isValid(), "Invalid PDF matte color");
    options.instanceId = source.instanceId;
    const auto dpi = source.document->canvas().dotsPerInch;
    require(std::isfinite(dpi) && dpi > 0, "Invalid document PPI");
    if (options.ppi == 0)
      options.ppi = dpi;
    require(std::isfinite(options.ppi) && options.ppi >= 1 &&
                options.ppi <= 2400,
            "PDF rendering PPI must be between 1 and 2400");
    plan.options = options;
    if (options.selection == PdfExportSelection::Custom)
      for (auto id : options.chosen)
        require(std::ranges::any_of(source.entries,
                                    [&](const auto &e) { return e.id == id; }),
                "An export item no longer exists");
    const auto chosen = chosenEntries(source, options);
    require(!chosen.empty(), "No chosen eligible entries. Select items or turn "
                             "off Ignore Hidden Layers");
    if (options.mode == PdfExportMode::Composite) {
      PdfExportPage page;
      page.entries = chosen;
      page.name = QStringLiteral("Composite");
      plan.pages.push_back(std::move(page));
    } else
      for (auto id : chosen) {
        const auto &entry = *std::ranges::find_if(
            source.entries, [&](const auto &e) { return e.id == id; });
        PdfExportPage page;
        page.entries = {id};
        page.name = entry.name;
        plan.pages.push_back(std::move(page));
      }
    if (options.mode == PdfExportMode::Pages && options.reverse)
      std::ranges::reverse(plan.pages);
    for (auto &page : plan.pages) {
      check(cancel);
      std::set<core::LayerId> leaves;
      for (const auto &entry : source.entries)
        if (contains(page.entries, entry.id))
          for (auto id : entry.leaves)
            if (!options.ignoreHidden ||
                source.document->isEffectivelyVisible(id))
              leaves.insert(id);
      // Stack operators accompany each page plan, never produce their own
      // blank page. Their original order/domain restricts them to included
      // lower content; no unselected drawable pixels are pulled in.
      for(const auto& layer:source.document->layers())
        if(std::holds_alternative<core::AdjustmentLayer>(layer.payload)
            &&(!options.ignoreHidden||source.document->isEffectivelyVisible(layer.id)))leaves.insert(layer.id);
      for (const auto &layer : source.document->layers())
        if (leaves.contains(layer.id))
          page.leaves.push_back(layer.id);
      const auto canvas = source.document->canvas().extent;
      page.rect = {0, 0, int(canvas.width), int(canvas.height)};
      if (options.mode == PdfExportMode::Pages &&
          options.pageSize == PdfExportPageSize::Fit) {
        auto document = pageDocument(source, page.leaves);
        EvaluatedDocumentBounds bounds;
        if (!page.leaves.empty())
          bounds =
              evaluatedLayerItemsBounds(document, page.leaves, [&](auto, auto) {
                return !cancel.load();
              });
        if (bounds.cancelled)
          throw Cancelled{};
        if (!bounds.error.isEmpty())
          throw std::runtime_error(bounds.error.toStdString());
        if (bounds.rect.empty())
          page.blankFallback = true;
        else
          page.rect = bounds.rect;
      }
      page.points =
          QSizeF(page.rect.width * 72.0 / dpi, page.rect.height * 72.0 / dpi);
      require(page.points.width() >= 0.01 && page.points.height() >= 0.01 &&
                  page.points.width() <= 14400 && page.points.height() <= 14400,
              "PDF physical page size must be between 0.01 and 14400 points "
              "per side");
      const double w =
          std::max(1.0, std::floor(page.rect.width * options.ppi / dpi + 0.5));
      const double h =
          std::max(1.0, std::floor(page.rect.height * options.ppi / dpi + 0.5));
      require(w <= 32768 && h <= 32768 && w * h <= double(pagePixels),
              "PDF page exceeds 32768 pixels or 32 megapixels. Lower PPI");
      require(double(std::max(double(page.rect.width), w)) *
                      std::max(double(page.rect.height), h) <=
                  128.0 * 1024 * 1024,
              "PDF reconstruction region exceeds the working budget");
      page.pixels = {int(w), int(h)};
      const auto bytes = std::uint64_t(w * h) * 4;
      plan.rasterBytes += bytes;
      plan.peakWorkingBytes = std::max<std::uint64_t>(
          plan.peakWorkingBytes, source.limits.existingBytes +
                                     source.sourceBytes + bytes * 5 +
                                     320ULL * 1024 * 1024);
      require(plan.peakWorkingBytes <= source.limits.workingBytes,
              "PDF exceeds the working memory budget. Lower PPI or close other "
              "documents");
      classifyText(source, plan, page);
    }
  } catch (const Cancelled &) {
    plan.cancelled = true;
    plan.pages.clear();
  } catch (const std::exception &e) {
    plan.error = QString::fromUtf8(e.what());
    plan.pages.clear();
  }
  return plan;
}
FlattenedDocumentResult renderPdfExportPage(const PdfExportSnapshot &source,
                                            const PdfExportPlan &plan,
                                            std::size_t page,
                                            const std::atomic_bool &cancel,
                                            std::optional<QSize> previewSize) {
  if (!plan || page >= plan.pages.size() ||
      source.instanceId != plan.instanceId || source.revision != plan.revision)
    return {{}, QStringLiteral("Invalid or stale PDF page plan"), false, {}};
  const auto &p = plan.pages[page];
  return render(source, p, p.leaves, plan.options, cancel,
                previewSize.value_or(p.pixels));
}
PdfExportResult writePdfExport(const PdfExportSnapshot &source,
                               const PdfExportPlan &plan,
                               const QString &destination,
                               const std::atomic_bool &cancel,
                               PdfExportProgress progress) {
  QElapsedTimer clock;
  clock.start();
  std::vector<qint64> pageTimes;
  std::size_t failedTextPage = plan.pages.size();
  try {
    check(cancel);
    require(bool(plan) && source.instanceId == plan.instanceId &&
                source.revision == plan.revision,
            "Invalid or stale PDF plan");
    require(
        QFileInfo(destination).suffix().compare("pdf", Qt::CaseInsensitive) ==
            0,
        "Choose a .pdf destination");
    QTemporaryFile staged;
    require(staged.open(), "Cannot create PDF staging file");
    struct Part {
      int page;
      core::LayerId text;
      double height;
      std::vector<QStringList> clusters;
    };
    std::vector<Part> parts;
    {
      QPdfWriter writer(&staged);
      writer.setResolution(72);
      writer.setPdfVersion(QPagedPaintDevice::PdfVersion_1_6);
      writer.setCreator(QStringLiteral("Vulkana"));
      writer.setAuthor({});
      writer.setTitle({});
      writer.setColorModel(QPdfWriter::ColorModel::RGB);
      QPdfOutputIntent intent;
      intent.setOutputProfile(QColorSpace(QColorSpace::SRgb));
      intent.setOutputConditionIdentifier(QStringLiteral("sRGB IEC61966-2.1"));
      writer.setOutputIntent(intent);
      QPainter painter;
      bool started = false;
      const auto startPart = [&](int index, core::LayerId text) {
        check(cancel);
        const auto &page = plan.pages[std::size_t(index)];
        // Qt's page matrix uses integer points. Enclose the exact page
        // here; qpdf applies the fractional boundary and origin below.
        const QSizeF extent(std::ceil(page.points.width()),
                            std::ceil(page.points.height()));
        QPageLayout layout(
            QPageSize(extent, QPageSize::Point, {}, QPageSize::ExactMatch),
            QPageLayout::Portrait, {}, QPageLayout::Point);
        layout.setMode(QPageLayout::FullPageMode);
        require(writer.setPageLayout(layout), "Cannot set PDF page layout");
        if (!started) {
          require(painter.begin(&writer), "Cannot start PDF writer");
          started = true;
        } else
          require(writer.newPage(), "Cannot create PDF page");
        painter.resetTransform();
        painter.setRenderHint(QPainter::LosslessImageRendering);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        parts.push_back({index, text, extent.height(), {}});
      };
      for (std::size_t i = 0; i < plan.pages.size(); ++i) {
        QElapsedTimer pageTime;
        pageTime.start();
        if (progress && !progress(int(i + 1), int(plan.pages.size()),
                                  QStringLiteral("Rendering")))
          throw Cancelled{};
        const auto &page = plan.pages[i];
        std::vector<core::LayerId> raster;
        const auto drawRaster = [&] {
          auto rendered =
              render(source, page, raster, plan.options, cancel, page.pixels);
          if (rendered.cancelled)
            throw Cancelled{};
          if (!rendered)
            throw std::runtime_error(rendered.error.toStdString());
          startPart(int(i), 0);
          painter.drawImage(QRectF(QPointF{}, page.points), rendered.image);
          require(staged.error() == QFileDevice::NoError &&
                      staged.size() <= fileLimit,
                  "PDF staging write failed or exceeded 1 GiB");
          raster.clear();
        };
        for (auto id : page.leaves) {
          if (!contains(page.preservedText, id)) {
            raster.push_back(id);
            continue;
          }
          if (!raster.empty())
            drawRaster();
          startPart(int(i), id);
          const auto &layer = *source.document->layer(id);
          QtTextLayout layout(std::get<core::TextLayer>(layer.payload));
          painter.scale(72.0 / source.document->canvas().dotsPerInch,
                        72.0 / source.document->canvas().dotsPerInch);
          painter.translate(-page.rect.x, -page.rect.y);
          painter.setWorldTransform(transform(layer.localToDocument), true);
          const auto pieces = textPieces(layout);
          for (std::size_t begin = 0; begin < pieces.size();) {
            check(cancel);
            const auto &firstPiece = pieces[begin];
            painter.setPen(firstPiece.color);
            // One selectable text code carries the complete Unicode
            // cluster. Auxiliary outlines (e.g. a contextual Arabic
            // dot) have no independent character. PDF readers disagree
            // on empty ToUnicode/ActualText for those extra glyphs, so
            // paint only those auxiliaries as real vector outlines.
            // The cluster's primary glyph remains actual embedded-font
            // text; no hidden text, duplicated character or reflow.
            QList<quint32> mainGlyphs;
            QList<QPointF> mainPositions;
            QStringList clusters;
            QPainterPath auxiliaries;
            std::size_t end = begin;
            for (; end < pieces.size() && pieces[end].run == firstPiece.run;
                 ++end) {
              const auto &piece = pieces[end];
              const auto glyphs = piece.glyphs.glyphIndexes();
              const auto positions = piece.glyphs.positions();
              const auto advances =
                  piece.glyphs.rawFont().advancesForGlyphIndexes(glyphs);
              qsizetype primary = 0;
              for (qsizetype g = 1; g < glyphs.size(); ++g)
                if (advances[g].x() > advances[primary].x())
                  primary = g;
              mainGlyphs.push_back(glyphs[primary]);
              mainPositions.push_back(positions[primary]);
              clusters.append(piece.text);
              for (qsizetype g = 0; g < glyphs.size(); ++g)
                if (g != primary)
                  auxiliaries.addPath(
                      piece.glyphs.rawFont().pathForGlyph(glyphs[g]).translated(
                          piece.origin + positions[g]));
            }
            auto main = firstPiece.glyphs;
            main.setGlyphIndexes(mainGlyphs);
            main.setPositions(mainPositions);
            painter.drawGlyphRun(firstPiece.origin, main);
            painter.fillPath(auxiliaries, firstPiece.color);
            parts.back().clusters.push_back(std::move(clusters));
            begin = end;
          }
        }
        if (!raster.empty() || page.leaves.empty())
          drawRaster();
        check(cancel);
        pageTimes.push_back(pageTime.elapsed());
      }
      require(started && painter.end(), "PDF finalization failed");
    }
    require(staged.flush() && staged.error() == QFileDevice::NoError,
            "Could not finalize staged PDF");
    check(cancel);
    QPDF input;
    input.processFile(staged.fileName().toUtf8().constData());
    auto generated = QPDFPageDocumentHelper(input).getAllPages();
    require(generated.size() == parts.size(),
            "Unexpected staged PDF page count");
    QPDF output;
    output.emptyPDF();
    QPDFPageDocumentHelper pages(output);
    auto intent = input.getRoot().getKey("/OutputIntents");
    if (!intent.isNull())
      output.getRoot().replaceKey(
          "/OutputIntents",
          output.copyForeignObject(input.makeIndirectObject(intent)));
    for (std::size_t i = 0; i < plan.pages.size(); ++i) {
      check(cancel);
      const auto &page = plan.pages[i];
      auto resources = Object::newDictionary();
      std::string content;
      for (std::size_t j = 0; j < parts.size(); ++j)
        if (parts[j].page == int(i)) {
          auto form =
              output.copyForeignObject(generated[j].getFormXObjectForPage());
          const auto name = "/Part" + std::to_string(j);
          resources.replaceKey(name, form);
          content += "q 1 0 0 1 0 " +
                     number(page.points.height() - parts[j].height) + " cm\n";
          if (parts[j].text) {
            failedTextPage = i;
            if (!embeddedTextFonts(form.getDict().getKey("/Resources")))
              throw NativeTextUnavailable(
                  "The PDF writer could not embed a resolved text font");
            const auto commands = form.getStreamData();
            const auto stream = QString::fromLatin1(
                reinterpret_cast<const char *>(commands->getBuffer()),
                qsizetype(commands->getSize()));
            const QRegularExpression block(
                QStringLiteral("(?m)^BT\\n.*?^ET\\n"),
                QRegularExpression::DotMatchesEverythingOption);
            const QRegularExpression fontPattern(
                QStringLiteral("(/F[0-9]+) [0-9.]+ Tf"));
            const QRegularExpression glyphPattern(
                QStringLiteral("<([0-9A-Fa-f]{4})> Tj"));
            auto originalResources = form.getDict().getKey("/Resources");
            auto fonts = originalResources.getKey("/Font");
            std::map<std::string, TextFontMapping> mappings;
            auto matches = block.globalMatch(stream);
            QString rewritten;
            qsizetype offset = 0, index = 0;
            while (matches.hasNext()) {
              const auto match = matches.next();
              if (std::size_t(index) >= parts[j].clusters.size())
                throw NativeTextUnavailable("Unexpected PDF text encoding");
              const auto &clusters = parts[j].clusters[std::size_t(index++)];
              auto command = match.captured();
              const auto fontMatch = fontPattern.match(command);
              if (!fontMatch.hasMatch())
                throw NativeTextUnavailable(
                    "Missing text font in PDF commands");
              const auto fontName = fontMatch.captured(1).toStdString();
              if (!mappings.contains(fontName))
                mappings.emplace(fontName,
                                 TextFontMapping(fonts.getKey(fontName)));
              auto glyphs = glyphPattern.globalMatch(command);
              if (command.contains("/ActualText <>"))
                throw NativeTextUnavailable(
                    "Synthetic bold differs from the canonical font outlines");
              QString mapped;
              qsizetype position = 0, glyphCount = 0;
              while (glyphs.hasNext()) {
                auto glyph = glyphs.next();
                bool ok = false;
                const auto cid = glyph.captured(1).toInt(&ok, 16);
                if (!ok)
                  throw NativeTextUnavailable("Invalid PDF text glyph");
                if (glyphCount >= clusters.size())
                  throw NativeTextUnavailable(
                      "Unexpected extra PDF text glyphs");
                const auto replacement =
                    mappings.at(fontName).add(cid, clusters[glyphCount]);
                ++glyphCount;
                mapped +=
                    command.mid(position, glyph.capturedStart() - position) +
                    QString::fromStdString(
                        "<" + TextFontMapping::hex(replacement) + "> Tj");
                position = glyph.capturedEnd();
              }
              if (glyphCount != clusters.size())
                throw NativeTextUnavailable("Missing PDF text glyphs");
              mapped += command.mid(position);
              rewritten +=
                  stream.mid(offset, match.capturedStart() - offset) + mapped;
              offset = match.capturedEnd();
            }
            if (std::size_t(index) != parts[j].clusters.size())
              throw NativeTextUnavailable(
                  "PDF writer did not retain all shaped text clusters");
            rewritten += stream.mid(offset);
            form.replaceStreamData(rewritten.toLatin1().toStdString(),
                                   Object::newNull(), Object::newNull());
            auto replacements = Object::newDictionary(fonts.getDictAsMap());
            for (auto &[name, mapping] : mappings)
              replacements.replaceKey(name, mapping.finish(output));
            auto ownResources =
                Object::newDictionary(originalResources.getDictAsMap());
            ownResources.replaceKey("/Font", replacements);
            form.getDict().replaceKey("/Resources", ownResources);
          }
          bool logicalOrder = false;
          if (parts[j].text) {
            const auto &layer = *source.document->layer(parts[j].text);
            const auto &t = layer.localToDocument;
            logicalOrder = t.m00 < 0 || t.m11 < 0 || t.m01 != 0 || t.m10 != 0;
            if (logicalOrder)
              content += "/Span << /ActualText " +
                         Object::newUnicodeString(
                             std::get<core::TextLayer>(layer.payload).utf8)
                             .unparse() +
                         " >> BDC\n";
          }
          // Transformed glyphs retain their real text/font mappings above.
          // Also expose their logical reading order to extractors that
          // would otherwise sort rotated/mirrored geometry left-to-right.
          content += name + " Do\n";
          if (logicalOrder)
            content += "EMC\n";
          content += "Q\n";
        }
      const auto box = Object::newArray(
          std::vector<Object>{Object::newInteger(0), Object::newInteger(0),
                              Object::newReal(page.points.width(), 9),
                              Object::newReal(page.points.height(), 9)});
      auto object = output.makeIndirectObject(
          dict({{"/Type", Object::newName("/Page")},
                {"/MediaBox", box},
                {"/Resources", dict({{"/XObject", resources}})},
                {"/Contents", output.newStream(content)}}));
      pages.addPage(QPDFPageObjectHelper(object), false);
    }
    require(pages.getAllPages().size() == plan.pages.size(),
            "PDF final page count mismatch");
    if (progress && !progress(int(plan.pages.size()), int(plan.pages.size()),
                              QStringLiteral("Writing")))
      throw Cancelled{};
    QSaveFile file(destination);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), "Cannot open PDF destination");
    SavePipeline pipeline(file, cancel);
    QPDFWriter writer(output);
    writer.setOutputPipeline(&pipeline);
    writer.setMinimumPDFVersion("1.6");
    writer.write();
    check(cancel);
    require(file.error() == QFileDevice::NoError,
            "PDF destination write failed");
    const auto bytes = file.pos();
    require(file.commit(), "Could not atomically replace the PDF destination");
    PdfExportResult result;
    result.bytes = bytes;
    result.milliseconds = clock.elapsed();
    result.pages = int(plan.pages.size());
    result.preserved = plan.preserved;
    result.rasterized = plan.rasterized;
    result.pageMilliseconds = std::move(pageTimes);
    result.finalizationMilliseconds = result.milliseconds;
    for (auto elapsed : result.pageMilliseconds)
      result.finalizationMilliseconds -= elapsed;
    return result;
  } catch (const Cancelled &) {
    PdfExportResult result;
    result.cancelled = true;
    return result;
  } catch (const NativeTextUnavailable &e) {
    // A backend/font limitation must not block export or silently emit glyph
    // paths as "preserved" text. Retry the affected page as canonical pixels;
    // unrelated pages keep their real text. No destination has been opened.
    auto fallback = plan;
    if (failedTextPage >= fallback.pages.size()) {
      PdfExportResult result;
      result.error = QString::fromUtf8(e.what());
      return result;
    }
    auto &failedPage = fallback.pages[failedTextPage];
    const int count = int(failedPage.preservedText.size());
    fallback.rasterized += count;
    fallback.preserved -= count;
    failedPage.preservedText.clear();
    auto result = writePdfExport(source, fallback, destination, cancel,
                                 std::move(progress));
    result.rasterizedPages.push_back(failedTextPage);
    result.textWarnings.append(
        QStringLiteral("Page %1: %2; text was rasterized.")
            .arg(failedTextPage + 1)
            .arg(QString::fromUtf8(e.what())));
    result.milliseconds = clock.elapsed();
    return result;
  } catch (const std::bad_alloc &) {
    PdfExportResult result;
    result.error = QStringLiteral(
        "Not enough memory to export PDF. Reduce PPI or selected pages.");
    return result;
  } catch (const std::exception &e) {
    PdfExportResult result;
    result.error = QString::fromUtf8(e.what());
    return result;
  }
}
} // namespace imageeditor::ui
