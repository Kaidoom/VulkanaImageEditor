#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
namespace {
const RasterSurface* pixels(const Layer& layer) noexcept
{
    const auto* raster = std::get_if<RasterLayer>(&layer.payload);
    return raster ? raster->surface.get() : nullptr;
}
bool neutralDataProperties(const Layer& layer) noexcept
{
    return layer.opacity == 1 && layer.fillOpacity == 1 && layer.blendMode == BlendMode::Normal
        && !layer.mask && !layer.crop && (!layer.adjustments || std::ranges::none_of(layer.adjustments->items, [](const auto& a) { return a.enabled && !adjustmentIsNeutral(a); }))
        && !hasActiveSpatialFilters(layer.filters) && !hasActiveLayerEffects(layer.effects);
}
}
bool isScalarLayer(const Layer& layer) noexcept
{ const auto* p = pixels(layer); return p && p->dataKind() == RasterDataKind::Scalar; }
bool isPackedDataLayer(const Layer& layer) noexcept
{ const auto* p = pixels(layer); return p && p->dataKind() == RasterDataKind::PackedData; }
const LayerContainer* channelGroupForLayer(const Document& document, LayerId id) noexcept
{
    const auto placement = document.tree().placement(id);
    const auto* parent = placement ? document.tree().container(placement->parent) : nullptr;
    return parent && parent->kind == ContainerKind::ChannelGroup ? parent : nullptr;
}
bool numericLayerPropertiesSupported(const Layer& layer, RasterDataKind kind) noexcept
{
    if (kind == RasterDataKind::Color) return true;
    if (kind == RasterDataKind::PackedData) return neutralDataProperties(layer);
    return kind == RasterDataKind::Scalar && layer.opacity == 1 && layer.fillOpacity == 1
        && layer.blendMode == BlendMode::Normal && !layer.mask && !layer.crop
        && !hasActiveLayerEffects(layer.effects) && scalarAdjustmentsSupported(layer.adjustments)
        && scalarFiltersSupported(layer.filters);
}
bool validChannelStructure(const LayerTree& tree, std::span<const Layer> layers) noexcept
{
    const auto find = [&](LayerId id) -> const Layer* {
        const auto it = std::ranges::find(layers, id, &Layer::id);
        return it == layers.end() ? nullptr : &*it;
    };
    for (const auto& layer : layers)
        if (const auto* surface = pixels(layer); surface && !numericLayerPropertiesSupported(layer, surface->dataKind()))
            return false;
    for (const auto& group : tree.containers) if (group.kind == ContainerKind::ChannelGroup) {
        const auto* first = find(group.channelRoles[0]);
        if (!first || !isScalarLayer(*first) || !first->visible) return false;
        for (const auto id : group.channelRoles) if (id) {
            const auto* layer = find(id);
            if (!layer || !isScalarLayer(*layer) || !layer->visible
                || pixels(*layer)->extent() != pixels(*first)->extent()
                || layer->localToDocument != first->localToDocument
                || layer->rasterOrigin != first->rasterOrigin
                || layer->rasterEffectFrame != first->rasterEffectFrame) return false;
        }
    }
    return true;
}
bool channelTransformsCompatible(const Document& document, std::span<const LayerTransformUpdate> updates) noexcept
{
    const auto after = [&](LayerId id) {
        const auto it = std::ranges::find(updates, id, &LayerTransformUpdate::id);
        return it == updates.end() ? document.layer(id)->localToDocument : it->after;
    };
    for (const auto& group : document.tree().containers) if (group.kind == ContainerKind::ChannelGroup) {
        const auto frame = after(group.channelRoles[0]);
        for (const auto id : group.channelRoles) if (id && after(id) != frame) return false;
    }
    return true;
}
bool canConvertToChannelGroup(const Document& document, LayerId id, std::string* reason)
{
    const auto reject = [&](const char* why) { if (reason) *reason = why; return false; };
    const auto* layer = document.layer(id);
    if (!layer || !pixels(*layer) || isScalarLayer(*layer))
        return reject("Select a raster layer to convert to channels.");
    if (!neutralDataProperties(*layer))
        return reject("Channel conversion requires a raster without masks, crops, blending or active adjustments, filters and effects. Bake its appearance first if desired.");
    if (channelGroupForLayer(document, id)) return reject("This layer already belongs to a Channel Group.");
    if (reason) reason->clear();
    return true;
}
std::unique_ptr<LayerStructureCommand> convertToChannelGroup(const Document& document, const LayerSelectionState& selection)
{
    std::string error;
    if (selection.ids.size() != 1 || !canConvertToChannelGroup(document, selection.ids.front(), &error))
        throw std::invalid_argument(error.empty() ? "Select one raster layer to convert to channels." : error);
    const auto& source = *document.layer(selection.ids.front());
    const auto* surface = pixels(source);
    const auto extent = surface->extent();
    LayerContainer group;
    group.id = source.id; group.name = source.name; group.kind = ContainerKind::ChannelGroup;
    group.visible = source.visible; group.colorLabel = ColorLabel(source.colorLabel);
    std::vector<Layer> channels;
    const std::array names {"Red", "Green", "Blue", "Alpha"};
    std::vector<std::byte> values(std::size_t(extent.width) * extent.height), row(std::size_t(extent.width) * 4);
    for (std::size_t role = 0; role < 4; ++role) {
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            surface->copyRgba8({0, int(y), int(extent.width), 1}, row, row.size());
            for (std::uint32_t x = 0; x < extent.width; ++x)
                values[std::size_t(y) * extent.width + x] = row[std::size_t(x) * 4 + role];
        }
        auto channel = Layer::raster(names[role], std::make_shared<ScalarRasterSurface>(extent, values, extent.width));
        channel.localToDocument = source.localToDocument;
        channel.rasterOrigin = source.rasterOrigin;
        channel.rasterEffectFrame = source.rasterEffectFrame;
        group.channelRoles[role] = channel.id;
        channels.push_back(std::move(channel));
    }
    // The panel lists topmost first. Display R, G, B, A without using order as identity.
    group.children.assign(group.channelRoles.rbegin(), group.channelRoles.rend());
    auto tree = document.tree(); tree.containers.push_back(group);
    return std::make_unique<LayerStructureCommand>("Convert to Channel Group", document, std::move(tree),
        std::vector<LayerId>{source.id}, std::move(channels), selection,
        LayerSelectionState{{group.id}, group.id, group.id});
}
std::shared_ptr<ContiguousRasterSurface> packChannelGroup(const Document& document, LayerId id)
{
    const auto* group = document.tree().container(id);
    if (!group || group->kind != ContainerKind::ChannelGroup || !validChannelStructure(document.tree(), document.layers()))
        throw std::invalid_argument("Invalid Channel Group");
    const auto extent = pixels(*document.layer(group->channelRoles[0]))->extent();
    std::vector<std::byte> rgba(std::size_t(extent.width) * extent.height * 4);
    std::vector<std::byte> row(extent.width);
    for (std::size_t role = 0; role < 4; ++role) {
        const auto source=group->channelRoles[role]?prepareScalarLayerPixels(*document.layer(group->channelRoles[role])):nullptr;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            if (source) source->copyNative(
                {0, int(y), int(extent.width), 1}, row, row.size());
            else std::fill(row.begin(), row.end(), std::byte{255});
            for (std::uint32_t x = 0; x < extent.width; ++x)
                rgba[(std::size_t(y) * extent.width + x) * 4 + role] = row[x];
        }
    }
    return std::make_shared<ContiguousRasterSurface>(extent, std::move(rgba), RasterDataKind::PackedData);
}
std::unique_ptr<LayerStructureCommand> convertChannelGroupToRaster(const Document& document, const LayerSelectionState& selection)
{
    if (selection.ids.size() != 1) throw std::invalid_argument("Select one Channel Group to convert to raster.");
    const auto id = selection.ids.front();
    const auto* group = document.tree().container(id);
    if (!group || group->kind != ContainerKind::ChannelGroup) throw std::invalid_argument("Select a Channel Group.");
    auto raster = Layer::raster(group->name, packChannelGroup(document, id));
    raster.id = id; raster.visible = group->visible; raster.colorLabel = std::uint8_t(group->colorLabel);
    const auto& first = *document.layer(group->channelRoles[0]);
    raster.localToDocument = first.localToDocument; raster.rasterOrigin = first.rasterOrigin;
    raster.rasterEffectFrame = first.rasterEffectFrame;
    auto tree = document.tree(); std::erase_if(tree.containers, [id](const auto& c) { return c.id == id; });
    return std::make_unique<LayerStructureCommand>("Convert Channel Group to raster", document, std::move(tree),
        group->children, std::vector<Layer>{std::move(raster)}, selection, selection);
}
} // namespace imageeditor::core
