// Implementation shared with the existing effect preparation helpers, not a
// second source renderer. All coordinates below are canonical source texels.
std::shared_ptr<const BevelDistanceCache>
bevelDistance(const SpatialPlane &a, const FilterPreparationOptions &options) {
  auto d = std::make_shared<BevelDistanceCache>();
  d->bounds = a.bounds;
  const int w = a.bounds.width, h = a.bounds.height;
  const auto count = a.pixels.size();
  d->distance.resize(count);
  d->alpha.resize(count);
  d->edgeAlpha.resize(count);
  constexpr float infinity = 1e18F;
  // Subpixel boundary seeds. Normalize by the local peak so a uniformly
  // translucent silhouette has the same geometry as an opaque one. Retain
  // unnormalized alpha separately for exterior coverage, never as height RGB.
  for (int y = 0; y < h; ++y) {
    cancellation(options);
    for (int x = 0; x < w; ++x) {
      const auto i = std::size_t(y) * std::size_t(w) + std::size_t(x);
      const float alpha = a.pixels[i];
      d->alpha[i] = alphaToByte(alpha);
      float peak = alpha;
      bool boundary = false;
      for (const auto offset :
           std::array<Vec2d, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
        const float n = at(a, a.bounds.x + x + int(offset.x),
                           a.bounds.y + y + int(offset.y));
        peak = std::max(peak, n);
        boundary |= (alpha > 0) != (n > 0);
      }
      if (boundary)
        for (int dy = -2; dy <= 2; ++dy)
          for (int dx = -2; dx <= 2; ++dx)
            peak =
                std::max(peak, at(a, a.bounds.x + x + dx, a.bounds.y + y + dy));
      const float seed =
          alpha > 0 ? std::clamp(alpha / std::max(peak, 1e-6F) * .5F, .01F, .5F)
                    : .5F;
      d->distance[i] = boundary ? seed * seed : infinity;
      d->edgeAlpha[i] = boundary ? alphaToByte(peak) : 0;
    }
  }
  // Lower envelope of parabolas: separable squared Euclidean distance to
  // weighted boundary seeds. O(width*height), independent of bevel width.
  const int length = std::max(w, h);
  std::vector<float> f(std::size_t(length), 0);
  std::vector<std::uint8_t> alpha(std::size_t(length), 0);
  std::vector<int> sites(std::size_t(length), 0);
  std::vector<double> cuts(std::size_t(length) + 1, 0);
  const auto line = [&](std::size_t start, std::size_t stride, int n) {
    int last = -1;
    for (int q = 0; q < n; ++q) {
      f[std::size_t(q)] = d->distance[start + std::size_t(q) * stride];
      alpha[std::size_t(q)] = d->edgeAlpha[start + std::size_t(q) * stride];
      if (f[std::size_t(q)] >= infinity)
        continue;
      double cut = -1e30;
      while (last >= 0) {
        const int p = sites[std::size_t(last)];
        cut = (double(f[std::size_t(q)]) + double(q) * q -
               double(f[std::size_t(p)]) - double(p) * p) /
              (2. * (q - p));
        if (cut > cuts[std::size_t(last)])
          break;
        --last;
      }
      ++last;
      sites[std::size_t(last)] = q;
      cuts[std::size_t(last)] = last ? cut : -1e30;
      cuts[std::size_t(last) + 1] = 1e30;
    }
    if (last < 0)
      return;
    int site = 0;
    for (int q = 0; q < n; ++q) {
      while (site < last && cuts[std::size_t(site) + 1] < q)
        ++site;
      const int p = sites[std::size_t(site)];
      const auto i = start + std::size_t(q) * stride;
      const float offset = float(q - p);
      d->distance[i] = offset * offset + f[std::size_t(p)];
      d->edgeAlpha[i] = alpha[std::size_t(p)];
    }
  };
  for (int y = 0; y < h; ++y) {
    if (y % 32 == 0)
      cancellation(options);
    line(std::size_t(y) * std::size_t(w), 1, w);
  }
  for (int x = 0; x < w; ++x) {
    if (x % 32 == 0)
      cancellation(options);
    line(std::size_t(x), std::size_t(w), h);
  }
  for (std::size_t i = 0; i < count; ++i)
    d->distance[i] = std::sqrt(d->distance[i]) * (d->alpha[i] ? 1.F : -1.F);
  return d;
}
std::shared_ptr<const BevelNormalCache>
bevelNormals(const BevelDistanceCache &d, const LayerEffect &e, double density,
             const FilterPreparationOptions &options) {
  SpatialPlane height{d.bounds, 1, std::vector<float>(d.distance.size())};
  const double width = e.size * density;
  const double start = e.bevel.style == BevelStyle::Inner   ? 0
                       : e.bevel.style == BevelStyle::Outer ? -width
                                                            : -width / 2;
  // Integrate four stratified profile samples across a source texel. Narrow
  // nonmonotonic contours do not become a single unstable point sample.
  for (std::size_t i = 0; i < height.pixels.size(); ++i) {
    if (i % 65536 == 0)
      cancellation(options);
    double value = 0;
    for (int tap = 0; tap < 4; ++tap) {
      const double t = std::clamp(
          (d.distance[i] + (tap + .5) / 4 - .5 - start) / width, 0., 1.);
      // Smooth technique has zero derivative at its two support endpoints.
      const double shaped = evaluateEffectContour(e.bevel.surface, t);
      value += shaped * shaped * (3 - 2 * shaped);
    }
    height.pixels[i] =
        float(value * .25 * width * e.bevel.depth * (e.bevel.down ? -1 : 1));
  }
  // Existing Gaussian implementation: one texel sigma reconstruction plus
  // user Soften, exclusively on derived relief, never on source pixels.
  height = soften(std::move(height), 3 + e.bevel.soften * density, options);
  auto n = std::make_shared<BevelNormalCache>();
  n->xy.resize(height.pixels.size());
  for (int y = 0; y < d.bounds.height; ++y) {
    if (y % 32 == 0)
      cancellation(options);
    for (int x = 0; x < d.bounds.width; ++x) {
      const int px = x + d.bounds.x, py = y + d.bounds.y;
      const double dx = (at(height, px + 1, py) - at(height, px - 1, py)) * .5;
      const double dy = (at(height, px, py + 1) - at(height, px, py - 1)) * .5;
      const double inverse = 1 / std::sqrt(1 + dx * dx + dy * dy);
      n->xy[std::size_t(y) * std::size_t(d.bounds.width) + std::size_t(x)] = {
          std::int16_t(std::lround(-dx * inverse * 32767)),
          std::int16_t(std::lround(-dy * inverse * 32767))};
    }
  }
  return n;
}
void prepareBevel(LayerEffectCache &result, const LayerEffect &e,
                  const SpatialPlane &alpha, const AffineTransform &mapping,
                  const LayerEffectCache *previous,
                  const FilterPreparationOptions &options) {
  const double density = 1 / mapping.m00;
  auto stageStart = std::chrono::steady_clock::now();
  const auto elapsed = [&] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - stageStart)
        .count();
  };
  if (previous && previous->bevelDistance &&
      previous->bevelDistance->bounds.x <= alpha.bounds.x &&
      previous->bevelDistance->bounds.y <= alpha.bounds.y &&
      previous->bevelDistance->bounds.right() >= alpha.bounds.right() &&
      previous->bevelDistance->bounds.bottom() >= alpha.bounds.bottom())
    result.bevelDistance = previous->bevelDistance;
  else {
    result.bevelDistance = bevelDistance(alpha, options);
    result.bevelDistanceMilliseconds = elapsed();
  }
  stageStart = std::chrono::steady_clock::now();
  if (previous && previous->bevelNormals && previous->bevelPrepared &&
      result.bevelDistance == previous->bevelDistance &&
      normalsEqual(e, previous->bevelPrepared->items[7]))
    result.bevelNormals = previous->bevelNormals;
  else {
    result.bevelNormals =
        bevelNormals(*result.bevelDistance, e, density, options);
    result.bevelNormalMilliseconds = elapsed();
  }
  stageStart = std::chrono::steady_clock::now();
  if (previous && previous->bevelPrepared &&
      result.bevelNormals == previous->bevelNormals &&
      geometryEqual(e, previous->bevelPrepared->items[7], 7))
    return;
  const auto &d = *result.bevelDistance;
  const auto &n = *result.bevelNormals;
  const double angle = e.angle * std::numbers::pi / 180,
               altitude = e.bevel.altitude * std::numbers::pi / 180;
  const double lx = std::cos(angle) * std::cos(altitude),
               ly = std::sin(angle) * std::cos(altitude),
               lz = std::sin(altitude);
  const double width = e.size * density;
  const double start = e.bevel.style == BevelStyle::Inner   ? 0
                       : e.bevel.style == BevelStyle::Outer ? -width
                                                            : -width / 2;
  const double end = start + width, halo = 3 + e.bevel.soften * density;
  const double neutral = evaluateEffectContour(e.bevel.gloss, .5);
  SpatialPlane response{d.bounds, 1, std::vector<float>(d.distance.size())};
  for (std::size_t i = 0; i < response.pixels.size(); ++i) {
    if (i % 65536 == 0)
      cancellation(options);
    const auto xy = n.xy[i];
    if ((xy[0] == 0 && xy[1] == 0) || d.distance[i] < start - halo ||
        d.distance[i] > end + halo)
      continue;
    const double nx = xy[0] / 32767., ny = xy[1] / 32767.;
    const double nz = std::sqrt(std::max(0., 1 - nx * nx - ny * ny));
    const double delta = nx * lx + ny * ly + (nz - 1) * lz;
    response.pixels[i] =
        float(2 * (evaluateEffectContour(e.bevel.gloss,
                                         std::clamp(.5 + .5 * delta, 0., 1.)) -
                   neutral));
  }
  // Antialias the *response* too: a steep gloss remap otherwise recreates
  // subtexel transitions after the normals' reconstruction filter.
  if (e.bevel.gloss.enabled)
    response = soften(std::move(response), 1.5, options);
  SpatialPlane mask{d.bounds, 1, std::vector<float>(d.distance.size())};
  // Independent material masks keep backdrop blend modes live in CPU/GLSL.
  for (int channel = 0; channel < 4; ++channel) {
    if ((channel < 2 && e.bevel.style == BevelStyle::Outer) ||
        (channel >= 2 && e.bevel.style == BevelStyle::Inner)) {
      result.masks[std::size_t(channel) + 6].reset();
      continue;
    }
    for (std::size_t i = 0; i < response.pixels.size(); ++i) {
      if (i % 65536 == 0)
        cancellation(options);
      const double distance = d.distance[i];
      const double strength = std::clamp(
          double(channel % 2 == 0 ? response.pixels[i] : -response.pixels[i]),
          0., 1.);
      const double support =
          std::clamp((distance - start + halo) / halo, 0., 1.) *
          std::clamp((end + halo - distance) / halo, 0., 1.);
      double coverage = 1;
      if (channel < 2)
        coverage = e.bevel.style == BevelStyle::Outer ? 0 : 1;
      else
        coverage = e.bevel.style == BevelStyle::Inner
                       ? 0
                       : (d.edgeAlpha[i] / 255.) * (1 - d.alpha[i] / 255.);
      mask.pixels[i] = float(strength * support * coverage);
    }
    result.masks[std::size_t(channel) + 6] = store(mask, mapping);
  }
  result.bevelLightingMilliseconds = elapsed();
  result.bevelPrepared = result.geometry;
}
