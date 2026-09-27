#include "imageeditor/core/RulerTicks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace imageeditor::core {

RulerTicks makeRulerTicks(double zoom, double zeroLogical, double stripStartLogical,
    double stripLength, double documentLength)
{
    RulerTicks result;
    if (!std::isfinite(zoom) || zoom <= 0 || !std::isfinite(zeroLogical)
        || !std::isfinite(stripStartLogical) || !std::isfinite(stripLength)
        || stripLength <= 0 || !std::isfinite(documentLength) || documentLength <= 0)
        return result;

    // Long-double intermediate positions keep a far-panned ruler stable and
    // prevent overflow before clipping. No loop walks the offscreen document.
    constexpr std::size_t maximumTicks = 4096;
    const auto scale = static_cast<long double>(zoom);
    const auto start = static_cast<long double>(stripStartLogical);
    const auto end = start + stripLength;
    const auto zero = static_cast<long double>(zeroLogical);
    const auto visibleStart = std::max(0.0L, (start - zero) / scale);
    const auto visibleEnd = std::min(static_cast<long double>(documentLength), (end - zero) / scale);
    if (visibleStart > visibleEnd) return result;

    // Each major span has at most five subdivisions. Increase spacing on
    // unusually huge strips so the allocation/work bound is unconditional.
    const auto wanted = std::max(80.0L, static_cast<long double>(stripLength)
        * 5 / static_cast<long double>(maximumTicks - 1)) / scale;
    const auto decade = std::pow(10.0L, std::floor(std::log10(wanted)));
    const auto relative = wanted / decade;
    const auto multiplier = relative <= 1 ? 1 : relative <= 2 ? 2 : relative <= 5 ? 5 : 10;
    const auto majorStep = decade * multiplier;
    const auto divisions = multiplier == 2 ? 4 : 5;
    const auto minorStep = majorStep / divisions;
    // Extreme zoom values can ask for a step outside the public double model.
    // An empty, subdued strip is safer than invalid tick/label coordinates.
    if (majorStep > std::numeric_limits<double>::max()
        || minorStep < std::numeric_limits<double>::denorm_min()) return result;
    result.majorStep = static_cast<double>(majorStep);
    result.minorStep = static_cast<double>(minorStep);

    const auto firstIndex = std::ceil(visibleStart / minorStep);
    const auto lastIndex = std::floor(visibleEnd / minorStep);
    if (lastIndex < firstIndex) return result;
    const auto count = static_cast<std::size_t>(std::min(
        static_cast<long double>(maximumTicks), lastIndex - firstIndex + 1));
    result.ticks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto index = firstIndex + static_cast<long double>(i);
        const auto document = index * minorStep;
        const auto logical = zero + document * scale;
        if (document < 0 || document > documentLength || logical < start || logical > end) continue;
        const auto documentDouble = static_cast<double>(document);
        const auto logicalDouble = static_cast<double>(logical);
        if (!std::isfinite(documentDouble) || !std::isfinite(logicalDouble)) continue;
        // At astronomical coordinates adjacent mathematical marks may map to
        // one representable value. Never emit duplicates or stall a loop.
        if (!result.ticks.empty() && (documentDouble <= result.ticks.back().document
                || logicalDouble <= result.ticks.back().logical)) continue;
        result.ticks.push_back({documentDouble, logicalDouble,
            std::fmod(index, static_cast<long double>(divisions)) == 0});
    }
    return result;
}

} // namespace imageeditor::core
