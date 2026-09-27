#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include <stdexcept>
#include <functional>

namespace imageeditor::core {
LayerTransfer captureLayerTransfer(const Document& source, std::span<const LayerId> selection)
{
    LayerTransfer result;
    const auto roots = source.tree().normalize(selection);
    std::size_t bytes = 0;
    for (auto id : source.expandedLayers(roots)) {
        bytes += retainedLayerMemory(*source.layer(id));
        if (bytes > 256ULL * 1024 * 1024) throw std::length_error("Layer transfer exceeds the 256 MiB history budget");
    }
    std::function<LayerId(LayerId, bool)> clone = [&](LayerId id, bool root) {
        auto next = makeLayerId();
        if (const auto* folder = source.tree().container(id)) {
            auto copy = *folder; copy.id = next;
            if (root) copy.visible = source.isEffectivelyVisible(id);
            for (auto& child : copy.children) child = clone(child, false);
            result.tree.containers.push_back(std::move(copy));
        } else {
            auto copy = *source.layer(id); copy.id = next;
            if (root) copy.visible = source.isEffectivelyVisible(id);
            copy.renderCache.reset(); copy.filterCache.reset(); copy.effectCache.reset();
            if (auto* raster = std::get_if<RasterLayer>(&copy.payload)) {
                if (!raster->surface) throw std::invalid_argument("Layer transfer has no source pixels");
                const auto e = raster->surface->extent();
                std::vector<std::byte> pixels(std::size_t(e.width)*e.height*4);
                raster->surface->copyRgba8({0,0,int(e.width),int(e.height)}, pixels, std::size_t(e.width)*4);
                raster->surface = std::make_shared<ContiguousRasterSurface>(e, std::move(pixels));
            }
            result.layers.push_back(std::move(copy));
        }
        return next;
    };
    for (auto root : roots) result.tree.roots.push_back(clone(root, true));
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
    if (ids.empty()) return {};
    const auto leaves = doc.expandedLayers(ids);
    std::size_t bytes = 0;
    for (auto id : leaves) {
        bytes += retainedLayerMemory(*doc.layer(id));
        if (bytes > 256ULL * 1024 * 1024)
            throw std::length_error("Duplication exceeds the 256 MiB history budget");
    }
    auto tree = doc.tree();
    std::vector<Layer> added;
    std::function<LayerId(LayerId, bool)> clone = [&](LayerId id, bool root) {
        const auto newId = makeLayerId();
        if (const auto* source = doc.tree().container(id)) {
            auto copy = *source;
            copy.id = newId;
            if (root) copy.name += " copy";
            for (auto& child : copy.children) child = clone(child, false);
            tree.containers.push_back(std::move(copy));
        } else {
            auto copy = *doc.layer(id);
            copy.id = newId;
            if (root) copy.name += " copy";
            copy.renderCache.reset(); copy.filterCache.reset(); copy.effectCache.reset();
            if (auto* raster = std::get_if<RasterLayer>(&copy.payload)) {
                if (!raster->surface) throw std::invalid_argument("Raster layer has no pixels");
                const auto e = raster->surface->extent();
                std::vector<std::byte> pixels(std::size_t(e.width) * e.height * 4);
                raster->surface->copyRgba8({0,0,int(e.width),int(e.height)}, pixels, std::size_t(e.width)*4);
                raster->surface = std::make_shared<ContiguousRasterSurface>(e, std::move(pixels));
            }
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
    return std::make_unique<LayerStructureCommand>("Duplicate layers", doc, std::move(tree),
        std::vector<LayerId>{}, std::move(added), state, std::move(after));
}

std::size_t retainedLayerMemory(const Layer& l) noexcept
{
    std::size_t result = sizeof(Layer) + l.name.capacity();
    result += adjustmentMemoryCost(l.adjustments);
    result += spatialFilterMemoryCost(l.filters);
    if(l.effects)result += sizeof(LayerEffectStack);
    if (const auto* r = std::get_if<RasterLayer>(&l.payload); r && r->surface)
        result += std::size_t(r->surface->extent().width) * r->surface->extent().height * 4;
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
    for (const auto& update : forwardVisibility_)
        reverseVisibility_.push_back({ update.id, update.after, update.before });
    for (auto id : removedIds_) {
        const auto* l = doc.layer(id);
        if (!l)
            throw std::invalid_argument("Structural edit target disappeared");
        removed_.push_back(*l);
        removed_.back().renderCache.reset(); // Disposable, not retained by history.
        removed_.back().filterCache.reset();
        removed_.back().effectCache.reset();
    }
    for (auto& l : added_) {
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
