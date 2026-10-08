#include "imageeditor/core/ObjectSelectionBoundary.hpp"
#include "imageeditor/core/SelectionGraphCut.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
int main()
{
    using namespace imageeditor::core;
    std::mt19937 rng(4901);
    std::atomic_bool cancel { false };
    int failures = 0;
    // Independent enumeration of every labeling, including asymmetric terminals
    // and ties, rather than comparing two implementations of max flow.
    for (int trial = 0; trial < 600; ++trial) {
        std::array<float, 9> fg, bg, right, down;
        std::array<std::uint8_t, 9> active;
        for (unsigned i = 0; i < 9; ++i) {
            fg[i] = float(rng() % 16) / 4;
            bg[i] = float(rng() % 16) / 4;
            right[i] = float(rng() % 12) / 4;
            down[i] = float(rng() % 12) / 4;
            active[i] = std::uint8_t(trial % 2 || rng() % 3);
        }
        const auto result = selectionGraphCut(3, 3, fg, bg, right, down, cancel, active);
        const auto energy = [&](unsigned mask) {
            double sum = 0;
            for (unsigned i = 0; i < 9; ++i) {
                const bool bit = (mask >> i) & 1;
                sum += bit ? fg[i] : bg[i];
                if (i % 3 < 2 && bit != bool((mask >> (i + 1)) & 1))
                    sum += right[i];
                if (i + 3 < 9 && bit != bool((mask >> (i + 3)) & 1))
                    sum += down[i];
                if (!active[i] && bit != (fg[i] < bg[i]))
                    return 1e9;
            }
            return sum;
        };
        double best = 1e9;
        for (unsigned label = 0; label < 512; ++label)
            best = std::min(best, energy(label));
        unsigned actual = 0;
        for (unsigned i = 0; i < 9; ++i)
            actual |= unsigned(result[i]) << i;
        if (std::abs(energy(actual) - best) > 1e-6) {
            ++failures;
            std::cerr << "Energy mismatch " << trial << '\n';
        }
    }
    // A coarse boundary is deliberately two pixels left of the native edge.
    // Local color evidence must recover the edge, not merely upsample logits.
    SmartReferenceImage image { { 64, 32 }, std::vector<Rgba8>(64 * 32),
        std::vector<std::uint8_t>(64 * 32, 1) };
    std::vector<float> logits(64 * 32);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            image.pixels[y * 64 + x] = x < 32 ? Rgba8 { 200, 30, 40, 255 } : Rgba8 { 20, 120, 200, 255 };
            logits[y * 64 + x] = (29.5f - float(x)) * 2;
        }
    const auto recovered = recoverObjectSelectionBoundary(image, logits, cancel);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 64; ++x)
            if (bool(recovered[y * 64 + x]) != (x < 32))
                ++failures;
    image.pixels[10] = { 255, 255, 255, 0 };
    if (recoverObjectSelectionBoundary(image, logits, cancel)[10] != 0)
        ++failures;
    std::array<float, 1> zero { }, invalid { std::numeric_limits<float>::quiet_NaN() };
    std::array<std::uint8_t, 1> fixed { };
    bool rejected = false;
    try {
        (void)selectionGraphCut(1, 1, invalid, zero, zero, zero, cancel, fixed);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected)
        ++failures;
    cancel = true;
    if (!selectionGraphCut(1, 1, zero, zero, zero, zero, cancel).empty())
        ++failures;
    std::cout << "Selection graph exhaustive-energy checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
