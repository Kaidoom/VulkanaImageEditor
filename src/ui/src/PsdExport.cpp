#include "imageeditor/ui/PsdExport.hpp"
#include "PsdNativeRecords.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/CompositionPlan.hpp"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace imageeditor::ui {
namespace {
using namespace psdwrite;
constexpr std::uint64_t MiB = 1024 * 1024;
bool integral(double n) {
  return std::isfinite(n) && std::abs(n - std::round(n)) < 1e-8;
}
bool translation(const core::AffineTransform &t) {
  return t.isAffine() && t.m00 == 1 && t.m11 == 1 && t.m01 == 0 && t.m10 == 0 &&
         integral(t.m02) && integral(t.m12);
}
bool liveMask(const core::Layer &l) {
  if (!l.mask)
    return true;
  if (l.mask->outside != 0 && l.mask->outside != 255)
    return false; // PSD's default mask coverage is explicitly black or white.
  const auto inverse = l.mask->localToMask.inverted();
  return inverse &&
         translation(core::composeTransform(l.localToDocument, *inverse));
}
bool adjusted(const core::Layer &l) {
  return l.adjustments &&
         std::ranges::any_of(l.adjustments->items, [](auto &a) {
           return a.enabled && !core::adjustmentIsNeutral(a);
         });
}
bool externalStyles(const core::Layer &l) {
  if (!l.effects)
    return false;
  const auto& bevel=l.effects->items[std::size_t(core::LayerEffectType::BevelEmboss)];
  if(bevel.enabled&&bevel.size>0&&bevel.bevel.depth>0&&bevel.bevel.style!=core::BevelStyle::Inner&&
     ((bevel.opacity>0&&bevel.blendMode!=core::BlendMode::Normal)||
      (bevel.bevel.shadowOpacity>0&&bevel.bevel.shadowBlend!=core::BlendMode::Normal)))return true;
  for (auto type :
       {core::LayerEffectType::Stroke, core::LayerEffectType::DropShadow,
        core::LayerEffectType::OuterGlow}) {
    auto &e = l.effects->items[size_t(type)];
    if (e.enabled && e.opacity > 0 && e.blendMode != core::BlendMode::Normal)
      return true;
  }
  return false;
}
std::shared_ptr<core::Document> structureCopy(const core::Document &source) {
  auto d = std::make_shared<core::Document>(source.canvas());
  for (auto l : source.layers()) {
    l.renderCache.reset();
    l.filterCache.reset();
    l.effectCache.reset();
    require(d->insertLayer(d->layers().size(), std::move(l)),
            "Cannot copy export structure");
  }
  if (d->tree() != source.tree())
    require(d->replaceStructure(d->tree(), source.tree()),
            "Cannot copy export hierarchy");
  return d;
}
FlattenedDocumentLimits renderLimits(const PsdExportSnapshot &s,
                                     std::uint64_t retained = 0) {
  const auto cap = s.limits.workingBytes;
  require(s.limits.existingBytes < cap &&
              s.sourceBytes < cap - s.limits.existingBytes &&
              retained < cap - s.limits.existingBytes - s.sourceBytes,
          "PSD working-memory budget exhausted");
  const auto remaining =
      cap - s.limits.existingBytes - s.sourceBytes - retained;
  require(remaining > 320 * MiB, "Not enough working memory for PSD rendering");
  FlattenedDocumentLimits limits;
  // Source, rendered RGBA and transfer scratch coexist; derived typed/effect
  // resources have a separate fixed allowance. Admission precedes allocation.
  limits.outputPixels =
      std::min(limits.outputPixels, (remaining - 320 * MiB) / 12);
  return limits;
}
void ensureImage(const QImage &im, const PsdExportSnapshot &s) {
  require(!im.isNull(), "PSD layer rendering produced no raster");
  require(im.width() <= 30000 && im.height() <= 30000,
          "PSD layers/channels cannot exceed 30000 pixels");
  require(std::uint64_t(im.sizeInBytes()) * 3 + s.sourceBytes +
                  s.limits.existingBytes + 256 * MiB <=
              s.limits.workingBytes,
          "PSD render exceeds available working memory");
}
QRect bounds(core::Vec2d origin, QSize size) {
  require(integral(origin.x) && integral(origin.y) && origin.x >= INT32_MIN &&
              origin.y >= INT32_MIN && origin.x + size.width() <= INT32_MAX &&
              origin.y + size.height() <= INT32_MAX,
          "PSD layer bounds exceed signed 32-bit coordinates");
  return {int(std::llround(origin.x)), int(std::llround(origin.y)),
          size.width(), size.height()};
}
core::Layer pixelLayer(const core::Layer &original,
                       const FlattenedDocumentResult &r) {
  auto l = original;
  std::vector<std::byte> bytes(size_t(r.image.width()) *
                               size_t(r.image.height()) * 4);
  for (int y = 0; y < r.image.height(); ++y)
    std::memcpy(bytes.data() + size_t(y) * size_t(r.image.width()) * 4,
                r.image.constScanLine(y), size_t(r.image.width()) * 4);
  l.payload = core::RasterLayer{std::make_shared<core::ContiguousRasterSurface>(
      core::Extent2u{uint(r.image.width()), uint(r.image.height())},
      std::move(bytes))};
  l.localToDocument = {.m02 = r.origin.x, .m12 = r.origin.y};
  l.rasterOrigin = {};
  l.rasterEffectFrame.reset();
  l.crop.reset();
  l.mask.reset();
  l.adjustments.reset();
  l.filters.reset();
  l.effects.reset();
  l.renderCache.reset();
  l.filterCache.reset();
  l.effectCache.reset();
  return l;
}
void omit(core::Document &d, core::LayerId id) {
  if (!d.containsItem(id))
    return;
  auto tree = d.tree();
  auto p = tree.placement(id);
  require(bool(p), "Missing PSD plan item");
  auto *siblings = tree.children(p->parent);
  std::erase(*siblings, id);
  const auto descendants = d.tree().descendants(id);
  std::vector<core::LayerId> removed;
  for (auto child : descendants)
    if (d.containsLayer(child))
      removed.push_back(child);
  // descendants() already includes the item itself for a leaf, and returns
  // leaves only for a container. Container membership is removed separately.
  std::erase_if(tree.containers, [&](auto &c) {
    return c.id == id || d.tree().isAncestor(id, c.id);
  });
  require(d.replaceStructure(d.tree(), std::move(tree), removed),
          "Cannot omit PSD export item");
}
// The accepted export-owned document drives BOTH records and the fresh merged
// image. Replacements never reach the live document or its histories/caches.
std::shared_ptr<core::Document> accepted(const PsdExportSnapshot &s,
                                         const PsdExportPlan &p,
                                         const std::atomic_bool &cancel,
                                         std::uint64_t &retained) {
  auto d = structureCopy(*s.document);
  if (p.options.mode == PsdExportMode::Flattened)
    return d;
  for (auto &e : p.entries)
    if (e.action == PsdExportAction::Omit)
      omit(*d, e.id);
  // Bottom-up lets a parent consolidation include explicitly accepted child
  // choices, without restoring an omitted child from the original snapshot.
  for (auto it = p.entries.rbegin(); it != p.entries.rend(); ++it) {
    check(cancel);
    auto &e = *it;
    if (e.action != PsdExportAction::Pixels || !d->containsItem(e.id))
      continue;
    if (std::ranges::any_of(p.entries, [&](auto &parent) {
          return parent.container && parent.action == PsdExportAction::Pixels &&
                 s.document->tree().isAncestor(parent.id, e.id);
        }))
      continue;
    if (auto *l = d->layer(e.id)) {
      auto r = rasterizeLayerContent(
          *d, e.id, [&](auto, auto) { return !cancel; },
          renderLimits(s, retained));
      if (r.cancelled)
        throw Cancelled{};
      require(bool(r), r.error.toUtf8().constData());
      ensureImage(r.image, s);
      *l = pixelLayer(*l, r);
      retained += std::uint64_t(r.image.sizeInBytes());
    } else {
      const auto container = *d->tree().container(e.id);
      auto visible = container.visible;
      std::array change{core::ItemVisibilityUpdate{e.id, visible, true}};
      if (!visible)
        require(d->setItemVisibilities(change), "Cannot stage hidden group");
      std::array root{e.id};
      const auto limits = renderLimits(s, retained);
      const auto evaluated =
          d->expandedLayers(root).empty()
              ? EvaluatedDocumentBounds{}
              : evaluatedLayerItemsBounds(
                    *d, root, [&](auto, auto) { return !cancel; }, limits);
      if (evaluated.cancelled)
        throw Cancelled{};
      require(evaluated.error.isEmpty(), evaluated.error.toUtf8().constData());
      auto r = evaluated.rect.empty()
                   ? FlattenedDocumentResult{}
                   : flattenLayerItems(
                         *d, root, [&](auto, auto) { return !cancel; }, limits);
      if (r.cancelled)
        throw Cancelled{};
      if (evaluated.rect.empty()) {
        r.image = QImage(1, 1, QImage::Format_RGBA8888);
        r.image.fill(Qt::transparent);
        r.error.clear();
      }
      require(bool(r), r.error.toUtf8().constData());
      ensureImage(r.image, s);
      auto leaf = pixelLayer(core::Layer{}, r);
      retained += std::uint64_t(r.image.sizeInBytes());
      leaf.id = e.id;
      leaf.name = container.name;
      leaf.visible = visible;
      leaf.colorLabel = std::uint8_t(container.colorLabel);
      auto tree = d->tree();
      auto descendants = tree.descendants(e.id);
      auto removed = d->expandedLayers(root);
      std::erase_if(tree.containers, [&](auto &c) {
        return c.id == e.id ||
               std::ranges::find(descendants, c.id) != descendants.end();
      });
      std::array added{leaf};
      require(d->replaceStructure(d->tree(), std::move(tree), removed, added),
              "Cannot consolidate PSD group");
    }
  }
  return d;
}
FlattenedDocumentResult sourceRaster(core::Document &d, core::LayerId id,
                                     const std::atomic_bool &cancel,
                                     const PsdExportSnapshot &s) {
  auto *l = d.layer(id);
  auto source = *l;
  source.mask.reset();
  source.effects.reset();
  source.effectCache.reset();
  if (auto *r = std::get_if<core::RasterLayer>(&source.payload);
      r && translation(source.localToDocument) &&
      integral(source.rasterOrigin.x) && integral(source.rasterOrigin.y) &&
      !source.crop && !adjusted(source) &&
      !core::hasActiveSpatialFilters(source.filters)) {
    auto size = r->surface->extent();
    require(std::uint64_t(size.width) * size.height <=
                renderLimits(s).outputPixels,
            "PSD layer source exceeds the working-memory limit");
    QImage image(int(size.width), int(size.height), QImage::Format_RGBA8888);
    require(!image.isNull(), "Cannot allocate PSD layer raster");
    for (uint y = 0; y < size.height; ++y) {
      check(cancel);
      r->surface->copyRgba8({0, int(y), int(size.width), 1},
                            std::span<std::byte>(reinterpret_cast<std::byte *>(
                                                     image.scanLine(int(y))),
                                                 size_t(size.width) * 4),
                            size_t(image.bytesPerLine()));
    }
    return {image, {}, false, source.localToDocument.map(source.rasterOrigin)};
  }
  // Work on a disposable document, preserving the accepted composite model.
  core::Document single(d.canvas());
  require(single.insertLayer(0, std::move(source)),
          "Cannot stage PSD source raster");
  auto result = rasterizeLayerContent(
      single, id, [&](auto, auto) { return !cancel; }, renderLimits(s));
  if (result.cancelled)
    throw Cancelled{};
  return result;
}
void addMask(Record &r, const core::Layer &l, QIODevice &spool,
             const std::atomic_bool &cancel, std::uint64_t limit) {
  if (!l.mask)
    return;
  require(liveMask(l), "Unreviewed mask transform");
  r.mask = true;
  r.maskEnabled = l.mask->enabled;
  r.maskOutside = l.mask->outside;
  const auto &mask = *l.mask;
  auto size = mask.coverage->extent();
  const auto map =
      core::composeTransform(l.localToDocument, *mask.localToMask.inverted());
  r.maskBounds = bounds(map.map({0, 0}), {int(size.width), int(size.height)});
  require(size.width <= 30000 && size.height <= 30000,
          "PSD mask exceeds standard dimensions");
  QImage image(int(size.width), int(size.height), QImage::Format_Grayscale8);
  require(!image.isNull(), "Cannot allocate PSD user mask");
  for (uint y = 0; y < size.height; ++y) {
    check(cancel);
    auto row = image.scanLine(int(y));
    for (uint x = 0; x < size.width; ++x)
      row[x] = mask.coverage->coverageAtDocumentPixel(int(x), int(y));
  }
  r.channels.push_back(spoolChannel(spool, -2, image, -1, cancel, limit));
}
} // namespace
PsdExportSnapshot capturePsdExport(const core::Document &d, std::uint64_t id,
                                   PsdExportLimits limits) {
  PsdExportSnapshot s;
  s.instanceId = id;
  s.revision = d.revision();
  s.limits = limits;
  require(limits.existingBytes < limits.workingBytes,
          "Open documents exceed the PSD working-memory budget");
  auto size = d.canvas().extent;
  require(size.width && size.height && size.width <= 30000 &&
              size.height <= 30000,
          "Standard PSD dimensions must be between 1 and 30000 pixels");
  require(d.tree().containers.size() * 2 + d.layers().size() <= 32767,
          "Too many PSD layer/group records");
  require(d.layers().size() <= 1024 &&
              std::uint64_t(size.width) * size.height <=
                  FlattenedDocumentLimits{}.outputPixels,
          "PSD composite exceeds the shared renderer limit (1024 layers / 64 "
          "Mi pixels)");
  std::map<core::SurfaceId, std::shared_ptr<core::RasterSurface>> surfaces;
  for (auto &l : d.layers())
    if (auto r = std::get_if<core::RasterLayer>(&l.payload)) {
      require(bool(r->surface), "Missing raster source");
      if (surfaces.emplace(r->surface->id(), nullptr).second) {
        auto e = r->surface->extent();
        require(e.width <= 30000 && e.height <= 30000,
                "PSD source layer exceeds 30000 pixels");
        s.sourceBytes += std::uint64_t(e.width) * e.height * 4;
      }
    }
  require(s.sourceBytes + limits.existingBytes +
                  std::uint64_t(size.width) * size.height * 12 + 320 * MiB <=
              limits.workingBytes,
          "Not enough memory for a frozen PSD export and composite; close "
          "other documents first");
  auto copy = structureCopy(d);
  for (auto &original : d.layers())
    if (auto r = std::get_if<core::RasterLayer>(&original.payload)) {
      auto &frozen = surfaces.at(r->surface->id());
      if (!frozen) {
        auto e = r->surface->extent();
        std::vector<std::byte> bytes(size_t(e.width) * e.height * 4);
        r->surface->copyRgba8({0, 0, int(e.width), int(e.height)}, bytes,
                              size_t(e.width) * 4);
        frozen = std::make_shared<core::ContiguousRasterSurface>(
            e, std::move(bytes));
      }
      std::get<core::RasterLayer>(copy->layer(original.id)->payload).surface =
          frozen;
    }
  s.document = std::move(copy);
  return s;
}
PsdExportPlan planPsdExport(const PsdExportSnapshot &s,
                            PsdExportOptions options,
                            const std::atomic_bool &cancel) {
  PsdExportPlan p;
  p.instanceId = s.instanceId;
  p.revision = s.revision;
  p.options = options;
  try {
    require(bool(s.document), "Missing PSD export snapshot");
    require(!options.instanceId || options.instanceId == s.instanceId,
            "PSD choices belong to another document");
    p.options.instanceId = s.instanceId;
    if (options.mode == PsdExportMode::Flattened) {
      p.notes << "Visible composition only; one pixel layer. Hidden layers and "
                 "editable source content are not included.";
      return p;
    }
    auto &d = *s.document;
    const auto visit = [&](auto &&self, const std::vector<core::LayerId> &ids,
                           core::LayerId parent) -> void {
      for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        check(cancel);
        PsdExportEntry e;
        e.id = *it;
        e.parent = parent;
        if (auto c = d.tree().container(*it)) {
          e.container = true;
          e.name = QString::fromStdString(c->name);
          e.type = c->kind == core::ContainerKind::ClippingMaskGroup
                       ? "Clipping group"
                       : "Folder";
          if (c->kind == core::ContainerKind::ClippingMaskGroup &&
              c->children.size() > 1) {
            for (auto child : c->children)
              if (d.tree().container(child)) {
                e.editable = false;
                e.reasons
                    << "Nested containers as clipping members require "
                       "consolidating this clipping group or flattened export.";
              }
            auto base = d.layer(c->children.front());
            if(base&&std::holds_alternative<core::AdjustmentLayer>(base->payload)) {
              e.editable=false;
              e.reasons<<"An adjustment supplies no native clipping-base coverage. Consolidate this group or use Flattened PSD.";
            }
            if (base && core::hasActiveLayerEffects(base->effects)) {
              e.attention = true;
              e.reasons << "PSD and Vulkana clipping stacks apply base styles "
                           "differently; native clipping is approximate here.";
            }
          }
        } else {
          auto &l = *d.layer(*it);
          e.name = QString::fromStdString(l.name);
          e.type = std::holds_alternative<core::TextLayer>(l.payload) ? "Text"
                   : std::holds_alternative<core::AdjustmentLayer>(l.payload) ? "Adjustment"
                   : std::holds_alternative<core::ShapeLayer>(l.payload)
                       ? "Shape"
                       : "Raster";
          try {
            if(std::holds_alternative<core::AdjustmentLayer>(l.payload)) {
              e.pixels=false;
              (void)adjustmentRecord(l);
              if(l.opacity!=1||l.mask) {
                e.attention=true;
                e.reasons<<"Editable correction: fractional strength/masks can differ outside Vulkana because its mixing is linear-light.";
              }
            } else if (std::holds_alternative<core::TextLayer>(l.payload)) {
              (void)textRecord(l, &e.fonts, &e.reasons);
              if (!options.preserveText) {
                e.editable = false;
                e.reasons << "Rasterize all text is selected.";
              }
              if (!e.reasons.empty())
                e.attention = true;
              if (QString::fromStdString(
                      std::get<core::TextLayer>(l.payload).utf8)
                      .contains('\n')) {
                e.attention = true;
                e.reasons << "Multiline text uses exported leading; "
                             "receiving-editor typography may differ.";
              }
            } else if (std::holds_alternative<core::ShapeLayer>(l.payload))
              (void)shapeRecords(l, d.canvas());
            if (l.effects && *l.effects != core::LayerEffectStack{}) {
              (void)effectRecord(l);
              e.attention = true;
              e.reasons
                  << "Editable styles: edge profiles, spread/softness, "
                     "gradient interpolation/anchors and clipping differ "
                     "between editors. Use native styles for editability, or a "
                     "scoped pixel/flattened choice for appearance.";
            }
          } catch (const Error &error) {
            e.editable = false;
            e.reasons << QString::fromUtf8(error.what());
          }
          if ((!std::holds_alternative<core::AdjustmentLayer>(l.payload)&&adjusted(l)) || core::hasActiveSpatialFilters(l.filters) ||
              l.crop) {
            e.editable = false;
            e.reasons << "Per-layer processing/crop is baked together with "
                         "masks and styles, in native order.";
          }
          if (!liveMask(l)) {
            e.editable = false;
            e.reasons
                << (std::holds_alternative<core::AdjustmentLayer>(l.payload)
                    ? "This adjustment mask requires group consolidation or Flattened PSD."
                    : "This mask grid or fractional outside coverage requires baking into the layer result.");
          }
          if (l.mask && std::holds_alternative<core::ShapeLayer>(l.payload)) {
            e.editable = false;
            e.reasons << "Combined vector geometry and a bitmap mask need a "
                         "pixel conversion in PSD Export V1.";
          }
          if (externalStyles(l)) {
            e.pixels = false;
            e.reasons
                << "Exterior effect blending needs the document backdrop. Use "
                   "editable equivalents if available, consolidate an "
                   "explicitly chosen group, or choose Flattened PSD.";
          }
          if (e.type == "Raster" && !translation(l.localToDocument)) {
            e.attention = true;
            e.reasons
                << "Raster transform is baked once at document resolution; the "
                   "pre-transform source resolution is not retained.";
          }
        }
        e.action = options.actions.value(e.id, PsdExportAction::Automatic);
        if (e.action == PsdExportAction::Automatic)
          e.action =
              e.editable ? PsdExportAction::Editable : PsdExportAction::Pixels;
        if (e.action == PsdExportAction::Pixels) {
          e.attention = true;
          if (e.container)
            e.reasons << "Consolidation replaces this subtree with pixels, "
                         "without sampling the outside backdrop.";
          if (!e.pixels)
            p.error = QStringLiteral(
                          "%1: cannot bake this layer independently. Choose an "
                          "enclosing consolidation or Flattened PSD.")
                          .arg(e.name);
        }
        if (e.action == PsdExportAction::Editable && !e.editable)
          p.error = QStringLiteral(
                        "%1: editable output is unavailable; review this item.")
                        .arg(e.name);
        if (e.action == PsdExportAction::Omit)
          e.attention = true;
        p.entries.push_back(e);
        if (auto c = d.tree().container(*it))
          self(self, c->children, *it);
      }
    };
    visit(visit, d.tree().roots, 0);
    // An explicit enclosing replacement owns the entire dependency span. Do not
    // block it because an independently encoded child would be unsupported.
    p.error.clear();
    for (auto &e : p.entries) {
      bool superseded = false, omittedByParent = false;
      for (auto &parent : p.entries)
        if (parent.container && parent.action != PsdExportAction::Editable &&
            d.tree().isAncestor(parent.id, e.id)) {
          superseded = true;
          omittedByParent |= parent.action == PsdExportAction::Omit;
        }
      if (superseded) {
        // Omission happens before consolidation. Even inside a consolidated
        // ancestor it must not turn a different clipped member into the base.
        if (auto c = d.tree().container(e.id);
            !omittedByParent && e.action != PsdExportAction::Omit && c &&
            c->kind == core::ContainerKind::ClippingMaskGroup &&
            c->children.size() > 1) {
          auto base = std::ranges::find_if(
              p.entries, [&](auto &x) { return x.id == c->children.front(); });
          if (base != p.entries.end() && base->action == PsdExportAction::Omit)
            p.error =
                e.name +
                ": retain the clipping base or omit this whole clipping group.";
        }
        continue;
      }
      if (e.action == PsdExportAction::Editable && !e.editable)
        p.error = e.name + ": editable output is unavailable.";
      if (e.action == PsdExportAction::Pixels && !e.pixels)
        p.error = e.name +
                  ": cannot bake a backdrop-dependent effect independently. "
                  "Consolidate an enclosing group or use Flattened PSD.";
      p.needsReview |= e.attention;
      if (auto c = d.tree().container(e.id);
          c && c->kind == core::ContainerKind::ClippingMaskGroup &&
          c->children.size() > 1 && e.action != PsdExportAction::Omit) {
        auto base = std::ranges::find_if(
            p.entries, [&](auto &x) { return x.id == c->children.front(); });
        if (base != p.entries.end() && base->action == PsdExportAction::Omit)
          p.error = e.name + ": retain the base when consolidating, or omit "
                             "the whole clipping group; "
                             "another member will not be silently promoted.";
        if (base != p.entries.end() &&
            base->action == PsdExportAction::Pixels &&
            e.action == PsdExportAction::Editable) {
          auto l = d.layer(base->id);
          if (l && core::hasActiveLayerEffects(l->effects))
            p.error =
                e.name +
                ": baking the base style changes clipping coverage. "
                "Consolidate this group or retain the editable base style.";
        }
      }
    }
    p.notes << "Layered RGB recomposition depends on the receiving editor's "
               "blend/color settings. Vulkana's linear-light result is in the "
               "compatibility composite; choose Flattened PSD for "
               "appearance-focused output. No global Photoshop preferences are "
               "changed.";
    p.notes << "Fonts are referenced by actual PostScript face, not embedded. "
               "The receiving computer needs the listed fonts. .vulkana "
               "remains the editable master.";
  } catch (const Cancelled &) {
    p.cancelled = true;
  } catch (const std::exception &e) {
    p.error = QString::fromUtf8(e.what());
  }
  return p;
}
FlattenedDocumentResult previewPsdExport(const PsdExportSnapshot &s,
                                         const PsdExportPlan &p,
                                         const std::atomic_bool &cancel) {
  try {
    require(bool(p) && p.instanceId == s.instanceId && p.revision == s.revision,
            "Stale or invalid PSD export plan");
    std::uint64_t retained = 0;
    auto d = accepted(s, p, cancel, retained);
    auto size = d->canvas().extent;
    auto factor = std::min(1., 640. / std::max(size.width, size.height));
    core::Extent2u output{uint(std::max(1., std::round(size.width * factor))),
                          uint(std::max(1., std::round(size.height * factor)))};
    return flattenDocumentAtSize(
        *d, output, [&](auto, auto) { return !cancel; },
        renderLimits(s, retained));
  } catch (const Cancelled &) {
    return {{}, {}, true, {}};
  } catch (const std::exception &e) {
    return {{}, QString::fromUtf8(e.what()), false, {}};
  }
}
PsdExportResult writePsdExport(const PsdExportSnapshot &s,
                               const PsdExportPlan &p,
                               const QString &destination,
                               const std::atomic_bool &cancel,
                               PsdExportProgress progress) {
  PsdExportResult result;
  QElapsedTimer clock;
  clock.start();
  try {
    require(bool(p) && p.instanceId == s.instanceId && p.revision == s.revision,
            "Stale or invalid PSD export plan");
    check(cancel);
    require(
        QFileInfo(destination).suffix().compare("psd", Qt::CaseInsensitive) ==
            0,
        "PSD output requires a .psd filename");
    std::uint64_t retained = 0;
    auto d = accepted(s, p, cancel, retained);
    auto renderSnapshot = s;
    renderSnapshot.sourceBytes += retained;
    const auto limit = std::min<std::uint64_t>(s.limits.fileBytes,
                                               2ULL * 1024 * 1024 * 1024 - 1);
    QTemporaryFile spool;
    require(spool.open(), "Cannot create PSD channel spool");
    std::vector<Record> records;
    int completed = 0;
    const auto step = [&](const QString &stage) {
      check(cancel);
      if (progress && !progress(completed, int(d->layers().size()), stage))
        throw Cancelled{};
    };
    auto layerRecord = [&](core::LayerId id, bool clipped) {
      step("Rendering layer");
      auto &l = *d->layer(id);
      Record r;
      r.name = QString::fromStdString(l.name);
      r.visible = l.visible;
      r.opacity = int(std::lround(l.opacity * 255));
      r.blend = blend(l.blendMode);
      r.label = l.colorLabel;
      r.clipping = clipped;
      if(std::holds_alternative<core::AdjustmentLayer>(l.payload)) {
        r.tags=adjustmentRecord(l);r.tags["clbl"]=QByteArray::fromHex("01000000");
        for(int c:{-1,0,1,2})r.channels.push_back(spoolChannel(spool,c,{},0,cancel,limit));
        addMask(r,l,spool,cancel,limit);records.push_back(std::move(r));++completed;return;
      }
      auto pixels = sourceRaster(*d, id, cancel, renderSnapshot);
      require(bool(pixels), pixels.error.toUtf8().constData());
      ensureImage(pixels.image, s);
      r.bounds = bounds(pixels.origin, pixels.image.size());
      if (std::holds_alternative<core::TextLayer>(l.payload))
        r.tags["TySh"] = textRecord(l);
      if (std::holds_alternative<core::ShapeLayer>(l.payload))
        r.tags = shapeRecords(l, d->canvas());
      auto effects = effectRecord(l);
      if (!effects.isEmpty())
        r.tags["lfx2"] = effects;
      r.tags["clbl"] = QByteArray::fromHex("01000000");
      for (int c : {-1, 0, 1, 2})
        r.channels.push_back(spoolChannel(spool, c, pixels.image,
                                          c == -1 ? 3 : c, cancel, limit));
      pixels.image = {};
      addMask(r, l, spool, cancel, limit);
      records.push_back(std::move(r));
      ++completed;
    };
    const auto visit = [&](auto &&self, const std::vector<core::LayerId> &ids,
                           bool clipping) -> void {
      for (size_t i = 0; i < ids.size(); ++i) {
        check(cancel);
        auto id = ids[i];
        if (auto c = d->tree().container(id)) {
          Record end;
          end.name = "</Layer group>";
          end.section = 3;
          end.blend = "norm";
          for (int channel : {-1, 0, 1, 2})
            end.channels.push_back(
                spoolChannel(spool, channel, {}, 0, cancel, limit));
          records.push_back(std::move(end));
          self(self, c->children,
               c->kind == core::ContainerKind::ClippingMaskGroup &&
                   c->children.size() > 1);
          Record start;
          start.name = QString::fromStdString(c->name);
          start.section = 1;
          start.blend = "pass";
          if(c->kind!=core::ContainerKind::ClippingMaskGroup)
            for(auto child:c->children)if(const auto* l=d->layer(child))
              if(const auto* a=std::get_if<core::AdjustmentLayer>(&l->payload);a&&a->scope==core::AdjustmentScope::ThisGroup)start.blend="norm";
          start.visible = c->visible;
          start.label = int(c->colorLabel);
          for (int channel : {-1, 0, 1, 2})
            start.channels.push_back(
                spoolChannel(spool, channel, {}, 0, cancel, limit));
          records.push_back(std::move(start));
        } else
          layerRecord(id, clipping && i > 0);
      }
    };
    if (p.options.mode == PsdExportMode::Layered)
      visit(visit, d->tree().roots, false);
    step("Rendering compatibility composite");
    auto composite = flattenDocument(
        *d, [&](auto, auto) { return !cancel; }, renderLimits(renderSnapshot));
    if (composite.cancelled)
      throw Cancelled{};
    require(bool(composite), composite.error.toUtf8().constData());
    ensureImage(composite.image, s);
    if (p.options.mode == PsdExportMode::Flattened || records.empty()) {
      Record r;
      r.name = p.options.mode == PsdExportMode::Flattened ? "Composite"
                                                          : "Empty document";
      r.bounds = {0, 0, composite.image.width(), composite.image.height()};
      for (int c : {-1, 0, 1, 2})
        r.channels.push_back(spoolChannel(spool, c, composite.image,
                                          c == -1 ? 3 : c, cancel, limit));
      records.push_back(std::move(r));
    }
    // Verify a seekable temporary output before touching the destination. A
    // bounded disk-to-disk copy then enters the existing atomic replacement.
    QTemporaryFile output(QFileInfo(destination).absolutePath() +
                          "/.vulkana-psd-XXXXXX");
    require(output.open(),
            "Cannot create PSD temporary output in destination directory");
    step("Writing PSD");
    write(output, spool, records, composite.image, d->canvas(), cancel, limit);
    require(output.flush(), "Could not flush PSD output");
    step("Verifying PSD");
    verify(output, d->canvas(), int(records.size()));
    check(cancel);
    result.bytes = output.size();
    QSaveFile target(destination);
    target.setDirectWriteFallback(false);
    require(target.open(QIODevice::WriteOnly),
            "Cannot open atomic PSD destination");
    require(output.seek(0), "Cannot rewind completed PSD");
    while (!output.atEnd()) {
      check(cancel);
      auto bytes = output.read(1024 * 1024);
      require(!bytes.isEmpty() && target.write(bytes) == bytes.size(),
              "Could not publish PSD (check disk space)");
    }
    check(cancel);
    require(target.commit(), "Could not commit PSD destination");
    result.layers = int(records.size());
  } catch (const Cancelled &) {
    result.cancelled = true;
    result.bytes = 0;
  } catch (const std::exception &e) {
    result.error = QString::fromUtf8(e.what());
    result.bytes = 0;
  }
  result.milliseconds = clock.elapsed();
  return result;
}
} // namespace imageeditor::ui
