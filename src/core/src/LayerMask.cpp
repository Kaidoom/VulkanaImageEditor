#include "imageeditor/core/LayerMask.hpp"
#include <cmath>
#include <stdexcept>

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
LayerMaskState expandedLayerMask(const LayerMaskState &mask, RectD bounds) {
  if (!mask || !validLayerMask(mask))
    throw std::invalid_argument("Invalid layer mask");
  if (bounds.empty()) return mask;
  if (!mask->localToMask.validOver(bounds))
    throw std::invalid_argument("Layer bounds cannot be mapped into the mask");
  const auto e = mask->coverage->extent();
  double left = 0, top = 0, right = e.width, bottom = e.height;
  for (const auto p : std::array{Vec2d{bounds.x, bounds.y}, Vec2d{bounds.right(), bounds.y},
                                Vec2d{bounds.x, bounds.bottom()}, Vec2d{bounds.right(), bounds.bottom()}}) {
    const auto q = mask->localToMask.map(p);
    left = std::min(left, std::floor(q.x)); top = std::min(top, std::floor(q.y));
    right = std::max(right, std::ceil(q.x)); bottom = std::max(bottom, std::ceil(q.y));
  }
  const auto width = right - left, height = bottom - top;
  if (width > 32768 || height > 32768 || width * height > 64.0 * 1024 * 1024)
    throw std::length_error("Layer mask exceeds the 64 megapixel limit");
  if (left == 0 && top == 0 && right == e.width && bottom == e.height) return mask;
  auto result = std::make_shared<LayerMask>(*mask);
  result->localToMask = composeTransform({1, 0, -left, 0, 1, -top}, mask->localToMask);
  const auto inverse = result->localToMask.inverted();
  if (!inverse || !inverse->validOver({0, 0, width, height}))
    throw std::invalid_argument("Expanded mask frame crosses a projective horizon");
  result->coverage = mask->coverage->padded({int(left), int(top), int(width), int(height)}, mask->outside);
  return result;
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
