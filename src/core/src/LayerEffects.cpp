#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <numbers>
#include <stdexcept>

namespace imageeditor::core {
namespace {
constexpr std::array<std::string_view, 7> names{
    "Stroke",     "Drop Shadow",   "Inner Shadow",    "Outer Glow",
    "Inner Glow", "Color Overlay", "Gradient Overlay"};
constexpr std::array<std::string_view, 7> ids{
    "stroke",     "drop-shadow",   "inner-shadow",    "outer-glow",
    "inner-glow", "color-overlay", "gradient-overlay"};
bool geometryEqual(const LayerEffect &a, const LayerEffect &b, std::size_t i) {
  if (i >= 5)
    return true;
  return a.size == b.size &&
         (i == 0 ? a.position == b.position : a.spread == b.spread) &&
         ((i != 1 && i != 2) ||
          (a.distance == b.distance && a.angle == b.angle));
}
template <class L> bool cacheValid(const L &l) {
  if (!hasActiveLayerEffects(l.effects))
    return true;
  const auto s = renderedSurface(l);
  const auto &c = l.effectCache;
  if (!s || !c || !c->geometry || s->id() != c->sourceId ||
      s->revision() != c->sourceRevision ||
      renderPixelsToLocal(l) != c->pixelsToLocal)
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
float at(const SpatialPlane &p, int x, int y) {
  x -= p.bounds.x;
  y -= p.bounds.y;
  return x < 0 || y < 0 || x >= p.bounds.width || y >= p.bounds.height
             ? 0
             : p.pixels[std::size_t(y) * p.bounds.width + x];
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
    const auto value = l.p[std::size_t(y) * l.w + x];
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
      next.p.resize(std::size_t(next.w) * next.h);
      for (int y = 0; y < next.h; ++y)
        for (int x = 0; x < next.w; ++x)
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
              if (x * 2 + dx < prev.w && y * 2 + dy < prev.h)
                next.p[std::size_t(y) * next.w + x] = std::max(
                    next.p[std::size_t(y) * next.w + x],
                    prev.p[std::size_t(y * 2 + dy) * prev.w + x * 2 + dx]);
      levels_.push_back(std::move(next));
    }
  }
  float sample(int x, int y, float radius) const {
    return query(int(levels_.size()) - 1, 0, 0, float(x), float(y), radius, 0);
  }
};
SpatialPlane morph(const SpatialPlane &p, double radius, bool erode,
                   const FilterPreparationOptions &o) {
  if (radius <= 0)
    return p;
  MaskMaxTree tree(p, erode);
  SpatialPlane result{p.bounds, 1, std::vector<float>(p.pixels.size())};
  for (int y = 0; y < p.bounds.height; ++y) {
    if (y % 16 == 0)
      cancellation(o);
    for (int x = 0; x < p.bounds.width; ++x) {
      float value = tree.sample(x, y, float(radius));
      if (erode) {
        // Outside the allocated padded plane is also transparent.
        const auto edge =
            std::min({x + 1, y + 1, p.bounds.width - x, p.bounds.height - y});
        value = std::max(
            value, std::clamp(float(radius) + 1.0F - float(edge), 0.0F, 1.0F));
        value = 1 - value;
      }
      result.pixels[std::size_t(y) * p.bounds.width + x] = value;
    }
  }
  return result;
}
SpatialPlane soften(SpatialPlane p, double radius,
                    const FilterPreparationOptions &o) {
  if (radius <= 0)
    return p;
  auto filter = defaultSpatialFilter(SpatialFilterType::Gaussian);
  filter.enabled = true;
  filter.parameters = GaussianBlurParameters{radius, radius};
  SpatialFilterOptions options;
  options.maxWorkingBytes = o.byteBudget;
  options.cancelled = o.cancelled;
  auto result = filterSpatialRegion(p.view(), p.bounds, filter, options);
  require(bool(result), result.error.c_str());
  return std::move(result.output);
}
std::shared_ptr<const LayerEffectMask> store(SpatialPlane plane,
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
  return e;
}
LayerEffectStack::LayerEffectStack() {
  for (std::size_t i = 0; i < items.size(); ++i)
    items[i] = defaultLayerEffect(LayerEffectType(i));
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
  for (const auto &e : s.items) {
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
bool hasActiveLayerEffects(const LayerEffectState &s) noexcept {
  return s && std::any_of(s->items.begin(), s->items.end(),
                          [](const auto &e) { return e.enabled; });
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
  if (x.algorithmVersion != y.algorithmVersion)
    return false;
  for (std::size_t i = 0; i < 5; ++i)
    if (x.items[i].enabled != y.items[i].enabled ||
        (x.items[i].enabled && !geometryEqual(x.items[i], y.items[i], i)))
      return false;
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
  const bool reuse = layer.effectCache &&
                     layer.effectCache->sourceId == source->id() &&
                     layer.effectCache->sourceRevision == source->revision() &&
                     layer.effectCache->pixelsToLocal == mapping;
  if (reuse)
    result->masks = layer.effectCache->masks;
  const auto e = source->extent();
  double margin = 1;
  for (std::size_t i = 0; i < 5; ++i)
    if (layer.effects->items[i].enabled) {
      const auto &effect = layer.effects->items[i];
      margin = std::max(
          margin,
          (effect.size + (i == 1 || i == 2 ? effect.distance : 0)) * density +
              2);
    }
  const int padding = int(std::ceil(margin));
  RectI bounds{-padding, -padding, int(e.width) + padding * 2,
               int(e.height) + padding * 2};
  const auto count = std::uint64_t(bounds.width) * std::uint64_t(bounds.height);
  require(
      bounds.width <= 32768 && bounds.height <= 32768 &&
          count <= o.byteBudget / 32,
      "Effects exceed working-memory budget; reduce size or source dimensions");
  result->workingBytes = std::size_t(count) * 32;
  std::optional<SpatialPlane> silhouette;
  const auto input = [&]() -> const SpatialPlane & {
    if (!silhouette) {
      silhouette =
          SpatialPlane{bounds, 1, std::vector<float>(std::size_t(count))};
      std::vector<std::byte> row(std::size_t(e.width) * 4);
      for (std::uint32_t y = 0; y < e.height; ++y) {
        if (y % 32 == 0)
          cancellation(o);
        source->copyRgba8({0, int(y), int(e.width), 1}, row, row.size());
        for (std::uint32_t x = 0; x < e.width; ++x)
          silhouette
              ->pixels[std::size_t(y + std::uint32_t(padding)) * bounds.width +
                       x + std::uint32_t(padding)] =
              float(std::to_integer<unsigned>(row[std::size_t(x) * 4 + 3])) /
              255;
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
      const auto outside = effect.position == StrokePosition::Inside ? 0
                           : effect.position == StrokePosition::Center
                               ? size / 2
                               : size;
      const auto inside = effect.position == StrokePosition::Outside  ? 0
                          : effect.position == StrokePosition::Center ? size / 2
                                                                      : size;
      auto outer = morph(alpha, outside, false, o),
           inner = morph(alpha, inside, true, o);
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
      auto mask = soften(morph(alpha, size * effect.spread, inner, o),
                         size * (1 - effect.spread), o);
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
              std::lerp(at(mask, ix, iy), at(mask, ix + 1, iy), fx),
              std::lerp(at(mask, ix, iy + 1), at(mask, ix + 1, iy + 1), fx),
              fy);
          const float base = at(alpha, x, y);
          shifted
              .pixels[std::size_t(y - bounds.y) * bounds.width + x - bounds.x] =
              inner ? (base > 0 ? std::clamp((base - value) / base, 0.0F, 1.0F)
                                : 0)
              : i == 3 ? std::max(0.0F, value - base)
                       : value;
        }
      }
      result->masks[i + 1] = store(std::move(shifted), mapping);
    }
    if (o.progress)
      o.progress(double(i + 1) / 5);
  }
  const auto a = mapping.map({0, 0}),
             b = mapping.map({double(e.width), double(e.height)});
  result->visualBounds = {a.x, a.y, b.x - a.x, b.y - a.y};
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
                                     RectD frame) {
  EffectParameters p{};
  p[224] = float(frame.x);
  p[225] = float(frame.y);
  p[226] = float(frame.width);
  p[227] = float(frame.height);
  if (state)
    for (std::size_t n = 0; n < 7; ++n) {
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
    }
  return p;
}
namespace {
struct Evaluation {
  const EffectParameters &parameters;
  std::array<float, 6> masks;
  using BVec4 = blend_detail::BVec4;
  static float bMax(float a, float b) { return std::max(a, b); }
  static float bAbs(float v) { return std::abs(v); }
  static float bSqrt(float v) { return std::sqrt(v); }
  static float bClamp(float v, float a, float b) { return std::clamp(v, a, b); }
  static BVec4 bComposite(BVec4 a, BVec4 b, float o, int m) {
    return blend_detail::bComposite(a, b, o, m);
  }
#define E_INLINE inline
#define E_PARAM(i) parameters[std::size_t(i)]
#define E_MASK(i) masks[std::size_t(i)]
#include "imageeditor/core/detail/LayerEffectMath.inc"
#undef E_INLINE
#undef E_PARAM
#undef E_MASK
};
} // namespace
PremultipliedColor
compositeLayerEffects(PremultipliedColor backdrop, PremultipliedColor base,
                      const EffectParameters &p, const LayerEffectCache *cache,
                      Vec2d local, float opacity, BlendMode mode,
                      float cropCoverage) noexcept {
  Evaluation e{p, {}};
  if (cache)
    for (std::size_t i = 0; i < 6; ++i)
      if (cache->masks[i])
        e.masks[i] = sampleEffectMask(*cache->masks[i], local);
  const auto c =
      e.eComposite({backdrop[0], backdrop[1], backdrop[2], backdrop[3]},
                   {base[0], base[1], base[2], base[3]}, float(local.x),
                   float(local.y), opacity * cropCoverage, int(mode));
  return {c.x, c.y, c.z, c.w};
}
} // namespace imageeditor::core
