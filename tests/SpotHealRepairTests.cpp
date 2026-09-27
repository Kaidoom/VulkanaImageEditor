#include "imageeditor/core/SpotHealRepair.hpp"
#include "imageeditor/core/BoundedParallel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool passed, std::string_view description)
{
    if (!passed) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}

unsigned processThreadCount()
{
    // Optional Linux observation: do not initialize the executor merely to
    // query it. Other platforms still exercise the budget/result assertions.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("Threads:")) continue;
        unsigned count = 0;
        std::istringstream(line.substr(8)) >> count;
        return count;
    }
    return 0;
}

void tinyMemoryBudgets()
{
    const std::array<PremultipliedColor, 1> pixels {PremultipliedColor {.2F, .3F, .4F, 1.F}};
    std::array<std::uint8_t, 1> unknown {};
    const SpotHealPatch patch {1, 1, pixels, unknown, {}};
    SpotHealOptions options;
    options.workerCount = 8;
    options.maxWorkingBytes = 256;
    const auto beforeThreads = processThreadCount();
    const auto unmarked = repairSpotHeal(patch, options);
    check(unmarked.status == SpotHealStatus::NoUnknownPixels && unmarked.pixels.size() == 1
            && unmarked.pixels.front() == pixels.front(),
        "one-pixel unmarked input succeeds within a 256-byte budget");
    check(unmarked.diagnostics.estimatedPeakWorkingBytes <= options.maxWorkingBytes,
        "tiny successful no-op reports an estimate within its budget");
    const auto afterNoopThreads = processThreadCount();
    if (beforeThreads && afterNoopThreads)
        check(afterNoopThreads == beforeThreads, "unmarked input does not start the reusable worker pool");
    unknown.front() = 255;
    const auto marked = repairSpotHeal(patch, options);
    check(marked.status == SpotHealStatus::LimitExceeded && marked.pixels.empty(),
        "tiny marked input rejects a budget unable to hold mandatory matching scratch");
    const auto afterFailureThreads = processThreadCount();
    if (beforeThreads && afterFailureThreads)
        check(afterFailureThreads == beforeThreads, "mandatory-scratch preflight fails before starting workers");
}
struct Fixture {
    int width {64}, height {56};
    std::vector<PremultipliedColor> pixels;
    std::vector<std::uint8_t> unknown, valid;
    Fixture() : pixels(std::size_t(width * height)), unknown(pixels.size()), valid(pixels.size(), 255)
    {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float texture = static_cast<float>(.025 * std::sin(x * .9) * std::cos(y * 1.2));
                pixels[at(x, y)] = {.25F + texture, .4F + texture, .55F + texture, 1};
            }
    }
    std::size_t at(int x, int y) const { return std::size_t(y * width + x); }
    SpotHealPatch patch() const { return {width, height, pixels, unknown, valid}; }
    void mark(int x, int y, int radius)
    {
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx)
                if (dx * dx + dy * dy <= radius * radius && x + dx >= 0 && y + dy >= 0
                    && x + dx < width && y + dy < height) {
                    const auto p = at(x + dx, y + dy);
                    unknown[p] = 255;
                    pixels[p] = {1, 0, .1F, 1};
                }
    }
};

void invariants()
{
    Fixture fixture;
    fixture.mark(29, 25, 5);
    fixture.mark(43, 31, 3);
    SpotHealOptions options;
    options.captureDiagnostics = true;
    const auto first = repairSpotHeal(fixture.patch(), options);
    check(bool(first), "two-island repair completes");
    if (!first) { std::cerr << first.diagnostics.message << '\n'; return; }
    check(first.diagnostics.smoothModelPixels == 0, "fine texture cannot enter the smooth-only model");
    const auto repeated = repairSpotHeal(fixture.patch(), options);
    check(first.pixels == repeated.pixels, "same seed/reference/mask is bit deterministic");
    auto corrupt = fixture;
    for (std::size_t p = 0; p < corrupt.pixels.size(); ++p)
        if (corrupt.unknown[p]) corrupt.pixels[p] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 0};
    const auto changed = repairSpotHeal(corrupt.patch(), options);
    check(bool(changed), "unknown NaNs/alpha are not inspected as evidence");
    for (std::size_t p = 0; p < fixture.pixels.size(); ++p) {
        if (!fixture.unknown[p]) {
            check(first.pixels[p] == fixture.pixels[p], "known pixels are exactly unchanged");
            continue;
        }
        if (changed) check(first.pixels[p] == changed.pixels[p], "unknown color/alpha cannot change inferred candidate");
        const auto donor = first.diagnostics.donors[p];
        check(donor.x >= 0 && donor.y >= 0, "each unknown has an exemplar assignment");
        if (donor.x < 0 || donor.y < 0) continue;
        check(first.diagnostics.invalidDonors[fixture.at(donor.x, donor.y)] == 0,
            "diagnostic donor is fully valid");
        const int r = static_cast<int>(first.diagnostics.patchRadius) + 1;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                const int x = donor.x + dx, y = donor.y + dy;
                check(x >= 0 && y >= 0 && x < fixture.width && y < fixture.height,
                    "donor patch and descriptor support stays inside reference");
                if (x >= 0 && y >= 0 && x < fixture.width && y < fixture.height)
                    check(!fixture.unknown[fixture.at(x, y)] && fixture.valid[fixture.at(x, y)],
                        "whole donor support excludes all marked islands");
            }
        const auto& pixel = first.pixels[p];
        check(std::all_of(pixel.begin(), pixel.end(), [](float v) { return std::isfinite(v); }),
            "finite final candidate");
    }
}

void boundariesAndLimits()
{
    Fixture fixture;
    const auto empty = repairSpotHeal(fixture.patch());
    check(empty.status == SpotHealStatus::NoUnknownPixels && empty.pixels == fixture.pixels,
        "empty operation is explicit exact identity");
    fixture.mark(0, 20, 4);
    const auto edge = repairSpotHeal(fixture.patch());
    check(bool(edge), "repair touching reference edge uses genuine neighboring context");
    fixture.valid.assign(fixture.valid.size(), 0);
    const auto invalid = repairSpotHeal(fixture.patch());
    check(invalid.status == SpotHealStatus::InsufficientContext && invalid.pixels.empty(),
        "absent context is explicit failure, never unrelated success");
    fixture.valid.assign(fixture.valid.size(), 255);
    fixture.unknown.assign(fixture.unknown.size(), 255);
    check(repairSpotHeal(fixture.patch()).status == SpotHealStatus::InsufficientContext,
        "entire unknown image cannot supply donors");
    SpotHealOptions options;
    options.maxWorkingBytes = 100;
    check(repairSpotHeal(fixture.patch(), options).status == SpotHealStatus::LimitExceeded,
        "memory preflight fails before allocation");
    options = {};
    options.cancelled = [] { return true; };
    const auto cancelled = repairSpotHeal(fixture.patch(), options);
    check(cancelled.status == SpotHealStatus::Cancelled && cancelled.pixels.empty(),
        "initial cancellation returns no partial image");
    fixture = Fixture();
    fixture.mark(30, 26, 8);
    options = {};
    options.maxComparisons = 5;
    check(repairSpotHeal(fixture.patch(), options).status == SpotHealStatus::LimitExceeded,
        "work budget stops without partial output");
    unsigned checks = 0;
    options = {};
    options.cancelled = [&] { return ++checks > 80; };
    const auto during = repairSpotHeal(fixture.patch(), options);
    check(during.status == SpotHealStatus::Cancelled && during.pixels.empty(),
        "cancellation during reconstruction returns no partial image");
}

void constantAndAlpha()
{
    Fixture fixture;
    for (auto& pixel : fixture.pixels) pixel = {.12F, .21F, .3F, .6F};
    fixture.mark(32, 28, 8);
    SpotHealOptions options;
    options.captureDiagnostics = true;
    double last = 0;
    options.progress = [&](double p) { check(p >= last && p <= 1, "progress is monotonic and bounded"); last = p; };
    const auto result = repairSpotHeal(fixture.patch(), options);
    check(bool(result), "partially transparent constant-context repair completes");
    if (!result) return;
    for (std::size_t p = 0; p < fixture.pixels.size(); ++p)
        if (fixture.unknown[p]) {
            check(std::abs(result.pixels[p][0] - .12F) < 1.0e-5F,
                "premultiplied partial alpha reconstructs without black fringe");
            check(std::abs(result.pixels[p][3] - .6F) < 1.0e-6F,
                "candidate alpha derives from valid reference, not damaged alpha");
        }
    check(last == 1, "successful progress finishes at one");
    check(result.diagnostics.pyramidLevels > 1, "larger defect exercises conservative multiscale path");
}

void smoothGradient()
{
    Fixture fixture;
    for (int y = 0; y < fixture.height; ++y)
        for (int x = 0; x < fixture.width; ++x)
            fixture.pixels[fixture.at(x, y)] = {
                .2F + float(x) * .001F + float(y) * .002F,
                .3F + float(x) * .002F, .4F - float(y) * .001F, 1};
    const auto clean = fixture.pixels;
    fixture.mark(27, 24, 7);
    const auto result = repairSpotHeal(fixture.patch());
    check(bool(result), "smooth illumination repair completes");
    if (!result) return;
    check(result.diagnostics.smoothModelPixels == result.diagnostics.unknownPixels,
        "affine context is validated rather than copied from unrelated offsets");
    for (std::size_t p = 0; p < fixture.pixels.size(); ++p)
        if (fixture.unknown[p])
            for (std::size_t c = 0; c < 3; ++c)
                check(std::abs(result.pixels[p][c] - clean[p][c]) < 1.0e-6F,
                    "smooth-only fit continues known illumination without microvariation");
    fixture.pixels = clean;
    for (auto& pixel : fixture.pixels) pixel = decodeColor(encodeColor(pixel));
    fixture.mark(27, 24, 7);
    const auto quantized = repairSpotHeal(fixture.patch());
    check(bool(quantized) && quantized.diagnostics.smoothModelPixels == quantized.diagnostics.unknownPixels,
        "smooth model distinguishes sRGB8 quantization noise from texture");
    fixture = Fixture();
    for (int y = 0; y < fixture.height; ++y)
        for (int x = 0; x < fixture.width; ++x) {
            const auto code = static_cast<std::uint8_t>(120 + ((x * 19 + y * 31 + x * y * 7) % 3) - 1);
            fixture.pixels[fixture.at(x, y)] = decodeColor({code, code, code, 255});
        }
    fixture.mark(27, 24, 7);
    const auto fineNoise = repairSpotHeal(fixture.patch());
    check(bool(fineNoise) && fineNoise.diagnostics.smoothModelPixels == 0,
        "real low-amplitude quantized grain does not receive the ramp noise allowance");
}

void parallelExactness()
{
    Fixture fixture;
    for (std::size_t p = 0; p < fixture.pixels.size(); ++p) {
        const float alpha = p % 5 ? 1.F : .41F;
        for (std::size_t c = 0; c < 3; ++c) fixture.pixels[p][c] *= alpha;
        fixture.pixels[p][3] = alpha;
    }
    fixture.mark(29, 25, 8);
    fixture.mark(46, 35, 4);
    SpotHealOptions options;
    options.workerCount = 1;
    options.seed = std::numeric_limits<std::uint64_t>::max() - 11;
    options.captureProfile = options.captureDiagnostics = true;
    const auto serial = repairSpotHeal(fixture.patch(), options);
    check(bool(serial), "serial partial-alpha multilevel parallel reference completes");
    if (!serial) return;
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> wrongThread {false};
    for (const unsigned workers : {2U, 4U, 8U}) {
        options.workerCount = workers;
        options.cancelled = [&] {
            if (std::this_thread::get_id() != caller) wrongThread.store(true);
            return false;
        };
        const auto result = repairSpotHeal(fixture.patch(), options);
        check(bool(result), "bounded parallel repair completes at each worker count");
        if (!result) continue;
        check(result.pixels == serial.pixels && result.diagnostics.beforeAdaptation == serial.diagnostics.beforeAdaptation,
            "worker count cannot alter reconstruction/adaptation pixel bits");
        check(result.diagnostics.donors == serial.diagnostics.donors
                && result.diagnostics.comparisons == serial.diagnostics.comparisons,
            "parallel wavefront preserves donor decisions, RNG wrap and logical comparison counts");
        check(result.diagnostics.profile.workersUsed == boundedParallelWorkerCount(workers),
            "profile records actual bounded participant count");
        check(result.diagnostics.profile.levels.size() == serial.diagnostics.profile.levels.size(),
            "parallelism cannot change pyramid or iteration schedule");
        for (std::size_t level = 0; level < result.diagnostics.profile.levels.size(); ++level) {
            const auto& left = serial.diagnostics.profile.levels[level];
            const auto& right = result.diagnostics.profile.levels[level];
            check(right.initialization.proposals == left.initialization.proposals
                    && right.initialization.scoredCandidates == left.initialization.scoredCandidates
                    && right.initialization.patchSamples == left.initialization.patchSamples,
                "parallel known-band initialization retains exact scoring work");
            for (std::size_t i = 0; i < right.iterations.size(); ++i)
                check(right.iterations[i].objectiveSum == left.iterations[i].objectiveSum
                        && right.iterations[i].changedAssignments == left.iterations[i].changedAssignments
                        && right.iterations[i].counters.patchSamples == left.iterations[i].counters.patchSamples,
                    "per-pass objective order and early-rejection work remain identical");
        }
    }
    check(!wrongThread, "legacy cancellation callback remains on the invoking thread");
    options.cancelled = {};
    options.maxComparisons = serial.diagnostics.comparisons - 1;
    const auto limited = repairSpotHeal(fixture.patch(), options);
    check(limited.status == SpotHealStatus::LimitExceeded && limited.pixels.empty()
            && limited.diagnostics.comparisons == options.maxComparisons + 1,
        "parallel shared work budget fails atomically at the serial logical limit");
    if (boundedParallelWorkerCount(8) > 1) {
        options.maxComparisons = 256ULL * 1024ULL * 1024ULL;
        options.cancelled = [] { return boundedParallelWorkerCount(8) == 1; };
        const auto cancelled = repairSpotHeal(fixture.patch(), options);
        check(cancelled.status == SpotHealStatus::Cancelled && cancelled.pixels.empty(),
            "cancellation inside a parallel wavefront drains workers without proposed pixels");
        options.cancelled = {};
        const auto afterCancel = repairSpotHeal(fixture.patch(), options);
        check(bool(afterCancel) && afterCancel.pixels == serial.pixels,
            "cancelled batch cannot poison pool reuse or later deterministic repair");
    }
}

void boundedScratchFallback()
{
    Fixture fixture;
    fixture.mark(29, 25, 8);
    SpotHealOptions options;
    options.workerCount = 8;
    options.captureDiagnostics = options.captureProfile = true;
    const auto normal = repairSpotHeal(fixture.patch(), options);
    check(bool(normal), "normally budgeted scratch reference completes");
    check(normal.diagnostics.estimatedPeakWorkingBytes <= options.maxWorkingBytes,
        "successful cached parallel repair reports an estimate within its budget");
    // Leaves room for the fixed target context, but not the optional original
    // RGB cache, retained reconstruction copies or parallel schedules.
    options.maxWorkingBytes = fixture.pixels.size() * 288U + 32768U;
    const auto fallback = repairSpotHeal(fixture.patch(), options);
    check(bool(fallback), "tight budget retains an exact serial uncached path");
    if (!normal || !fallback) return;
    check(fallback.diagnostics.estimatedPeakWorkingBytes <= options.maxWorkingBytes
            && fallback.diagnostics.profile.workersUsed == 1,
        "mandatory scratch is accounted and optional parallel allocation falls back within budget");
    check(fallback.pixels == normal.pixels
            && fallback.diagnostics.beforeAdaptation == normal.diagnostics.beforeAdaptation
            && fallback.diagnostics.donors == normal.diagnostics.donors,
        "low-memory fallback preserves exact reconstruction and donor decisions");
}
} // namespace

int main()
{
    tinyMemoryBudgets(); // Must precede any test that intentionally starts workers.
    invariants();
    boundariesAndLimits();
    constantAndAlpha();
    smoothGradient();
    parallelExactness();
    boundedScratchFallback();
    if (failures) { std::cerr << failures << " Spot Heal checks failed\n"; return 1; }
    std::cout << "Spot Heal reconstruction checks passed\n";
}
