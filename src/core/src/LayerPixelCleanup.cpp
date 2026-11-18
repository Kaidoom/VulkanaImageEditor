#include "imageeditor/core/LayerPixelCleanup.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RegionalRasterSurface.hpp"
#include <stdexcept>

namespace imageeditor::core {
namespace {
bool writableColorRaster(const Layer* layer)
{
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    return raster && raster->surface && !layerPixelsReadOnly(*layer)
        && raster->surface->dataKind() == RasterDataKind::Color;
}
}

bool canCleanLayerPixels(const Document& doc, std::span<const LayerId> selected)
{
    const auto ids = doc.expandedLayers(selected);
    return !ids.empty() && std::ranges::all_of(ids, [&](auto id) { return writableColorRaster(doc.layer(id)); });
}

std::unique_ptr<Command> makeLayerPixelCleanupCommand(const Document& doc, const LayerSelectionState& selected,
    LayerPixelCleanup operation, std::uint8_t threshold, const std::function<bool()>& keepGoing)
{
    const auto ids = doc.expandedLayers(selected.ids);
    if (ids.empty())
        return { };
    // Pin all sources before the first progress callback. No document pixels are
    // published until the entire selection has been processed successfully.
    std::vector<std::pair<std::shared_ptr<RasterSurface>, Revision>> inputs;
    inputs.reserve(ids.size());
    for (auto id : ids) {
        const auto* layer = doc.layer(id);
        if (!writableColorRaster(layer))
            return { };
        const auto source = std::get<RasterLayer>(layer->payload).surface;
        inputs.emplace_back(source, source->revision());
    }
    const auto canvas = doc.canvas().extent;
    const RectD canvasBounds { 0, 0, double(canvas.width), double(canvas.height) };
    const auto revision = doc.revision();
    std::vector<LayerId> changed;
    std::vector<Layer> replacements;
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto id = ids[i];
        const auto layer = *doc.layer(id);
        const auto& [source, pixelRevision] = inputs[i];
        const auto size = source->extent();
        const int width = int(size.width), height = int(size.height);
        const auto mapping = intrinsicTransform(layer);
        if (operation == LayerPixelCleanup::TrimToCanvas) {
            const RectD frame { 0, 0, double(width), double(height) };
            if (!mapping.validOver(frame))
                throw std::runtime_error("Invalid layer transform for canvas trim");
            const std::array<Vec2d, 4> corners { { { .5, .5 }, { width - .5, .5 }, { .5, height - .5 },
                { width - .5, height - .5 } } };
            if (std::ranges::all_of(corners, [&](Vec2d p) { return canvasBounds.contains(mapping.map(p)); }))
                continue;
        }
        const std::size_t stride = size_t(width) * sizeof(Rgba8);
        constexpr int bandHeight = RegionalRasterSurface::tileSide;
        std::vector<Rgba8> band(size_t(width) * size_t(std::min(height, bandHeight)));
        std::shared_ptr<RegionalRasterSurface> edited;
        for (int y = 0; y < height; y += bandHeight) {
            if (keepGoing && !keepGoing())
                return { };
            if (doc.revision() != revision || source->revision() != pixelRevision)
                throw std::runtime_error("Layer changed during cleanup");
            const int rows = std::min(height - y, bandHeight);
            source->copyRgba8({ 0, y, width, rows }, std::as_writable_bytes(std::span(band)), stride);
            int left = width, right = -1;
            for (int row = 0; row < rows; ++row) {
                auto* pixels = band.data() + size_t(row) * size_t(width);
                for (int x = 0; x < width; ++x) {
                    if (!pixels[x].alpha)
                        continue;
                    if (operation == LayerPixelCleanup::RemoveLowAlpha) {
                        if (pixels[x].alpha > threshold)
                            continue;
                    } else {
                        const auto p = mapping.map({ x + .5, y + row + .5 });
                        if (canvasBounds.contains(p))
                            continue;
                    }
                    pixels[x].alpha = 0;
                    left = std::min(left, x);
                    right = std::max(right, x);
                }
            }
            if (right < left)
                continue;
            if (!edited)
                edited = std::make_shared<RegionalRasterSurface>(source, layer.rasterOrigin);
            edited->replaceRgba8(
                { left, y, right - left + 1, rows }, std::as_bytes(std::span(band).subspan(size_t(left))), stride);
        }
        if (edited) {
            changed.push_back(id);
            auto& next = replacements.emplace_back(layer);
            next.payload = RasterLayer { std::move(edited) };
        }
    }
    if (std::ranges::any_of(inputs, [](const auto& input) { return input.first->revision() != input.second; }))
        throw std::runtime_error("Layer changed during cleanup");
    if (changed.empty())
        return { };
    return std::make_unique<LayerStructureCommand>(
        operation == LayerPixelCleanup::TrimToCanvas ? "Trim to Canvas Bounds" : "Clean Up Transparency", doc,
        doc.tree(), std::move(changed), std::move(replacements), selected, selected);
}
} // namespace imageeditor::core
