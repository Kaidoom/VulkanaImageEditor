#include "imageeditor/core/DocumentCommands.hpp"
#include <stdexcept>

namespace imageeditor::core {

ChangeCanvasSpecCommand::ChangeCanvasSpecCommand(CanvasSpec after)
    : after_(after)
{
}

bool ChangeCanvasSpecCommand::apply(Document& document)
{
    if (!before_) {
        before_ = document.canvas();
        beforeSelection_ = document.selection();
        beforeRemembered_ = document.lastSelection();
        if (beforeSelection_) (void)beforeSelection_->boundaryEdges();
        if (beforeRemembered_) (void)beforeRemembered_->boundaryEdges();
    }
    return document.setCanvas(after_);
}

bool ChangeCanvasSpecCommand::undo(Document& document)
{
    if (!before_ || !document.setCanvas(*before_)) return false;
    document.setSelection(beforeSelection_);
    document.setLastSelection(beforeRemembered_);
    return true;
}

bool ReframeCanvasCommand::apply(Document& document)
{
    if (!before_) {
        if (frame_.empty() || std::abs(double(frame_.x)) > 1e9 || std::abs(double(frame_.y)) > 1e9)
            throw std::invalid_argument("Invalid canvas frame");
        after_ = document.canvas();
        after_.extent = {std::uint32_t(frame_.width), std::uint32_t(frame_.height)};
        if (after_ == document.canvas() && frame_.x == 0 && frame_.y == 0) return false;
        const AffineTransform shift{1, 0, -double(frame_.x), 0, 1, -double(frame_.y)};
        forward_.clear(); reverse_.clear();
        for (const auto& layer : document.layers()) {
            const auto moved = composeTransform(shift, layer.localToDocument);
            forward_.push_back({layer.id, layer.localToDocument, moved});
            reverse_.push_back({layer.id, moved, layer.localToDocument});
        }
        beforeSelection_ = document.selection();
        beforeRemembered_ = document.lastSelection();
        beforeEvidence_ = document.selectionEvidence();
        afterSelection_ = beforeSelection_ ? beforeSelection_->reframed(after_.extent, -frame_.x, -frame_.y) : nullptr;
        afterRemembered_ = beforeRemembered_ ? beforeRemembered_->reframed(after_.extent, -frame_.x, -frame_.y) : nullptr;
        if (afterRemembered_ && !beforeRemembered_->bounds().empty() && afterRemembered_->bounds().empty()) afterRemembered_.reset();
        before_ = document.canvas();
    }
    return document.setCanvasFrame(*before_, after_, forward_, afterSelection_, afterRemembered_);
}
bool ReframeCanvasCommand::undo(Document& document)
{
    return before_ && document.setCanvasFrame(after_, *before_, reverse_, beforeSelection_, beforeRemembered_, beforeEvidence_);
}
std::size_t ReframeCanvasCommand::memoryCost() const noexcept
{
    std::size_t bytes = sizeof(*this) + (forward_.capacity()+reverse_.capacity())*sizeof(LayerTransformUpdate);
    for (const auto& mask : {beforeSelection_, afterSelection_, beforeRemembered_, afterRemembered_})
        if (mask) bytes += mask->memoryCost();
    if (beforeEvidence_) bytes += beforeEvidence_->memoryCost();
    return bytes;
}

} // namespace imageeditor::core
