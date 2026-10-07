#include "imageeditor/core/EditorSession.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace imageeditor::core {
bool EditorSession::editingLayerMask() const noexcept {
    const auto* l=document_&&activeLayer_?document_->layer(*activeLayer_):nullptr;
    return l&&l->mask&&maskTarget_==activeLayer_;
}
void EditorSession::setEditingLayerMask(bool enabled) noexcept {
    maskTarget_=enabled?activeLayer_:std::nullopt;
    if(!editingLayerMask())maskTarget_.reset();
}

void EditorSession::replaceDocument(std::unique_ptr<Document> document)
{
    document_ = std::move(document);
    history_.clear();
    activeLayer_.reset();
    maskTarget_.reset();
    selectedLayers_.clear();
    selectionAnchor_.reset();
    if (document_ && !document_->layers().empty())
        setActiveLayer(document_->canvasTarget(document_->layers().back().id));
}

void EditorSession::setActiveLayer(std::optional<LayerId> layerId)
{
    if (!document_ || (layerId && !document_->containsItem(*layerId))) {
        return;
    }
    const std::array ids { layerId.value_or(0) };
    setLayerSelection(layerId ? std::span<const LayerId>(ids) : std::span<const LayerId> { },
        layerId, layerId);
}

bool EditorSession::isLayerSelected(LayerId layerId) const noexcept
{
    return std::ranges::find(selectedLayers_, layerId) != selectedLayers_.end();
}

std::optional<LayerId> EditorSession::topmostSelectedLayer() const noexcept
{
    if (!document_) return {};
    const auto visit=[&](auto&& self,const std::vector<LayerId>& ids)->std::optional<LayerId> {
        for(auto it=ids.rbegin();it!=ids.rend();++it) {
            if (isLayerSelected(*it)) return *it;
            if(const auto* c=document_->tree().container(*it)) if(auto hit=self(self,c->children)) return hit;
        }
        return {};
    };
    return visit(visit,document_->tree().roots);
}

void EditorSession::setLayerSelection(std::span<const LayerId> layerIds,
    std::optional<LayerId> primary, std::optional<LayerId> anchor)
{
    // Build separately: callers may pass selectedLayers() as the input span.
    std::vector<LayerId> selected;
    selected.reserve(layerIds.size());
    if (document_) {
        for (const auto id : layerIds) {
            if (document_->containsItem(id) && std::ranges::find(selected, id) == selected.end())
                selected.push_back(id);
        }
    }
    if(activeLayer_!=primary)maskTarget_.reset();
    selectedLayers_ = std::move(selected);
    activeLayer_ = primary && isLayerSelected(*primary) ? primary : topmostSelectedLayer();
    selectionAnchor_ = anchor && document_ && document_->containsItem(*anchor)
        ? anchor
        : activeLayer_;
}

void EditorSession::toggleSelectedLayer(LayerId layerId)
{
    maskTarget_.reset();
    if (!document_ || !document_->containsItem(layerId))
        return;
    if (isLayerSelected(layerId)) {
        std::erase(selectedLayers_, layerId);
        if (activeLayer_ == layerId)
            activeLayer_ = topmostSelectedLayer();
    } else {
        selectedLayers_.push_back(layerId);
        activeLayer_ = layerId;
    }
    selectionAnchor_ = layerId;
}

void EditorSession::selectLayerRange(LayerId layerId,
    std::span<const LayerId> displayedOrder, bool additive)
{
    if (!document_ || !document_->containsItem(layerId))
        return;
    const auto end = std::ranges::find(displayedOrder, layerId);
    if (end == displayedOrder.end())
        return;
    const auto anchor = selectionAnchor_.value_or(activeLayer_.value_or(layerId));
    auto start = std::ranges::find(displayedOrder, anchor);
    if (start == displayedOrder.end())
        start = end;
    const auto first = std::min(start, end);
    const auto last = std::max(start, end) + 1;
    auto selected = additive ? selectedLayers_ : std::vector<LayerId> { };
    selected.insert(selected.end(), first, last);
    setLayerSelection(selected, layerId, *start);
}

void EditorSession::applyActiveLayerHint()
{
    if (const auto* hint=history_.layerSelectionHint()) {
        if(activeLayer_!=hint->primary)maskTarget_.reset();
        selectedLayers_.assign(hint->ids.begin(),hint->ids.end()); // Reserved before history mutation.
        activeLayer_=hint->primary;
        selectionAnchor_=hint->anchor;
        return;
    }
    if (const auto hint = history_.activeLayerHint(); hint && document_->containsItem(*hint)) {
        // Editing one primary layer must not collapse an existing group. New
        // layer/copy commands still select their newly introduced target alone.
        if (isLayerSelected(*hint)) {
            if(activeLayer_!=hint)maskTarget_.reset();
            activeLayer_ = hint;
        } else
            setActiveLayer(hint);
    }
}

bool EditorSession::execute(std::unique_ptr<Command> command)
{
    selectedLayers_.reserve(LayerTree::maxItems);
    if (!document_ || !history_.execute(*document_, std::move(command))) {
        normalizeActiveLayer();
        return false;
    }
    applyActiveLayerHint();
    if (auto hint=history_.maskEditingHint()) setEditingLayerMask(*hint);
    normalizeActiveLayer();
    return true;
}

bool EditorSession::adoptApplied(std::unique_ptr<Command> command)
{
    selectedLayers_.reserve(LayerTree::maxItems);
    if (!document_ || !history_.adoptApplied(*document_, command)) {
        normalizeActiveLayer();
        return false;
    }
    applyActiveLayerHint();
    if (auto hint=history_.maskEditingHint()) setEditingLayerMask(*hint);
    normalizeActiveLayer();
    return true;
}

bool EditorSession::undo()
{
    selectedLayers_.reserve(LayerTree::maxItems);
    if (!document_ || !history_.undo(*document_)) {
        normalizeActiveLayer();
        return false;
    }
    applyActiveLayerHint();
    if (auto hint=history_.maskEditingHint()) setEditingLayerMask(*hint);
    normalizeActiveLayer();
    return true;
}

bool EditorSession::redo()
{
    selectedLayers_.reserve(LayerTree::maxItems);
    if (!document_ || !history_.redo(*document_)) {
        normalizeActiveLayer();
        return false;
    }
    applyActiveLayerHint();
    if (auto hint=history_.maskEditingHint()) setEditingLayerMask(*hint);
    normalizeActiveLayer();
    return true;
}

void EditorSession::normalizeActiveLayer()
{
    if (!document_) {
        activeLayer_.reset();
        selectedLayers_.clear();
        selectionAnchor_.reset();
        return;
    }
    const bool hadSelection = !selectedLayers_.empty();
    std::erase_if(selectedLayers_, [this](LayerId id) { return !document_->containsItem(id); });
    if (!activeLayer_ || !isLayerSelected(*activeLayer_))
        activeLayer_ = topmostSelectedLayer();
    if (hadSelection && selectedLayers_.empty() && !document_->layers().empty()) {
        // Preserve the existing deletion fallback, but do not repopulate an
        // intentionally empty selection following unrelated history changes.
        selectedLayers_.push_back(document_->canvasTarget(document_->layers().back().id));
        activeLayer_=selectedLayers_.back();
    }
    if (selectionAnchor_ && !document_->containsItem(*selectionAnchor_))
        selectionAnchor_ = activeLayer_;
}

} // namespace imageeditor::core
