#include "imageeditor/core/SelectionRefinement.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/SpatialFilters.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <numeric>
#include <stdexcept>

namespace imageeditor::core {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
struct Cancelled {};
void check(const RefinementOptions &o) {
  if (o.cancelled && o.cancelled())
    throw Cancelled{};
}
struct Plane {
  int w, h;
  std::vector<float> p;
  float at(int x, int y) const {
    return p[std::size_t(std::clamp(y, 0, h - 1)) * std::size_t(w) +
             std::size_t(std::clamp(x, 0, w - 1))];
  }
};
// Exact squared Euclidean distance to a set of pixel centers. No artificial
// seeds beyond the canvas: a filled canvas has no boundary to contract.
std::vector<float> distance(std::vector<float> values, int w, int h,
                            const RefinementOptions &o) {
  constexpr float inf = 1e16F;
  const auto length = std::size_t(std::max(w, h));
  std::vector<float> f(length);
  std::vector<int> sites(length);
  std::vector<double> cuts(length + 1);
  auto line = [&](std::size_t start, std::size_t stride, int n) {
    int last = -1;
    for (int q = 0; q < n; ++q) {
      f[std::size_t(q)] = values[start + std::size_t(q) * stride];
      if (f[std::size_t(q)] >= inf)
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
      sites[std::size_t(++last)] = q;
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
      const float delta = float(q - p);
      values[start + std::size_t(q) * stride] =
          delta * delta + f[std::size_t(p)];
    }
  };
  for (int y = 0; y < h; ++y) {
    check(o);
    line(std::size_t(y) * std::size_t(w), 1, w);
  }
  for (int x = 0; x < w; ++x) {
    check(o);
    line(std::size_t(x), std::size_t(w), h);
  }
  return values;
}
void paint(Plane &p, const RefinementStroke &stroke,
           const RefinementOptions &o) {
  for (const auto &dab : stroke.dabs) {
    check(o);
    const auto center =
        (dab.documentCenter - o.origin) * o.pixelsPerDocumentPixel;
    const double r =
        std::max(.5, dab.diameterPixels * .5 * o.pixelsPerDocumentPixel);
    if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
        !std::isfinite(r) || r > 32768 || !std::isfinite(dab.strokeOpacity) ||
        !std::isfinite(dab.flow) || dab.strokeOpacity < 0 ||
        dab.strokeOpacity > 1 || dab.flow < 0 || dab.flow > 1)
      throw std::invalid_argument("Invalid refinement brush geometry.");
    if (center.x + r + 1 < 0 || center.y + r + 1 < 0 ||
        center.x - r - 1 > p.w || center.y - r - 1 > p.h)
      continue;
    const int x0 = int(std::max(0., std::floor(center.x - r - 1))),
              x1 = int(std::min(double(p.w), std::ceil(center.x + r + 1)));
    const int y0 = int(std::max(0., std::floor(center.y - r - 1))),
              y1 = int(std::min(double(p.h), std::ceil(center.y + r + 1)));
    for (int y = y0; y < y1; ++y) {
      check(o);
      for (int x = x0; x < x1; ++x) {
        const double d = std::hypot(x + .5 - center.x, y + .5 - center.y);
        const double falloff = std::clamp(
            (r + .5 - d) / std::max(1., r * (1 - stroke.hardness)), 0., 1.);
        const float a = float(falloff * dab.strokeOpacity * dab.flow);
        auto &v = p.p[std::size_t(y) * std::size_t(p.w) + std::size_t(x)];
        // A stroke is a coverage constraint, not repeated alpha deposition.
        // Duplicate events and repeated edge-region strokes are idempotent.
        v = std::max(v, a);
      }
    }
  }
}
std::array<float, 3> color(Rgba8 c) {
  return {float(srgbToLinear(c.red)), float(srgbToLinear(c.green)),
          float(srgbToLinear(c.blue))};
}
struct Seed {
  int x, y;
  std::array<float, 3> rgb;
};
// Small spatial k-d tree of trusted-region boundary samples. Never a dense
// image-sized linear system; only uncertain pixels perform pair fitting.
class Seeds {
public:
  std::vector<Seed> data;
  void build() {
    low.fill(1.F);
    high.fill(0.F);
    for (const auto &s : data)
      for (std::size_t k = 0; k < 3; ++k) {
        low[k] = std::min(low[k], s.rgb[k]);
        high[k] = std::max(high[k], s.rgb[k]);
      }
    uniform = true;
    for (std::size_t k = 0; k < 3; ++k)
      uniform &= high[k] - low[k] < .005F;
    if (!uniform)
      partition(0, data.size(), 0);
    spatial.resize(data.size());
    std::iota(spatial.begin(), spatial.end(), 0U);
    partitionSpatial(0, spatial.size(), 0);
  }
  std::vector<const Seed *> nearest(int x, int y, double reach,
                                    const std::array<float, 3> &rgb) const {
    if (uniform)
      return nearby(x, y, reach);
    std::array<std::pair<double, const Seed *>, 8> best;
    for (auto &v : best)
      v = {1e30, nullptr};
    query(0, data.size(), 0, x, y, reach, rgb, best);
    std::vector<const Seed *> result;
    for (auto v : best)
      if (v.second)
        result.push_back(v.second);
    return result;
  }
  std::vector<const Seed *> nearby(int x, int y, double reach) const {
    std::array<std::pair<double, const Seed *>, 4> best;
    for (auto &v : best)
      v = {reach * reach, nullptr};
    querySpatial(0, spatial.size(), 0, x, y, best);
    std::vector<const Seed *> result;
    for (auto v : best)
      if (v.second)
        result.push_back(v.second);
    return result;
  }
  double colorDistanceBound(const std::array<float, 3> &rgb) const {
    double sum = 0;
    for (std::size_t k = 0; k < 3; ++k) {
      const double d =
          rgb[k] - std::clamp(rgb[k], low[k], std::max(low[k], high[k]));
      sum += d * d;
    }
    return sum;
  }

private:
  std::array<float, 3> low{}, high{};
  bool uniform{};
  std::vector<std::uint32_t> spatial;
  void partitionSpatial(std::size_t lo, std::size_t hi, int axis) {
    if (lo >= hi)
      return;
    const auto mid = (lo + hi) / 2;
    std::nth_element(spatial.begin() + std::ptrdiff_t(lo),
                     spatial.begin() + std::ptrdiff_t(mid),
                     spatial.begin() + std::ptrdiff_t(hi), [&](auto a, auto b) {
                       return axis ? std::pair(data[a].y, data[a].x) <
                                         std::pair(data[b].y, data[b].x)
                                   : std::pair(data[a].x, data[a].y) <
                                         std::pair(data[b].x, data[b].y);
                     });
    partitionSpatial(lo, mid, 1 - axis);
    partitionSpatial(mid + 1, hi, 1 - axis);
  }
  void
  querySpatial(std::size_t lo, std::size_t hi, int axis, int x, int y,
               std::array<std::pair<double, const Seed *>, 4> &best) const {
    if (lo >= hi)
      return;
    const auto mid = (lo + hi) / 2;
    const auto &s = data[spatial[mid]];
    const double dx = s.x - x, dy = s.y - y, d = dx * dx + dy * dy;
    if (d < best.back().first) {
      best.back() = {d, &s};
      std::stable_sort(best.begin(), best.end(),
                       [](auto a, auto b) { return a.first < b.first; });
    }
    const double delta = axis ? dy : dx;
    if (delta > 0) {
      querySpatial(lo, mid, 1 - axis, x, y, best);
      if (delta * delta <= best.back().first)
        querySpatial(mid + 1, hi, 1 - axis, x, y, best);
    } else {
      querySpatial(mid + 1, hi, 1 - axis, x, y, best);
      if (delta * delta <= best.back().first)
        querySpatial(lo, mid, 1 - axis, x, y, best);
    }
  }
  void partition(std::size_t lo, std::size_t hi, int axis) {
    if (lo >= hi)
      return;
    const auto mid = (lo + hi) / 2;
    std::nth_element(
        data.begin() + std::ptrdiff_t(lo), data.begin() + std::ptrdiff_t(mid),
        data.begin() + std::ptrdiff_t(hi),
        [axis](const Seed &a, const Seed &b) {
          const float av = coordinate(a, axis), bv = coordinate(b, axis);
          return av != bv ? av < bv : std::pair(a.y, a.x) < std::pair(b.y, b.x);
        });
    partition(lo, mid, (axis + 1) % 5);
    partition(mid + 1, hi, (axis + 1) % 5);
  }
  static float coordinate(const Seed &s, int axis) {
    return axis == 0   ? float(s.x)
           : axis == 1 ? float(s.y)
                       : s.rgb[std::size_t(axis - 2)];
  }
  void query(std::size_t lo, std::size_t hi, int axis, int x, int y,
             double reach, const std::array<float, 3> &rgb,
             std::array<std::pair<double, const Seed *>, 8> &best) const {
    if (lo >= hi)
      return;
    const auto mid = (lo + hi) / 2;
    const auto &s = data[mid];
    const double dx = s.x - x, dy = s.y - y, d = dx * dx + dy * dy;
    const double spatialWeight = .01 / (reach * reach);
    double score = d * spatialWeight;
    for (std::size_t k = 0; k < 3; ++k)
      score += double(s.rgb[k] - rgb[k]) * (s.rgb[k] - rgb[k]);
    if (d <= reach * reach && score < best.back().first) {
      best.back() = {score, &s};
      std::stable_sort(best.begin(), best.end(),
                       [](auto a, auto b) { return a.first < b.first; });
    }
    const double delta =
        axis == 0   ? dx
        : axis == 1 ? dy
                    : s.rgb[std::size_t(axis - 2)] - rgb[std::size_t(axis - 2)];
    const double bound = delta * delta * (axis < 2 ? spatialWeight : 1.);
    if (delta > 0) {
      query(lo, mid, (axis + 1) % 5, x, y, reach, rgb, best);
      if (bound <= best.back().first && (axis >= 2 || std::abs(delta) <= reach))
        query(mid + 1, hi, (axis + 1) % 5, x, y, reach, rgb, best);
    } else {
      query(mid + 1, hi, (axis + 1) % 5, x, y, reach, rgb, best);
      if (bound <= best.back().first && (axis >= 2 || std::abs(delta) <= reach))
        query(lo, mid, (axis + 1) % 5, x, y, reach, rgb, best);
    }
  }
};
// A wide uncertain band can swallow an entire thin foreground component.
// Keep its deepest input core as an appearance proposal in that case, not as
// a fixed alpha constraint. Radius/Edge strokes must not erase all evidence of
// what the user selected. Components with trusted samples keep the ordinary
// trimap path, so an imprecise outer rim does not train foreground colors.
void recoverForeground(Seeds &fg, const Plane &work, const Plane &region,
                       const SmartReferenceImage &image,
                       const RefinementOptions &o) {
  std::vector<std::uint8_t> visited(work.p.size());
  std::vector<std::uint32_t> component;
  std::vector<float> inward;
  auto valid = [&](std::size_t i) {
    return image.valid[i] && image.pixels[i].alpha >= 250;
  };
  for (std::size_t start = 0; start < work.p.size(); ++start) {
    if (start % 65536 == 0)
      check(o);
    if (visited[start] || work.p[start] < .98F)
      continue;
    component.clear();
    component.push_back(std::uint32_t(start));
    visited[start] = 1;
    bool trusted = false, affected = false;
    for (std::size_t head = 0; head < component.size(); ++head) {
      if (head % 65536 == 0)
        check(o);
      const auto i = component[head];
      const int x = int(i % std::uint32_t(work.w)),
                y = int(i / std::uint32_t(work.w));
      trusted |= region.p[i] == 0 && valid(i);
      affected |= region.p[i] > 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (x + dx < 0 || x + dx >= work.w || y + dy < 0 || y + dy >= work.h)
            continue;
          const auto j =
              std::size_t(y + dy) * std::size_t(work.w) + std::size_t(x + dx);
          if (!visited[j] && work.p[j] >= .98F) {
            visited[j] = 1;
            component.push_back(std::uint32_t(j));
          }
        }
    }
    if (trusted || !affected)
      continue;
    if (inward.empty()) {
      inward.resize(work.p.size());
      for (std::size_t i = 0; i < work.p.size(); ++i)
        inward[i] = work.p[i] >= .98F ? 1e16F : 0.F;
      inward = distance(std::move(inward), work.w, work.h, o);
    }
    float deepest = 0;
    std::size_t peak = start;
    for (auto i : component)
      if (valid(i) && inward[i] > deepest) {
        deepest = inward[i];
        peak = i;
      }
    if (deepest == 0)
      continue;
    auto add = [&](std::size_t i) {
      fg.data.push_back({int(i % std::size_t(work.w)),
                         int(i / std::size_t(work.w)), color(image.pixels[i])});
    };
    add(peak);
    for (auto i : component)
      if (i != peak && valid(i) && inward[i] >= deepest * .25F &&
          (i % std::uint32_t(work.w)) % 2 == 0 &&
          (i / std::uint32_t(work.w)) % 2 == 0)
        add(i);
  }
}
void matte(Plane &work, const Plane &region, const SmartReferenceImage &image,
           double radius, RefinementResult &result,
           const RefinementOptions &o) {
  Seeds fg, bg;
  auto valid = [&](std::size_t i) {
    return image.valid[i] && image.pixels[i].alpha >= 250;
  };
  for (int y = 0; y < work.h; ++y) {
    check(o);
    for (int x = 0; x < work.w; ++x) {
      const auto i = std::size_t(y) * std::size_t(work.w) + std::size_t(x);
      const float a = work.p[i];
      if (!valid(i) || region.p[i] > 0 || (a > .02F && a < .98F))
        continue;
      bool boundary = false;
      for (auto d : std::array<Vec2d, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}})
        boundary |= region.at(x + int(d.x), y + int(d.y)) > 0;
      // Sparse interior samples help explicit brushed regions with no nearby
      // continuous boundary and thin disconnected foreground components.
      if (boundary || (x % 8 == 0 && y % 8 == 0))
        (a >= .98F ? fg : bg).data.push_back({x, y, color(image.pixels[i])});
    }
  }
  recoverForeground(fg, work, region, image, o);
  fg.build();
  const double reach = std::max(32. * o.pixelsPerDocumentPixel, radius * 4 + 8);
  // An incomplete selection may leave more of the same foreground just
  // outside the analysis band. Such samples are not independent background
  // color evidence, even though their coverage must remain unchanged there.
  // Only use this exclusion for a compact foreground color cluster. Broad or
  // multimodal foreground samples can legitimately share a background color.
  std::array<double, 3> mean{}, square{};
  for (const auto &f : fg.data)
    for (std::size_t k = 0; k < 3; ++k) {
      mean[k] += f.rgb[k];
      square[k] += double(f.rgb[k]) * f.rgb[k];
    }
  double variance = 0;
  const double sampleCount = double(std::max(std::size_t(1), fg.data.size()));
  for (std::size_t k = 0; k < 3; ++k)
    variance += square[k] / sampleCount - std::pow(mean[k] / sampleCount, 2);
  std::size_t retained = 0;
  for (std::size_t i = 0; i < bg.data.size(); ++i) {
    if (i % 4096 == 0)
      check(o);
    const auto &b = bg.data[i];
    bool distinct = true;
    if (variance < .01 && fg.colorDistanceBound(b.rgb) < .01)
      for (auto f : fg.nearest(b.x, b.y, reach, b.rgb)) {
        double difference = 0;
        for (std::size_t k = 0; k < 3; ++k)
          difference += double(f->rgb[k] - b.rgb[k]) * (f->rgb[k] - b.rgb[k]);
        if (difference < .01) {
          distinct = false;
          break;
        }
      }
    if (distinct)
      bg.data[retained++] = b;
  }
  bg.data.resize(retained);
  bg.build();
  Plane proposal = work;
  std::vector<float> confidence(work.p.size());
  std::atomic_size_t analyzed{}, unresolved{};
  auto rows = [&](unsigned rank, unsigned count) {
    for (int y = int(rank); y < work.h; y += int(count)) {
      check(o);
      for (int x = 0; x < work.w; ++x) {
        const auto i = std::size_t(y) * std::size_t(work.w) + std::size_t(x);
        if (region.p[i] <= 0)
          continue;
        ++analyzed;
        if (!valid(i)) {
          ++unresolved;
          continue;
        }
        const auto c = color(image.pixels[i]);
        const auto foreground = fg.nearby(x, y, reach),
                   background = bg.nearby(x, y, reach);
        double best = 1e30, chosen = work.p[i], residual = 1;
        auto fit = [&](const auto &foregroundSamples,
                       const auto &backgroundSamples) {
          for (auto f : foregroundSamples)
            for (auto b : backgroundSamples) {
              double norm = 0, dot = 0;
              for (int k = 0; k < 3; ++k) {
                const auto j = std::size_t(k);
                const double v = f->rgb[j] - b->rgb[j];
                norm += v * v;
                dot += (c[j] - b->rgb[j]) * v;
              }
              if (norm < .0025)
                continue;
              const double a = std::clamp(dot / norm, 0., 1.);
              double error = 0;
              for (int k = 0; k < 3; ++k) {
                const auto j = std::size_t(k);
                const double r =
                    c[j] - (b->rgb[j] + a * (f->rgb[j] - b->rgb[j]));
                error += r * r;
              }
              const double travel = (std::hypot(f->x - x, f->y - y) +
                                     std::hypot(b->x - x, b->y - y)) /
                                    reach;
              const double score =
                  error + .00005 * travel + .00002 * std::pow(a - work.p[i], 2);
              if (score < best) {
                best = score;
                chosen = a;
                residual = error / norm;
              }
            }
        };
        fit(foreground, background);
        if (residual > .0001) {
          auto colorsF = fg.nearest(x, y, reach, c),
               colorsB = bg.nearest(x, y, reach, c);
          colorsF.insert(colorsF.end(), foreground.begin(), foreground.end());
          colorsB.insert(colorsB.end(), background.begin(), background.end());
          fit(colorsF, colorsB);
        }
        if (best < 1e29) {
          proposal.p[i] = float(chosen);
          confidence[i] =
              residual <= .0001 ? 1.F : float(std::exp(-residual / .02));
        }
      }
    }
  };
  if (!boundedParallel(0, rows, o.cancelled))
    throw Cancelled{};
  auto consolidate = [&](unsigned rank, unsigned count) {
    for (int y = int(rank); y < work.h; y += int(count)) {
      check(o);
      for (int x = 0; x < work.w; ++x) {
        const auto i = std::size_t(y) * std::size_t(work.w) + std::size_t(x);
        if (region.p[i] <= 0 || !valid(i))
          continue;
        double chosen = proposal.p[i];
        if (confidence[i] < 1) {
          const auto rgb = color(image.pixels[i]);
          double sum = 0, weight = 0;
          for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
              const int px = x + dx, py = y + dy;
              if (px < 0 || px >= work.w || py < 0 || py >= work.h)
                continue;
              const auto j =
                  std::size_t(py) * std::size_t(work.w) + std::size_t(px);
              if (confidence[j] < .01F)
                continue;
              const auto other = color(image.pixels[j]);
              double difference = 0;
              for (std::size_t k = 0; k < 3; ++k)
                difference += double(rgb[k] - other[k]) * (rgb[k] - other[k]);
              const double w =
                  confidence[j] *
                  std::exp(-difference / .01 - (dx * dx + dy * dy) / 8.);
              sum += w * proposal.p[j];
              weight += w;
            }
          if (weight < .25) {
            ++unresolved;
            continue;
          }
          chosen = sum / weight;
        }
        work.p[i] = float(work.p[i] + region.p[i] * (chosen - work.p[i]));
      }
    }
  };
  if (!boundedParallel(0, consolidate, o.cancelled))
    throw Cancelled{};
  result.analyzedPixels = analyzed;
  result.unresolvedPixels = unresolved;
}
// Disk morphology of scalar coverage, not rectangular bounds or binary labels.
// Sliding row extrema make the cost O(pixels * radius), not O(pixels *
// radius²).
Plane disk(const Plane &p, int radius, bool grow, const RefinementOptions &o) {
  Plane out{p.w, p.h, std::vector<float>(p.p.size(), grow ? 0.F : 1.F)};
  std::vector<float> row(std::size_t(p.w), 0);
  for (int dy = -radius; dy <= radius; ++dy) {
    const int rx =
        int(std::floor(std::sqrt(double(radius) * radius - double(dy) * dy)));
    for (int y = 0; y < p.h; ++y) {
      check(o);
      std::deque<int> q;
      for (int x = -rx; x < p.w + rx; ++x) {
        const float v = p.at(x, y + dy);
        while (!q.empty() && (grow ? p.at(q.back(), y + dy) <= v
                                   : p.at(q.back(), y + dy) >= v))
          q.pop_back();
        q.push_back(x);
        while (q.front() < x - 2 * rx)
          q.pop_front();
        if (x >= rx)
          row[std::size_t(x - rx)] = p.at(q.front(), y + dy);
      }
      for (int x = 0; x < p.w; ++x) {
        auto &v = out.p[std::size_t(y) * std::size_t(p.w) + std::size_t(x)];
        v = grow ? std::max(v, row[std::size_t(x)])
                 : std::min(v, row[std::size_t(x)]);
      }
    }
  }
  return out;
}
void globals(Plane &p, const RefinementSettings &s,
             const RefinementOptions &o) {
  if (s.smooth > 0) {
    Plane next = p;
    const int passes = int(std::ceil(s.smooth));
    for (int pass = 0; pass < passes; ++pass) {
      const float amount = float(std::min(1., s.smooth - pass) * .5);
      for (int y = 0; y < p.h; ++y) {
        check(o);
        for (int x = 0; x < p.w; ++x) {
          std::array<float, 9> v;
          int k = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
              v[std::size_t(k++)] = p.at(x + dx, y + dy);
          std::nth_element(v.begin(), v.begin() + 4, v.end());
          const auto i = std::size_t(y) * std::size_t(p.w) + std::size_t(x);
          next.p[i] = p.p[i] + amount * (v[4] - p.p[i]);
        }
      }
      p.p.swap(next.p);
    }
  }
  if (s.shift != 0) {
    const double r = std::abs(s.shift) * o.pixelsPerDocumentPixel;
    const int lower = int(std::floor(r));
    if (double(p.p.size()) * (2 * std::ceil(r) + 1) > 2e9)
      throw std::length_error(
          "Refinement offset exceeds the work limit; reduce Shift Edge.");
    auto low = lower ? disk(p, lower, s.shift > 0, o) : p;
    if (r > lower) {
      auto high = disk(p, lower + 1, s.shift > 0, o);
      for (std::size_t i = 0; i < p.p.size(); ++i)
        low.p[i] += float(r - lower) * (high.p[i] - low.p[i]);
    }
    p.p.swap(low.p);
  }
  if (s.feather > 0) {
    const auto kernel =
        makeGaussianKernel(s.feather * o.pixelsPerDocumentPixel);
    const int r = int(kernel.size() / 2);
    Plane next = p;
    for (int axis = 0; axis < 2; ++axis) {
      for (int y = 0; y < p.h; ++y) {
        check(o);
        for (int x = 0; x < p.w; ++x) {
          double sum = 0;
          for (int k = -r; k <= r; ++k)
            sum += kernel[std::size_t(k + r)] *
                   p.at(x + (axis ? 0 : k), y + (axis ? k : 0));
          next.p[std::size_t(y) * std::size_t(p.w) + std::size_t(x)] =
              float(sum);
        }
      }
      p.p.swap(next.p);
    }
  }
  if (s.contrast > 0) {
    const double power = 1 + 15 * s.contrast;
    for (std::size_t i = 0; i < p.p.size(); ++i) {
      if (i % 65536 == 0)
        check(o);
      const double a = p.p[i];
      const double u = std::pow(a, power), v = std::pow(1 - a, power);
      p.p[i] = float(u / (u + v));
    }
  }
}
SelectionState publish(const Plane &p, const SelectionState &original,
                       const RefinementOptions &o) {
  std::vector<std::uint8_t> bytes(p.p.size());
  RectI changed;
  for (int y = 0; y < p.h; ++y) {
    check(o);
    int first = p.w, last = -1;
    for (int x = 0; x < p.w; ++x) {
      const auto i = std::size_t(y) * std::size_t(p.w) + std::size_t(x);
      bytes[i] = std::uint8_t(std::clamp(std::lround(p.p[i] * 255), 0L, 255L));
      if (!original || bytes[i] != original->coverageAtDocumentPixel(x, y)) {
        first = std::min(first, x);
        last = x;
      }
    }
    if (last >= first)
      changed = changed.united({first, y, last - first + 1, 1});
  }
  if (original) {
    if (changed.empty())
      return original;
    const CoveragePatch patch{
        changed,
        std::span(bytes).subspan(std::size_t(changed.y) * std::size_t(p.w) +
                                 std::size_t(changed.x)),
        std::size_t(p.w)};
    return original->replacedR8(std::span(&patch, 1));
  }
  return SelectionMask::fromR8({std::uint32_t(p.w), std::uint32_t(p.h)}, bytes,
                               std::size_t(p.w));
}
} // namespace
bool RefinementPath::begin(RefinementBrush kind, const BrushSettings &settings,
                           const NormalizedPointerSample &sample) {
  stroke_ = {kind, settings.hardness, {}};
  auto brush = settings;
  brush.tip = {};
  brush.grain = {};
  return engine_.beginStroke(brush, sample, *this);
}
bool RefinementPath::append(const NormalizedPointerSample &sample) {
  return engine_.appendSample(sample, *this);
}
bool RefinementPath::end(const NormalizedPointerSample &sample) {
  return engine_.endStroke(sample, *this);
}
RefinementStrokeState RefinementPath::stroke() const {
  return std::make_shared<const RefinementStroke>(stroke_);
}
RefinementResult refineSelection(const SelectionState &input,
                                 const SmartReferenceImage *reference,
                                 const RefinementState &state,
                                 const RefinementOptions &o) {
  if (!input)
    throw std::invalid_argument("Refinement requires explicit coverage.");
  const auto e = input->extent();
  const auto n = std::uint64_t(e.width) * e.height;
  const auto &s = state.settings;
  if (!std::isfinite(o.pixelsPerDocumentPixel) ||
      o.pixelsPerDocumentPixel <= 0 || o.pixelsPerDocumentPixel > 16 ||
      !std::isfinite(s.radius) || s.radius < 0 || s.radius > 64 ||
      !std::isfinite(s.smooth) || s.smooth < 0 || s.smooth > 12 ||
      !std::isfinite(s.feather) || s.feather < 0 || s.feather > 64 ||
      !std::isfinite(s.shift) || std::abs(s.shift) > 64 ||
      !std::isfinite(s.contrast) || s.contrast < 0 || s.contrast > 1)
    throw std::invalid_argument("Invalid refinement settings.");
  RefinementResult result;
  try {
    check(o);
    if (state == RefinementState{}) {
      result.coverage = input;
      return result;
    }
    // Includes float intermediates, region, worst-case seed tree, reference
    // and R8 publication. Check before any image-sized scratch allocation.
    if (!n || e.width > 32768 || e.height > 32768 || n > o.maxWorkingBytes / 64)
      throw std::length_error("Refinement exceeds the working-memory limit.");
    result.workingBytes = std::size_t(n) * 64;
    std::size_t dabs = 0;
    std::vector<RefinementStrokeState> regions;
    for (const auto &stroke : state.strokes) {
      if (!stroke || !std::isfinite(stroke->hardness) || stroke->hardness < 0 ||
          stroke->hardness > 1)
        throw std::invalid_argument("Invalid refinement stroke.");
      dabs += stroke->dabs.size();
      if (dabs > 262144 || state.strokes.size() > 4096)
        throw std::length_error(
            "Refinement stroke history exceeds its memory limit.");
      if (stroke->kind == RefinementBrush::Edge)
        regions.push_back(stroke);
    }
    if (reference && (reference->extent != e || reference->pixels.size() != n ||
                      reference->valid.size() != n))
      throw std::invalid_argument("Refinement reference dimensions differ.");
    Plane work{int(e.width), int(e.height), std::vector<float>(std::size_t(n))};
    for (int y = 0; y < work.h; ++y) {
      check(o);
      for (int x = 0; x < work.w; ++x)
        work.p[std::size_t(y) * e.width + std::size_t(x)] =
            float(input->coverageAtDocumentPixel(x, y)) / 255;
    }
    Plane region{work.w, work.h, std::vector<float>(std::size_t(n))};
    auto time = Clock::now();
    const bool reuse =
        o.analysis && o.referenceOwner && reference == o.referenceOwner.get() &&
        o.analysis->input == input &&
        o.analysis->reference == o.referenceOwner &&
        o.analysis->radius == s.radius &&
        o.analysis->scale == o.pixelsPerDocumentPixel &&
        o.analysis->origin == o.origin && o.analysis->regions == regions;
    if (reuse) {
      work.p = o.analysis->coverage;
      result.region = o.analysis->region;
      result.analyzedPixels = o.analysis->analyzed;
      result.unresolvedPixels = o.analysis->unresolved;
      result.analysis = o.analysis;
    } else {
      if (s.radius > 0) {
        std::vector<float> seeds(std::size_t(n), 1e16F);
        for (int y = 0; y < work.h; ++y) {
          check(o);
          for (int x = 0; x < work.w; ++x) {
            const auto i = std::size_t(y) * e.width + std::size_t(x);
            const float a = work.p[i];
            // Gray values are estimates, not hard labels. Their gradients
            // also define boundaries when all coverage lies below 50%.
            if ((a > 0 && a < 1) || std::abs(a - work.at(x + 1, y)) > .001F ||
                std::abs(a - work.at(x - 1, y)) > .001F ||
                std::abs(a - work.at(x, y + 1)) > .001F ||
                std::abs(a - work.at(x, y - 1)) > .001F)
              seeds[i] = 0;
          }
        }
        seeds = distance(std::move(seeds), work.w, work.h, o);
        const double radius = s.radius * o.pixelsPerDocumentPixel;
        for (std::size_t i = 0; i < region.p.size(); ++i)
          region.p[i] = seeds[i] <= radius * radius ? 1.F : 0.F;
      }
      for (const auto &stroke : state.strokes)
        if (stroke->kind == RefinementBrush::Edge)
          paint(region, *stroke, o);
      if (reference && std::any_of(region.p.begin(), region.p.end(),
                                   [](float a) { return a > 0; })) {
        // An explicit broad uncertain region needs context on both sides,
        // even with automatic Radius at zero. This changes only sample search,
        // never the requested region or trustworthy coverage outside it.
        double context = s.radius;
        for (const auto &stroke : regions)
          for (const auto &dab : stroke->dabs)
            context = std::max(context, dab.diameterPixels * .5);
        matte(work, region, *reference, context * o.pixelsPerDocumentPixel,
              result, o);
      } else
        result.unresolvedPixels = std::size_t(std::count_if(
            region.p.begin(), region.p.end(), [](float v) { return v > 0; }));
      result.region = publish(region, {}, o);
      if (o.referenceOwner && reference == o.referenceOwner.get()) {
        auto cache = std::make_shared<RefinementAnalysis>();
        cache->input = input;
        cache->reference = o.referenceOwner;
        cache->radius = s.radius;
        cache->scale = o.pixelsPerDocumentPixel;
        cache->origin = o.origin;
        cache->regions = std::move(regions);
        cache->coverage = work.p;
        cache->region = result.region;
        cache->analyzed = result.analyzedPixels;
        cache->unresolved = result.unresolvedPixels;
        result.analysis = std::move(cache);
      }
    }
    result.analysisMs = elapsed(time);
    time = Clock::now();
    globals(work, s, o);
    result.globalMs = elapsed(time);
    time = Clock::now();
    Plane correction{work.w, work.h, std::vector<float>(std::size_t(n))};
    for (const auto &stroke : state.strokes)
      if (stroke->kind != RefinementBrush::Edge) {
        std::fill(correction.p.begin(), correction.p.end(), 0);
        paint(correction, *stroke, o);
        for (std::size_t i = 0; i < work.p.size(); ++i) {
          if (i % 65536 == 0)
            check(o);
          const float target = stroke->kind == RefinementBrush::Add ? 1.F : 0.F;
          work.p[i] += correction.p[i] * (target - work.p[i]);
        }
      }
    result.coverage = publish(work, input, o);
    result.correctionMs = elapsed(time);
    check(o);
  } catch (const Cancelled &) {
    result = {};
    result.cancelled = true;
  }
  return result;
}
} // namespace imageeditor::core
