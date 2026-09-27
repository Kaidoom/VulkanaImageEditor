#include "imageeditor/core/SpotHealRepair.hpp"

#include "imageeditor/core/Healing.hpp"
#include "imageeditor/core/BoundedParallel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
namespace {

constexpr float alphaFloor = 1.0e-6F;
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr std::uint64_t randomIncrement = 0x9e3779b97f4a7c15ULL;
using Texture = std::array<float, 2>;

struct Interrupted { };
struct WorkLimit { };
struct NoContext { };

bool usable(const PremultipliedColor& p)
{
    return p[3] > alphaFloor && p[3] <= 1
        && std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v); });
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
    std::vector<std::array<double, 3>> originalColor;
    std::vector<int> contextActive;
    DiagonalWavefront activeSchedule, contextSchedule;
    std::vector<PremultipliedColor> nextImage;
    std::vector<Texture> nextTexture;
    std::vector<int> nextField;
    std::uint16_t deepest {};

    [[nodiscard]] int count() const { return width * height; }
    [[nodiscard]] bool contains(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < width && y < height;
    }
    [[nodiscard]] int index(int x, int y) const { return y * width + x; }
};

struct TargetSample {
    int offset {};
    std::array<double, 3> color {};
    float alpha {};
    Texture texture {};
    double factor {};
    bool compareTexture {};
};

struct TargetPatch {
    // Matching support is at most radius 7. Keep this scratch per solve, not
    // per candidate; sample order remains exactly the original dy/dx order.
    std::array<TargetSample, 225> samples;
    std::size_t size {};
    double maximumWeight {};
};

struct alignas(64) SearchContext {
    TargetPatch target;
    std::uint64_t random {};
    SpotHealCounters workCounters;
    SpotHealCounters* counters {};
    unsigned rank {};
};

class Repair {
public:
    Repair(const SpotHealPatch& patch, const SpotHealOptions& options)
        : patch_(patch), options_(options), random_(options.seed) { }

    SpotHealResult run()
    {
        started_ = tick();
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
        // The prepared target has a fixed maximum patch capacity even on tiny
        // inputs. Account for the first context explicitly: a per-pixel budget
        // alone cannot cover this fixed cost for every supported grid size.
        if (sizeof(SearchContext) > options_.maxWorkingBytes
            - result_.diagnostics.estimatedPeakWorkingBytes) throw WorkLimit {};
        result_.diagnostics.estimatedPeakWorkingBytes += sizeof(SearchContext);
        // An optional immutable double-color cache removes repeated divisions.
        // If the existing context leaves too little room, retain the exact
        // uncached calculation, never narrow the donor domain to fit a cache.
        const auto cacheAllowance = count * 40U;
        cacheOriginalColor_ = cacheAllowance <= options_.maxWorkingBytes
            - result_.diagnostics.estimatedPeakWorkingBytes;
        if (cacheOriginalColor_) result_.diagnostics.estimatedPeakWorkingBytes += cacheAllowance;
        // The original peak budget already covers one reconstruction copy.
        // Reusing copies across pyramid levels adds at most the smaller levels
        // (~one third of the finest 28-byte copy), rounded conservatively up.
        const auto reconstructionAllowance = count * 16U;
        keepReconstructionScratch_ = reconstructionAllowance <= options_.maxWorkingBytes
            - result_.diagnostics.estimatedPeakWorkingBytes;
        if (keepReconstructionScratch_) result_.diagnostics.estimatedPeakWorkingBytes += reconstructionAllowance;
        unsigned requestedWorkers = boundedParallelWorkerCount(options_.workerCount);
        // Retain the complete context when auxiliary parallel scratch does not
        // fit; worker count is an engineering choice, never a donor restriction.
        // Both diagonal schedules (entries + offsets + known-band IDs), summed
        // over the conservative pyramid, fit in 64 extra bytes per input pixel.
        const auto scheduleAllowance = count * 64U;
        while (requestedWorkers > 1 && scheduleAllowance + std::size_t(requestedWorkers - 1) * sizeof(SearchContext)
            > options_.maxWorkingBytes - result_.diagnostics.estimatedPeakWorkingBytes) --requestedWorkers;
        contexts_.resize(requestedWorkers);
        result_.diagnostics.estimatedPeakWorkingBytes += std::size_t(requestedWorkers - 1) * sizeof(SearchContext);
        if (requestedWorkers > 1) result_.diagnostics.estimatedPeakWorkingBytes += scheduleAllowance;
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
        if (options_.captureProfile) {
            result_.diagnostics.profile.preparationMilliseconds = elapsed(started_);
            result_.diagnostics.profile.levels.reserve(pyramid.size());
        }
        report(.05);
        for (std::size_t level = pyramid.size(); level-- > 0;) {
            check();
            auto& current = pyramid[level];
            SpotHealLevelProfile* profile = nullptr;
            if (options_.captureProfile) {
                auto& entries = result_.diagnostics.profile.levels;
                entries.emplace_back();
                profile = &entries.back();
                profile->level = static_cast<unsigned>(level);
                profile->width = current.width; profile->height = current.height;
                profile->radius = static_cast<unsigned>(current.radius);
                profile->thickness = current.deepest;
                profile->unknownPixels = current.repairOrder.size();
                profile->activePixels = current.active.size();
                profile->validDonors = current.donors.size();
                counters_ = &profile->initialization;
            }
            const auto initializationStart = tick();
            initialize(current, level + 1 < pyramid.size() ? &pyramid[level + 1] : nullptr, profile);
            if (profile) profile->initializationMilliseconds = elapsed(initializationStart);
            constexpr unsigned iterations = 7;
            for (unsigned iteration = 0; iteration < iterations; ++iteration) {
                SpotHealIterationProfile* stepProfile = nullptr;
                if (profile) {
                    profile->iterations.emplace_back();
                    stepProfile = &profile->iterations.back();
                    stepProfile->iteration = iteration;
                    counters_ = &stepProfile->counters;
                }
                const auto searchStart = tick();
                const bool reverse = (iteration & 1U) != 0;
                searchPass(current, current.active, current.activeSchedule, reverse, false,
                    iteration < 2 ? .35 : .7, stepProfile);
                if (stepProfile) stepProfile->searchMilliseconds = elapsed(searchStart);
                // Continuous optimization may average compatible votes on
                // coarse grids. The source-resolution reconstruction always
                // selects an actual exemplar pixel, never a texture-smearing
                // average of incompatible patches.
                const auto reconstructionStart = tick();
                reconstruct(current, level != 0 && iteration + 1 < iterations);
                if (stepProfile) stepProfile->reconstructionMilliseconds = elapsed(reconstructionStart);
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
        counters_ = nullptr;
        const auto smoothStart = tick();
        smoothModels(finest);
        if (options_.captureProfile) result_.diagnostics.profile.smoothMilliseconds = elapsed(smoothStart);
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
        const auto adaptationStart = tick();
        adapt(finest);
        if (options_.captureProfile) {
            result_.diagnostics.profile.adaptationMilliseconds = elapsed(adaptationStart);
            result_.diagnostics.profile.totalMilliseconds = elapsed(started_);
        }
        check();
        result_.status = SpotHealStatus::Complete;
        result_.diagnostics.message = "Spot Heal reconstructed the marked region from valid surrounding texture.";
        report(1);
        return std::move(result_);
    }

    SpotHealResult failed(SpotHealStatus status, std::string message)
    {
        if (options_.captureProfile) result_.diagnostics.profile.totalMilliseconds = elapsed(started_);
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
    using Clock = std::chrono::steady_clock;
    Clock::time_point started_;
    SpotHealCounters* counters_ {};
    bool cacheOriginalColor_ {};
    bool keepReconstructionScratch_ {};
    std::vector<SearchContext> contexts_;
    std::atomic<std::uint64_t> parallelComparisons_ {0};
    std::atomic<bool> parallelCancelled_ {false};
    bool parallelSearching_ {false};

    Clock::time_point tick() const { return options_.captureProfile ? Clock::now() : Clock::time_point{}; }
    double elapsed(Clock::time_point start) const
    {
        return options_.captureProfile ? std::chrono::duration<double, std::milli>(Clock::now() - start).count() : 0;
    }

    void check() const
    {
        if (options_.cancelled && options_.cancelled()) throw Interrupted {};
    }
    void report(double progress) const
    {
        check();
        if (options_.progress) options_.progress(progress);
    }
    static std::uint64_t random(SearchContext& context)
    {
        // Fixed SplitMix64 sequence, not implementation-defined distributions.
        std::uint64_t z = (context.random += randomIncrement);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
    static int randomCoordinate(SearchContext& context, int low, int high)
    {
        return low + static_cast<int>(random(context) % static_cast<unsigned>(high - low + 1));
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
        level.contextActive.clear();
        level.repairOrder.clear();
        level.deepest = 0;
        level.image = level.original;
        level.texture = level.originalTexture;
        level.filled = level.known;
        level.field.assign(size, -1);
        level.cost.assign(size, infinity);
        level.donorValid.assign(size, 0);
        level.depth.assign(size, std::numeric_limits<std::uint16_t>::max());
        if (cacheOriginalColor_ && level.originalColor.empty()) {
            level.originalColor.resize(size);
            for (std::size_t p = 0; p < size; ++p) {
                if ((p & 4095U) == 0) check();
                if (!level.known[p]) continue;
                const auto& color = level.original[p];
                if (!usable(color)) {
                    level.originalColor[p][0] = std::numeric_limits<double>::quiet_NaN();
                    continue;
                }
                for (std::size_t c = 0; c < 3; ++c)
                    level.originalColor[p][c] = double(color[c]) / color[3];
            }
        }
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
            if (active[std::size_t(p)] && (level.known[std::size_t(p)] || level.unknown[std::size_t(p)])) {
                level.active.push_back(p);
                if (!level.unknown[std::size_t(p)]) level.contextActive.push_back(p);
            }
        if (contexts_.size() > 1) {
            level.activeSchedule = DiagonalWavefront(level.active, level.width);
            level.contextSchedule = DiagonalWavefront(level.contextActive, level.width);
        }
        std::stable_sort(level.repairOrder.begin(), level.repairOrder.end(), [&](int a, int b) {
            return level.depth[std::size_t(a)] < level.depth[std::size_t(b)];
        });
    }

    void prepareTarget(const Level& level, int target, double provisionalWeight, SearchContext& context)
    {
        auto& targetPatch = context.target;
        targetPatch.size = 0;
        targetPatch.maximumWeight = 0;
        const int tx = target % level.width, ty = target / level.width;
        for (int dy = -level.radius; dy <= level.radius; ++dy)
            for (int dx = -level.radius; dx <= level.radius; ++dx) {
                if (!level.contains(tx + dx, ty + dy)) continue;
                const auto p = std::size_t(level.index(tx + dx, ty + dy));
                if (!level.filled[p] || !usable(level.image[p])) continue;
                const auto& color = level.image[p];
                auto& sample = targetPatch.samples[targetPatch.size++];
                sample.offset = dy * level.width + dx;
                for (std::size_t c = 0; c < 3; ++c)
                    sample.color[c] = double(color[c]) / color[3];
                sample.alpha = color[3];
                sample.texture = level.texture[p];
                sample.factor = level.known[p] ? 1.0 : provisionalWeight;
                sample.compareTexture = level.featureValid[p] || !level.known[p];
                targetPatch.maximumWeight += sample.factor * sample.alpha;
            }
    }

    double distance(Level& level, int target, int donor, double /*provisionalWeight*/, SearchContext& context)
    {
        const auto comparison = parallelSearching_ ? parallelComparisons_.fetch_add(1, std::memory_order_relaxed) + 1
                                                  : ++result_.diagnostics.comparisons;
        if (comparison > options_.maxComparisons) throw WorkLimit {};
        if ((comparison & 1023U) == 0) {
            if (!parallelSearching_) check();
            else {
                if (context.rank == 0 && options_.cancelled && options_.cancelled())
                    parallelCancelled_.store(true, std::memory_order_relaxed);
                if (parallelCancelled_.load(std::memory_order_relaxed)) throw Interrupted {};
            }
        }
        if (donor < 0 || donor >= level.count() || !level.donorValid[std::size_t(donor)]) return infinity;
        const int tx = target % level.width, ty = target / level.width;
        const int sx = donor % level.width, sy = donor / level.width;
        double cost = 0, weights = 0;
        const double incumbent = level.cost[std::size_t(target)];
        const auto& targetPatch = context.target;
        auto* counters = context.counters;
        for (std::size_t i = 0; i < targetPatch.size; ++i) {
                const auto& a = targetPatch.samples[i];
                const auto q = std::size_t(donor + a.offset);
                const auto& b = level.original[q];
                if (counters) ++counters->patchSamples;
                const double weight = a.factor * std::min(a.alpha, b[3]);
                double d = 1;
                if (level.originalColor.empty() ? usable(b) : std::isfinite(level.originalColor[q][0])) {
                    double sum = 0;
                    for (std::size_t c = 0; c < 3; ++c) {
                        const double color = level.originalColor.empty() ? double(b[c]) / b[3] : level.originalColor[q][c];
                        const double difference = a.color[c] - color;
                        sum += difference * difference;
                    }
                    // These subtractions intentionally remain float before
                    // promotion, matching the frozen scoring arithmetic.
                    const double da = a.alpha - b[3];
                    d = sum / 3 + .15 * da * da;
                }
                if (a.compareTexture)
                    for (std::size_t c = 0; c < 2; ++c) {
                        const double gradient = a.texture[c] - level.originalTexture[q][c];
                        d += .65 * gradient * gradient;
                    }
                cost += weight * d;
                weights += weight;
                // Every term is nonnegative. In the SAME accumulation order,
                // min(targetAlpha, donorAlpha) <= targetAlpha, so the final
                // denominator cannot exceed maximumWeight, even with rounded
                // partial-alpha sums. The omitted locality penalty is >= 0.
                // Strict > retains all equal-score/lower-donor-ID ties; no
                // epsilon or generic unnormalized SSD cutoff is involved.
                if ((i & 7U) == 7U && std::isfinite(incumbent)
                    && cost / targetPatch.maximumWeight > incumbent) {
                    if (counters) ++counters->earlyRejectedCandidates;
                    return infinity;
                }
            }
        if (weights < .25) return infinity;
        // A very weak, continuous local preference breaks near-equivalent
        // matches without cutting off a better distant structural exemplar.
        const double deltaX = double(tx - sx), deltaY = double(ty - sy);
        const double locality = 1.0e-7 * std::log1p((deltaX * deltaX + deltaY * deltaY)
            / double(level.radius * level.radius + 1));
        return cost / weights + locality;
    }

    void consider(Level& level, int target, int donor, double provisionalWeight, SearchContext& context)
    {
        auto* counters = context.counters;
        if (counters) ++counters->proposals;
        if (donor < 0 || donor >= level.count() || !level.donorValid[std::size_t(donor)]) {
            if (counters) ++counters->invalidProposals;
            return;
        }
        if (counters) ++counters->scoredCandidates;
        const double candidate = distance(level, target, donor, provisionalWeight, context);
        auto& cost = level.cost[std::size_t(target)];
        auto& best = level.field[std::size_t(target)];
        if (candidate < cost || (candidate == cost && (best < 0 || donor < best))) {
            if (counters) {
                ++counters->acceptedCandidates;
                if (std::isfinite(cost)) counters->acceptedImprovement += cost - candidate;
            }
            cost = candidate;
            best = donor;
        }
    }

    void translated(Level& level, int p, int q, double provisionalWeight, SearchContext& context)
    {
        if (q < 0 || q >= level.count() || level.field[std::size_t(q)] < 0) return;
        const int donor = level.field[std::size_t(q)];
        const int x = donor % level.width + p % level.width - q % level.width;
        const int y = donor / level.width + p / level.width - q / level.width;
        if (level.contains(x, y)) consider(level, p, level.index(x, y), provisionalWeight, context);
    }

    void search(Level& level, int p, bool reverse, bool initial, double provisionalWeight, SearchContext& context)
    {
        prepareTarget(level, p, provisionalWeight, context);
        level.cost[std::size_t(p)] = infinity;
        consider(level, p, level.field[std::size_t(p)], provisionalWeight, context);
        const int x = p % level.width, y = p / level.width;
        const int step = reverse ? 1 : -1;
        if (level.contains(x + step, y)) translated(level, p, p + step, provisionalWeight, context);
        if (level.contains(x, y + step)) translated(level, p, p + step * level.width, provisionalWeight, context);
        if (initial) {
            // Boundary initialization needs diverse starts before a coherent
            // field exists. Deterministic spatial strata cover the reference;
            // later PatchMatch propagation carries successful offsets inward.
            const std::size_t stride = std::max(std::size_t(1), level.donors.size() / 128);
            const std::size_t start = static_cast<std::size_t>(random(context) % stride);
            for (std::size_t i = start; i < level.donors.size(); i += stride)
                consider(level, p, level.donors[i], provisionalWeight, context);
            for (int dy = -12; dy <= 12; dy += 3)
                for (int dx = -12; dx <= 12; dx += 3)
                    if (level.contains(x + dx, y + dy))
                        consider(level, p, level.index(x + dx, y + dy), provisionalWeight, context);
        }
        // Global-to-local random search plus independent restarts. The donor
        // domain is the complete supplied coherent reference, not brush size.
        for (int restart = 0; restart < (initial ? 12 : 3); ++restart)
            consider(level, p, level.donors[static_cast<std::size_t>(random(context) % level.donors.size())],
                provisionalWeight, context);
        for (int radius = std::max(level.width, level.height); radius >= 1; radius /= 2) {
            const int best = level.field[std::size_t(p)];
            const int bx = best >= 0 ? best % level.width : x;
            const int by = best >= 0 ? best / level.width : y;
            for (int trial = 0; trial < 2; ++trial) {
                const int sx = randomCoordinate(context, std::max(0, bx - radius), std::min(level.width - 1, bx + radius));
                const int sy = randomCoordinate(context, std::max(0, by - radius), std::min(level.height - 1, by + radius));
                consider(level, p, level.index(sx, sy), provisionalWeight, context);
            }
        }
    }

    void searchPass(Level& level, const std::vector<int>& pixels, const DiagonalWavefront& schedule,
        bool reverse, bool initial, double provisionalWeight, SpotHealIterationProfile* profile = nullptr)
    {
        if (pixels.empty()) return;
        const unsigned workers = pixels.size() < 64 ? 1U : static_cast<unsigned>(contexts_.size());
        std::vector<int> previous;
        if (profile) {
            previous.reserve(pixels.size());
            for (int p : pixels) previous.push_back(level.field[std::size_t(p)]);
        }
        if (workers == 1) {
            auto& context = contexts_.front();
            context.random = random_;
            context.counters = counters_;
            context.rank = 0;
            for (std::size_t ordinal = 0; ordinal < pixels.size(); ++ordinal) {
                if ((ordinal & 63U) == 0) check();
                const int p = pixels[reverse ? pixels.size() - 1 - ordinal : ordinal];
                search(level, p, reverse, initial, provisionalWeight, context);
            }
            random_ = context.random;
        } else {
            result_.diagnostics.profile.workersUsed = std::max(result_.diagnostics.profile.workersUsed, workers);
            std::uint64_t draws = initial ? 13 : 3;
            for (int radius = std::max(level.width, level.height); radius >= 1; radius /= 2) draws += 4;
            // Every search consumes this fixed count, regardless of candidate
            // validity or early rejection. Reassign the serial stream slice by
            // original active ordinal, not by thread or diagonal visit order.
            const auto firstRandom = random_;
            for (unsigned rank = 0; rank < workers; ++rank) {
                auto& context = contexts_[rank];
                context.rank = rank;
                context.workCounters = {};
                context.counters = counters_ ? &context.workCounters : nullptr;
            }
            parallelComparisons_.store(result_.diagnostics.comparisons, std::memory_order_relaxed);
            parallelCancelled_.store(false, std::memory_order_relaxed);
            parallelSearching_ = true;
            bool completed = false;
            std::exception_ptr error;
            try {
                completed = schedule.run(workers, reverse, [&](int p, std::size_t ordinal, unsigned rank) {
                    auto& context = contexts_[rank];
                    context.random = firstRandom + std::uint64_t(ordinal) * draws * randomIncrement;
                    search(level, p, reverse, initial, provisionalWeight, context);
                }, [&] {
                    if (options_.cancelled && options_.cancelled()) parallelCancelled_.store(true, std::memory_order_relaxed);
                    return parallelCancelled_.load(std::memory_order_relaxed);
                });
            } catch (...) { error = std::current_exception(); }
            parallelSearching_ = false;
            result_.diagnostics.comparisons = parallelComparisons_.load(std::memory_order_relaxed);
            if (result_.diagnostics.comparisons > options_.maxComparisons)
                result_.diagnostics.comparisons = options_.maxComparisons + 1;
            if (counters_) {
                for (unsigned rank = 0; rank < workers; ++rank) {
                    const auto& source = contexts_[rank].workCounters;
                    counters_->proposals += source.proposals;
                    counters_->invalidProposals += source.invalidProposals;
                    counters_->duplicateProposals += source.duplicateProposals;
                    counters_->scoredCandidates += source.scoredCandidates;
                    counters_->earlyRejectedCandidates += source.earlyRejectedCandidates;
                    counters_->acceptedCandidates += source.acceptedCandidates;
                    counters_->patchSamples += source.patchSamples;
                    counters_->acceptedImprovement += source.acceptedImprovement;
                }
            }
            if (error) std::rethrow_exception(error);
            if (!completed) throw Interrupted {};
            random_ = firstRandom + std::uint64_t(pixels.size()) * draws * randomIncrement;
        }
        if (profile) {
            // Preserve the original summation order for reported objective
            // totals too. No unordered reduction can perturb convergence data.
            for (std::size_t ordinal = 0; ordinal < pixels.size(); ++ordinal) {
                const auto index = reverse ? pixels.size() - 1 - ordinal : ordinal;
                const auto p = std::size_t(pixels[index]);
                profile->changedAssignments += level.field[p] != previous[index];
                if (std::isfinite(level.cost[p])) profile->objectiveSum += level.cost[p];
            }
        }
    }

    void initialize(Level& level, const Level* coarse, SpotHealLevelProfile* profile)
    {
        const auto unknownStart = tick();
        auto& context = contexts_.front();
        context.random = random_;
        context.rank = 0;
        context.counters = counters_;
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
            prepareTarget(level, p, .25, context);
            // All four neighbors help an inward front regardless of scan order.
            level.cost[std::size_t(p)] = infinity;
            if (x + 1 < level.width) translated(level, p, p + 1, .25, context);
            if (y + 1 < level.height) translated(level, p, p + level.width, .25, context);
            search(level, p, false, true, .25, context);
            const int donor = level.field[std::size_t(p)];
            if (donor < 0) throw NoContext {};
            level.image[std::size_t(p)] = level.original[std::size_t(donor)];
            level.texture[std::size_t(p)] = level.originalTexture[std::size_t(donor)];
            level.filled[std::size_t(p)] = 1;
        }
        random_ = context.random;
        if (profile) profile->initializationUnknownMilliseconds = elapsed(unknownStart);
        const auto contextStart = tick();
        searchPass(level, level.contextActive, level.contextSchedule, false, true, .25);
        if (profile) profile->initializationContextMilliseconds = elapsed(contextStart);
    }

    void reconstruct(Level& level, bool average)
    {
        if (level.nextImage.empty()) {
            level.nextImage = level.image;
            level.nextTexture = level.texture;
            level.nextField = level.field;
        } else {
            // Known image/texture values never change; only correspondence
            // centers in the active band need refreshing between buffer swaps.
            for (int p : level.contextActive) level.nextField[std::size_t(p)] = level.field[std::size_t(p)];
        }
        auto& image = level.nextImage;
        auto& texture = level.nextTexture;
        auto& field = level.nextField;
        auto reconstructPixel = [&](const int p) {
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
        };
        const unsigned workers = level.repairOrder.size() < 256 ? 1U : static_cast<unsigned>(contexts_.size());
        if (workers == 1) {
            for (const int p : level.repairOrder) {
                if ((static_cast<unsigned>(p) & 63U) == 0) check();
                reconstructPixel(p);
            }
        } else {
            std::atomic<bool> stop {false};
            const bool completed = boundedParallel(workers, [&](unsigned rank, unsigned count) {
                try {
                    const auto begin = level.repairOrder.size() * rank / count;
                    const auto end = level.repairOrder.size() * (rank + 1) / count;
                    for (auto ordinal = begin; ordinal < end; ++ordinal) {
                        if ((ordinal & 63U) == 0) {
                            if (rank == 0 && options_.cancelled && options_.cancelled()) stop.store(true, std::memory_order_relaxed);
                            if (stop.load(std::memory_order_relaxed)) throw Interrupted {};
                        }
                        reconstructPixel(level.repairOrder[ordinal]);
                    }
                } catch (...) { stop.store(true, std::memory_order_relaxed); throw; }
            }, options_.cancelled);
            if (!completed) throw Interrupted {};
        }
        level.image.swap(level.nextImage);
        level.texture.swap(level.nextTexture);
        level.field.swap(level.nextField);
        if (!keepReconstructionScratch_) {
            std::vector<PremultipliedColor>().swap(level.nextImage);
            std::vector<Texture>().swap(level.nextTexture);
            std::vector<int>().swap(level.nextField);
        }
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
