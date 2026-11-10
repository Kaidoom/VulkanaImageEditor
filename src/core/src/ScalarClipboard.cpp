#include "imageeditor/core/ScalarClipboard.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
bool validScalarFragment(const ScalarFragment& fragment) noexcept
{
    const auto e = fragment.extent;
    const auto pixels = std::uint64_t(e.width) * e.height;
    return !e.empty() && e.width <= 16384 && e.height <= 16384 && pixels <= 40ULL * 1024 * 1024
        && fragment.values.size() == pixels && fragment.coverage.size() == pixels
        && fragment.pixelsToDocument.inverted().has_value()
        && fragment.pixelsToDocument.validOver({0, 0, double(e.width), double(e.height)});
}
std::optional<ScalarFragment> captureScalarFragment(const Document& document, LayerId id)
{
    const auto* layer = document.layer(id);
    if (!layer || !isScalarLayer(*layer)) return {};
    const auto surface = std::get<RasterLayer>(layer->payload).surface;
    const auto extent = surface->extent();
    if (extent.width > 16384 || extent.height > 16384 || std::uint64_t(extent.width) * extent.height > 40ULL * 1024 * 1024)
        throw std::length_error("Channel clipboard exceeds the image-size limit");
    const auto transform = intrinsicTransform(*layer);
    const auto selection = document.selection();
    const auto coverageAt = [&](int x, int y) -> std::uint8_t {
        if (!selection) return 255;
        const auto p = transform.map({double(x) + .5, double(y) + .5});
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0 || p.y < 0
            || p.x >= document.canvas().extent.width || p.y >= document.canvas().extent.height) return 0;
        return selection->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y)));
    };
    RectI bounds = selection ? RectI{} : RectI{0, 0, int(extent.width), int(extent.height)};
    if (selection) for (int y = 0; y < int(extent.height); ++y)
        for (int x = 0; x < int(extent.width); ++x)
            if (coverageAt(x, y)) bounds = bounds.united({x, y, 1, 1});
    if (bounds.empty()) return {};
    ScalarFragment fragment;
    fragment.extent = {std::uint32_t(bounds.width), std::uint32_t(bounds.height)};
    fragment.pixelsToDocument = composeTransform(transform, {1, 0, double(bounds.x), 0, 1, double(bounds.y)});
    fragment.values.resize(std::size_t(bounds.width) * bounds.height);
    fragment.coverage.resize(fragment.values.size());
    surface->copyNative(bounds, fragment.values, std::size_t(bounds.width));
    for (int y = 0; y < bounds.height; ++y)
        for (int x = 0; x < bounds.width; ++x)
            fragment.coverage[std::size_t(y) * bounds.width + x] = std::byte(coverageAt(bounds.x + x, bounds.y + y));
    return fragment;
}
RasterEditCommitResult pasteScalarFragment(Document& document, LayerId id, const ScalarFragment& fragment, History& history)
{
    const auto* layer = document.layer(id);
    if (!layer || !isScalarLayer(*layer)) return RasterEditCommitResult::TargetUnavailable;
    if (!validScalarFragment(fragment)) throw std::invalid_argument("Invalid scalar clipboard data");
    const auto surface = std::get<RasterLayer>(layer->payload).surface;
    const auto extent = surface->extent();
    const auto mapping = composeTransform(*fragment.pixelsToDocument.inverted(), intrinsicTransform(*layer));
    RasterEditTransaction transaction(document, id, "Paste channel pixels", {.ignoreSelection = true});
    // Bounded row scratch; the transaction stores only tiles actually changed.
    std::vector<std::byte> row(std::size_t(extent.width) * 4);
    for (int y = 0; y < int(extent.height); ++y) {
        surface->copyRgba8({0, y, int(extent.width), 1}, row, row.size());
        bool changed = false;
        for (int x = 0; x < int(extent.width); ++x) {
            const auto point = mapping.map({double(x) + .5, double(y) + .5});
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
                || point.x >= fragment.extent.width || point.y >= fragment.extent.height) continue;
            // Nearest source texel keeps grid-aligned clipboard transfers exact.
            const auto index = std::size_t(std::floor(point.y)) * fragment.extent.width + std::size_t(std::floor(point.x));
            const auto coverage = double(std::to_integer<unsigned char>(fragment.coverage[index])) / 255;
            const auto before = std::to_integer<unsigned char>(row[std::size_t(x) * 4]);
            const auto value = std::byte(std::uint8_t(std::lround(before + coverage *
                (std::to_integer<unsigned char>(fragment.values[index]) - double(before)))));
            changed |= value != row[std::size_t(x) * 4];
            row[std::size_t(x) * 4] = row[std::size_t(x) * 4 + 1] = row[std::size_t(x) * 4 + 2] = value;
        }
        if (changed) (void)transaction.writeRgba8({0, y, int(extent.width), 1}, row, row.size());
    }
    return transaction.commit(history);
}
} // namespace imageeditor::core
