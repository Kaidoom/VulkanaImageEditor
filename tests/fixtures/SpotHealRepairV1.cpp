#include "imageeditor/core/SpotHealRepair.hpp"

#include "imageeditor/core/Healing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
namespace {

constexpr float alphaFloor = 1.0e-6F;
constexpr double infinity = std::numeric_limits<double>::infinity();
using Texture = std::array<float, 2>;

struct Interrupted { };
struct WorkLimit { };
struct NoContext { };

bool usable(const PremultipliedColor& p)
{
    return p[3] > alphaFloor && p[3] <= 1
        && std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); });
}

double colorDifference(const PremultipliedColor& a, const PremultipliedColor& b)
{
    if (!usable(a) || !usable(b)) return 1;
    double sum = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        const double d = double(a[c]) / a[3] - double(b[c]) / b[3];
        sum += d * d;
    }
    const double da = a[3] - b[3];
    return sum / 3 + .15 * da * da;
}

float absoluteGradient(const PremultipliedColor& a, const PremultipliedColor& b)
{
    float sum = 0;
    for (std::size_t c = 0; c < 3; ++c)
        sum += static_cast<float>(std::abs(double(a[c]) / a[3] - double(b[c]) / b[3]));
    return sum / 3;
}

struct Level {
    int width {}, height {}, radius {3};
    std::vector<PremultipliedColor> original, image;
    std::vector<Texture> originalTexture, texture;
    std::vector<std::uint8_t> unknown, known, featureValid, donorValid, filled;
    std::vector<int> donors, active, repairOrder, field;
    std::vector<std::uint16_t> depth;
    std::vector<double> cost;
    std::uint16_t deepest {};

    [[nodiscard]] int count() const { return width * height; }
    [[nodiscard]] bool contains(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < width && y < height;
    }
    [[nodiscard]] int index(int x, int y) const { return y * width + x; }
};

class Repair {
public:
    Repair(const SpotHealPatch& patch, const SpotHealOptions& options)
        : patch_(patch), options_(options), random_(options.seed) { }

    SpotHealResult run()
    {
        check();
        if (patch_.width <= 0 || patch_.height <= 0 || !std::isfinite(options_.adaptation)
            || patch_.width > 32768 || patch_.height > 32768) {
            result_.diagnostics.message = "Spot Heal needs finite settings and valid image dimensions.";
            return std::move(result_);
        }
        const auto count = std::size_t(patch_.width) * std::size_t(patch_.height);
        if (count > options_.maxPixels || count > std::size_t(std::numeric_limits<int>::max())
            || count > options_.maxWorkingBytes / (options_.captureDiagnostics ? 288U : 256U))
            throw WorkLimit {};
        if (patch_.pixels.size() != count || patch_.unknown.size() != count
            || (!patch_.valid.empty() && patch_.valid.size() != count)) {
            result_.diagnostics.message = "Spot Heal reference and mask dimensions do not agree.";
            return std::move(result_);
        }
        // Includes conservative pyramid/vector capacity, final candidates and
        // the existing Heal operator's output + screened-solver scratch.
        result_.diagnostics.estimatedPeakWorkingBytes = count * (options_.captureDiagnostics ? 288U : 256U);
        Level base;
        base.width = patch_.width;
        base.height = patch_.height;
        base.original.resize(count);
        base.unknown.resize(count);
        base.known.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            if ((i & 4095U) == 0) check();
            base.unknown[i] = patch_.unknown[i] != 0;
            result_.diagnostics.unknownPixels += base.unknown[i];
            // This branch is the corruption-invariance boundary. Unknown
            // values (including NaNs/alpha) are not even inspected.
            if (!base.unknown[i] && (patch_.valid.empty() || patch_.valid[i])
                && usable(patch_.pixels[i])) {
                base.original[i] = patch_.pixels[i];
                base.known[i] = 1;
            }
        }
        if (!result_.diagnostics.unknownPixels) {
            result_.status = SpotHealStatus::NoUnknownPixels;
            result_.pixels.assign(patch_.pixels.begin(), patch_.pixels.end());
            result_.diagnostics.message = "Spot Heal has no marked region.";
            report(1);
            return std::move(result_);
        }
        base.radius = std::clamp(std::min(base.width, base.height) / 16, 1, 4);
        computeFeatures(base);
        prepare(base);
        if (base.donors.empty() || base.repairOrder.empty()) throw NoContext {};
        // A narrow scratch still needs patches spanning BOTH of its banks;
        // choosing support from image dimensions leaves its center constrained
        // only by provisional estimates and can invent short edge fragments.
        const int desiredRadius = std::min(std::max(1, std::min(base.width, base.height) / 8),
            std::clamp(int(base.deepest) + 1, 3, 7));
        if (desiredRadius != base.radius) {
            base.radius = desiredRadius;
            prepare(base);
        }
        result_.diagnostics.patchRadius = static_cast<unsigned>(base.radius);
        result_.diagnostics.validDonorCenters = base.donors.size();
        std::vector<Level> pyramid;
        pyramid.push_back(std::move(base));
        while (pyramid.size() < 5 && pyramid.back().deepest > 5
            && std::min(pyramid.back().width, pyramid.back().height) >= 40) {
            auto next = reduce(pyramid.back());
            prepare(next);
            if (next.donors.empty() || next.repairOrder.empty()) break;
            pyramid.push_back(std::move(next));
        }
        result_.diagnostics.pyramidLevels = static_cast<unsigned>(pyramid.size());
        report(.05);
        for (std::size_t level = pyramid.size(); level-- > 0;) {
            check();
            auto& current = pyramid[level];
            initialize(current, level + 1 < pyramid.size() ? &pyramid[level + 1] : nullptr);
            constexpr unsigned iterations = 7;
            for (unsigned iteration = 0; iteration < iterations; ++iteration) {
                const bool reverse = (iteration & 1U) != 0;
                for (std::size_t ordinal = 0; ordinal < current.active.size(); ++ordinal) {
                    if ((ordinal & 63U) == 0) check();
                    const int p = current.active[reverse ? current.active.size() - 1 - ordinal : ordinal];
                    search(current, p, reverse, false, iteration < 2 ? .35 : .7);
                }
                // Continuous optimization may average compatible votes on
                // coarse grids. The source-resolution reconstruction always
                // selects an actual exemplar pixel, never a texture-smearing
                // average of incompatible patches.
                reconstruct(current, level != 0 && iteration + 1 < iterations);
                ++result_.diagnostics.refinementIterations;
                const double done = double(pyramid.size() - 1 - level)
                    + double(iteration + 1) / iterations;
                report(.05 + .8 * done / double(pyramid.size()));
            }
        }
        auto& finest = pyramid.front();
        result_.pixels.assign(patch_.pixels.begin(), patch_.pixels.end());
        for (const int p : finest.repairOrder)
            result_.pixels[std::size_t(p)] = finest.image[std::size_t(p)];
        smoothModels(finest);
        if (options_.captureDiagnostics) {
            result_.diagnostics.beforeAdaptation = result_.pixels;
            result_.diagnostics.invalidDonors.resize(count);
            result_.diagnostics.donors.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                result_.diagnostics.invalidDonors[i] = finest.donorValid[i] ? 0 : 255;
                if (finest.unknown[i] && finest.field[i] >= 0)
                    result_.diagnostics.donors[i] = {
                        finest.field[i] % finest.width, finest.field[i] / finest.width };
            }
        }
        adapt(finest);
        check();
        result_.status = SpotHealStatus::Complete;
        result_.diagnostics.message = "Spot Heal reconstructed the marked region from valid surrounding texture.";
        report(1);
        return std::move(result_);
    }

    SpotHealResult failed(SpotHealStatus status, std::string message)
    {
        result_.status = status;
        std::vector<PremultipliedColor>().swap(result_.pixels);
        std::vector<SpotHealDonor>().swap(result_.diagnostics.donors);
        std::vector<std::uint8_t>().swap(result_.diagnostics.invalidDonors);
        std::vector<PremultipliedColor>().swap(result_.diagnostics.beforeAdaptation);
        result_.diagnostics.message = std::move(message);
        return std::move(result_);
    }

private:
    const SpotHealPatch& patch_;
    const SpotHealOptions& options_;
    SpotHealResult result_;
    std::uint64_t random_;
    std::vector<std::uint8_t> modelled_;

    void check() const
    {
        if (options_.cancelled && options_.cancelled()) throw Interrupted {};
    }
    void report(double progress) const
    {
        check();
        if (options_.progress) options_.progress(progress);
    }
    std::uint64_t random()
    {
        // Fixed SplitMix64 sequence, not implementation-defined distributions.
        std::uint64_t z = (random_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
    int randomCoordinate(int low, int high)
    {
        return low + static_cast<int>(random() % static_cast<unsigned>(high - low + 1));
    }

    void computeFeatures(Level& level)
    {
        const auto size = std::size_t(level.count());
        level.originalTexture.resize(size);
        level.featureValid.assign(size, 0);
        for (int y = 1; y + 1 < level.height; ++y) {
            check();
            for (int x = 1; x + 1 < level.width; ++x) {
                const int p = level.index(x, y);
                if (!level.known[std::size_t(p)] || !level.known[std::size_t(p - 1)]
                    || !level.known[std::size_t(p + 1)] || !level.known[std::size_t(p - level.width)]
                    || !level.known[std::size_t(p + level.width)]
                    || !level.known[std::size_t(p - level.width - 1)]
                    || !level.known[std::size_t(p - level.width + 1)]
                    || !level.known[std::size_t(p + level.width - 1)]
                    || !level.known[std::size_t(p + level.width + 1)]) continue;
                const auto& center = level.original[std::size_t(p)];
                level.originalTexture[std::size_t(p)] = {
                    .5F * (absoluteGradient(center, level.original[std::size_t(p - 1)])
                        + absoluteGradient(center, level.original[std::size_t(p + 1)])),
                    .5F * (absoluteGradient(center, level.original[std::size_t(p - level.width)])
                        + absoluteGradient(center, level.original[std::size_t(p + level.width)])) };
                level.featureValid[std::size_t(p)] = 1;
            }
        }
    }

    Level reduce(const Level& fine)
    {
        Level coarse;
        coarse.width = (fine.width + 1) / 2;
        coarse.height = (fine.height + 1) / 2;
        coarse.radius = 3;
        const auto count = std::size_t(coarse.count());
        coarse.original.resize(count);
        coarse.originalTexture.resize(count);
        coarse.unknown.resize(count);
        coarse.known.resize(count);
        coarse.featureValid.resize(count);
        for (int y = 0; y < coarse.height; ++y) {
            check();
            for (int x = 0; x < coarse.width; ++x) {
                const auto p = std::size_t(coarse.index(x, y));
                bool valid = true, featureValid = true;
                unsigned samples = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = x * 2 + dx, sy = y * 2 + dy;
                        if (!fine.contains(sx, sy)) { valid = false; continue; }
                        const auto q = std::size_t(fine.index(sx, sy));
                        coarse.unknown[p] |= fine.unknown[q];
                        valid &= fine.known[q] != 0;
                        featureValid &= fine.featureValid[q] != 0;
                        if (fine.known[q]) {
                            for (std::size_t c = 0; c < 4; ++c)
                                coarse.original[p][c] += fine.original[q][c] * .25F;
                            for (std::size_t c = 0; c < 2; ++c)
                                coarse.originalTexture[p][c] += fine.originalTexture[q][c] * .25F;
                            ++samples;
                        }
                    }
                // Do not normalize a mixed known/unknown cell into trusted
                // context: its complete box-filter support must be known.
                coarse.known[p] = valid && samples == 4 && !coarse.unknown[p];
                coarse.featureValid[p] = coarse.known[p] && featureValid;
                if (!coarse.known[p]) {
                    coarse.original[p] = {};
                    coarse.originalTexture[p] = {};
                }
            }
        }
        return coarse;
    }

    void prepare(Level& level)
    {
        const int count = level.count();
        const auto size = std::size_t(count);
        level.donors.clear();
        level.active.clear();
        level.repairOrder.clear();
        level.deepest = 0;
        level.image = level.original;
        level.texture = level.originalTexture;
        level.filled = level.known;
        level.field.assign(size, -1);
        level.cost.assign(size, infinity);
        level.donorValid.assign(size, 0);
        level.depth.assign(size, std::numeric_limits<std::uint16_t>::max());
        // Integral invalid count makes whole-patch plus inherited descriptor
        // support validation O(1). No center-only validity shortcuts.
        std::vector<std::uint32_t> invalid(std::size_t(level.width + 1) * std::size_t(level.height + 1));
        const int stride = level.width + 1;
        for (int y = 0; y < level.height; ++y) {
            check();
            std::uint32_t row = 0;
            for (int x = 0; x < level.width; ++x) {
                const auto p = std::size_t(level.index(x, y));
                row += !(level.known[p] && level.featureValid[p]);
                invalid[std::size_t(y + 1) * std::size_t(stride) + std::size_t(x + 1)]
                    = invalid[std::size_t(y) * std::size_t(stride) + std::size_t(x + 1)] + row;
                if (level.known[p]) level.depth[p] = 0;
            }
        }
        auto rangeInvalid = [&](int x, int y, int r) {
            const auto a = std::size_t(y - r) * std::size_t(stride);
            const auto b = std::size_t(y + r + 1) * std::size_t(stride);
            return invalid[b + std::size_t(x + r + 1)] - invalid[a + std::size_t(x + r + 1)]
                - invalid[b + std::size_t(x - r)] + invalid[a + std::size_t(x - r)];
        };
        for (int radius = level.radius; radius >= 1; --radius) {
            for (int y = radius; y + radius < level.height; ++y) {
                check();
                for (int x = radius; x + radius < level.width; ++x)
                    if (rangeInvalid(x, y, radius) == 0) {
                        const int p = level.index(x, y);
                        level.donorValid[std::size_t(p)] = 1;
                        level.donors.push_back(p);
                    }
            }
            if (!level.donors.empty()) { level.radius = radius; break; }
        }
        // Eight-connected distance from trusted original context, with a
        // bounded integer metric. Invalid pixels are not trusted boundary.
        auto relax = [&](int p, int q) {
            level.depth[std::size_t(p)] = std::min(level.depth[std::size_t(p)],
                static_cast<std::uint16_t>(std::min(65535, int(level.depth[std::size_t(q)]) + 1)));
        };
        for (int y = 0; y < level.height; ++y) {
            check();
            for (int x = 0; x < level.width; ++x) {
                const int p = level.index(x, y);
                if (x) relax(p, p - 1);
                if (y) {
                    relax(p, p - level.width);
                    if (x) relax(p, p - level.width - 1);
                    if (x + 1 < level.width) relax(p, p - level.width + 1);
                }
            }
        }
        for (int y = level.height; y-- > 0;) {
            check();
            for (int x = level.width; x-- > 0;) {
                const int p = level.index(x, y);
                if (x + 1 < level.width) relax(p, p + 1);
                if (y + 1 < level.height) {
                    relax(p, p + level.width);
                    if (x) relax(p, p + level.width - 1);
                    if (x + 1 < level.width) relax(p, p + level.width + 1);
                }
            }
        }
        std::vector<std::uint8_t> active(size);
        for (int p = 0; p < count; ++p) {
            if ((static_cast<unsigned>(p) & 4095U) == 0) check();
            if (!level.unknown[std::size_t(p)]) continue;
            level.repairOrder.push_back(p);
            level.deepest = std::max(level.deepest, level.depth[std::size_t(p)]);
            const int x = p % level.width, y = p / level.width;
            for (int dy = -level.radius; dy <= level.radius; ++dy)
                for (int dx = -level.radius; dx <= level.radius; ++dx)
                    if (level.contains(x + dx, y + dy))
                        active[std::size_t(level.index(x + dx, y + dy))] = 1;
        }
        for (int p = 0; p < count; ++p)
            if (active[std::size_t(p)] && (level.known[std::size_t(p)] || level.unknown[std::size_t(p)]))
                level.active.push_back(p);
        std::stable_sort(level.repairOrder.begin(), level.repairOrder.end(), [&](int a, int b) {
            return level.depth[std::size_t(a)] < level.depth[std::size_t(b)];
        });
    }

    double distance(Level& level, int target, int donor, double provisionalWeight)
    {
        if (++result_.diagnostics.comparisons > options_.maxComparisons) throw WorkLimit {};
        if ((result_.diagnostics.comparisons & 1023U) == 0) check();
        if (donor < 0 || donor >= level.count() || !level.donorValid[std::size_t(donor)]) return infinity;
        const int tx = target % level.width, ty = target / level.width;
        const int sx = donor % level.width, sy = donor / level.width;
        double cost = 0, weights = 0;
        for (int dy = -level.radius; dy <= level.radius; ++dy)
            for (int dx = -level.radius; dx <= level.radius; ++dx) {
                if (!level.contains(tx + dx, ty + dy)) continue;
                const auto p = std::size_t(level.index(tx + dx, ty + dy));
                if (!level.filled[p]) continue;
                const auto q = std::size_t(level.index(sx + dx, sy + dy));
                const auto& a = level.image[p];
                const auto& b = level.original[q];
                if (!usable(a)) continue;
                const double weight = (level.known[p] ? 1.0 : provisionalWeight)
                    * std::min(a[3], b[3]);
                double d = colorDifference(a, b);
                if (level.featureValid[p] || !level.known[p])
                    for (std::size_t c = 0; c < 2; ++c) {
                        const double gradient = level.texture[p][c] - level.originalTexture[q][c];
                        d += .65 * gradient * gradient;
                    }
                cost += weight * d;
                weights += weight;
            }
        if (weights < .25) return infinity;
        // A very weak, continuous local preference breaks near-equivalent
        // matches without cutting off a better distant structural exemplar.
        const double deltaX = double(tx - sx), deltaY = double(ty - sy);
        const double locality = 1.0e-7 * std::log1p((deltaX * deltaX + deltaY * deltaY)
            / double(level.radius * level.radius + 1));
        return cost / weights + locality;
    }

    void consider(Level& level, int target, int donor, double provisionalWeight)
    {
        if (donor < 0 || donor >= level.count() || !level.donorValid[std::size_t(donor)]) return;
        const double candidate = distance(level, target, donor, provisionalWeight);
        auto& cost = level.cost[std::size_t(target)];
        auto& best = level.field[std::size_t(target)];
        if (candidate < cost || (candidate == cost && (best < 0 || donor < best))) {
            cost = candidate;
            best = donor;
        }
    }

    void translated(Level& level, int p, int q, double provisionalWeight)
    {
        if (q < 0 || q >= level.count() || level.field[std::size_t(q)] < 0) return;
        const int donor = level.field[std::size_t(q)];
        const int x = donor % level.width + p % level.width - q % level.width;
        const int y = donor / level.width + p / level.width - q / level.width;
        if (level.contains(x, y)) consider(level, p, level.index(x, y), provisionalWeight);
    }

    void search(Level& level, int p, bool reverse, bool initial, double provisionalWeight)
    {
        level.cost[std::size_t(p)] = infinity;
        consider(level, p, level.field[std::size_t(p)], provisionalWeight);
        const int x = p % level.width, y = p / level.width;
        const int step = reverse ? 1 : -1;
        if (level.contains(x + step, y)) translated(level, p, p + step, provisionalWeight);
        if (level.contains(x, y + step)) translated(level, p, p + step * level.width, provisionalWeight);
        if (initial) {
            // Boundary initialization needs diverse starts before a coherent
            // field exists. Deterministic spatial strata cover the reference;
            // later PatchMatch propagation carries successful offsets inward.
            const std::size_t stride = std::max(std::size_t(1), level.donors.size() / 128);
            const std::size_t start = static_cast<std::size_t>(random() % stride);
            for (std::size_t i = start; i < level.donors.size(); i += stride)
                consider(level, p, level.donors[i], provisionalWeight);
            for (int dy = -12; dy <= 12; dy += 3)
                for (int dx = -12; dx <= 12; dx += 3)
                    if (level.contains(x + dx, y + dy))
                        consider(level, p, level.index(x + dx, y + dy), provisionalWeight);
        }
        // Global-to-local random search plus independent restarts. The donor
        // domain is the complete supplied coherent reference, not brush size.
        for (int restart = 0; restart < (initial ? 12 : 3); ++restart)
            consider(level, p, level.donors[static_cast<std::size_t>(random() % level.donors.size())],
                provisionalWeight);
        for (int radius = std::max(level.width, level.height); radius >= 1; radius /= 2) {
            const int best = level.field[std::size_t(p)];
            const int bx = best >= 0 ? best % level.width : x;
            const int by = best >= 0 ? best / level.width : y;
            for (int trial = 0; trial < 2; ++trial) {
                const int sx = randomCoordinate(std::max(0, bx - radius), std::min(level.width - 1, bx + radius));
                const int sy = randomCoordinate(std::max(0, by - radius), std::min(level.height - 1, by + radius));
                consider(level, p, level.index(sx, sy), provisionalWeight);
            }
        }
    }

    void initialize(Level& level, const Level* coarse)
    {
        if (coarse) {
            for (const int p : level.repairOrder) {
                const int x = p % level.width, y = p / level.width;
                const int q = coarse->index(x / 2, y / 2);
                if (coarse->field[std::size_t(q)] < 0) continue;
                const int cd = coarse->field[std::size_t(q)];
                const int dx = (cd % coarse->width - x / 2) * 2;
                const int dy = (cd / coarse->width - y / 2) * 2;
                if (!level.contains(x + dx, y + dy)) continue;
                const int donor = level.index(x + dx, y + dy);
                if (!level.donorValid[std::size_t(donor)]) continue;
                level.field[std::size_t(p)] = donor;
                level.image[std::size_t(p)] = level.original[std::size_t(donor)];
                level.texture[std::size_t(p)] = level.originalTexture[std::size_t(donor)];
                level.filled[std::size_t(p)] = 1;
            }
        }
        for (std::size_t ordinal = 0; ordinal < level.repairOrder.size(); ++ordinal) {
            if ((ordinal & 31U) == 0) check();
            const int p = level.repairOrder[ordinal];
            const int x = p % level.width, y = p / level.width;
            // All four neighbors help an inward front regardless of scan order.
            level.cost[std::size_t(p)] = infinity;
            if (x + 1 < level.width) translated(level, p, p + 1, .25);
            if (y + 1 < level.height) translated(level, p, p + level.width, .25);
            search(level, p, false, true, .25);
            const int donor = level.field[std::size_t(p)];
            if (donor < 0) throw NoContext {};
            level.image[std::size_t(p)] = level.original[std::size_t(donor)];
            level.texture[std::size_t(p)] = level.originalTexture[std::size_t(donor)];
            level.filled[std::size_t(p)] = 1;
        }
        for (const int p : level.active)
            if (!level.unknown[std::size_t(p)]) search(level, p, false, true, .25);
    }

    void reconstruct(Level& level, bool average)
    {
        auto image = level.image;
        auto texture = level.texture;
        auto field = level.field;
        for (const int p : level.repairOrder) {
            if ((static_cast<unsigned>(p) & 63U) == 0) check();
            const int x = p % level.width, y = p / level.width;
            int best = level.field[std::size_t(p)];
            double bestCost = infinity;
            PremultipliedColor sum {};
            Texture textureSum {};
            double weights = 0;
            for (int dy = -level.radius; dy <= level.radius; ++dy)
                for (int dx = -level.radius; dx <= level.radius; ++dx) {
                    if (!level.contains(x + dx, y + dy)) continue;
                    const int q = level.index(x + dx, y + dy);
                    const int d = level.field[std::size_t(q)];
                    if (d < 0) continue;
                    const int sx = d % level.width - dx, sy = d / level.width - dy;
                    if (!level.contains(sx, sy)) continue;
                    const int donor = level.index(sx, sy);
                    if (!level.donorValid[std::size_t(donor)]) continue;
                    const double cost = level.cost[std::size_t(q)]
                        * (1 + .05 * double(dx * dx + dy * dy) / double(level.radius * level.radius));
                    if (cost < bestCost || (cost == bestCost && donor < best)) {
                        bestCost = cost;
                        best = donor;
                    }
                    if (average && std::isfinite(cost)) {
                        const double weight = 1 / (cost + 1.0e-5);
                        for (std::size_t c = 0; c < 4; ++c)
                            sum[c] += static_cast<float>(level.original[std::size_t(donor)][c] * weight);
                        for (std::size_t c = 0; c < 2; ++c)
                            textureSum[c] += static_cast<float>(level.originalTexture[std::size_t(donor)][c] * weight);
                        weights += weight;
                    }
                }
            if (best < 0) throw NoContext {};
            field[std::size_t(p)] = best;
            image[std::size_t(p)] = level.original[std::size_t(best)];
            texture[std::size_t(p)] = level.originalTexture[std::size_t(best)];
            if (average && weights > 0) {
                for (std::size_t c = 0; c < 4; ++c) image[std::size_t(p)][c] = static_cast<float>(sum[c] / weights);
                for (std::size_t c = 0; c < 2; ++c) texture[std::size_t(p)][c] = static_cast<float>(textureSum[c] / weights);
            }
        }
        level.image = std::move(image);
        level.texture = std::move(texture);
        level.field = std::move(field);
    }

    void smoothModels(const Level& level)
    {
        modelled_.assign(std::size_t(level.count()), 0);
        std::vector<std::uint8_t> visited(std::size_t(level.count()));
        std::vector<int> component;
        using Basis = std::array<double, 3>;
        using Coefficients = std::array<Basis, 4>;
        for (const int seed : level.repairOrder) {
            if (visited[std::size_t(seed)]) continue;
            check();
            component.clear();
            component.push_back(seed);
            visited[std::size_t(seed)] = 1;
            int left = level.width, top = level.height, right = 0, bottom = 0;
            for (std::size_t head = 0; head < component.size(); ++head) {
                const int p = component[head], x = p % level.width, y = p / level.width;
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!level.contains(x + dx, y + dy)) continue;
                        const int q = level.index(x + dx, y + dy);
                        if (level.unknown[std::size_t(q)] && !visited[std::size_t(q)]) {
                            visited[std::size_t(q)] = 1;
                            component.push_back(q);
                        }
                    }
            }
            const double cx = (left + right) * .5, cy = (top + bottom) * .5;
            const double scale = std::max(1.0, double(std::max(right - left, bottom - top)) * .5);
            const int halo = std::max(6, level.radius);
            left = std::max(0, left - halo); top = std::max(0, top - halo);
            right = std::min(level.width - 1, right + halo);
            bottom = std::min(level.height - 1, bottom + halo);
            std::array<Basis, 3> matrix {};
            Coefficients rhs {}, coefficients {};
            std::array<double, 4> totals {};
            double weightSum = 0;
            std::size_t samples = 0;
            auto sample = [&](int x, int y, const auto& function) {
                const auto p = std::size_t(level.index(x, y));
                if (!level.known[p]) return;
                const auto& color = level.original[p];
                const double weight = color[3];
                const Basis basis {1, (x - cx) / scale, (y - cy) / scale};
                std::array<double, 4> values {double(color[0]) / color[3], double(color[1]) / color[3],
                    double(color[2]) / color[3], color[3]};
                function(basis, values, weight);
            };
            for (int y = top; y <= bottom; ++y) {
                check();
                for (int x = left; x <= right; ++x)
                    sample(x, y, [&](const Basis& basis, const auto& values, double weight) {
                        ++samples;
                        weightSum += weight;
                        for (std::size_t row = 0; row < 3; ++row) {
                            for (std::size_t column = 0; column < 3; ++column)
                                matrix[row][column] += weight * basis[row] * basis[column];
                            for (std::size_t c = 0; c < 4; ++c) rhs[c][row] += weight * basis[row] * values[c];
                        }
                        for (std::size_t c = 0; c < 4; ++c) totals[c] += weight * values[c];
                    });
            }
            if (samples < 16 || weightSum <= 0) continue;
            bool solved = true;
            for (std::size_t c = 0; c < 4; ++c) {
                auto a = matrix;
                auto b = rhs[c];
                for (std::size_t column = 0; column < 3; ++column) {
                    std::size_t pivot = column;
                    for (std::size_t row = column + 1; row < 3; ++row)
                        if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
                    if (std::abs(a[pivot][column]) < weightSum * 1.0e-10) { solved = false; break; }
                    std::swap(a[pivot], a[column]); std::swap(b[pivot], b[column]);
                    for (std::size_t row = column + 1; row < 3; ++row) {
                        const double factor = a[row][column] / a[column][column];
                        for (std::size_t k = column; k < 3; ++k) a[row][k] -= factor * a[column][k];
                        b[row] -= factor * b[column];
                    }
                }
                if (!solved) break;
                for (std::size_t row = 3; row-- > 0;) {
                    double value = b[row];
                    for (std::size_t k = row + 1; k < 3; ++k) value -= a[row][k] * coefficients[c][k];
                    coefficients[c][row] = value / a[row][row];
                }
            }
            if (!solved) continue;
            auto evaluate = [&](const Basis& basis, std::size_t c) {
                return std::inner_product(basis.begin(), basis.end(), coefficients[c].begin(), 0.0);
            };
            double residual = 0, variation = 0, maximumError = 0, quantizationVariance = 0;
            for (int y = top; y <= bottom; ++y) {
                check();
                for (int x = left; x <= right; ++x)
                    sample(x, y, [&](const Basis& basis, const auto& values, double weight) {
                        for (std::size_t c = 0; c < 3; ++c) {
                            const double error = values[c] - evaluate(basis, c);
                            const double centered = values[c] - totals[c] / weightSum;
                            residual += weight * error * error;
                            variation += weight * centered * centered;
                            maximumError = std::max(maximumError, std::abs(error));
                            const auto code = linearToSrgb(values[c]);
                            // Only exact decoded RGBA8 levels receive a noise
                            // allowance; arbitrary float appearance is not
                            // silently assumed to have quantization noise.
                            if (std::abs(values[c] - srgbToLinear(code)) < 1.0e-6) {
                                const auto low = static_cast<std::uint8_t>(std::max(0, int(code) - 1));
                                const auto high = static_cast<std::uint8_t>(std::min(255, int(code) + 1));
                                const double step = (srgbToLinear(high) - srgbToLinear(low))
                                    / double(int(high) - int(low));
                                quantizationVariance += weight * step * step / 12;
                            }
                        }
                    });
            }
            // Account for the measured sRGB8 code-bin variance (step²/12)
            // before testing explained spatial variation. Otherwise even a
            // truly linear lighting ramp fails on quantized authoritative
            // pixels. The finite-sample margin does not absorb genuine grain:
            // noise exceeding code-bin uncertainty still rejects the model.
            const double excessResidual = std::max(0.0, residual - 1.25 * quantizationVariance);
            if (excessResidual / (3 * weightSum) > 2.25e-6 || maximumError > .006
                || excessResidual > .01 * std::max(variation, weightSum * 1.0e-12)) continue;
            for (const int p : component) {
                const Basis basis {1, (p % level.width - cx) / scale, (p / level.width - cy) / scale};
                auto& pixel = result_.pixels[std::size_t(p)];
                pixel[3] = static_cast<float>(std::clamp(evaluate(basis, 3), 0.0, 1.0));
                for (std::size_t c = 0; c < 3; ++c)
                    pixel[c] = static_cast<float>(std::clamp(evaluate(basis, c), 0.0, 1.0) * pixel[3]);
                modelled_[std::size_t(p)] = 1;
            }
            result_.diagnostics.smoothModelPixels += component.size();
        }
    }

    void adapt(const Level& level)
    {
        if (options_.adaptation <= 0 || result_.diagnostics.smoothModelPixels == level.repairOrder.size()) return;
        check();
        // Build a candidate continuation in the known context ring from the
        // same correspondence field. Copying the destination into this ring
        // would erase the lighting residual and make adaptation meaningless.
        std::vector<PremultipliedColor> candidate = result_.pixels;
        std::vector<PremultipliedColor> destination = level.original;
        std::vector<float> coverage(std::size_t(level.count()));
        for (const int p : level.active) {
            const int donor = level.field[std::size_t(p)];
            if (donor >= 0 && level.donorValid[std::size_t(donor)])
                candidate[std::size_t(p)] = level.original[std::size_t(donor)];
        }
        for (const int p : level.repairOrder) {
            candidate[std::size_t(p)] = result_.pixels[std::size_t(p)];
            coverage[std::size_t(p)] = 1;
            // Defect gradients/alpha cannot enter the boundary fit/PDE.
            destination[std::size_t(p)] = {};
        }
        HealingOptions settings;
        settings.adaptation = std::clamp(options_.adaptation, 0.0F, 1.0F);
        settings.maxPixels = std::size_t(level.count());
        settings.cancelled = options_.cancelled;
        const auto healed = healPatch({level.width, level.height, candidate, destination, coverage}, settings);
        if (healed.status == HealingStatus::Cancelled) throw Interrupted {};
        if (healed.status != HealingStatus::Converged || healed.pixels.size() != candidate.size()) return;
        for (const int p : level.repairOrder)
            if (!modelled_[std::size_t(p)]) result_.pixels[std::size_t(p)] = healed.pixels[std::size_t(p)];
        result_.diagnostics.adaptationApplied = true;
        report(.98);
    }
};

} // namespace

SpotHealResult repairSpotHeal(const SpotHealPatch& patch, const SpotHealOptions& options)
{
    Repair repair(patch, options);
    try {
        return repair.run();
    } catch (const Interrupted&) {
        return repair.failed(SpotHealStatus::Cancelled, "Spot Heal cancelled; no repair was published.");
    } catch (const WorkLimit&) {
        return repair.failed(SpotHealStatus::LimitExceeded,
            "Spot Heal exceeds its memory or reconstruction budget. Try a shorter stroke.");
    } catch (const NoContext&) {
        return repair.failed(SpotHealStatus::InsufficientContext,
            "Spot Heal needs more unmarked, visible surrounding texture to reconstruct this region.");
    } catch (const std::bad_alloc&) {
        return repair.failed(SpotHealStatus::LimitExceeded,
            "Spot Heal could not allocate its bounded repair buffers. Try a smaller repair.");
    }
}

} // namespace imageeditor::core
