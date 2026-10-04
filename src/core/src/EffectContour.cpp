#include "imageeditor/core/LayerEffects.hpp"
#include <algorithm>
#include <cmath>
namespace imageeditor::core {
std::size_t layerEffectMemoryCost(const LayerEffectState &state) noexcept {
  if (!state)
    return 0;
  std::size_t bytes = sizeof(LayerEffectStack);
  for (const auto &e : state->items)
    bytes +=
        (e.bevel.surface.points.capacity() + e.bevel.gloss.points.capacity()) *
        sizeof(EffectContourPoint);
  return bytes;
}
bool validEffectContour(const EffectContour &c) noexcept {
  if (c.points.size() < 2 || c.points.size() > 16 ||
      std::uint32_t(c.interpolation) > 1)
    return false;
  if (c.points.front().input != 0 || c.points.back().input != 1)
    return false;
  double previous = -1;
  for (const auto &p : c.points) {
    if (!std::isfinite(p.input) || !std::isfinite(p.output) || p.input < 0 ||
        p.input > 1 || p.output < 0 || p.output > 1 ||
        p.input - previous < 1e-6)
      return false;
    previous = p.input;
  }
  return true;
}
double evaluateEffectContour(const EffectContour &c, double x) noexcept {
  x = std::clamp(x, 0., 1.);
  if (!c.enabled || c.points.size() < 2)
    return x;
  const auto &p = c.points;
  auto it =
      std::upper_bound(p.begin(), p.end(), x,
                       [](double v, const auto &a) { return v < a.input; });
  if (it == p.begin())
    return p.front().output;
  if (it == p.end())
    return p.back().output;
  const auto i = std::size_t(it - p.begin() - 1);
  const double h = p[i + 1].input - p[i].input, t = (x - p[i].input) / h;
  if (c.interpolation == EffectContourInterpolation::Linear)
    return std::lerp(p[i].output, p[i + 1].output, t);
  const auto secant = [&](std::size_t j) {
    return (p[j + 1].output - p[j].output) / (p[j + 1].input - p[j].input);
  };
  const auto slope = [&](std::size_t j) {
    if (j == 0)
      return secant(0);
    if (j + 1 == p.size())
      return secant(j - 1);
    const double a = secant(j - 1), b = secant(j);
    if (a * b <= 0)
      return 0.;
    const double left = p[j].input - p[j - 1].input,
                 right = p[j + 1].input - p[j].input;
    return 3 * (left + right) /
           ((left + 2 * right) / a + (2 * left + right) / b);
  };
  const double a = p[i].corner ? secant(i) : slope(i),
               b = p[i + 1].corner ? secant(i) : slope(i + 1);
  const double y = (2 * t * t * t - 3 * t * t + 1) * p[i].output +
                   (t * t * t - 2 * t * t + t) * h * a +
                   (-2 * t * t * t + 3 * t * t) * p[i + 1].output +
                   (t * t * t - t * t) * h * b;
  return std::clamp(y, std::min(p[i].output, p[i + 1].output),
                    std::max(p[i].output, p[i + 1].output));
}
EffectContour effectContourPreset(bool gloss, int index) {
  EffectContour c;
  c.enabled = true;
  if (gloss) {
    if (index == 1)
      c.points = {{0, 0, false},
                  {.25, .08, false},
                  {.5, .5, false},
                  {.75, .92, false},
                  {1, 1, false}};
    if (index == 2)
      c.points = {{0, 0, false},
                  {.25, .8, false},
                  {.5, .15, false},
                  {.75, .8, false},
                  {1, 1, false}};
    if (index == 3)
      c.points = {{0, 0, false},   {.2, .9, false}, {.4, .1, false},
                  {.6, .9, false}, {.8, .1, false}, {1, 1, false}};
  } else {
    if (index == 1)
      c.points = {{0, 0, false},
                  {.25, .38, false},
                  {.5, .71, false},
                  {.75, .92, false},
                  {1, 1, false}};
    if (index == 2)
      c.points = {{0, 0, false},
                  {.25, .08, false},
                  {.5, .29, false},
                  {.75, .62, false},
                  {1, 1, false}};
    if (index == 3)
      c.points = {{0, 0, false},
                  {.3, .8, false},
                  {.55, .35, false},
                  {.8, .9, false},
                  {1, 1, false}};
  }
  return c;
}
} // namespace imageeditor::core
