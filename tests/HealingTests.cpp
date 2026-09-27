#include "imageeditor/core/Healing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;

void check(bool condition, std::string_view message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void near(double actual, double expected, double tolerance, std::string_view message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        if (failures < 25) std::cerr << "FAIL: " << message << " actual=" << actual << " expected=" << expected << '\n';
        ++failures;
    }
}

struct Fixture {
    int width, height;
    std::vector<PremultipliedColor> source, destination, truth;
    std::vector<float> coverage;
    Fixture(int w, int h) : width(w), height(h), source(std::size_t(w) * std::size_t(h)),
        destination(source.size()), truth(source.size()), coverage(source.size()) {}
    HealingPatch patch() const { return {width, height, source, destination, coverage}; }
    std::size_t index(int x, int y) const
    { return std::size_t(y) * std::size_t(width) + std::size_t(x); }
};

PremultipliedColor color(double red, double green, double blue, float alpha = 1)
{
    return {float(red * alpha), float(green * alpha), float(blue * alpha), alpha};
}

Fixture lightingFixture(int width = 72, int height = 64, bool soft = false)
{
    Fixture fixture(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto i = fixture.index(x, y);
            const double texture = .035 * std::sin(x * 1.7) * std::cos(y * 1.2) + .017 * std::sin((x + y) * 2.1);
            const double nx = double(x) * 72 / width, ny = double(y) * 64 / height;
            fixture.source[i] = color(.25 + texture, .32 + texture * .8, .38 + texture * .6);
            fixture.truth[i] = color(.25 + texture + .12 + .002 * nx + .0007 * ny,
                .32 + texture * .8 + .07 - .0004 * nx + .001 * ny,
                .38 + texture * .6 - .07 + .0009 * nx - .0006 * ny);
            fixture.destination[i] = fixture.truth[i];
            const int edge = std::min({x - 17, width - 18 - x, y - 15, height - 16 - y});
            if (edge >= 0) {
                fixture.coverage[i] = soft ? std::min(1.0F, float(edge + 1) / 6.0F) : 1.0F;
                // A destination scratch/spot must not become guidance gradients.
                if (x > width / 2 - 4 && x < width / 2 + 4)
                    fixture.destination[i] = color(.85, .06, .1);
            }
        }
    }
    return fixture;
}

HealingOptions fullAdaptation()
{
    HealingOptions options;
    options.adaptation = 1.0F;
    return options;
}

double maskedMse(const Fixture& fixture, const std::vector<PremultipliedColor>& pixels)
{
    double squared = 0, count = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if (fixture.coverage[i] == 0) continue;
        for (std::size_t c = 0; c < 3; ++c) {
            const double delta = pixels[i][c] - fixture.truth[i][c];
            squared += delta * delta;
            ++count;
        }
    }
    return squared / count;
}

std::vector<PremultipliedColor> composed(const Fixture& fixture,
    const std::vector<PremultipliedColor>& repair, bool feather)
{
    std::vector<PremultipliedColor> result(fixture.destination);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const float coverage = feather ? fixture.coverage[i] : (fixture.coverage[i] > 0 ? 1.0F : 0.0F);
        for (std::size_t c = 0; c < 4; ++c)
            result[i][c] = repair[i][c] * coverage + fixture.destination[i][c] * (1.0F - coverage);
    }
    return result;
}

void lightingAndTexture()
{
    const auto fixture = lightingFixture();
    const auto result = healPatch(fixture.patch(), fullAdaptation());
    check(result.status == HealingStatus::Converged, "affine lighting reconstruction converges");
    check(result.diagnostics.healedPixels == result.diagnostics.affectedPixels, "all covered pixels healed");
    check(result.diagnostics.absoluteResidualRms <= 1.0e-8F,
        "near-zero affine RHS converges to absolute residual floor even if relative ratio is large");
    const double healError = maskedMse(fixture, result.pixels);
    const double stampError = maskedMse(fixture, fixture.source);
    check(healError < 1.0e-10, "spatial RGB illumination fitted to independent analytic truth");
    check(healError < stampError * 1.0e-5, "Heal improves brightness/color-cast mismatch over Stamp");
    for (int y = 23; y < 42; ++y) {
        for (int x = 25; x < 46; ++x) {
            const auto i = fixture.index(x, y);
            for (std::size_t c = 0; c < 3; ++c) {
                const double repairedDetail = result.pixels[i - 1][c] + result.pixels[i + 1][c]
                    + result.pixels[i - std::size_t(fixture.width)][c] + result.pixels[i + std::size_t(fixture.width)][c]
                    - 4.0 * result.pixels[i][c];
                const double sourceDetail = fixture.source[i - 1][c] + fixture.source[i + 1][c]
                    + fixture.source[i - std::size_t(fixture.width)][c] + fixture.source[i + std::size_t(fixture.width)][c]
                    - 4.0 * fixture.source[i][c];
                near(repairedDetail, sourceDetail, 5.0e-6, "source fine texture Laplacian preserved");
            }
        }
    }
    auto altered = fixture;
    for (std::size_t i = 0; i < altered.source.size(); ++i)
        if (altered.coverage[i] > 0) altered.destination[i] = color(.02, .97, .6);
    const auto second = healPatch(altered.patch(), fullAdaptation());
    check(second.pixels == result.pixels, "destination defect gradients never influence repair");

    auto soft = lightingFixture(72, 64, true);
    const auto softResult = healPatch(soft.patch(), fullAdaptation());
    check(softResult.pixels == result.pixels, "soft coverage magnitude is not applied by solver");
    const auto featherOnly = composed(soft, soft.source, true);
    const auto healed = composed(soft, softResult.pixels, true);
    check(maskedMse(soft, healed) < maskedMse(soft, featherOnly) * .2,
        "actual soft Heal beats feather-only clone on lighting and scratch fixture");
    std::cout << "Lighting comparison linear-RGB MSE: Stamp=" << stampError
        << " feather=" << maskedMse(soft, featherOnly) << " Heal=" << healError
        << " soft-Heal=" << maskedMse(soft, healed) << '\n';
}

void nonlinearReconstruction()
{
    auto fixture = lightingFixture(96, 88);
    for (int y = 0; y < fixture.height; ++y) {
        for (int x = 0; x < fixture.width; ++x) {
            const auto i = fixture.index(x, y);
            // x*y is harmonic but not an affine plane: a mean/plane-only
            // replacement cannot reproduce this independent analytical field.
            const double correction = .12 + .00009 * (x - 48) * (y - 44);
            for (std::size_t c = 0; c < 3; ++c)
                fixture.truth[i][c] = fixture.source[i][c] + float(correction);
            fixture.destination[i] = fixture.coverage[i] > 0 ? color(.9, .1, .8) : fixture.truth[i];
        }
    }
    const auto result = healPatch(fixture.patch(), fullAdaptation());
    check(result.status == HealingStatus::Converged, "non-affine screened reconstruction converges");
    check(result.diagnostics.iterations > 0, "non-affine illumination exercises iterative PDE");
    check(result.diagnostics.relativeResidual <= fullAdaptation().relativeTolerance,
        "substantial non-affine RHS meets relative residual criterion");
    // Screening intentionally attenuates distant non-affine boundary influence.
    // It must still improve substantially over the constant/affine baseline.
    double planeError = 0, healError = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < fixture.source.size(); ++i) {
        if (fixture.coverage[i] == 0) continue;
        const double delta = fixture.source[i][0] + .12 - fixture.truth[i][0];
        const double repaired = result.pixels[i][0] - fixture.truth[i][0];
        planeError += delta * delta; healError += repaired * repaired; ++count;
    }
    check(healError < planeError * .2, "PDE improves non-affine lighting over single plane/mean correction");
    std::cout << "Non-affine MSE: plane=" << planeError / double(count) << " Heal=" << healError / double(count)
        << " iterations=" << result.diagnostics.iterations
        << " relative residual=" << result.diagnostics.relativeResidual << '\n';

    auto noIterations = fullAdaptation();
    noIterations.maxIterations = 0;
    const auto fallback = healPatch(fixture.patch(), noIterations);
    check(fallback.status == HealingStatus::StampFallback, "exhausted solver reports Stamp fallback");
    check(fallback.pixels == fixture.source, "failed component returns source instead of partial channels");
    check(fallback.diagnostics.unconvergedComponents == 1, "nonconvergence is diagnosed");
}

void strongNeighborProtection()
{
    const auto clean = lightingFixture();
    auto edged = clean;
    for (int y = 0; y < edged.height; ++y) {
        for (int x = edged.width - 17; x < edged.width; ++x) {
            const auto i = edged.index(x, y);
            edged.destination[i] = color(.98, .98, .98);
        }
    }
    const auto result = healPatch(edged.patch(), fullAdaptation());
    check(result.status == HealingStatus::Converged, "repair next to high-contrast neighbor converges");
    check(result.diagnostics.rejectedContextSamples > 20, "unrelated high-contrast boundary rejected");
    check(maskedMse(clean, result.pixels) < 1.0e-8, "high-contrast external object does not bleed into repair");
}

void differentTextureTransfer()
{
    auto fixture = lightingFixture(112, 104);
    for (int y = 0; y < fixture.height; ++y) {
        for (int x = 0; x < fixture.width; ++x) {
            const auto i = fixture.index(x, y);
            const double texture = .02 * std::cos(x * 1.5 + y * 2.3);
            fixture.destination[i] = color(.48 + texture, .49 + texture, .44 + texture);
            if (fixture.coverage[i] > 0) fixture.destination[i] = color(.8, .04, .2);
        }
    }
    const auto result = healPatch(fixture.patch(), fullAdaptation());
    check(result.status == HealingStatus::Converged, "different source/destination textures reconstruct");
    double detailError = 0, detailEnergy = 0, repairedMean = 0;
    std::size_t count = 0;
    for (int y = 32; y < 72; ++y) {
        for (int x = 35; x < 77; ++x) {
            const auto i = fixture.index(x, y);
            const double original = fixture.source[i - 1][0] + fixture.source[i + 1][0] - 2.0 * fixture.source[i][0];
            const double repaired = result.pixels[i - 1][0] + result.pixels[i + 1][0] - 2.0 * result.pixels[i][0];
            detailError += (repaired - original) * (repaired - original);
            detailEnergy += original * original;
            repairedMean += result.pixels[i][0];
            ++count;
        }
    }
    check(detailError < detailEnergy * .0001, "transferred fine detail follows source despite destination texture mismatch");
    near(repairedMean / double(count), .48, .008, "different texture adopts surrounding destination tone");
}

void disconnectedHolesAndEdges()
{
    Fixture fixture(100, 36);
    for (int y = 0; y < fixture.height; ++y) {
        for (int x = 0; x < fixture.width; ++x) {
            const auto i = fixture.index(x, y);
            fixture.source[i] = color(.4, .45, .5);
            fixture.destination[i] = fixture.truth[i] = x < 50 ? color(.55, .6, .65) : color(.3, .35, .4);
            if ((x < 23 || (x > 73 && x < 94)) && y > 6 && y < 30) fixture.coverage[i] = .4F;
            if (x > 7 && x < 15 && y > 14 && y < 22) fixture.coverage[i] = 0;
        }
    }
    const auto result = healPatch(fixture.patch(), fullAdaptation());
    check(result.status == HealingStatus::Converged, "disconnected edge-touching region and hole converge");
    check(result.diagnostics.components == 2, "distant regions have independent boundary models");
    for (std::size_t i = 0; i < fixture.source.size(); ++i) {
        if (fixture.coverage[i] == 0) check(result.pixels[i] == fixture.source[i], "holes/exterior carry no proposed edit");
        else for (std::size_t c = 0; c < 3; ++c)
            near(result.pixels[i][c], fixture.truth[i][c], 1.0e-5, "component-local tone at image boundary");
    }
    // One component loses all context; the other should still heal.
    for (int y = 0; y < fixture.height; ++y)
        for (int x = 50; x < fixture.width; ++x) fixture.destination[fixture.index(x, y)] = {1, 0, 1, 0};
    const auto partial = healPatch(fixture.patch(), fullAdaptation());
    check(partial.status == HealingStatus::PartialFallback, "missing context affects only its component");
    check(partial.diagnostics.fallbackComponents == 1 && partial.diagnostics.healedComponents == 1,
        "partial fallback diagnosed by component");
}

void alphaAndAdaptation()
{
    auto fixture = lightingFixture();
    for (std::size_t i = 0; i < fixture.source.size(); ++i) {
        const float sourceAlpha = i % 7 == 0 ? .15F : .45F;
        for (std::size_t c = 0; c < 3; ++c) {
            fixture.source[i][c] *= sourceAlpha;
            fixture.destination[i][c] *= .3F;
        }
        fixture.source[i][3] = sourceAlpha;
        fixture.destination[i][3] = .3F;
    }
    const auto transparent = fixture.index(30, 30);
    fixture.source[transparent] = {1, .2F, .8F, 0};
    auto options = fullAdaptation();
    options.adaptation = .6F;
    const auto result = healPatch(fixture.patch(), options);
    check(result.status == HealingStatus::Converged, "translucent context supports healing");
    for (std::size_t i = 0; i < fixture.source.size(); ++i) {
        near(result.pixels[i][3], fixture.source[i][3], 0, "source alpha preserved, never rectangular opacity");
        if (fixture.coverage[i] == 0 || fixture.source[i][3] == 0) continue;
        for (std::size_t c = 0; c < 3; ++c) {
            const double source = fixture.source[i][c] / fixture.source[i][3];
            near(result.pixels[i][c] / result.pixels[i][3],
                source + .6 * (fixture.truth[i][c] - source), 1.0e-5,
                "adaptation controls spatial tone correction independently of alpha");
        }
    }
    check(result.pixels[transparent] == PremultipliedColor {}, "hidden source RGB cannot paint");
    const auto p = fixture.index(35, 35);
    const auto original = color(.1, .2, .3, .5F);
    const float coverage = .4F;
    const float opacity = .7F;
    const auto onRetouch = compositeLayer({}, result.pixels[p], coverage * opacity, BlendMode::Normal);
    near(onRetouch[3], fixture.source[p][3] * coverage * opacity, 1.0e-7,
        "retouch output alpha equals source alpha times coverage and opacity once");
    const auto composite = compositeLayer(original, onRetouch, 1, BlendMode::Normal);
    near(composite[3], onRetouch[3] + original[3] * (1 - onRetouch[3]), 1.0e-7,
        "unflattened retouch obeys source-over alpha contract");

    options.adaptation = 0;
    const auto stamp = healPatch(fixture.patch(), options);
    check(stamp.status == HealingStatus::Converged, "zero adaptation valid");
    check(stamp.pixels[p] == fixture.source[p], "zero adaptation is exact source pass-through");
    for (auto& pixel : fixture.destination) pixel = {1, .5F, 1, 0};
    const auto empty = healPatch(fixture.patch(), fullAdaptation());
    check(empty.status == HealingStatus::StampFallback, "transparent black/hidden RGB is not valid context");
    check(empty.diagnostics.fallbackPixels > 0 && !empty.diagnostics.message.empty(), "fallback is visible and counted");
}

void tinyNarrowLimitsCancellation()
{
    Fixture row(41, 1);
    for (int x = 0; x < row.width; ++x) {
        row.source[std::size_t(x)] = color(.2, .3, .4);
        row.destination[std::size_t(x)] = color(.3 + x * .005, .4, .5);
        row.coverage[std::size_t(x)] = x >= 18 && x <= 22 ? 1.0F : 0.0F;
    }
    const auto narrow = healPatch(row.patch(), fullAdaptation());
    check(narrow.status == HealingStatus::Converged, "single-row plane degeneracy handled");
    near(narrow.pixels[20][0], .4, 1.0e-5, "single-row spatial tone");
    Fixture tiny(1, 1);
    tiny.source[0] = color(.3, .4, .5, .25F);
    tiny.destination[0] = color(.4, .5, .6);
    tiny.coverage[0] = .5F;
    const auto noContext = healPatch(tiny.patch(), fullAdaptation());
    check(noContext.status == HealingStatus::StampFallback && noContext.pixels == tiny.source,
        "one-pixel patch with no boundary has explicit fallback");
    tiny.source[0] = color(.3, .4, .5, 1.0e-8F);
    const auto tinyAlpha = healPatch(tiny.patch(), fullAdaptation());
    check(tinyAlpha.pixels == tiny.source, "very small positive source alpha survives unchanged");
    auto fixture = lightingFixture();
    auto limited = fullAdaptation(); limited.maxPixels = 4;
    const auto large = healPatch(fixture.patch(), limited);
    check(large.status == HealingStatus::LimitExceeded && large.pixels.empty(), "bounded allocation refusal");
    HealingPatch invalid = fixture.patch(); invalid.width = -1;
    check(healPatch(invalid).status == HealingStatus::InvalidInput, "invalid dimensions rejected");
    invalid = fixture.patch(); invalid.coverage = {};
    check(healPatch(invalid).status == HealingStatus::InvalidInput, "mismatched arrays rejected");
    auto cancelled = fullAdaptation(); cancelled.cancelled = [] { return true; };
    const auto stopped = healPatch(fixture.patch(), cancelled);
    check(stopped.status == HealingStatus::Cancelled && stopped.pixels.empty(), "pre-solve cancellation produces no publishable result");
    int calls = 0;
    cancelled.cancelled = [&] { return ++calls > 5; };
    const auto during = healPatch(fixture.patch(), cancelled);
    check(during.status == HealingStatus::Cancelled && during.pixels.empty(), "cooperative cancellation during reconstruction");
    auto badOptions = fullAdaptation(); badOptions.adaptation = std::numeric_limits<float>::quiet_NaN();
    check(healPatch(fixture.patch(), badOptions).status == HealingStatus::InvalidInput, "nonfinite adaptation rejected");
}

// Optional artifact generation is not part of CTest. PPM is a lossless image
// containing SOURCE | DEFECT | STAMP | FEATHER | HEAL for each fixture row.
void writeComparison(std::string_view path)
{
    constexpr int panelWidth = 216, panelHeight = 192;
    std::ofstream stream(std::string(path), std::ios::binary);
    stream << "P6\n" << panelWidth * 5 << ' ' << panelHeight * 2 << "\n255\n";
    for (int row = 0; row < 2; ++row) {
        auto fixture = lightingFixture(72, 64, true);
        if (row == 1) {
            for (int y = 0; y < fixture.height; ++y)
                for (int x = fixture.width - 17; x < fixture.width; ++x)
                    fixture.destination[fixture.index(x, y)] = color(.98, .98, .98);
        }
        const auto result = healPatch(fixture.patch(), fullAdaptation());
        const std::array panels {fixture.source, fixture.destination, composed(fixture, fixture.source, false),
            composed(fixture, fixture.source, true), composed(fixture, result.pixels, true)};
        for (int y = 0; y < panelHeight; ++y) {
            for (const auto& panel : panels) {
                for (int x = 0; x < panelWidth; ++x) {
                    const auto pixel = encodeColor(panel[fixture.index(x / 3, y / 3)]);
                    const char rgb[] {char(pixel.red), char(pixel.green), char(pixel.blue)};
                    stream.write(rgb, 3);
                }
            }
        }
    }
    check(bool(stream), "comparison PPM written");
}

void benchmark()
{
    for (const int side : {128, 512, 960}) {
        for (const bool nonlinear : {false, true}) {
            auto fixture = lightingFixture(side, side);
            if (nonlinear) {
                for (int y = 0; y < side; ++y) {
                    for (int x = 0; x < side; ++x) {
                        const auto i = fixture.index(x, y);
                        const float correction = .12F + .1F * (float(x) * 2 / float(side) - 1) * (float(y) * 2 / float(side) - 1);
                        for (std::size_t c = 0; c < 3; ++c)
                            fixture.destination[i][c] = fixture.coverage[i] > 0 ? .8F : fixture.source[i][c] + correction;
                    }
                }
            }
            const auto start = std::chrono::steady_clock::now();
            const auto result = healPatch(fixture.patch(), fullAdaptation());
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "Heal benchmark " << (nonlinear ? "curved " : "affine ") << side << 'x' << side << " ms=" << elapsed
                << " scratch_bound=" << result.diagnostics.estimatedScratchBytes
                << " iterations=" << result.diagnostics.iterations
                << " status=" << static_cast<int>(result.status) << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    lightingAndTexture();
    nonlinearReconstruction();
    strongNeighborProtection();
    differentTextureTransfer();
    disconnectedHolesAndEdges();
    alphaAndAdaptation();
    tinyNarrowLimitsCancellation();
    if (argc == 3 && std::string_view(argv[1]) == "--comparison") writeComparison(argv[2]);
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
    if (failures) std::cerr << failures << " Healing test failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
