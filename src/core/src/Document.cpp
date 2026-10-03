#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include <atomic>
#include "imageeditor/core/RichText.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
bool Document::setLayerMask(LayerId id,LayerMaskState state)
{
    auto* target=layer(id);
    if (!target || !validLayerMask(state) || target->mask==state) return false;
    target->mask=std::move(state);
    touch();
    return true;
}
void Document::advanceContentState() noexcept
{
    static std::atomic<std::uint64_t> next {1};
    contentState_ = next.fetch_add(1, std::memory_order_relaxed);
}
namespace {

void validateCanvasSpec(const CanvasSpec& canvas)
{
    if (canvas.extent.empty()) {
        throw std::invalid_argument("Document canvas must be non-empty");
    }
    if (!std::isfinite(canvas.dotsPerInch) || canvas.dotsPerInch <= 0.0) {
        throw std::invalid_argument("Document DPI must be finite and positive");
    }
}

} // namespace

Document::Document(CanvasSpec canvas)
    : canvas_(canvas)
{
    validateCanvasSpec(canvas_);
    advanceContentState();
    markSaved();
}

const Layer* Document::layer(LayerId id) const noexcept
{
    const auto found = std::find_if(layers_.begin(), layers_.end(),
        [id](const Layer& candidate) { return candidate.id == id; });
    return found == layers_.end() ? nullptr : &*found;
}

Layer* Document::layer(LayerId id) noexcept
{
    const auto found = std::find_if(layers_.begin(), layers_.end(),
        [id](const Layer& candidate) { return candidate.id == id; });
    return found == layers_.end() ? nullptr : &*found;
}

bool Document::containsLayer(LayerId id) const noexcept
{
    return layer(id) != nullptr;
}

std::vector<LayerId> Document::expandedLayers(std::span<const LayerId> input) const
{
    std::vector<LayerId> result;
    for (auto id : tree_.normalize(input)) {
        const auto leaves = tree_.descendants(id);
        result.insert(result.end(), leaves.begin(), leaves.end());
    }
    return result;
}

LayerId Document::canvasTarget(LayerId leaf) const noexcept
{
    auto result = leaf;
    auto p = tree_.placement(leaf);
    while (p && p->parent) {
        const auto* c = tree_.container(p->parent);
        if (!c) break;
        if (isGroup(c->kind)) result = c->id;
        p = tree_.placement(c->id);
    }
    return result;
}

std::optional<bool> Document::itemVisibility(LayerId id) const noexcept
{
    if (const auto* leaf = layer(id)) return leaf->visible;
    if (const auto* container = tree_.container(id)) return container->visible;
    return {};
}

bool Document::isEffectivelyVisible(LayerId id) const noexcept
{
    const auto own = itemVisibility(id);
    if (!own || !*own) return false;
    for (std::size_t depth = 0; depth <= LayerTree::maxDepth; ++depth) {
        const auto position = tree_.placement(id);
        if (!position) return false;
        if (!position->parent) return true;
        const auto* parent = tree_.container(position->parent);
        if (!parent || !parent->visible) return false;
        id = parent->id;
    }
    return false;
}

bool Document::setItemVisibilities(std::span<const ItemVisibilityUpdate> updates)
{
    bool changed = false;
    for (std::size_t i = 0; i < updates.size(); ++i) {
        const auto& update = updates[i];
        if (itemVisibility(update.id) != std::optional(update.before)) return false;
        for (std::size_t previous = 0; previous < i; ++previous)
            if (updates[previous].id == update.id) return false;
        changed |= update.before != update.after;
    }
    if (!changed) return false;
    // All admission precedes publication. No allocation, per-target history,
    // raster mutation or renderer cache regeneration in this batch.
    for (const auto& update : updates) {
        if (auto* leaf = layer(update.id)) leaf->visible = update.after;
        else tree_.container(update.id)->visible = update.after;
    }
    touch();
    return true;
}

bool Document::replaceStructure(const LayerTree& expected, LayerTree replacement,
    std::span<const LayerId> removed, std::span<const Layer> added,
    std::span<const ItemVisibilityUpdate> retainedLeafVisibility)
{
    if (tree_ != expected || (replacement == tree_ && removed.empty() && added.empty())) return false;
    for (std::size_t i = 0; i < retainedLeafVisibility.size(); ++i) {
        const auto& update = retainedLeafVisibility[i];
        if (!containsLayer(update.id) || itemVisibility(update.id) != std::optional(update.before)
            || std::ranges::find(removed, update.id) != removed.end()
            || std::ranges::find(added, update.id, &Layer::id) != added.end()) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (retainedLeafVisibility[j].id == update.id) return false;
    }
    for (std::size_t i=0; i<removed.size(); ++i)
        if (!containsLayer(removed[i]) || std::find(removed.begin(), removed.begin()+std::ptrdiff_t(i), removed[i]) != removed.begin()+std::ptrdiff_t(i)) return false;
    std::vector<LayerId> ids;
    ids.reserve(layers_.size()+added.size());
    for (const auto& l : layers_)
        if (std::ranges::find(removed, l.id) == removed.end()) ids.push_back(l.id);
    for (const auto& l : added) {
        if (l.colorLabel > std::uint8_t(ColorLabel::Purple) || !isValidBlendMode(l.blendMode)
            || !validLayerMask(l.mask)
            || (l.crop && !validLayerCrop(*l.crop))
            || (l.adjustments && !validAdjustments(*l.adjustments))
            || (l.filters && !validSpatialFilters(*l.filters))) return false;
        if (const auto* shape=std::get_if<ShapeLayer>(&l.payload); shape && !validShape(*shape)) return false;
        ids.push_back(l.id);
    }
    const auto ordered = replacement.orderedLeaves(ids);
    std::vector<Layer> prepared;
    prepared.reserve(ordered.size());
    for (auto id : ordered) {
        const auto it=std::ranges::find(added, id, &Layer::id);
        if (it != added.end()) {
            prepared.push_back(*it);
            if (auto* text=std::get_if<TextLayer>(&prepared.back().payload)) *text=normalizedText(std::move(*text));
        } else prepared.push_back(*layer(id));
        for (const auto& update : retainedLeafVisibility)
            if (update.id == id) prepared.back().visible = update.after;
    }
    // All validation, allocations, and authoritative text copies precede
    // publication. Pixels/caches are shared; no renderer sees a half-edit.
    layers_.swap(prepared);
    std::swap(tree_, replacement);
    touch();
    return true;
}

bool Document::insertLayerAt(ItemPlacement p, Layer value)
{
    if (!value.id || containsItem(value.id)) return false;
    auto next=tree_;
    auto* siblings=next.children(p.parent);
    if (!siblings || p.index > siblings->size()) return false;
    siblings->insert(siblings->begin()+std::ptrdiff_t(p.index), value.id);
    return replaceStructure(tree_, std::move(next), {}, std::span<const Layer>(&value,1));
}

bool Document::setItemMetadata(LayerId id, std::string name, ColorLabel label)
{
    if (name.empty() || name.size()>4096 || label>ColorLabel::Purple) return false;
    if (auto* l=layer(id)) {
        if (l->name == name && l->colorLabel == std::uint8_t(label)) return false;
        l->name=std::move(name); l->colorLabel=std::uint8_t(label);
    } else if (auto* c=tree_.container(id)) {
        if (c->name == name && c->colorLabel == label) return false;
        c->name=std::move(name); c->colorLabel=label;
    } else return false;
    touch(); return true;
}

bool Document::insertLayer(std::size_t index, Layer insertedLayer)
{
    const auto p=index<layers_.size() ? *tree_.placement(layers_[index].id) : ItemPlacement{0,tree_.roots.size()};
    return insertLayerAt(p,std::move(insertedLayer));
}

std::optional<RemovedLayer> Document::takeLayer(LayerId id)
{
    const auto found = std::find_if(layers_.begin(), layers_.end(),
        [id](const Layer& candidate) { return candidate.id == id; });
    if (found == layers_.end()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(std::distance(layers_.begin(), found));
    Layer removed = *found;
    const auto p=*tree_.placement(id);
    auto next=tree_;
    std::erase(*next.children(p.parent),id);
    if (!replaceStructure(tree_,std::move(next),std::span<const LayerId>(&id,1))) return {};
    return RemovedLayer {.layer = std::move(removed), .index = index, .placement=p};
}

bool Document::moveLayer(LayerId id, std::size_t destinationIndex)
{
    const auto found = std::find_if(layers_.begin(), layers_.end(),
        [id](const Layer& candidate) { return candidate.id == id; });
    if (found == layers_.end() || layers_.size() < 2) {
        return false;
    }

    const auto sourceIndex = static_cast<std::size_t>(
        std::distance(layers_.begin(), found));
    destinationIndex = std::min(destinationIndex, layers_.size() - 1U);
    if (sourceIndex == destinationIndex) {
        return false;
    }

    auto next=tree_;
    auto p=*tree_.placement(layers_[destinationIndex].id);
    if (destinationIndex>sourceIndex) ++p.index;
    if (!next.reparent(std::span<const LayerId>(&id,1),p)) return false;
    return replaceStructure(tree_,std::move(next));
}

bool Document::setLayerVisibility(LayerId id, bool visible)
{
    const auto before = itemVisibility(id);
    if (!before) return false;
    const ItemVisibilityUpdate update { id, *before, visible };
    return setItemVisibilities(std::span(&update, 1));
}

bool Document::setLayerOpacity(LayerId id, float opacity)
{
    auto* target = layer(id);
    opacity = std::clamp(opacity, 0.0F, 1.0F);
    if (!target || target->opacity == opacity) {
        return false;
    }
    target->opacity = opacity;
    touch();
    return true;
}

bool Document::setLayerBlendMode(LayerId id, BlendMode mode)
{
    auto* target = layer(id);
    if (!target || !isValidBlendMode(mode) || target->blendMode == mode) return false;
    target->blendMode = mode;
    touch();
    return true;
}

bool Document::setLayerAdjustments(LayerId id, AdjustmentState state)
{
    auto* target = layer(id);
    if (!target || (state && !validAdjustments(*state))
        || equivalentAdjustments(target->adjustments, state)) return false;
    target->adjustments = std::move(state);
    ++target->adjustmentRevision;
    touch();
    return true;
}

bool Document::setLayerFilters(LayerId id, SpatialFilterState state)
{
    auto* target = layer(id);
    if (!target || (state && !validSpatialFilters(*state))
        || equivalentSpatialFilters(target->filters, state)) return false;
    target->filters = std::move(state);
    if(!hasActiveSpatialFilters(target->filters))target->filterCache.reset();
    ++target->filterRevision;
    touch();
    return true;
}

bool Document::renameLayer(LayerId id, std::string name)
{
    const auto* target=layer(id);
    return target && setItemMetadata(id,std::move(name),ColorLabel(target->colorLabel));
}

bool Document::setLayerCrop(LayerId id, std::optional<LayerCrop> crop)
{
    auto* target=layer(id);
    if(!target || target->crop==crop || (crop && !validLayerCrop(*crop)))return false;
    target->crop=crop;touch();return true;
}

bool Document::setCanvas(CanvasSpec canvas)
{
    validateCanvasSpec(canvas);
    if (canvas_ == canvas) {
        return false;
    }
    const auto selection = selection_ && canvas.extent != canvas_.extent
        ? selection_->resized(canvas.extent) : selection_;
    auto remembered = lastSelection_ && canvas.extent != canvas_.extent
        ? lastSelection_->resized(canvas.extent) : lastSelection_;
    // Boundary resizing preserves document coordinates, not a stretched mask.
    if (remembered && !lastSelection_->bounds().empty() && remembered->bounds().empty()) remembered.reset();
    canvas_ = canvas;
    setSelection(selection);
    setLastSelection(std::move(remembered));
    touch();
    return true;
}

void Document::setLastSelection(SelectionState selection)
{
    if (selection && selection->extent() != canvas_.extent)
        throw std::invalid_argument("Remembered selection must match the document canvas");
    lastSelection_ = std::move(selection);
}

bool Document::setSelection(SelectionState selection)
{
    if (selection && selection->extent() != canvas_.extent)
        throw std::invalid_argument("Selection must match the document canvas");
    if (selection == selection_ || (selection && selection_ && selection->equivalent(*selection_)))
        return false;
    return setSelection(std::move(selection), {});
}

bool Document::setSelection(SelectionState selection, SelectionEvidenceState evidence)
{
    if (selection && selection->extent() != canvas_.extent)
        throw std::invalid_argument("Selection must match the document canvas");
    const bool sameMask = selection == selection_
        || (selection && selection_ && selection->equivalent(*selection_));
    const bool sameEvidence = evidence == selectionEvidence_
        || (evidence && selectionEvidence_ && evidence->equivalent(*selectionEvidence_));
    if (sameMask && sameEvidence)
        return false;
    selection_ = std::move(selection);
    selectionEvidence_ = std::move(evidence);
    ++selectionRevision_;
    // Selection has an independent revision: it never dirties layer pixels.
    return true;
}

bool Document::setLayerTransform(LayerId id, const AffineTransform& transform)
{
    auto* target = layer(id);
    if (!target || target->localToDocument == transform || !transform.inverted()) return false;
    target->localToDocument = transform;
    touch();
    return true;
}

bool Document::setLayerRasterStorage(LayerId id,std::shared_ptr<RasterSurface> surface,Vec2d origin,std::optional<RectD> frame)
{
    auto* target=layer(id);
    if(!target||!std::holds_alternative<RasterLayer>(target->payload)||!surface||surface->extent().empty()
        ||!std::isfinite(origin.x)||!std::isfinite(origin.y)||std::floor(origin.x)!=origin.x||std::floor(origin.y)!=origin.y)return false;
    target->payload=RasterLayer{std::move(surface)};target->rasterOrigin=origin;target->rasterEffectFrame=frame;touch();return true;
}

bool Document::setLayerTransforms(std::span<const LayerTransformUpdate> updates)
{
    bool changed = false;
    for (std::size_t i = 0; i < updates.size(); ++i) {
        const auto& update = updates[i];
        const auto* target = layer(update.id);
        if (!target || target->localToDocument != update.before || !update.after.inverted())
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (updates[j].id == update.id) return false;
        changed |= update.before != update.after;
    }
    if (!changed) return false;
    // No allocations, callbacks or fallible work after the all-target guard.
    for (const auto& update : updates) layer(update.id)->localToDocument = update.after;
    touch();
    return true;
}

bool Document::setLayerText(LayerId id, TextLayer text)
{
    auto* target = layer(id);
    auto* old = target ? std::get_if<TextLayer>(&target->payload) : nullptr;
    if (!old) return false;
    text = normalizedText(std::move(text));
    if (*old == text) return false;
    *old = std::move(text);
    ++target->textRevision;
    target->renderCache.reset();
    touch();
    return true;
}

bool Document::setLayerShape(LayerId id, ShapeLayer shape)
{
    auto* target = layer(id);
    auto* old = target ? std::get_if<ShapeLayer>(&target->payload) : nullptr;
    if (!old || !validShape(shape) || *old == shape) return false;
    *old = std::move(shape);
    ++target->shapeRevision;
    target->renderCache.reset();
    touch();
    return true;
}

bool Document::setLayerShapeGeometry(LayerId id, ShapeLayer shape, const AffineTransform& transform)
{
    auto* target = layer(id);
    auto* old = target ? std::get_if<ShapeLayer>(&target->payload) : nullptr;
    if (!old || !validShape(shape) || !transform.inverted()
        || (*old == shape && target->localToDocument == transform)) return false;
    // Value admission/copy happens before mutation; moves and affine assignment
    // below cannot allocate. Geometry and anchor publish as one document change.
    if (*old != shape) {
        *old = std::move(shape);
        ++target->shapeRevision;
        target->renderCache.reset();
    }
    target->localToDocument = transform;
    touch();
    return true;
}

DocumentSnapshot Document::snapshot() const
{
    DocumentSnapshot result {
        .documentRevision = revision_,
        .canvas = canvas_,
        .layersBottomToTop = {},
        .selection = selection_,
        .selectionRevision = selectionRevision_,
        .tree = tree_,
    };
    result.layersBottomToTop.reserve(layers_.size());
    for (const auto& source : layers_) {
        LayerSnapshot snapshot {
            .id = source.id,
            .name = source.name,
            // Flat documents retain the original linear snapshot cost. Only
            // potentially visible hierarchical leaves need ancestor lookup.
            .visible = source.visible && (tree_.containers.empty() || isEffectivelyVisible(source.id)),
            .opacity = source.opacity,
            .blendMode = source.blendMode,
            .adjustments = source.adjustments,
            .adjustmentRevision = source.adjustmentRevision,
            .filters = source.filters,
            .filterRevision = source.filterRevision,
            .filterCache = source.filterCache,
            .effects = source.effects,
            .effectRevision = source.effectRevision,
            .effectCache = source.effectCache,
            .localToDocument = source.localToDocument,
            .rasterOrigin = source.rasterOrigin,
            .rasterEffectFrame = source.rasterEffectFrame,
            .crop = source.crop,
            .mask = source.mask,
            .payload = std::visit([](const auto& payload) -> LayerSnapshotPayload {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, RasterLayer>) {
                    return RasterLayerSnapshot {.surface = payload.surface};
                } else {
                    return payload;
                }
            }, source.payload),
            .textRevision = source.textRevision,
            .shapeRevision = source.shapeRevision,
            .renderCache = source.renderCache,
        };
        result.layersBottomToTop.push_back(std::move(snapshot));
    }
    return result;
}

} // namespace imageeditor::core
