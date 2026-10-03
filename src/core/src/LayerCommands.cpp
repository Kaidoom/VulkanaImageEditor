#include "imageeditor/core/LayerCommands.hpp"

#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/RichText.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace imageeditor::core {
namespace {

std::size_t saturatingAdd(std::size_t left, std::size_t right) noexcept
{
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return right > maximum - left ? maximum : left + right;
}

std::size_t retainedLayerCost(const Layer& layer) noexcept
{
    auto result = saturatingAdd(sizeof(Layer), layer.name.size());
    result = saturatingAdd(result, adjustmentMemoryCost(layer.adjustments));
    result = saturatingAdd(result, spatialFilterMemoryCost(layer.filters));
    if(layer.effects)result=saturatingAdd(result,sizeof(LayerEffectStack));
    std::visit([&result](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, RasterLayer>) {
            if (!payload.surface) {
                return;
            }
            const auto extent = payload.surface->extent();
            const auto pixels = static_cast<std::size_t>(extent.width)
                * static_cast<std::size_t>(extent.height);
            const auto maximum = std::numeric_limits<std::size_t>::max();
            const auto bytes = pixels > maximum / 4U ? maximum : pixels * 4U;
            result = saturatingAdd(result, bytes);
        } else if constexpr (std::is_same_v<Payload, TextLayer>) {
            result = saturatingAdd(result, textMemoryCost(payload));
        } else {
            result = saturatingAdd(result, shapeMemoryCost(payload));
        }
    }, layer.payload);
    return result;
}

} // namespace

SetItemsVisibilityCommand::SetItemsVisibilityCommand(const Document& document,
    std::span<const LayerId> selection, VisibilityOperation operation)
    : operation_(operation)
{
    for (auto id : selection)
        if (!document.containsItem(id)) throw std::invalid_argument("Visibility target disappeared");
    const auto roots = document.tree().normalize(selection);
    const auto add = [&](LayerId id, bool after) {
        const bool before = *document.itemVisibility(id);
        if (before != after) {
            forward_.push_back({ id, before, after });
            reverse_.push_back({ id, after, before });
        }
    };
    if (operation == VisibilityOperation::Hide || operation == VisibilityOperation::Show) {
        for (auto id : roots) add(id, operation == VisibilityOperation::Show);
    } else if (operation == VisibilityOperation::ShowAll) {
        for (const auto& leaf : document.layers()) add(leaf.id, true);
        for (const auto& container : document.tree().containers) add(container.id, true);
    } else if (!roots.empty()) {
        const auto visit = [&](auto&& self, LayerId id) -> void {
            if (std::ranges::find(roots, id) != roots.end()) {
                add(id, true); // Selected container is authoritative; retain children's own choices.
                return;
            }
            const bool ancestor = std::ranges::any_of(roots, [&](auto target) { return document.tree().isAncestor(id, target); });
            add(id, ancestor);
            if (ancestor)
                for (auto child : *document.tree().children(id)) self(self, child);
        };
        for (auto id : document.tree().roots) visit(visit, id);
    }
}
bool SetItemsVisibilityCommand::apply(Document& document) { return document.setItemVisibilities(forward_); }
bool SetItemsVisibilityCommand::undo(Document& document) { return document.setItemVisibilities(reverse_); }
std::string_view SetItemsVisibilityCommand::label() const noexcept
{
    switch (operation_) {
    case VisibilityOperation::Hide: return "Hide selected layers";
    case VisibilityOperation::Show: return "Show selected layers";
    case VisibilityOperation::Isolate: return "Isolate selected layers";
    case VisibilityOperation::ShowAll: return "Show all layers";
    }
    return "Layer visibility";
}

SetLayerVisibilityCommand::SetLayerVisibilityCommand(LayerId layerId, bool visible)
    : layerId_(layerId)
    , after_(visible)
{
}

bool SetLayerVisibilityCommand::apply(Document& document)
{
    const auto target = document.itemVisibility(layerId_);
    if (!target) {
        return false;
    }
    if (!before_) {
        before_ = *target;
    }
    return document.setLayerVisibility(layerId_, after_);
}

bool SetLayerVisibilityCommand::undo(Document& document)
{
    return before_.has_value() && document.setLayerVisibility(layerId_, *before_);
}

SetLayerOpacityCommand::SetLayerOpacityCommand(
    LayerId layerId, float opacity, std::uint64_t mergeKey)
    : layerId_(layerId)
    , after_(std::clamp(opacity, 0.0F, 1.0F))
    , mergeKey_(mergeKey)
{
}

bool SetLayerOpacityCommand::apply(Document& document)
{
    const auto* target = document.layer(layerId_);
    if (!target) {
        return false;
    }
    if (!before_) {
        before_ = target->opacity;
    }
    return document.setLayerOpacity(layerId_, after_);
}

bool SetLayerOpacityCommand::undo(Document& document)
{
    return before_.has_value() && document.setLayerOpacity(layerId_, *before_);
}

bool SetLayerOpacityCommand::mergeWith(const Command& other)
{
    const auto* opacityCommand = dynamic_cast<const SetLayerOpacityCommand*>(&other);
    if (!opacityCommand || mergeKey_ == 0 || opacityCommand->mergeKey_ != mergeKey_
        || opacityCommand->layerId_ != layerId_ || opacityCommand->after_ == before_) {
        return false;
    }
    after_ = opacityCommand->after_;
    return true;
}

SetLayerBlendModeCommand::SetLayerBlendModeCommand(LayerId layerId, BlendMode mode)
    : layerId_(layerId), after_(mode) {}

bool SetLayerBlendModeCommand::apply(Document& document)
{
    const auto* target = document.layer(layerId_);
    if (!target || !isValidBlendMode(after_)) return false;
    if (!before_) before_ = target->blendMode;
    return document.setLayerBlendMode(layerId_, after_);
}

bool SetLayerBlendModeCommand::undo(Document& document)
{ return before_ && document.setLayerBlendMode(layerId_, *before_); }

AddLayerCommand::AddLayerCommand(Layer layer, std::size_t index, std::optional<LayerId> previousActive)
    : layer_(std::move(layer))
    , index_(index)
    , previousActive_(previousActive)
{
    if (!std::holds_alternative<RasterLayer>(layer_.payload)) layer_.renderCache.reset();
    layer_.filterCache.reset(); // Derived images are not retained by history.
    layer_.effectCache.reset();
}

bool AddLayerCommand::apply(Document& document)
{
    if (!placement_) {
        placement_=index_<document.layers().size()?*document.tree().placement(document.layers()[index_].id)
            :ItemPlacement{0,document.tree().roots.size()};
        if(previousActive_) {
            const auto next=document.tree().insertionAbove(*previousActive_);
            if(const auto* c=document.tree().container(next.parent);c&&c->kind==ContainerKind::ClippingMaskGroup)placement_=next;
        }
    }
    return document.insertLayerAt(*placement_, layer_);
}

bool AddLayerCommand::undo(Document& document)
{
    return document.takeLayer(layer_.id).has_value();
}

std::size_t AddLayerCommand::memoryCost() const noexcept
{
    return saturatingAdd(sizeof(*this), retainedLayerCost(layer_));
}

RemoveLayerCommand::RemoveLayerCommand(LayerId layerId)
    : layerId_(layerId)
{
}

bool RemoveLayerCommand::apply(Document& document)
{
    // An editor document always keeps a drawable layer. AddLayerCommand's
    // internal undo path still uses Document::takeLayer directly, so this
    // product invariant applies specifically to user-facing removal.
    if (document.layers().size() <= 1) {
        return false;
    }
    auto removed = document.takeLayer(layerId_);
    if (!removed) {
        return false;
    }
    if (!removedLayer_) {
        removedLayer_ = std::move(removed->layer);
        if (!std::holds_alternative<RasterLayer>(removedLayer_->payload)) removedLayer_->renderCache.reset();
        removedLayer_->filterCache.reset();
        removedLayer_->effectCache.reset();
        index_ = removed->index;
        placement_ = removed->placement;
    }
    return true;
}

bool RemoveLayerCommand::undo(Document& document)
{
    return removedLayer_.has_value() && placement_ && document.insertLayerAt(*placement_, *removedLayer_);
}

std::size_t RemoveLayerCommand::memoryCost() const noexcept
{
    return removedLayer_
        ? saturatingAdd(sizeof(*this), retainedLayerCost(*removedLayer_))
        : sizeof(*this);
}

MoveLayerCommand::MoveLayerCommand(LayerId layerId, std::size_t destinationIndex)
    : layerId_(layerId)
    , afterIndex_(destinationIndex)
{
}

bool MoveLayerCommand::apply(Document& document)
{
    if (beforeTree_ && afterTree_) return document.replaceStructure(*beforeTree_,*afterTree_);
    const auto& layers = document.layers();
    const auto found = std::find_if(layers.begin(), layers.end(),
        [this](const Layer& candidate) { return candidate.id == layerId_; });
    if (found == layers.end()) {
        return false;
    }
    if (!beforeIndex_) {
        beforeIndex_ = static_cast<std::size_t>(std::distance(layers.begin(), found));
    }
    auto before=document.tree();
    auto after=before;
    const auto destination=std::min(afterIndex_,layers.size()-1);
    auto p=*before.placement(layers[destination].id);
    if (destination>*beforeIndex_) ++p.index;
    if (!after.reparent(std::span<const LayerId>(&layerId_,1),p)) return false;
    beforeTree_=std::move(before); afterTree_=std::move(after);
    return document.replaceStructure(*beforeTree_,*afterTree_);
}

bool MoveLayerCommand::undo(Document& document)
{
    return beforeTree_ && afterTree_ && document.replaceStructure(*afterTree_,*beforeTree_);
}

} // namespace imageeditor::core
