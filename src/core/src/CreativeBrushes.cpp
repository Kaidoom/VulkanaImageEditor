#include "imageeditor/core/CreativeBrushes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace imageeditor::core {
namespace {
constexpr double pi = std::numbers::pi;
enum Kind { Precision, Chisel, Marker, Hatch, Acrylic, Bristles, Oils, Chalk,
    Charcoal, Grunge, Sponge, Smoke, Spark, Confetti, Cells, Star, Heart, Vine, Count };
struct Recipe {
    const char* slug;
    const char* name;
    double size, aspect, angle;
    bool follow;
    double hardness, flow, spacing, grain;
    bool pressureSize;
};
constexpr std::array<Recipe, Count> recipes {{
    {"precision-ink", "Precision Ink", 3, 1, 0, false, 1, 1, 15, 0, false},
    {"thin-chisel", "Thin Chisel", 7, .24, -35, false, 1, 1, 5, 0, false},
    {"dry-marker", "Dry Marker", 35, .32, -35, false, .65, .9, 5, .65, false},
    {"hatch-pen", "Hatch Pen", 24, .35, 90, true, .8, 1, 5, 0, false},
    {"acrylic", "Acrylic", 48, .34, 90, true, .74, .8, 3, .22, true},
    {"bristles", "Bristles", 42, .3, 90, true, 1, .95, 5, 0, true},
    {"oils", "Oils", 46, .48, 90, true, .58, .6, 5, .15, true},
    {"chalk", "Chalk", 30, .8, -20, false, .72, .9, 9, .78, true},
    {"charcoal", "Charcoal", 38, .66, -25, false, .58, .65, 8, .8, true},
    {"grunge", "Grunge", 76, 1, 0, false, .65, .9, 48, .18, true},
    {"sponge", "Sponge", 64, 1, 0, false, .62, .9, 35, .2, true},
    {"smoke", "Smoke", 100, 1, 0, false, .44, .22, 28, 0, true},
    {"spark", "Spark", 60, 1, 0, false, .7, 1, 85, 0, false},
    {"confetti", "Confetti", 72, 1, 0, false, .8, 1, 75, 0, false},
    {"cells", "Cells", 56, 1, 0, false, .75, 1, 105, 0, false},
    {"star", "Star", 32, 1, 0, false, .75, 1, 125, 0, false},
    {"heart", "Heart", 32, 1, 0, false, .75, 1, 120, 0, false},
    {"vine", "Vine", 46, 1, 0, true, .75, 1, 85, 0, false},
}};

std::uint64_t mix(std::uint64_t value) noexcept
{
    value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27; value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
double random(std::uint64_t value) noexcept
{
    return double(mix(value + 0x9e3779b97f4a7c15ULL) >> 11) * 0x1.0p-53;
}
double smooth(double lo, double hi, double x) noexcept
{
    const auto t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
double noise(double x, double y, std::uint64_t seed) noexcept
{
    const auto ix = static_cast<std::int64_t>(std::floor(x));
    const auto iy = static_cast<std::int64_t>(std::floor(y));
    const auto u = smooth(0, 1, x - double(ix)), v = smooth(0, 1, y - double(iy));
    const auto at = [seed](std::int64_t a, std::int64_t b) {
        return random(seed ^ (std::uint64_t(a) * 0x9e3779b97f4a7c15ULL)
            ^ (std::uint64_t(b) * 0xd1b54a32d192ed03ULL));
    };
    return std::lerp(std::lerp(at(ix, iy), at(ix + 1, iy), u),
        std::lerp(at(ix, iy + 1), at(ix + 1, iy + 1), u), v);
}
double turbulence(double x, double y) noexcept
{
    return .5 * noise(x * 3, y * 3, 21) + .28 * noise(x * 8, y * 8, 47)
        + .15 * noise(x * 19, y * 19, 83) + .07 * noise(x * 43, y * 43, 99);
}
double segment(double x, double y, Vec2d a, Vec2d b) noexcept
{
    const auto dx = b.x - a.x, dy = b.y - a.y;
    const auto t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
    return std::hypot(x - a.x - t * dx, y - a.y - t * dy);
}
double polygon(double x, double y, std::span<const Vec2d> points) noexcept
{
    bool inside = false;
    double d = 4;
    auto a = points.back();
    for (const auto b : points) {
        d = std::min(d, segment(x, y, a, b));
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
        a = b;
    }
    return inside ? -d : d;
}
double box(double x, double y, double w, double h) noexcept
{
    const auto dx = std::abs(x) - w, dy = std::abs(y) - h;
    return std::hypot(std::max(dx, 0.0), std::max(dy, 0.0)) + std::min(std::max(dx, dy), 0.0);
}

struct Particle { double x, y, c, s, w, h, opacity; };
const std::vector<Particle>& particles(Kind kind)
{
    static const auto sets = [] {
        std::array<std::vector<Particle>, Count> result;
        for (auto k : {Confetti, Spark, Sponge, Grunge}) {
            const int count = k == Confetti ? 19 : k == Spark ? 13 : k == Sponge ? 80 : 115;
            auto& set = result[std::size_t(k)]; set.reserve(std::size_t(count));
            for (int i = 0; i < count; ++i) {
                const auto key = std::uint64_t(i) * 13 + 1234 + std::uint64_t(k) * 851;
                const auto a = random(key) * 2 * pi;
                const auto r = std::sqrt(random(key + 1)) * .76;
                const auto angle = random(key + 2) * 2 * pi;
                const auto width = k == Confetti ? .027 + random(key + 3) * .04
                    : k == Spark ? .05 + random(key + 3) * .12
                    : .019 + random(key + 3) * (k == Sponge ? .073 : .11);
                set.push_back({r * std::cos(a), r * std::sin(a), std::cos(angle), std::sin(angle),
                    width, .02 + random(key + 4) * .025, .35 + .65 * random(key + 5)});
            }
        }
        return result;
    }();
    return sets[std::size_t(kind)];
}

// Original analytic patterns are sampled once into coverage, never RGB artwork.
// All have an empty guard band. Existing BitmapMaskTip supplies mip filtering,
// transform handling and hardness; no texture decoding/allocation per dab.
double generatedCoverage(Kind kind, double x, double y)
{
    constexpr double aa = .004;
    const auto edge = [](double d) { return 1 - smooth(-aa, aa, d); };
    const auto radius = std::hypot(x, y);
    if (kind == Star || kind == Heart) {
        static const auto star = [] {
            std::array<Vec2d, 10> points;
            for (int i = 0; i < 10; ++i) {
                const auto angle = -pi / 2 + i * pi / 5;
                const auto r = i % 2 ? .4 : .94;
                points[std::size_t(i)] = {r * std::cos(angle), r * std::sin(angle)};
            }
            return points;
        }();
        static const auto heart = [] {
            std::array<Vec2d, 128> points;
            for (int i = 0; i < 128; ++i) {
                const auto t = i * 2 * pi / 128;
                const auto s = std::sin(t);
                points[std::size_t(i)] = {.057 * 16 * s * s * s,
                    -.06 * (13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t)) - .14};
            }
            return points;
        }();
        return edge(kind == Star ? polygon(x, y, star) : polygon(x, y, heart));
    }
    if (kind == Cells) {
        double d = 4;
        for (int row = -1; row <= 1; ++row) for (int col = -1; col <= 1; ++col) {
            if (row != 0 && col == 1) continue;
            const double cx = (col + (row == 0 ? 0 : .5)) * .52;
            const double cy = row * .45;
            const auto qx = std::abs(x - cx), qy = std::abs(y - cy);
            const auto hex = std::max(qx * .866025403784 + qy * .5, qy) - .26;
            d = std::min(d, std::abs(hex) - .024);
        }
        return edge(d) * (1 - smooth(.92, .98, radius));
    }
    if (kind == Hatch) {
        double d = 4;
        for (int i = -2; i <= 2; ++i)
            d = std::min(d, segment(x, y, {i * .33, -.7}, {i * .33, .7}));
        return edge(d - .05);
    }
    if (kind == Vine) {
        auto value = edge(std::abs(y - .06 * std::sin(pi * x)) - .027) * (1 - smooth(.93, .98, std::abs(x)));
        for (int side : {-1, 1}) {
            const auto cx = side * .21, cy = side * .34;
            const auto dx = x - cx, dy = y - cy;
            const auto u = (dx + dy) * .70710678118 / .41;
            const auto v = (dy - dx) * .70710678118 / .17;
            // Pointed leaves, with a fine unpainted vein joining the stem.
            const auto leaf = edge(std::abs(v) + u * u - 1)
                * (.75 + .25 * smooth(.018, .038, std::abs(v)));
            value = std::max(value, leaf);
            value = std::max(value, edge(segment(x, y, {cx - side * .22, 0}, {cx, cy}) - .019));
        }
        return value;
    }
    if (kind == Confetti || kind == Spark || kind == Sponge || kind == Grunge) {
        double value = 0;
        for (const auto& p : particles(kind)) {
            const auto dx = x - p.x, dy = y - p.y;
            const auto u = p.c * dx + p.s * dy;
            const auto v = -p.s * dx + p.c * dy;
            double part = 0;
            if (kind == Confetti) part = edge(box(u, v, p.w, p.h));
            else if (kind == Spark) {
                const auto length = p.w;
                part = edge(std::min(std::abs(u) / length + std::abs(v) / .013,
                    std::abs(v) / (length * .65) + std::abs(u) / .013) - 1);
            } else {
                const auto size = p.w;
                part = (1 - smooth(size * .55, size, std::hypot(u, v * (kind == Grunge ? 1.8 : 1))))
                    * p.opacity;
            }
            value = std::max(value, part);
        }
        return value;
    }
    if (kind == Smoke) {
        const auto warpX = x + .3 * (noise(3 * x, 3 * y, 819) - .5);
        const auto warpY = y + .3 * (noise(3 * x, 3 * y, 136) - .5);
        const auto n = turbulence(warpX, warpY);
        return (1 - smooth(.4, .95, radius)) * smooth(.25, .73, n)
            * (.25 + .75 * (1 - smooth(.05, .22, std::abs(n - .55))));
    }
    if (kind == Chalk || kind == Charcoal) {
        const auto n = turbulence(x, y);
        const auto silhouette = 1 - smooth(.66 + .18 * n, .86 + .1 * n, radius);
        const auto speckle = noise(x * 110, y * 110, 152);
        return silhouette * smooth(kind == Chalk ? .28 : .2, .72, n)
            * (.18 + .82 * smooth(.25, .7, speckle));
    }
    // Flat paint tools: parallel bundles across the long axis. Following the
    // stroke at +90 degrees trails these bundles into bristle lines.
    const auto fiber = noise(x * 32, 0, 614);
    const auto rough = noise(x * 22, y * 5, 905);
    const auto silhouette = edge(box(x, y, .91, .65 + .16 * rough));
    if (kind == Marker) return silhouette * (.38 + .62 * smooth(.32, .6, fiber))
        * (.65 + .35 * rough);
    if (kind == Acrylic) return silhouette * (.18 + .82 * smooth(.28, .65, fiber))
        * (.58 + .42 * rough);
    return silhouette * (.35 + .65 * fiber) * (1 - smooth(.35, .92, std::abs(y)));
}

struct MaskCache {
    std::array<std::shared_ptr<const GrayscaleMaskAsset>, Count> masks;
    BrushAssetCacheStats stats;
    MaskCache()
    {
        const auto presets = creativeBrushPresets();
        for (int k = Marker; k < Count; ++k) {
            if (k == Bristles) continue; // analytic hairs keep subpixel gaps sharp
            const auto extent = k >= Spark || k == Hatch ? 512U : 256U;
            std::vector<std::uint8_t> pixels(std::size_t(extent) * extent);
            for (unsigned y = 0; y < extent; ++y) for (unsigned x = 0; x < extent; ++x) {
                const auto value = generatedCoverage(Kind(k), (x + .5) / extent * 2 - 1, (y + .5) / extent * 2 - 1);
                pixels[std::size_t(y) * extent + x] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255));
            }
            auto mask = std::make_shared<GrayscaleMaskAsset>(presets[std::size_t(k)].settings.tip.assetId, 1, extent, extent, pixels);
            stats.retainedBytes += mask->retainedBytes(); ++stats.grayscaleMaskCount;
            masks[std::size_t(k)] = std::move(mask);
        }
    }
};
const MaskCache& cache() { static const MaskCache result; return result; }

class GeometricTip final : public IBrushTip {
public:
    bool supportsConcurrentSampling() const noexcept override { return true; }
    explicit GeometricTip(bool pixel) : pixel_(pixel) {}
    BrushTipBounds prepareDab(const BrushDab& dab, double hardness, double footprint) noexcept override
    {
        if (!(dab.diameterPixels > 0)) { w_ = h_ = 0; return {}; }
        center_ = dab.documentCenter;
        const auto size = pixel_ ? std::max(1.0, std::round(dab.diameterPixels)) : dab.diameterPixels;
        w_ = size * .5; h_ = w_ * dab.tipAspectRatio;
        if (pixel_) {
            const auto offset = std::fmod(size, 2) == 0 ? 0.0 : .5;
            center_ = {std::floor(center_.x) + offset, std::floor(center_.y) + offset};
        }
        const auto angle = dab.tipAngleDegrees * pi / 180;
        c_ = std::cos(angle); s_ = std::sin(angle);
        feather_ = std::max(.25, footprint * .5) + (1 - std::clamp(hardness, 0.0, 1.0)) * std::min(w_, h_);
        const auto ex = std::abs(c_) * w_ + std::abs(s_) * h_ + feather_;
        const auto ey = std::abs(s_) * w_ + std::abs(c_) * h_ + feather_;
        return {center_.x - ex, center_.y - ey, center_.x + ex, center_.y + ey};
    }
    double coverage(Vec2d point) const noexcept override
    {
        if (w_ <= 0 || h_ <= 0) return 0;
        const auto dx = point.x - center_.x, dy = point.y - center_.y;
        const auto x = dx * c_ + dy * s_, y = -dx * s_ + dy * c_;
        if (pixel_) return std::hypot(x / std::max(.5, w_ - .25), y / std::max(.5, h_ - .25)) <= 1 ? 1 : 0;
        return 1 - smooth(-feather_, feather_, box(x, y, w_, h_));
    }
private:
    bool pixel_;
    Vec2d center_;
    double w_ {}, h_ {}, c_ {1}, s_ {}, feather_ {.5};
};

class BristleTip final : public IBrushTip {
public:
    bool supportsConcurrentSampling() const noexcept override { return true; }
    BrushTipBounds prepareDab(const BrushDab& dab, double hardness, double footprint) noexcept override
    {
        if (!(dab.diameterPixels > 0)) { active_ = false; return {}; }
        active_ = true;
        center_ = dab.documentCenter;
        const auto w = dab.diameterPixels * .5;
        const auto h = w * std::clamp(dab.tipAspectRatio, .02, 1.0);
        const auto angle = dab.tipAngleDegrees * pi / 180;
        c_ = std::cos(angle); s_ = std::sin(angle);
        // Unequal loaded hairs, deliberately separated at everyday sizes.
        // Evaluate in document pixels: filtering a pre-rasterized thin strand
        // again spreads its AA into the gap before flow accumulates it.
        constexpr std::array<double, 8> centers {-.85, -.59, -.35, -.12, .13, .36, .61, .84};
        constexpr std::array<double, 8> radii {.025, .04, .032, .043, .025, .034, .028, .035};
        double ex = 0, ey = 0;
        for (std::size_t i = 0; i < hairs_.size(); ++i) {
            const auto r = w * radii[i];
            auto& hair = hairs_[i];
            hair = {w * centers[i], h * (random(i + 371) - .5) * .38,
                h * (.3 + random(i + 705) * .25), r,
                std::max(.25, footprint * .5) + (1 - std::clamp(hardness,0.0,1.0)) * r};
            ex = std::max(ex, std::abs(hair.x) + r + hair.feather);
            ey = std::max(ey, std::abs(hair.y) + hair.length + r + hair.feather);
        }
        const auto dx = std::abs(c_) * ex + std::abs(s_) * ey;
        const auto dy = std::abs(s_) * ex + std::abs(c_) * ey;
        return {center_.x-dx, center_.y-dy, center_.x+dx, center_.y+dy};
    }
    double coverage(Vec2d point) const noexcept override
    {
        if (!active_) return 0;
        const auto dx = point.x - center_.x, dy = point.y - center_.y;
        const auto x = dx * c_ + dy * s_, y = -dx * s_ + dy * c_;
        double coverage = 0;
        for (const auto& hair : hairs_) {
            const auto ax = std::abs(x - hair.x);
            const auto ay = std::max(0., std::abs(y - hair.y) - hair.length);
            if (ax >= hair.radius + hair.feather || ay >= hair.radius + hair.feather) continue;
            const auto distance = (ay > 0 ? std::hypot(ax, ay) : ax) - hair.radius;
            coverage = std::max(coverage, 1 - smooth(-hair.feather, hair.feather, distance));
        }
        return coverage;
    }
private:
    struct Hair { double x, y, length, radius, feather; };
    std::array<Hair, 8> hairs_ {};
    Vec2d center_;
    double c_ {1}, s_ {0};
    bool active_ {false};
};

class GeneratedTip final : public IBrushTip {
public:
    bool supportsConcurrentSampling() const noexcept override { return true; }
    explicit GeneratedTip(Kind kind) : kind_(kind), tip_(cache().masks[std::size_t(kind)], BitmapMaskTip::Filtering::Anisotropic) {}
    BrushTipBounds prepareDab(const BrushDab& source, double hardness, double footprint) noexcept override
    {
        auto dab = source;
        if (kind_ == Grunge || kind_ == Sponge || kind_ == Smoke || kind_ == Spark || kind_ == Confetti) {
            const auto key = source.deterministicSeed ^ mix(source.sequenceIndex + 1729);
            dab.tipAngleDegrees += random(key) * 360;
            dab.diameterPixels *= .82 + .18 * random(key + 1);
        }
        return tip_.prepareDab(dab, hardness, footprint);
    }
    double coverage(Vec2d point) const noexcept override { return tip_.coverage(point); }
private:
    Kind kind_;
    BitmapMaskTip tip_;
};
} // namespace

std::span<const BrushPresetRecord> creativeBrushPresets() noexcept
{
    static const auto presets = [] {
        std::array<BrushPresetRecord, Count> result;
        for (std::size_t i = 0; i < recipes.size(); ++i) {
            const auto& r = recipes[i];
            auto& preset = result[i];
            preset.id = std::string("builtin.preset.") + r.slug + ".v1";
            preset.displayName = r.name;
            auto& s = preset.settings;
            s.tip.assetId = std::string("builtin.tip.generated.") + r.slug + ".v1";
            s.sizePixels = r.size; s.tip.aspectRatio = r.aspect; s.tip.angleDegrees = r.angle;
            s.tip.rotationMode = r.follow ? BrushTipRotationMode::FollowStrokeDirection : BrushTipRotationMode::Fixed;
            s.hardness = r.hardness; s.flow = r.flow; s.spacingPercent = r.spacing;
            s.pressureToSize = r.pressureSize; s.pressureToFlow = r.pressureSize;
            s.deterministicSeed = 0x56424b00ULL + i;
            if (r.grain > 0) {
                s.grain.assetId = BrushAssetIds::DryInkPaperGrain;
                s.grain.strength = r.grain;
                s.grain.scalePixels = i == Chalk ? 28 : i == Charcoal ? 46 : 64;
            }
        }
        return result;
    }();
    return presets;
}
std::unique_ptr<IBrushTip> makeCreativeBrushTip(std::string_view id)
{
    const auto presets = creativeBrushPresets();
    for (std::size_t i = 0; i < presets.size(); ++i) if (presets[i].settings.tip.assetId == id) {
        if (i == Precision || i == Chisel) return std::make_unique<GeometricTip>(i == Precision);
        if (i == Bristles) return std::make_unique<BristleTip>();
        return std::make_unique<GeneratedTip>(Kind(i));
    }
    return {};
}
BrushAssetCacheStats creativeBrushCacheStats() noexcept { return cache().stats; }
bool isCreativeStampTip(std::string_view id) noexcept
{
    const auto presets = creativeBrushPresets();
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (presets[i].settings.tip.assetId == id) return recipes[i].spacing >= 75;
    return false;
}
} // namespace imageeditor::core
