#include "imageeditor/core/SpotHealRepair.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace imageeditor::core {
// The unmodified V1 source is compiled with only its public function renamed.
SpotHealResult repairSpotHealFrozenReference(const SpotHealPatch&, const SpotHealOptions&);
}

namespace {
namespace c = imageeditor::core;
int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #value "\n"; ++failures; } } while (false)

struct Fixture {
    int width = 63, height = 55;
    std::vector<c::PremultipliedColor> pixels;
    std::vector<std::uint8_t> unknown, valid;
    c::SpotHealPatch patch() const { return {width, height, pixels, unknown, valid}; }
};

Fixture fixture(int kind)
{
    Fixture f;
    const auto count = std::size_t(f.width) * std::size_t(f.height);
    f.pixels.resize(count); f.unknown.resize(count); f.valid.assign(count, 255);
    for (int y = 0; y < f.height; ++y) for (int x = 0; x < f.width; ++x) {
        const auto p = std::size_t(y * f.width + x);
        const float alpha = kind == 1 ? float(1 + ((x * 13 + y * 29) % 255)) / 255.0F : 1.0F;
        const double grain = kind == 3 ? 0 : .025 * std::sin(x * 1.9 + y * .7)
            + .012 * std::cos(x * .3 - y * 2.7);
        f.pixels[p] = {float(.3 + x * .002 + grain) * alpha,
            float(.4 + y * .001 + grain) * alpha, float(.2 + x * .001 - grain) * alpha, alpha};
        const int dx = x - 31, dy = y - 27;
        const bool mark = kind == 1 ? dx * dx + dy * dy < 81
            : kind == 2 ? (x < 4 && y >= 13 && y <= 19) || (x >= 32 && x <= 36 && y >= 28 && y <= 33)
            : x >= 26 && x <= 35 && y >= 23 && y <= 29;
        if (mark) { f.unknown[p] = 255; f.pixels[p] = {1, 0, 1, .2F}; }
        if (kind == 2 && x > 57) {
            f.valid[p] = 0;
            f.pixels[p] = {std::numeric_limits<float>::quiet_NaN(), 12, -6, 0};
        }
    }
    return f;
}

bool samePixels(const std::vector<c::PremultipliedColor>& a,
    const std::vector<c::PremultipliedColor>& b)
{
    return a.size() == b.size() && (a.empty()
        || std::memcmp(a.data(), b.data(), a.size() * sizeof(a.front())) == 0);
}

void frozenOutputAndWorkerCounts()
{
    for (int kind = 0; kind < 4; ++kind) {
        auto input = fixture(kind);
        c::SpotHealOptions options;
        options.seed = 9021043U + std::uint64_t(kind);
        options.captureDiagnostics = true;
        const auto frozen = c::repairSpotHealFrozenReference(input.patch(), options);
        CHECK(frozen.status == c::SpotHealStatus::Complete);
        for (const unsigned workers : {1U, 2U, 4U, 8U}) {
            options.workerCount = workers;
            options.captureProfile = true;
            const auto actual = c::repairSpotHeal(input.patch(), options);
            CHECK(actual.status == frozen.status);
            CHECK(samePixels(actual.pixels, frozen.pixels));
            CHECK(samePixels(actual.diagnostics.beforeAdaptation, frozen.diagnostics.beforeAdaptation));
            CHECK(actual.diagnostics.smoothModelPixels == frozen.diagnostics.smoothModelPixels);
            // A wholly admitted smooth model can bypass unused exemplar work;
            // all non-modelled repairs must retain the same donor assignments.
            if (actual.diagnostics.smoothModelPixels != actual.diagnostics.unknownPixels)
                CHECK(actual.diagnostics.donors == frozen.diagnostics.donors);
            CHECK(actual.diagnostics.invalidDonors == frozen.diagnostics.invalidDonors);
        }

        // The optional descriptor cache must not shrink the donor domain when
        // there is only room for the original conservative working budget and
        // the explicitly budgeted fixed per-target matching scratch.
        options.workerCount = 1;
        options.captureProfile = false;
        options.maxWorkingBytes = input.pixels.size() * 288U + 32768U;
        const auto uncached = c::repairSpotHeal(input.patch(), options);
        CHECK(uncached.status == frozen.status);
        CHECK(samePixels(uncached.pixels, frozen.pixels));

        // Corrupted alpha/NaNs are unknown, not appearance evidence, including
        // with parallel workers and fractional known-context alpha.
        options.maxWorkingBytes = 768U * 1024U * 1024U;
        options.workerCount = 4;
        for (std::size_t p = 0; p < input.pixels.size(); ++p) if (input.unknown[p])
            input.pixels[p] = {std::numeric_limits<float>::quiet_NaN(), -7, 9, 0};
        const auto corruptionChanged = c::repairSpotHeal(input.patch(), options);
        CHECK(corruptionChanged.status == frozen.status);
        CHECK(samePixels(corruptionChanged.pixels, frozen.pixels));

        options.maxComparisons = 20;
        const auto limited = c::repairSpotHeal(input.patch(), options);
        const auto frozenLimited = c::repairSpotHealFrozenReference(input.patch(), options);
        if (limited.diagnostics.smoothModelPixels != limited.diagnostics.unknownPixels)
            CHECK(limited.status == frozenLimited.status);
        CHECK(limited.status == c::SpotHealStatus::LimitExceeded || kind == 3);
    }
}
}

int main()
{
    frozenOutputAndWorkerCounts();
    if (failures) return 1;
    std::cout << "Spot Heal frozen V1 byte-exact equivalence passed\n";
}
