#include "imageeditor/core/LiveWire.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/FreehandSelectionPath.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace imageeditor::core {
struct MagneticEdgeCache::Impl {
    static constexpr int side = 64;
    static constexpr std::size_t tileLimit = 64;
    using Color = std::array<float, 4>;
    struct Tile {
        std::array<Color, side * side> raw { }, blurred { };
        std::array<Feature, side * side> features { };
        std::array<std::uint8_t, side * side> flags { };
        std::uint64_t used { };
    };
    Extent2u extent;
    Sample sample;
    std::unordered_map<std::uint64_t, std::unique_ptr<Tile>> tiles;
    std::uint64_t tick { };
    Stats stats;
    Tile& tile(int x, int y)
    {
        const auto key = (std::uint64_t(y / side) << 32) | std::uint32_t(x / side);
        auto it = tiles.find(key);
        if (it == tiles.end()) {
            if (tiles.size() == tileLimit) {
                auto oldest = std::min_element(tiles.begin(), tiles.end(),
                    [](const auto& a, const auto& b) { return a.second->used < b.second->used; });
                tiles.erase(oldest);
                ++stats.evictions;
            }
            it = tiles.emplace(key, std::make_unique<Tile>()).first;
        }
        it->second->used = ++tick;
        return *it->second;
    }
    Color raw(int x, int y)
    {
        // Constant extension suppresses artificial canvas-border edges. The
        // path itself is never clamped; off-canvas searches use manual geometry.
        x = std::clamp(x, 0, int(extent.width) - 1);
        y = std::clamp(y, 0, int(extent.height) - 1);
        auto& t = tile(x, y);
        const auto i = std::size_t(y % side * side + x % side);
        if (!(t.flags[i] & 1)) {
            const auto c = sample(x, y);
            const float a = float(c.alpha) / 255;
            t.raw[i] = { float(srgbToLinear(c.red)) * a, float(srgbToLinear(c.green)) * a,
                float(srgbToLinear(c.blue)) * a, a };
            t.flags[i] |= 1;
            ++stats.samples;
        }
        return t.raw[i];
    }
    Color blur(int x, int y)
    {
        x = std::clamp(x, 0, int(extent.width) - 1);
        y = std::clamp(y, 0, int(extent.height) - 1);
        auto& t = tile(x, y);
        const auto i = std::size_t(y % side * side + x % side);
        if (!(t.flags[i] & 2)) {
            Color c { };
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto p = raw(x + dx, y + dy);
                    const float weight = float((dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1)) / 16;
                    for (int k = 0; k < 4; ++k)
                        c[std::size_t(k)] += p[std::size_t(k)] * weight;
                }
            t.blurred[i] = c;
            t.flags[i] |= 2;
        }
        return t.blurred[i];
    }
    Feature feature(int x, int y)
    {
        if (x < 0 || y < 0 || x > int(extent.width) || y > int(extent.height))
            return { };
        auto& t = tile(x, y);
        const auto i = std::size_t(y % side * side + x % side);
        if (!(t.flags[i] & 4)) {
            // Grid vertices are document pixel corners, not texel centers.
            const auto a = blur(x - 1, y - 1), b = blur(x, y - 1), c = blur(x - 1, y), d = blur(x, y);
            float best = 0, gx = 0, gy = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                const float dx = (b[k] + d[k] - a[k] - c[k]) * 0.5F;
                const float dy = (c[k] + d[k] - a[k] - b[k]) * 0.5F;
                const float magnitude = dx * dx + dy * dy;
                if (magnitude > best) {
                    best = magnitude;
                    gx = dx;
                    gy = dy;
                }
            }
            const float magnitude = std::sqrt(best);
            t.features[i] = magnitude > 0.02F ? Feature { gx / magnitude, gy / magnitude,
                std::clamp((magnitude - 0.02F) / 0.33F, 0.0F, 1.0F) }
                                              : Feature { };
            t.flags[i] |= 4;
            ++stats.features;
        }
        return t.features[i];
    }
};
MagneticEdgeCache::MagneticEdgeCache(Extent2u extent, Sample sample)
    : impl_(std::make_unique<Impl>())
{
    if (extent.empty() || extent.width > 32768 || extent.height > 32768 || !sample)
        throw std::invalid_argument("Invalid magnetic reference");
    impl_->extent = extent;
    impl_->sample = std::move(sample);
}
MagneticEdgeCache::~MagneticEdgeCache() = default;
MagneticEdgeCache::Feature MagneticEdgeCache::feature(int x, int y) { return impl_->feature(x, y); }
Extent2u MagneticEdgeCache::extent() const { return impl_->extent; }
std::size_t MagneticEdgeCache::memoryBytes() const
{
    return sizeof(Impl) + impl_->tiles.size() * sizeof(Impl::Tile);
}
MagneticEdgeCache::Stats MagneticEdgeCache::stats() const { return impl_->stats; }

namespace {
    double distanceSquared(Vec2d p, Vec2d a, Vec2d b)
    {
        const auto d = b - a;
        const double length = d.x * d.x + d.y * d.y;
        const double t
            = length > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / length, 0.0, 1.0) : 0;
        const auto q = p - (a + d * t);
        return q.x * q.x + q.y * q.y;
    }
}
struct LiveWireSearch::Impl {
    struct Node {
        double cost = std::numeric_limits<double>::infinity();
        MagneticEdgeCache::Feature feature;
        float proximity { };
        int parent = -1, heap = -1;
        bool eligible = false, settled = false;
    };
    MagneticEdgeCache& cache;
    std::vector<Vec2d> guide, output;
    std::vector<Node> nodes;
    std::vector<int> heap;
    RectI region;
    double radius;
    Result result = Result::Searching;
    Stats stats;
    std::size_t preparing = 0;
    int source = 0, target = 0;
    float strongest = 0;
    Impl(MagneticEdgeCache& c, std::span<const Vec2d> g, double r)
        : cache(c)
        , radius(r)
    {
        if (!std::isfinite(r) || r < 1 || r > 64 || g.empty() || g.size() > maximumGuidePoints)
            throw std::invalid_argument("Invalid magnetic guide or search radius");
        FreehandSelectionPath clean;
        for (auto p : g)
            clean.append(p);
        guide.assign(clean.points().begin(), clean.points().end());
        if (guide.size() < 2) {
            fallback();
            return;
        }
        const auto e = c.extent();
        double left = guide[0].x, right = left, top = guide[0].y, bottom = top;
        for (auto p : guide) {
            if (p.x < 0 || p.y < 0 || p.x > e.width || p.y > e.height) {
                fallback();
                return;
            }
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        const int x0 = int(std::max(0.0, std::floor(left - r - 1))),
                  y0 = int(std::max(0.0, std::floor(top - r - 1)));
        const int x1 = int(std::min(double(e.width), std::ceil(right + r + 1))),
                  y1 = int(std::min(double(e.height), std::ceil(bottom + r + 1)));
        region = { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
        const auto size = std::size_t(region.width) * std::size_t(region.height);
        if (size > maximumNodes) {
            fallback();
            return;
        }
        nodes.resize(size);
        heap.reserve(size);
        source = index(int(std::lround(guide.front().x)), int(std::lround(guide.front().y)));
        target = index(int(std::lround(guide.back().x)), int(std::lround(guide.back().y)));
    }
    void fallback()
    {
        output = guide;
        result = Result::ManualFallback;
    }
    int index(int x, int y) const { return (y - region.y) * region.width + x - region.x; }
    Vec2d position(int i) const
    {
        return { double(region.x + i % region.width), double(region.y + i / region.width) };
    }
    double priority(int i) const
    {
        const auto d = position(i) - position(target);
        return nodes[std::size_t(i)].cost + 0.15 * std::hypot(d.x, d.y);
    }
    bool less(int a, int b) const
    {
        const auto x = priority(a), y = priority(b);
        return x < y || (x == y && a < b);
    }
    void swapHeap(std::size_t a, std::size_t b)
    {
        std::swap(heap[a], heap[b]);
        nodes[std::size_t(heap[a])].heap = int(a);
        nodes[std::size_t(heap[b])].heap = int(b);
    }
    void update(int i)
    {
        auto& n = nodes[std::size_t(i)];
        if (n.heap < 0) {
            n.heap = int(heap.size());
            heap.push_back(i);
        }
        auto p = std::size_t(n.heap);
        while (p && less(heap[p], heap[(p - 1) / 2])) {
            swapHeap(p, (p - 1) / 2);
            p = (p - 1) / 2;
        }
        stats.peakQueue = std::max(stats.peakQueue, heap.size());
    }
    int pop()
    {
        const int i = heap.front();
        swapHeap(0, heap.size() - 1);
        heap.pop_back();
        nodes[std::size_t(i)].heap = -1;
        std::size_t p = 0;
        while (2 * p + 1 < heap.size()) {
            auto c = 2 * p + 1;
            if (c + 1 < heap.size() && less(heap[c + 1], heap[c]))
                ++c;
            if (!less(heap[c], heap[p]))
                break;
            swapHeap(c, p);
            p = c;
        }
        return i;
    }
    void complete()
    {
        std::vector<Vec2d> reverse;
        for (int i = target; i >= 0; i = nodes[std::size_t(i)].parent) {
            reverse.push_back(position(i));
            if (i == source)
                break;
            if (reverse.size() > nodes.size())
                throw std::runtime_error("Invalid live-wire predecessor chain");
        }
        FreehandSelectionPath clean;
        clean.append(guide.front());
        for (auto it = reverse.rbegin(); it != reverse.rend(); ++it)
            clean.append(*it);
        clean.append(guide.back());
        output.assign(clean.points().begin(), clean.points().end());
        result = Result::Edge;
    }
    bool step(std::size_t budget)
    {
        std::size_t work = 0;
        while (result == Result::Searching && preparing < nodes.size()) {
            const auto p = position(int(preparing));
            double distance = std::numeric_limits<double>::infinity();
            for (std::size_t k = 1; k < guide.size(); ++k)
                distance = std::min(distance, distanceSquared(p, guide[k - 1], guide[k]));
            auto& n = nodes[preparing];
            n.eligible = distance <= radius * radius || int(preparing) == source || int(preparing) == target;
            if (n.eligible) {
                n.feature = cache.feature(int(p.x), int(p.y));
                n.proximity = float(distance / (radius * radius));
                strongest = std::max(strongest, n.feature.strength);
            }
            ++preparing;
            ++stats.prepared;
            work += guide.size() + 16;
            if (work >= std::max<std::size_t>(1, budget))
                return false;
        }
        if (result != Result::Searching)
            return true;
        if (strongest < 0.05F) {
            fallback();
            return true;
        }
        if (stats.expanded == 0 && heap.empty()) {
            nodes[std::size_t(source)].cost = 0;
            update(source);
        }
        constexpr std::array<std::array<int, 2>, 8> offsets { { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
            { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } } };
        while (!heap.empty()) {
            const int current = pop();
            auto& from = nodes[std::size_t(current)];
            from.settled = true;
            ++stats.expanded;
            if (current == target) {
                complete();
                return true;
            }
            const int x = current % region.width, y = current / region.width;
            for (auto d : offsets) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= region.width || ny >= region.height)
                    continue;
                const int next = ny * region.width + nx;
                auto& to = nodes[std::size_t(next)];
                if (!to.eligible || to.settled)
                    continue;
                const double length = d[0] && d[1] ? std::sqrt(2.0) : 1.0;
                const double strength = (from.feature.strength + to.feature.strength) * 0.5;
                const auto tangent = [&](const auto& f) {
                    return std::abs(double(f.x) * d[1] - double(f.y) * d[0]) / length;
                };
                const double alignment = 1 - (tangent(from.feature) + tangent(to.feature)) * 0.5;
                const double cost = length
                    * (0.15 + 3.5 * (1 - strength) + 1.2 * strength * alignment
                        + (from.proximity + to.proximity));
                if (from.cost + cost < to.cost) {
                    to.cost = from.cost + cost;
                    to.parent = current;
                    update(next);
                }
            }
            work += 8;
            if (work >= std::max<std::size_t>(1, budget))
                return false;
        }
        fallback();
        return true;
    }
};
LiveWireSearch::LiveWireSearch(MagneticEdgeCache& cache, std::span<const Vec2d> guide, double radius)
    : impl_(std::make_unique<Impl>(cache, guide, radius))
{
}
LiveWireSearch::~LiveWireSearch() = default;
bool LiveWireSearch::step(std::size_t budget) { return impl_->step(budget); }
LiveWireSearch::Result LiveWireSearch::result() const { return impl_->result; }
std::span<const Vec2d> LiveWireSearch::path() const { return impl_->output; }
LiveWireSearch::Stats LiveWireSearch::stats() const { return impl_->stats; }
std::size_t LiveWireSearch::memoryBytes() const
{
    return sizeof(Impl) + impl_->nodes.capacity() * sizeof(Impl::Node) + impl_->heap.capacity() * sizeof(int)
        + (impl_->guide.capacity() + impl_->output.capacity()) * sizeof(Vec2d);
}
}
