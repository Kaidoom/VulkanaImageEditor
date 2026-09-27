#include "imageeditor/core/RegionFinder.hpp"
#include "imageeditor/core/SmartSelection.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
SmartSelectionResult buildMagicWand(const SmartReferenceImage& image, Vec2d seed, int tolerance,
    SelectionState original, SelectionOperation operation, const std::atomic_bool& cancelled)
{
    const auto extent = image.extent;
    const auto pixels = std::uint64_t(extent.width) * extent.height;
    if (!pixels || pixels > SmartSelectionReference::maximumPixels || extent.width > 65536
        || extent.height > 65536 || pixels != image.pixels.size() || pixels != image.valid.size())
        throw std::invalid_argument("Invalid Magic Wand reference image");
    if (!std::isfinite(seed.x) || !std::isfinite(seed.y) || seed.x < 0 || seed.y < 0 || seed.x >= extent.width
        || seed.y >= extent.height)
        throw std::invalid_argument("Click inside the canvas and the active layer's valid image extent");
    const int seedX = int(std::floor(seed.x)), seedY = int(std::floor(seed.y));
    const auto seedIndex = std::size_t(seedY) * extent.width + std::size_t(seedX);
    if (!image.valid[seedIndex])
        throw std::invalid_argument("Click inside the canvas and the active layer's valid image extent");
    if (cancelled)
        return { };
    tolerance = std::clamp(tolerance, 0, 255);

    SmartSelectionStats stats;
    ColorSelectionField field { extent, image.pixels[seedIndex], { } };
    field.distances.reserve(std::size_t(pixels));
    for (std::size_t i = 0; i < pixels; ++i) {
        if (i % 4096 == 0 && cancelled)
            return { };
        field.distances.push_back(
            image.valid[i] ? colorSelectionDistance(field.sampled, image.pixels[i]) : 65535);
    }
    stats.evaluatedPixels = pixels;

    // Reuse Bucket's bounded span traversal, while Select by Color owns the
    // comparison/alpha contract. Mapping eligible pixels to one constant
    // bypasses RegionFinder's deliberately coarser, rounded-byte comparator.
    // Fractional-byte matches remain connected evidence at tolerance zero.
    RegionFinder region(extent, seedX, seedY, 0, [&](int x, int y) -> std::optional<Rgba8> {
        if (cancelled)
            return { };
        ++stats.evaluatedPixels;
        const auto index = std::size_t(y) * extent.width + std::size_t(x);
        if (!colorSelectionCoverage(field.distances[index], tolerance))
            return { };
        return Rgba8 { 0, 0, 0, 255 };
    });
    while (!region.step(4096))
        if (cancelled)
            return { };
    if (cancelled)
        return { };

    const auto bounds = region.bounds();
    const int left = std::max(0, bounds.x - 1), top = std::max(0, bounds.y - 1);
    const int right = std::min(int(extent.width), bounds.right() + 1);
    const int bottom = std::min(int(extent.height), bounds.bottom() + 1);
    stats.workRegion = { left, top, right - left, bottom - top };
    const auto stride = std::size_t(stats.workRegion.width);
    std::vector<std::uint8_t> coverage(stride * std::size_t(stats.workRegion.height), 0);
    for (int y = top; y < bottom; ++y) {
        if (cancelled)
            return { };
        for (int x = left; x < right; ++x) {
            const auto index = std::size_t(y) * extent.width + std::size_t(x);
            const auto distance = field.distances[index];
            if (distance == 65535)
                continue;
            const bool connected = region.contains(x, y);
            // A contour can cover a fraction of the neighboring pixel. This
            // one-pixel fringe never propagates, crosses invalid alpha/crop,
            // or admits an independently matching disconnected component.
            const bool fringe = !connected && !colorSelectionCoverage(distance, tolerance)
                && (region.contains(x - 1, y) || region.contains(x + 1, y) || region.contains(x, y - 1)
                    || region.contains(x, y + 1));
            if (connected || fringe) {
                ++stats.evaluatedPixels;
                coverage[std::size_t(y - top) * stride + std::size_t(x - left)]
                    = colorSelectionPixelCoverage(field, index, tolerance);
            }
        }
    }
    if (cancelled)
        return { };
    auto incoming = SelectionMask::fromR8Region(extent, stats.workRegion, coverage, stride);
    if (cancelled)
        return { };
    auto combined = combineSelection(original, incoming, operation);
    if (!prepareSelectionBoundary(incoming, cancelled))
        return { };
    if (combined != incoming && !prepareSelectionBoundary(combined, cancelled))
        return { };
    stats.workspaceBytes = sizeof(field) + field.distances.capacity() * sizeof(std::uint16_t)
        + coverage.capacity() + region.memoryBytes() + incoming->memoryCost()
        + (combined != incoming ? combined->memoryCost() : 0);
    return { std::move(incoming), std::move(combined), { }, stats };
}
}
