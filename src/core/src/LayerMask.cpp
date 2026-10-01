#include "imageeditor/core/LayerMask.hpp"
#include <cmath>

namespace imageeditor::core {
bool validLayerMask(const LayerMaskState &state) noexcept {
  if (!state)
    return true;
  if (!state->coverage)
    return false;
  const auto e = state->coverage->extent();
  const auto inverse = state->localToMask.inverted();
  return !e.empty() && e.width <= 32768 && e.height <= 32768 &&
         std::uint64_t(e.width) * e.height <= 64ULL * 1024 * 1024 && inverse &&
         inverse->validOver({0, 0, double(e.width), double(e.height)});
}
bool equivalentLayerMasks(const LayerMaskState &a,
                          const LayerMaskState &b) noexcept {
  return a == b ||
         (a && b && a->enabled == b->enabled && a->outside == b->outside &&
          a->localToMask == b->localToMask &&
          a->coverage->equivalent(*b->coverage));
}
float layerMaskCoverage(const LayerMaskState &mask, Vec2d local) noexcept {
  if (!mask || !mask->enabled)
    return 1;
  const auto p = mask->localToMask.map(local) - Vec2d{.5, .5};
  const auto e = mask->coverage->extent();
  const float outside = float(mask->outside) / 255;
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < -1 || p.y < -1 ||
      p.x >= e.width || p.y >= e.height)
    return outside;
  const int x = int(std::floor(p.x)), y = int(std::floor(p.y));
  const float fx = float(p.x - x), fy = float(p.y - y);
  const auto read = [&](int px, int py) {
    return px < 0 || py < 0 || px >= int(e.width) || py >= int(e.height)
               ? outside
               : float(mask->coverage->coverageAtDocumentPixel(px, py)) / 255;
  };
  return (read(x, y) * (1 - fx) + read(x + 1, y) * fx) * (1 - fy) +
         (read(x, y + 1) * (1 - fx) + read(x + 1, y + 1) * fx) * fy;
}
std::uint8_t maskGray(Rgba8 c) noexcept {
  // Coverage is numeric grayscale, not an sRGB color/alpha transfer function.
  return std::uint8_t(std::clamp(
      std::lround(.2126 * c.red + .7152 * c.green + .0722 * c.blue), 0L, 255L));
}
} // namespace imageeditor::core
