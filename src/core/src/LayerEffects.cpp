#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <numbers>
#include <chrono>
#include <stdexcept>

namespace imageeditor::core {
namespace {
constexpr std::array<std::string_view, layerEffectCount> names{
    "Stroke",     "Drop Shadow",   "Inner Shadow",    "Outer Glow",
    "Inner Glow", "Color Overlay", "Gradient Overlay", "Bevel & Emboss"};
constexpr std::array<std::string_view, layerEffectCount> ids{
    "stroke",     "drop-shadow",   "inner-shadow",    "outer-glow",
    "inner-glow", "color-overlay", "gradient-overlay", "bevel-emboss"};
bool bevelActive(const LayerEffect& e) {
  return e.enabled && e.size>0 && e.bevel.depth>0 &&
    (e.opacity*e.color.alpha>0 || e.bevel.shadowOpacity*e.secondColor.alpha>0);
}
bool normalsEqual(const LayerEffect& a, const LayerEffect& b) {
  return a.size==b.size && a.bevel.style==b.bevel.style &&
    a.bevel.depth==b.bevel.depth && a.bevel.down==b.bevel.down &&
    a.bevel.soften==b.bevel.soften && a.bevel.surface==b.bevel.surface;
}
bool geometryEqual(const LayerEffect &a, const LayerEffect &b, std::size_t i) {
  if(i==7) return normalsEqual(a,b) && a.angle==b.angle &&
    a.bevel.altitude==b.bevel.altitude && a.bevel.gloss==b.bevel.gloss;
  if (i >= 5)
    return true;
  return a.size == b.size &&
         (i == 0 ? a.position == b.position : a.spread == b.spread) &&
         ((i != 1 && i != 2) ||
          (a.distance == b.distance && a.angle == b.angle));
}
template <class L> LayerMaskState shapeMask(const L& layer) {
  return layer.effects && !layer.effects->maskEffectsAfterRendering &&
                 layer.mask && layer.mask->enabled ? layer.mask : LayerMaskState{};
}
template <class L> bool silhouetteMatches(const LayerEffectCache& cache, const L& layer) {
  const auto mask = shapeMask(layer);
  return cache.geometry && layer.effects &&
         cache.geometry->pixelAlphaShapesEffects == layer.effects->pixelAlphaShapesEffects &&
         cache.geometry->maskEffectsAfterRendering == layer.effects->maskEffectsAfterRendering &&
         (layer.effects->pixelAlphaShapesEffects || cache.paintFrame == layerEffectReferenceFrame(layer)) &&
         cache.geometryMask == mask &&
         (!mask || cache.geometryMaskRevision == mask->coverage->revision());
}
template <class L> bool cacheValid(const L &l) {
  if (!hasActiveLayerEffects(l.effects))
    return true;
  const auto s = renderedSurface(l);
  const auto &c = l.effectCache;
  if (!s || !c || !c->geometry || s->id() != c->sourceId ||
      s->revision() != c->sourceRevision ||
      renderPixelsToLocal(l) != c->pixelsToLocal || !silhouetteMatches(*c, l))
    return false;
  for (std::size_t i = 0; i < 5; ++i)
    if (l.effects->items[i].enabled &&
        (!c->geometry || !c->geometry->items[i].enabled ||
         !geometryEqual(l.effects->items[i], c->geometry->items[i], i)))
      return false;
  // Disabling an exterior effect changes visual bounds, even though its
  // cached geometry remains reusable for a subsequent re-enable.
  for (auto i : {0, 1, 3})
    if (c->geometry->items[std::size_t(i)].enabled !=
        l.effects->items[std::size_t(i)].enabled)
      return false;
  const auto& bevel=l.effects->items[7];
  if(bevelActive(bevel) && (!c->bevelNormals || !c->bevelPrepared ||
      !geometryEqual(bevel,c->bevelPrepared->items[7],7)))return false;
  if(bevelActive(bevel)!=bevelActive(c->geometry->items[7]))return false;
  return true;
}
void require(bool v, const char *message) {
  if (!v)
    throw std::runtime_error(message);
}
void cancellation(const FilterPreparationOptions &o) {
  if (o.cancelled && o.cancelled())
    throw std::runtime_error("Effect preparation cancelled");
}
float at(const SpatialPlane &p, int x, int y, float outside = 0) {
  x -= p.bounds.x;
  y -= p.bounds.y;
  return x < 0 || y < 0 || x >= p.bounds.width || y >= p.bounds.height
             ? outside
             : p.pixels[std::size_t(y) * std::size_t(p.bounds.width) + std::size_t(x)];
}
// Exact grayscale circular max morphology with a one-pixel analytic radial
// coverage ramp. A max pyramid prunes constant interiors; it never substitutes
// a square kernel or thresholds the input alpha. Erosion is the dual operator.
class MaskMaxTree {
  struct Level {
    int w, h;
    std::vector<float> p;
  };
  std::vector<Level> levels_;
  float query(int level, int x, int y, float px, float py, float radius,
              float best) const {
    const auto &l = levels_[std::size_t(level)];
    if (x >= l.w || y >= l.h)
      return best;
    const auto value = l.p[std::size_t(y) * std::size_t(l.w) + std::size_t(x)];
    if (value <= best)
      return best;
    const int span = 1 << level;
    const float left = float(x * span), top = float(y * span);
    const float right = float(std::min((x + 1) * span, levels_[0].w) - 1),
                bottom = float(std::min((y + 1) * span, levels_[0].h) - 1);
    const auto dx = std::max({left - px, 0.0F, px - right}),
               dy = std::max({top - py, 0.0F, py - bottom});
    const auto possible = std::min(
        value, std::clamp(radius + 1.0F - std::hypot(dx, dy), 0.0F, 1.0F));
    if (possible <= best)
      return best;
    if (level == 0)
      return possible;
    const auto farX = std::max(std::abs(left - px), std::abs(right - px)),
               farY = std::max(std::abs(top - py), std::abs(bottom - py));
    if (std::hypot(farX, farY) <= radius)
      return value;
    // Visit the nearest quadrant first for fast upper-bound termination.
    const int firstX = px >= left + float(span) / 2 ? 1 : 0,
              firstY = py >= top + float(span) / 2 ? 1 : 0;
    for (int iy = 0; iy < 2; ++iy)
      for (int ix = 0; ix < 2; ++ix) {
        best = query(level - 1, x * 2 + (firstX ^ ix), y * 2 + (firstY ^ iy),
                     px, py, radius, best);
        if (best >= possible)
          return best;
      }
    return best;
  }

public:
  explicit MaskMaxTree(const SpatialPlane &p, bool inverse) {
    levels_.push_back({p.bounds.width, p.bounds.height, p.pixels});
    if (inverse)
      for (auto &a : levels_[0].p)
        a = 1 - a;
    while (levels_.back().w > 1 || levels_.back().h > 1) {
      const auto &prev = levels_.back();
      Level next{(prev.w + 1) / 2, (prev.h + 1) / 2, {}};
      next.p.resize(std::size_t(next.w) * std::size_t(next.h));
      for (int y = 0; y < next.h; ++y)
        for (int x = 0; x < next.w; ++x)
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
              if (x * 2 + dx < prev.w && y * 2 + dy < prev.h)
                next.p[std::size_t(y) * std::size_t(next.w) + std::size_t(x)] = std::max(
                    next.p[std::size_t(y) * std::size_t(next.w) + std::size_t(x)],
                    prev.p[std::size_t(y * 2 + dy) * std::size_t(prev.w) + std::size_t(x * 2 + dx)]);
      levels_.push_back(std::move(next));
    }
  }
  float sample(int x, int y, float radius) const {
    return query(int(levels_.size()) - 1, 0, 0, float(x), float(y), radius, 0);
  }
};
SpatialPlane morph(const SpatialPlane &p, double radius, bool erode,
                   const FilterPreparationOptions &o, float outside = 0) {
  if (radius <= 0)
    return p;
  MaskMaxTree tree(p, erode);
  SpatialPlane result{p.bounds, 1, std::vector<float>(p.pixels.size())};
  for (int y = 0; y < p.bounds.height; ++y) {
    if (y % 16 == 0)
      cancellation(o);
    for (int x = 0; x < p.bounds.width; ++x) {
      float value = tree.sample(x, y, float(radius));
      // Ignoring pixel alpha must not manufacture an edge at the work buffer.
      const auto edge = std::min({x + 1, y + 1, p.bounds.width - x, p.bounds.height - y});
      value = std::max(value, std::min(erode ? 1 - outside : outside,
          std::clamp(float(radius) + 1.0F - float(edge), 0.0F, 1.0F)));
      if (erode) value = 1 - value;
      result.pixels[std::size_t(y) * std::size_t(p.bounds.width) + std::size_t(x)] = value;
    }
  }
  return result;
}
SpatialPlane soften(SpatialPlane p, double radius,
                    const FilterPreparationOptions &o, float outside = 0) {
  if (radius <= 0)
    return p;
  auto filter = defaultSpatialFilter(SpatialFilterType::Gaussian);
  filter.enabled = true;
  filter.parameters = GaussianBlurParameters{radius, radius};
  SpatialFilterOptions options;
  options.maxWorkingBytes = o.byteBudget;
  options.cancelled = o.cancelled;
  // Gaussian convolution is linear. Filtering deviations from the constant
  // exterior uses the existing zero-boundary filter without a false frame edge.
  if (outside != 0) for (auto& value : p.pixels) value -= outside;
  auto result = filterSpatialRegion(p.view(), p.bounds, filter, options);
  require(bool(result), result.error.c_str());
  if (outside != 0) for (auto& value : result.output.pixels) value += outside;
  return std::move(result.output);
}
std::shared_ptr<const LayerEffectMask> store(const SpatialPlane& plane,
                                             const AffineTransform &mapping) {
  std::vector<std::uint8_t> values(plane.pixels.size());
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = alphaToByte(plane.pixels[i]);
  AffineTransform offset;
  offset.m02 = plane.bounds.x;
  offset.m12 = plane.bounds.y;
  const auto toLocal = composeAffine(mapping, offset);
  const auto coverage = SelectionMask::fromR8(
      {std::uint32_t(plane.bounds.width), std::uint32_t(plane.bounds.height)},
      values, std::size_t(plane.bounds.width));
  const auto nonzero = coverage->bounds();
  // Bilinear coverage has a half-texel fringe outside the nonzero cell box.
  const double fringe=nonzero.empty()?0:.5;
  const auto lo = toLocal.map({double(nonzero.x)-fringe, double(nonzero.y)-fringe}),
             hi = toLocal.map(
                 {double(nonzero.right())+fringe, double(nonzero.bottom())+fringe});
  return std::make_shared<const LayerEffectMask>(LayerEffectMask{
      coverage, *toLocal.inverted(), {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y}});
}
#include "BevelRelief.inc"
} // namespace
LayerEffect defaultLayerEffect(LayerEffectType type) {
  LayerEffect e;
  if (type == LayerEffectType::DropShadow ||
      type == LayerEffectType::InnerShadow) {
    e.color = {0, 0, 0, 255};
    e.blendMode = BlendMode::Multiply;
    e.opacity = .65;
  }
  if (type == LayerEffectType::OuterGlow ||
      type == LayerEffectType::InnerGlow) {
    e.color = {255, 225, 150, 255};
    e.blendMode = BlendMode::Screen;
    e.opacity = .75;
    e.size = 12;
  }
  if(type==LayerEffectType::BevelEmboss) {
    e.blendMode=BlendMode::Screen;e.opacity=.65;e.angle=-135;
  }
  return e;
}
LayerEffectStack::LayerEffectStack() {
  for (std::size_t i = 0; i < items.size(); ++i)
    items[i] = defaultLayerEffect(LayerEffectType(i));
}
std::vector<GradientStop> effectiveGradientStops(const LayerEffect &effect) {
  if (!effect.gradientStops.empty()) return effect.gradientStops;
  return {{0, effect.color}, {1, effect.secondColor}};
}
PremultipliedColor sampleLayerEffectGradient(const LayerEffect &effect, double position) noexcept {
  const float t = std::isfinite(position) ? float(std::clamp(position, 0., 1.)) : 0.F;
  const auto mix = [](Rgba8 first, Rgba8 second, float amount) {
    const auto a = decodeColor(first), b = decodeColor(second);
    PremultipliedColor result;
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = a[i] * (1 - amount) + b[i] * amount;
    return result;
  };
  const auto &stops = effect.gradientStops;
  if (stops.empty()) return mix(effect.color, effect.secondColor, t);
  // Compare the same float positions used by the GPU, including stops so close
  // that they coincide after compilation. upper_bound gives deterministic hard
  // edges without dividing by a zero-width segment.
  const auto next = std::upper_bound(stops.begin(), stops.end(), t,
      [](float value, const GradientStop &stop) { return value < float(stop.position); });
  if (next == stops.begin()) return decodeColor(stops.front().color);
  if (next == stops.end()) return decodeColor(stops.back().color);
  const auto &previous = *(next - 1);
  return mix(previous.color, next->color,
      std::clamp((t - float(previous.position)) /
          (float(next->position) - float(previous.position)), 0.F, 1.F));
}
std::string_view layerEffectName(LayerEffectType t) noexcept {
  return names[std::size_t(t)];
}
std::string_view layerEffectIdentifier(LayerEffectType t) noexcept {
  return ids[std::size_t(t)];
}
std::optional<LayerEffectType>
layerEffectFromIdentifier(std::string_view value) noexcept {
  for (std::size_t i = 0; i < ids.size(); ++i)
    if (ids[i] == value)
      return LayerEffectType(i);
  return {};
}
bool validLayerEffects(const LayerEffectStack &s) noexcept {
  if (s.algorithmVersion != layerEffectVersion)
    return false;
  for (std::size_t effect = 0; effect < s.items.size(); ++effect) {
    const auto &e = s.items[effect];
    if (!e.gradientStops.empty()) {
      if (effect != std::size_t(LayerEffectType::GradientOverlay) ||
          e.gradientStops.size() < 2 || e.gradientStops.size() > maximumGradientStops)
        return false;
      double previous = -1;
      for (const auto &stop : e.gradientStops) {
        if (!std::isfinite(stop.position) || stop.position < 0 || stop.position > 1 ||
            stop.position < previous) return false;
        previous = stop.position;
      }
    }
    const auto& b=e.bevel;
    if(b.version!=1 || std::uint32_t(b.style)>2 || !std::isfinite(b.depth) ||
       b.depth<0 || b.depth>10 || !std::isfinite(b.soften) || b.soften<0 || b.soften>64 ||
       !std::isfinite(b.altitude) || b.altitude<0 || b.altitude>90 ||
       !std::isfinite(b.shadowOpacity) || b.shadowOpacity<0 || b.shadowOpacity>1 ||
       std::find(allBlendModes.begin(),allBlendModes.end(),b.shadowBlend)==allBlendModes.end() ||
       !validEffectContour(b.surface) || !validEffectContour(b.gloss))return false;
    for (double v : {e.opacity, e.size, e.angle, e.distance, e.spread, e.scale})
      if (!std::isfinite(v))
        return false;
    if (std::find(allBlendModes.begin(), allBlendModes.end(), e.blendMode) ==
            allBlendModes.end() ||
        e.opacity < 0 || e.opacity > 1 || e.size < 0 ||
        e.size > maximumEffectSize || e.angle < -180 || e.angle > 180 ||
        e.distance < 0 || e.distance > maximumEffectDistance || e.spread < 0 ||
        e.spread > 1 || e.scale < .01 || e.scale > 10 ||
        std::uint32_t(e.position) > 2 || std::uint32_t(e.gradient) > 1)
      return false;
  }
  return true;
}
bool equivalentLayerEffects(const LayerEffectState &a,
                            const LayerEffectState &b) noexcept {
  return a == b ||
         (a ? *a : LayerEffectStack{}) == (b ? *b : LayerEffectStack{});
}
bool hasDissolveLayerEffects(const LayerEffectState &s) noexcept {
  if(!s)return false;
  for(const auto& e:s->items)if(e.enabled&&e.blendMode==BlendMode::Dissolve)return true;
  return s->items[7].enabled&&s->items[7].bevel.shadowBlend==BlendMode::Dissolve;
}
bool hasActiveLayerEffects(const LayerEffectState &s) noexcept {
  if(!s)return false;
  for(std::size_t i=0;i<layerEffectCount;++i)
    if(i==7 ? bevelActive(s->items[i]) : s->items[i].enabled)return true;
  return false;
}
bool layerEffectCacheValid(const Layer &l) noexcept { return cacheValid(l); }
bool equivalentLayerEffectGeometry(const LayerEffectState &a,
                                   const LayerEffectState &b) noexcept {
  if (a == b)
    return true;
  if (hasActiveLayerEffects(a) != hasActiveLayerEffects(b))
    return false;
  const LayerEffectStack empty;
  const auto &x = a ? *a : empty;
  const auto &y = b ? *b : empty;
  if (x.algorithmVersion != y.algorithmVersion ||
      x.pixelAlphaShapesEffects != y.pixelAlphaShapesEffects ||
      x.maskEffectsAfterRendering != y.maskEffectsAfterRendering)
    return false;
  for (std::size_t i = 0; i < 5; ++i)
    if (x.items[i].enabled != y.items[i].enabled ||
        (x.items[i].enabled && !geometryEqual(x.items[i], y.items[i], i)))
      return false;
  if(bevelActive(x.items[7])!=bevelActive(y.items[7]) ||
      (bevelActive(x.items[7])&&!geometryEqual(x.items[7],y.items[7],7)))return false;
  return true;
}
bool layerEffectCacheValid(const LayerSnapshot &l) noexcept {
  return cacheValid(l);
}
std::shared_ptr<const LayerEffectCache>
prepareLayerEffects(const Layer &layer, const FilterPreparationOptions &o) {
  if (!hasActiveLayerEffects(layer.effects))
    return {};
  if (layerEffectCacheValid(layer))
    return layer.effectCache;
  require(validLayerEffects(*layer.effects), "Invalid layer effects");
  cancellation(o);
  const auto source = renderedSurface(layer);
  require(bool(source), "Effect source is not prepared");
  const auto mapping = renderPixelsToLocal(layer);
  require(mapping.m01 == 0 && mapping.m10 == 0 && mapping.m00 > 0 &&
              mapping.m00 == mapping.m11,
          "Effects require a canonical local source");
  const double density = 1 / mapping.m00;
  require(density <= 8, "Effect source density exceeds limit");
  auto result = std::make_shared<LayerEffectCache>();
  result->sourceId = source->id();
  result->sourceRevision = source->revision();
  result->pixelsToLocal = mapping;
  result->geometry = layer.effects;
  result->geometryMask = shapeMask(layer);
  result->geometryMaskRevision = result->geometryMask ? result->geometryMask->coverage->revision() : 0;
  result->paintFrame = layerEffectReferenceFrame(layer);
  const bool reuse = layer.effectCache &&
                     layer.effectCache->sourceId == source->id() &&
                     layer.effectCache->sourceRevision == source->revision() &&
                     layer.effectCache->pixelsToLocal == mapping &&
                     silhouetteMatches(*layer.effectCache, layer);
  if (reuse) {
    result->masks = layer.effectCache->masks;
    result->bevelDistance=layer.effectCache->bevelDistance;
    result->bevelNormals=layer.effectCache->bevelNormals;
    result->bevelPrepared=layer.effectCache->bevelPrepared;
  }
  const auto e = source->extent();
  const bool pixelAlpha = layer.effects->pixelAlphaShapesEffects;
  const float outside = pixelAlpha ? 0 : result->geometryMask ? float(result->geometryMask->outside) / 255 : 1;
  RectI domain{0, 0, int(e.width), int(e.height)};
  if (!pixelAlpha && result->geometryMask) {
    // A mask can define geometry beyond the original pixels. Include its real
    // transformed domain, without introducing a boundary at the image extent.
    const auto maskToLocal = result->geometryMask->localToMask.inverted();
    const auto localToSource = mapping.inverted();
    const auto extent = result->geometryMask->coverage->extent();
    double left = 0, top = 0, right = e.width, bottom = e.height;
    for (const auto p : std::array{Vec2d{0,0}, Vec2d{double(extent.width),0},
                                  Vec2d{0,double(extent.height)}, Vec2d{double(extent.width),double(extent.height)}}) {
      const auto q = localToSource->map(maskToLocal->map(p));
      require(std::isfinite(q.x) && std::isfinite(q.y), "Invalid effect mask mapping");
      left = std::min(left, std::floor(q.x)); top = std::min(top, std::floor(q.y));
      right = std::max(right, std::ceil(q.x)); bottom = std::max(bottom, std::ceil(q.y));
    }
    require(right-left <= 32768 && bottom-top <= 32768,
            "Effect mask domain exceeds supported dimensions");
    domain = {int(left), int(top), int(right-left), int(bottom-top)};
  }
  double margin = 1;
  for (std::size_t i = 0; i < 5; ++i)
    if (layer.effects->items[i].enabled) {
      const auto &effect = layer.effects->items[i];
      margin = std::max(
          margin,
          (effect.size + (i == 1 || i == 2 ? effect.distance : 0)) * density +
              2);
    }
  const auto& bevel=layer.effects->items[7];
  if(bevelActive(bevel))margin=std::max(margin,(bevel.size+bevel.bevel.soften)*density+4);
  const int padding = int(std::ceil(margin));
  RectI bounds{domain.x-padding, domain.y-padding, domain.width+padding*2, domain.height+padding*2};
  const auto count = std::uint64_t(bounds.width) * std::uint64_t(bounds.height);
  require(
      bounds.width <= 32768 && bounds.height <= 32768 &&
          count <= o.byteBudget / 32,
      "Effects exceed working-memory budget; reduce size or source dimensions");
  result->workingBytes = std::size_t(count) * 32;
  std::optional<SpatialPlane> silhouette;
  const auto input = [&]() -> const SpatialPlane & {
    if (!silhouette) {
      silhouette = SpatialPlane{bounds, 1, std::vector<float>(std::size_t(count), pixelAlpha ? 0.F : 1.F)};
      std::vector<std::byte> row(std::size_t(e.width) * 4);
      if (pixelAlpha) for (std::uint32_t y = 0; y < e.height; ++y) {
        if (y % 32 == 0)
          cancellation(o);
        source->copyRgba8({0, int(y), int(e.width), 1}, row, row.size());
        for (std::uint32_t x = 0; x < e.width; ++x)
          silhouette
              ->pixels[std::size_t(int(y) - bounds.y) * std::size_t(bounds.width) +
                       std::size_t(int(x) - bounds.x)] =
              float(std::to_integer<unsigned>(row[std::size_t(x) * 4 + 3])) /
              255;
      }
      if (result->geometryMask) for (int y = 0; y < bounds.height; ++y) {
        if (y % 32 == 0) cancellation(o);
        for (int x = 0; x < bounds.width; ++x)
          silhouette->pixels[std::size_t(y) * std::size_t(bounds.width) + std::size_t(x)] *=
              layerMaskCoverage(result->geometryMask, mapping.map({bounds.x + x + .5, bounds.y + y + .5}));
      }
    }
    return *silhouette;
  };
  for (std::size_t i = 0; i < 5; ++i) {
    const auto &effect = layer.effects->items[i];
    if (!effect.enabled)
      continue;
    if (reuse && layer.effectCache->geometry->items[i].enabled &&
        geometryEqual(effect, layer.effectCache->geometry->items[i], i))
      continue;
    const auto &alpha = input();
    const double size = effect.size * density;
    if (i == 0) {
      const auto outerRadius = effect.position == StrokePosition::Inside ? 0
                           : effect.position == StrokePosition::Center
                               ? size / 2
                               : size;
      const auto inside = effect.position == StrokePosition::Outside  ? 0
                          : effect.position == StrokePosition::Center ? size / 2
                                                                      : size;
      auto outer = morph(alpha, outerRadius, false, o, outside),
           inner = morph(alpha, inside, true, o, outside);
      for (std::size_t p = 0; p < alpha.pixels.size(); ++p) {
        outer.pixels[p] = std::max(0.0F, outer.pixels[p] - alpha.pixels[p]);
        inner.pixels[p] = alpha.pixels[p] > 0
                              ? std::clamp((alpha.pixels[p] - inner.pixels[p]) /
                                               alpha.pixels[p],
                                           0.0F, 1.0F)
                              : 0;
      }
      result->masks[0] = store(std::move(outer), mapping);
      result->masks[1] = store(std::move(inner), mapping);
    } else {
      const bool inner = i == 2 || i == 4;
      auto mask = soften(morph(alpha, size * effect.spread, inner, o, outside),
                         size * (1 - effect.spread), o, outside);
      const auto radians = effect.angle * std::numbers::pi / 180;
      const double dx = (i == 1 || i == 2)
                            ? std::cos(radians) * effect.distance * density
                            : 0,
                   dy = (i == 1 || i == 2)
                            ? std::sin(radians) * effect.distance * density
                            : 0;
      SpatialPlane shifted{bounds, 1, std::vector<float>(std::size_t(count))};
      for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        if ((y - bounds.y) % 32 == 0)
          cancellation(o);
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
          const auto sx = double(x) - dx, sy = double(y) - dy;
          const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
          const float fx = float(sx - ix), fy = float(sy - iy);
          const float value = std::lerp(
              std::lerp(at(mask, ix, iy, outside), at(mask, ix + 1, iy, outside), fx),
              std::lerp(at(mask, ix, iy + 1, outside), at(mask, ix + 1, iy + 1, outside), fx),
              fy);
          const float base = at(alpha, x, y);
          shifted
              .pixels[std::size_t(y - bounds.y) * std::size_t(bounds.width) + std::size_t(x - bounds.x)] =
              inner ? (base > 0 ? std::clamp((base - value) / base, 0.0F, 1.0F)
                                : 0)
              // Without pixel-alpha shaping the shape, not the pixels, occludes
              // its shadow. A constant opaque exterior cannot cast a shadow of
              // the arbitrary scratch-buffer rectangle into the document.
              : (i == 3 || !pixelAlpha) ? std::max(0.0F, value - base)
                       : value;
        }
      }
      result->masks[i + 1] = store(std::move(shifted), mapping);
    }
    if (o.progress)
      o.progress(double(i + 1) / 5);
  }
  if(bevelActive(bevel)) {
    const auto* previous=reuse?layer.effectCache.get():nullptr;
    const int p=int(std::ceil((bevel.size+bevel.bevel.soften)*density+4));
    const RectI required{domain.x-p,domain.y-p,domain.width+2*p,domain.height+2*p};
    if(previous&&previous->bevelDistance&&previous->bevelDistance->bounds.x<=required.x&&
       previous->bevelDistance->bounds.y<=required.y&&previous->bevelDistance->bounds.right()>=required.right()&&
       previous->bevelDistance->bounds.bottom()>=required.bottom())
      prepareBevel(*result,bevel,SpatialPlane{required,1,{}},mapping,previous,o,outside);
    else prepareBevel(*result,bevel,input(),mapping,previous,o,outside);
  }
  const auto a = mapping.map({0, 0}),
             b = mapping.map({double(e.width), double(e.height)});
  result->visualBounds = {a.x, a.y, b.x - a.x, b.y - a.y};
  if (!pixelAlpha && !result->paintFrame.empty()) {
    auto& v = result->visualBounds;
    const auto& frame = result->paintFrame;
    const double right = std::max(v.right(), frame.right()), bottom = std::max(v.bottom(), frame.bottom());
    v.x = std::min(v.x,frame.x); v.y = std::min(v.y,frame.y);
    v.width = right-v.x; v.height = bottom-v.y;
  }
  for (auto i : {0, 2, 4})
    if (layer.effects->items[std::size_t(i == 0 ? 0 : i - 1)].enabled &&
        result->masks[std::size_t(i)]) {
      const auto &r = result->masks[std::size_t(i)]->localBounds;
      if(r.empty())continue;
      auto &v = result->visualBounds;
      const double right = std::max(v.x + v.width, r.x + r.width),
                   bottom = std::max(v.y + v.height, r.y + r.height);
      v.x = std::min(v.x, r.x);
      v.y = std::min(v.y, r.y);
      v.width = right - v.x;
      v.height = bottom - v.y;
    }
  if(bevelActive(bevel))for(auto i:{8,9}) {
    if(!result->masks[std::size_t(i)])continue;
    const auto& r=result->masks[std::size_t(i)]->localBounds;if(r.empty())continue;
    auto& v=result->visualBounds;
    const double right=std::max(v.x+v.width,r.x+r.width),bottom=std::max(v.y+v.height,r.y+r.height);
    v.x=std::min(v.x,r.x);v.y=std::min(v.y,r.y);v.width=right-v.x;v.height=bottom-v.y;
  }
  require(source->revision() == result->sourceRevision,
          "Effect source changed while preparing");
  return result;
}
float sampleEffectMask(const LayerEffectMask &m, Vec2d local) noexcept {
  const auto p = m.localToMask.map(local) - Vec2d{.5, .5};
  const auto e = m.coverage->extent();
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < -1 || p.y < -1 ||
      p.x >= e.width || p.y >= e.height)
    return 0;
  const auto x = int(std::floor(p.x)), y = int(std::floor(p.y));
  const float fx = float(p.x - x), fy = float(p.y - y);
  return std::lerp(
             std::lerp(float(m.coverage->coverageAtDocumentPixel(x, y)),
                       float(m.coverage->coverageAtDocumentPixel(x + 1, y)),
                       fx),
             std::lerp(float(m.coverage->coverageAtDocumentPixel(x, y + 1)),
                       float(m.coverage->coverageAtDocumentPixel(x + 1, y + 1)),
                       fx),
             fy) /
         255;
}
EffectParameters compileLayerEffects(const LayerEffectState &state,
                                     RectD frame, std::uint32_t seed) {
  EffectParameters p{};
  p[3]=float(seed&65535U);p[19]=float(seed>>16);
  p[20]=hasDissolveLayerEffects(state)?1.F:0.F;
  p[21]=state && state->maskEffectsAfterRendering ? 1.F : 0.F;
  p[22]=!state || state->pixelAlphaShapesEffects ? 1.F : 0.F;
  p[23]=float(effectGradientStopOffset);
  p[256] = float(frame.x);
  p[257] = float(frame.y);
  p[258] = float(frame.width);
  p[259] = float(frame.height);
  if (state)
    for (std::size_t n = 0; n < layerEffectCount; ++n) {
      const auto &e = state->items[n];
      const auto i = n * 32;
      p[i] = e.enabled ? 1.0F : 0.0F;
      p[i + 1] = float(e.blendMode);
      p[i + 2] = float(e.opacity);
      const auto c = decodeColor(e.color), d = decodeColor(e.secondColor);
      std::copy(c.begin(), c.end(), p.begin() + std::ptrdiff_t(i + 4));
      std::copy(d.begin(), d.end(), p.begin() + std::ptrdiff_t(i + 8));
      p[i + 12] = float(e.gradient);
      p[i + 13] = float(std::cos(e.angle * std::numbers::pi / 180));
      p[i + 14] = float(std::sin(e.angle * std::numbers::pi / 180));
      p[i + 15] = float(e.scale);
      p[i + 16] = e.reverse ? 1.0F : 0.0F;
      if (n == std::size_t(LayerEffectType::GradientOverlay) && !e.gradientStops.empty()) {
        const auto count = std::min(e.gradientStops.size(), maximumGradientStops);
        p[i + 17] = float(count);
        p[i + 18] = float(effectGradientStopOffset);
        p[23] = float(effectGradientStopOffset + count * effectGradientStopStride);
        for (std::size_t stop = 0; stop < count; ++stop) {
          const auto at = effectGradientStopOffset + stop * effectGradientStopStride;
          p[at] = float(e.gradientStops[stop].position);
          const auto color = decodeColor(e.gradientStops[stop].color);
          std::copy(color.begin(), color.end(), p.begin() + std::ptrdiff_t(at + 1));
        }
      }
      if(n==7) {
        p[i]=bevelActive(e)?1.F:0.F;
        p[i+17]=float(e.bevel.shadowBlend);p[i+18]=float(e.bevel.shadowOpacity);
      }
    }
  return p;
}
namespace {
struct Evaluation {
  const EffectParameters &parameters;
  std::array<float, layerEffectMaskCount> masks;
  int space{};
  using BVec4 = blend_detail::BVec4;
  static float bMax(float a, float b) { return std::max(a, b); }
  static float bAbs(float v) { return std::abs(v); }
  static float bSqrt(float v) { return std::sqrt(v); }
  static float bClamp(float v, float a, float b) { return std::clamp(v, a, b); }
  using BUint = std::uint32_t;
  static float bUnassociatedLinear(float c,float a) { return blend_detail::bUnassociatedLinear(c,a); }
  static bool bDissolve(float p,float x,float y,BUint seed) { return blend_detail::bDissolve(p,x,y,seed); }
  BVec4 bComposite(BVec4 a, BVec4 b, float o, int m) { return blend_detail::bCompositeSpace(a,b,o,m,space); }
  BVec4 bContribution(BVec4 a,BVec4 b,float o,int m,BVec4 e,float x,float y,BUint seed) {
    return blend_detail::bContributionSpace(a,b,o,m,e,x,y,seed,space);
  }
  BVec4 bCompositeAt(BVec4 a,BVec4 b,float o,int m,float x,float y,BUint seed,bool protect) {
    return blend_detail::bCompositeAtSpace(a,b,o,m,x,y,seed,protect,space);
  }
  static BVec4 bMixSpace(BVec4 a,BVec4 b,float t,int s) { return blend_detail::bMixSpace(a,b,t,s); }
#define E_OUTPUT BVec4&
#define E_INLINE inline
#define E_PARAM(i) parameters[std::size_t(i)]
#define E_MASK(i) masks[std::size_t(i)]
#define E_SPACE space
#include "imageeditor/core/detail/LayerEffectMath.inc"
#undef E_OUTPUT
#undef E_INLINE
#undef E_PARAM
#undef E_MASK
#undef E_SPACE
};
} // namespace
PremultipliedColor
compositeLayerEffects(PremultipliedColor backdrop, PremultipliedColor base,
                      const EffectParameters &p, const LayerEffectCache *cache,
                      Vec2d local, float opacity, BlendMode mode,
                      float cropCoverage, bool protectedCoverage, CompositingSpace space, float fillOpacity, float maskCoverage) noexcept {
  Evaluation e{p, {}, int(space)};
  if (cache)
    for (std::size_t i = 0; i < layerEffectMaskCount; ++i)
      if (cache->masks[i])
        e.masks[i] = sampleEffectMask(*cache->masks[i], local);
  const auto c =
      e.eComposite({backdrop[0], backdrop[1], backdrop[2], backdrop[3]},
                   {base[0], base[1], base[2], base[3]}, float(local.x),
                   float(local.y), opacity, int(mode), cropCoverage, protectedCoverage, fillOpacity, maskCoverage);
  return {c.x, c.y, c.z, c.w};
}
} // namespace imageeditor::core
