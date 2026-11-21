#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/ProjectPaint.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <functional>

namespace imageeditor::core {
namespace {
// Multiple placements of a linked project retain the same immutable image.
std::size_t repeatedProjectBytes(const Layer& layer, std::unordered_set<SurfaceId>& seen)
{
    const auto* raster = std::get_if<RasterLayer>(&layer.payload);
    if (!layer.projectSource || !raster || !raster->surface) return 0;
    const auto base = projectBasePixels(raster->surface);
    if (!seen.insert(raster->surface->id()).second) return raster->surface->memoryCost();
    // Independently painted placements still share their source allocation.
    if (base != raster->surface && !seen.insert(base->id()).second) return base->memoryCost();
    return 0;
}

Layer independentLayerCopy(const Layer& layer)
{
    auto copy = layer;
    copy.renderCache.reset();
    copy.filterCache.reset();
    copy.effectCache.reset();
    auto* raster = std::get_if<RasterLayer>(&copy.payload);
    if (!raster || layerPixelsReadOnly(copy)) return copy;
    if (!raster->surface) throw std::invalid_argument("Raster layer has no pixels");
    if (auto scalar = std::dynamic_pointer_cast<ScalarRasterSurface>(raster->surface)) {
        raster->surface = scalar->clone();
    } else {
        const auto extent = raster->surface->extent();
        const std::size_t stride = std::size_t(extent.width) * 4;
        std::vector<std::byte> pixels(stride * extent.height);
        raster->surface->copyRgba8({0, 0, int(extent.width), int(extent.height)}, pixels, stride);
        raster->surface = std::make_shared<ContiguousRasterSurface>(extent, std::move(pixels), raster->surface->dataKind());
    }
    return copy;
}
}

std::unique_ptr<Document> duplicateDocument(const Document& source)
{
    auto copy = std::make_unique<Document>(source.canvas());
    std::vector<Layer> layers;
    layers.reserve(source.layers().size());
    for (const auto& layer : source.layers()) layers.push_back(independentLayerCopy(layer));
    // IDs are local to a document. Keeping them preserves mirrors, channel
    // roles, clipping groups and metadata references in the new tree.
    if (!source.tree().roots.empty() && !copy->replaceStructure(copy->tree(), source.tree(), {}, layers))
        throw std::runtime_error("Unable to duplicate the layer tree");
    copy->setSelection(source.selection(), source.selectionEvidence());
    copy->setLastSelection(source.lastSelection());
    copy->setSnapshotTimeline(copy->snapshotTimeline(), source.snapshotTimeline());
    copy->setGuides(copy->guides(), source.guides());
    copy->setBrushSymmetry(copy->brushSymmetry(), source.brushSymmetry());
    copy->markUnsaved();
    return copy;
}

LayerTransfer captureLayerTransfer(const Document& source, std::span<const LayerId> selection)
{
    LayerTransfer result;
    std::unordered_map<LayerId,LayerId> copiedIds;
    const auto roots = source.tree().normalize(selection);
    const auto leaves = source.expandedLayers(roots);
    std::unordered_set<SurfaceId> projectSurfaces;
    std::size_t bytes = 0;
    for (auto id : leaves) {
        const auto& layer=*source.layer(id);
        bytes += retainedLayerMemory(layer) - repeatedProjectBytes(layer, projectSurfaces);
        // A mirror copied without its source becomes a real, independent image.
        // Admit that materialized storage before allocating it below.
        if (layer.mirror && std::ranges::find(leaves,layer.mirror->source)==leaves.end())
            if (const auto* raster=std::get_if<RasterLayer>(&layer.payload)) {
                const auto e=raster->surface->extent();
                bytes+=std::size_t(e.width)*e.height*4;
            }
        if (bytes > 256ULL * 1024 * 1024) throw std::length_error("Layer transfer exceeds the 256 MiB history budget");
    }
    std::function<LayerId(LayerId, bool)> clone = [&](LayerId id, bool root) {
        auto next = makeLayerId();
        copiedIds[id]=next;
        if (const auto* folder = source.tree().container(id)) {
            auto copy = *folder; copy.id = next;
            if (root) copy.visible = source.isEffectivelyVisible(id);
            for (auto& child : copy.children) {
                const auto old = child; child = clone(child, false);
                for (auto& role : copy.channelRoles) if (role == old) role = child;
            }
            result.tree.containers.push_back(std::move(copy));
        } else {
            auto copy = independentLayerCopy(*source.layer(id)); copy.id = next;
            if (root) copy.visible = source.isEffectivelyVisible(id);
            result.layers.push_back(std::move(copy));
        }
        return next;
    };
    for (auto root : roots) result.tree.roots.push_back(clone(root, true));
    for (auto& layer:result.layers) if (layer.mirror) {
        const auto source=copiedIds.find(layer.mirror->source);
        if (source!=copiedIds.end()) layer.mirror->source=source->second;
        else layer=promotedMirroredLayer(layer);
    }
    return result;
}
std::unique_ptr<LayerStructureCommand> insertLayerTransfer(const Document& destination, LayerTransfer transfer,
    ItemPlacement placement, LayerSelectionState before, Vec2d translation)
{
    if (transfer.tree.roots.empty()) return {};
    auto tree = destination.tree();
    for (const auto& layer : transfer.layers) if (destination.containsItem(layer.id)) throw std::invalid_argument("Transfer identity collision");
    for (const auto& folder : transfer.tree.containers) if (destination.containsItem(folder.id)) throw std::invalid_argument("Transfer identity collision");
    tree.containers.insert(tree.containers.end(), transfer.tree.containers.begin(), transfer.tree.containers.end());
    auto* siblings = tree.children(placement.parent);
    if (!siblings || placement.index > siblings->size()) throw std::invalid_argument("Transfer destination changed");
    siblings->insert(siblings->begin() + std::ptrdiff_t(placement.index), transfer.tree.roots.begin(), transfer.tree.roots.end());
    for (auto& layer : transfer.layers) {
        layer.localToDocument=composeTransform({1,0,translation.x,0,1,translation.y},layer.localToDocument);
    }
    LayerSelectionState after {transfer.tree.roots, transfer.tree.roots.back(), transfer.tree.roots.back()};
    return std::make_unique<LayerStructureCommand>("Copy layers from document", destination, std::move(tree),
        std::vector<LayerId>{}, std::move(transfer.layers), std::move(before), std::move(after));
}
std::unique_ptr<LayerStructureCommand> consolidateLayerItems(
    const Document& doc, const LayerSelectionState& state, Layer raster)
{
    for (auto id : doc.expandedLayers(state.ids))
        if (isScalarLayer(*doc.layer(id)) || isPackedDataLayer(*doc.layer(id)))
            throw std::invalid_argument("Use Channel Group conversion to preserve numeric data instead of merging.");
    auto tree = doc.tree();
    if (!std::holds_alternative<RasterLayer>(raster.payload) || !tree.consolidate(state.ids, raster.id))
        throw std::invalid_argument("Merge targets changed; no layers were changed");
    const auto id = raster.id;
    return std::make_unique<LayerStructureCommand>("Merge layers", doc, std::move(tree),
        doc.expandedLayers(state.ids), std::vector<Layer>{std::move(raster)}, state,
        LayerSelectionState{{id}, id, id});
}
std::unique_ptr<LayerStructureCommand> duplicateLayerItems(const Document& doc, const LayerSelectionState& state)
{
    const auto ids = doc.tree().normalize(state.ids);
    for (auto id : ids) if (channelGroupForLayer(doc, id))
        throw std::invalid_argument("Duplicate the Channel Group to preserve channel roles.");
    if (ids.empty()) return {};
    const auto leaves = doc.expandedLayers(ids);
    std::unordered_set<SurfaceId> projectSurfaces;
    std::size_t bytes = 0;
    for (auto id : leaves) {
        const auto& layer = *doc.layer(id);
        bytes += retainedLayerMemory(layer) - repeatedProjectBytes(layer, projectSurfaces);
        if (bytes > 256ULL * 1024 * 1024)
            throw std::length_error("Duplication exceeds the 256 MiB history budget");
    }
    auto tree = doc.tree();
    std::vector<Layer> added;
    std::unordered_map<LayerId,LayerId> copiedIds;
    std::function<LayerId(LayerId, bool)> clone = [&](LayerId id, bool root) {
        const auto newId = makeLayerId();
        copiedIds[id]=newId;
        if (const auto* source = doc.tree().container(id)) {
            auto copy = *source;
            copy.id = newId;
            if (root) copy.name += " copy";
            for (auto& child : copy.children) {
                const auto old = child; child = clone(child, false);
                for (auto& role : copy.channelRoles) if (role == old) role = child;
            }
            tree.containers.push_back(std::move(copy));
        } else {
            auto copy = independentLayerCopy(*doc.layer(id));
            copy.id = newId;
            if (root) copy.name += " copy";
            added.push_back(std::move(copy));
        }
        return newId;
    };
    LayerSelectionState after;
    for (auto id : ids) {
        const auto newId = clone(id, true);
        const auto p = *tree.placement(id);
        auto& siblings = *tree.children(p.parent);
        siblings.insert(siblings.begin()+std::ptrdiff_t(p.index+1), newId);
        after.ids.push_back(newId);
        if (state.primary && (*state.primary == id || doc.tree().isAncestor(id, *state.primary))) after.primary = newId;
    }
    if (!after.primary) after.primary = after.ids.back();
    after.anchor = after.primary;
    std::vector<LayerId> allLeaves;
    for (const auto& l : doc.layers()) allLeaves.push_back(l.id);
    for (const auto& l : added) allLeaves.push_back(l.id);
    (void)tree.orderedLeaves(allLeaves); // Counts, depth and membership before commit.
    for (auto& layer:added) if (layer.mirror) {
        const auto source=copiedIds.find(layer.mirror->source);
        if (source!=copiedIds.end()) layer.mirror->source=source->second;
    }
    return std::make_unique<LayerStructureCommand>("Duplicate layers", doc, std::move(tree),
        std::vector<LayerId>{}, std::move(added), state, std::move(after));
}

std::size_t retainedLayerMemory(const Layer& l) noexcept
{
    std::size_t result = sizeof(Layer) + l.name.capacity();
    result += adjustmentMemoryCost(l.adjustments);
    result += spatialFilterMemoryCost(l.filters);
    result += layerEffectMemoryCost(l.effects);
    if(l.mask)result += sizeof(LayerMask)+l.mask->coverage->memoryCost();
    if (const auto* r = std::get_if<RasterLayer>(&l.payload); r && r->surface && !l.mirror)
        result += r->surface->memoryCost();
    if (const auto* t = std::get_if<TextLayer>(&l.payload))
        result += textMemoryCost(*t);
    if (const auto* s = std::get_if<ShapeLayer>(&l.payload))
        result += shapeMemoryCost(*s);
    return result;
}
LayerStructureCommand::LayerStructureCommand(std::string label, const Document& doc, LayerTree after,
    std::vector<LayerId> removed, std::vector<Layer> added,
    std::optional<LayerSelectionState> beforeSelection, std::optional<LayerSelectionState> afterSelection,
    std::vector<ItemVisibilityUpdate> retainedLeafVisibility)
    : label_(std::move(label))
    , before_(doc.tree())
    , after_(std::move(after))
    , removedIds_(std::move(removed))
    , added_(std::move(added))
    , beforeSelection_(std::move(beforeSelection))
    , afterSelection_(std::move(afterSelection))
    , forwardVisibility_(std::move(retainedLeafVisibility))
{
    // Deleting or changing the kind of a source preserves surviving mirrors
    // as independent layers. Keep their linked originals in the same history
    // entry so undo restores both the source and the original relationships.
    for (const auto& layer:doc.layers()) if (layer.mirror
        && std::ranges::find(removedIds_,layer.id)==removedIds_.end()
        && std::ranges::find(removedIds_,layer.mirror->source)!=removedIds_.end()) {
        const auto replacement=std::ranges::find(added_,layer.mirror->source,&Layer::id);
        if (replacement!=added_.end() && canMirrorLayer(*replacement)) continue;
        removedIds_.push_back(layer.id);
        added_.push_back(promotedMirroredLayer(layer));
    }
    for (const auto& update : forwardVisibility_)
        reverseVisibility_.push_back({ update.id, update.after, update.before });
    std::unordered_set<SurfaceId> projectSurfaces;
    for (auto id : removedIds_) {
        const auto* l = doc.layer(id);
        if (!l)
            throw std::invalid_argument("Structural edit target disappeared");
        repeatedProjectPixelBytes_ += repeatedProjectBytes(*l, projectSurfaces);
        removed_.push_back(*l);
        removed_.back().renderCache.reset(); // Disposable, not retained by history.
        removed_.back().filterCache.reset();
        removed_.back().effectCache.reset();
    }
    for (auto& l : added_) {
        repeatedProjectPixelBytes_ += repeatedProjectBytes(l, projectSurfaces);
        addedIds_.push_back(l.id);
        l.renderCache.reset();
        l.filterCache.reset();
        l.effectCache.reset();
    }
}
bool LayerStructureCommand::apply(Document& doc)
{
    return doc.replaceStructure(before_, after_, removedIds_, added_, forwardVisibility_);
}
bool LayerStructureCommand::undo(Document& doc)
{
    return doc.replaceStructure(after_, before_, addedIds_, removed_, reverseVisibility_);
}
bool LayerStructureCommand::captureAddedLayerTransforms(const Document& doc)
{
    if (!removedIds_.empty() || added_.empty() || doc.tree() != after_) return false;
    for (const auto& layer : added_) {
        const auto* current = doc.layer(layer.id);
        if (!current || !current->localToDocument.inverted()) return false;
    }
    for (auto& layer : added_) layer.localToDocument = doc.layer(layer.id)->localToDocument;
    return true;
}
const LayerSelectionState* LayerStructureCommand::layerSelectionAfter(bool undo) const noexcept
{
    const auto& state = undo ? beforeSelection_ : afterSelection_;
    return state ? &*state : nullptr;
}
std::size_t LayerStructureCommand::memoryCost() const noexcept
{
    auto result = sizeof(*this) + label_.capacity() + before_.memoryCost() + after_.memoryCost()
        + (removedIds_.capacity() + addedIds_.capacity()) * sizeof(LayerId)
        + (removed_.capacity() + added_.capacity()) * sizeof(Layer);
    result += (forwardVisibility_.capacity() + reverseVisibility_.capacity()) * sizeof(ItemVisibilityUpdate);
    for (const auto& l : removed_)
        result += retainedLayerMemory(l) - sizeof(Layer);
    for (const auto& l : added_)
        result += retainedLayerMemory(l) - sizeof(Layer);
    result -= repeatedProjectPixelBytes_;
    if (beforeSelection_)
        result += beforeSelection_->ids.capacity() * sizeof(LayerId);
    if (afterSelection_)
        result += afterSelection_->ids.capacity() * sizeof(LayerId);
    return result;
}
bool SetItemMetadataCommand::apply(Document& doc)
{
    if (!captured_) {
        if (const auto* l = doc.layer(id_)) {
            beforeName_ = l->name;
            beforeLabel_ = ColorLabel(l->colorLabel);
        } else if (const auto* c = doc.tree().container(id_)) {
            beforeName_ = c->name;
            beforeLabel_ = c->colorLabel;
        } else
            return false;
        captured_ = true;
    }
    return doc.setItemMetadata(id_, afterName_, afterLabel_);
}
bool SetItemMetadataCommand::undo(Document& doc)
{
    return captured_ && doc.setItemMetadata(id_, beforeName_, beforeLabel_);
}
} // namespace imageeditor::core
