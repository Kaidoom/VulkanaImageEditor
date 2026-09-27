#include "imageeditor/core/AdjustmentCommands.hpp"

namespace imageeditor::core {
SetLayerAdjustmentsCommand::SetLayerAdjustmentsCommand(LayerId id, AdjustmentState before, AdjustmentState after)
    : id_(id), before_(std::move(before)), after_(std::move(after)) {}
bool SetLayerAdjustmentsCommand::apply(Document& document)
{
    const auto* layer = document.layer(id_);
    return layer && equivalentAdjustments(layer->adjustments, before_)
        && document.setLayerAdjustments(id_, after_);
}
bool SetLayerAdjustmentsCommand::undo(Document& document)
{
    const auto* layer = document.layer(id_);
    return layer && equivalentAdjustments(layer->adjustments, after_)
        && document.setLayerAdjustments(id_, before_);
}
bool SetLayerAdjustmentsCommand::canAdoptApplied(const Document& document) const noexcept
{
    const auto* layer = document.layer(id_);
    return layer && !equivalentAdjustments(before_, after_)
        && equivalentAdjustments(layer->adjustments, after_);
}
std::size_t SetLayerAdjustmentsCommand::memoryCost() const noexcept
{ return sizeof(*this) + adjustmentMemoryCost(before_, after_); }
AdjustmentEditTransaction::AdjustmentEditTransaction(Document& document, LayerId id)
    : document_(document), id_(id), contentState_(document.contentState())
{
    if (const auto* layer = document.layer(id)) {
        before_ = after_ = layer->adjustments;
        revision_ = layer->adjustmentRevision;
        active_ = true;
    }
}
AdjustmentEditTransaction::~AdjustmentEditTransaction() { (void)cancel(); }
bool AdjustmentEditTransaction::ownsPreview() const noexcept
{
    const auto* layer = document_.layer(id_);
    return active_ && layer && layer->adjustmentRevision == revision_
        && equivalentAdjustments(layer->adjustments, after_);
}
bool AdjustmentEditTransaction::update(AdjustmentState state)
{
    if (!ownsPreview() || document_.contentState() != contentState_) {
        (void)cancel(); return false;
    }
    if (equivalentAdjustments(after_, state)) return false;
    if (!document_.setLayerAdjustments(id_, state)) return false;
    after_ = std::move(state);
    revision_ = document_.layer(id_)->adjustmentRevision;
    return true;
}
bool AdjustmentEditTransaction::commit(History& history)
{
    if (!ownsPreview() || document_.contentState() != contentState_) {
        (void)cancel(); return false;
    }
    if (equivalentAdjustments(before_, after_)) { active_ = false; return false; }
    try {
        std::unique_ptr<Command> command = std::make_unique<SetLayerAdjustmentsCommand>(id_, before_, after_);
        if (!history.adoptApplied(document_, command)) { (void)cancel(); return false; }
    } catch (...) {
        (void)cancel(); // Admission allocation failure must not strand a preview.
        throw;
    }
    active_ = false;
    return true;
}
bool AdjustmentEditTransaction::cancel()
{
    // An unrelated command can invalidate admission without replacing our
    // target's preview. Roll that still-owned metadata back even then, while
    // never overwriting a newer adjustment on the target itself.
    const bool owns = ownsPreview();
    active_ = false;
    return owns && document_.setLayerAdjustments(id_, before_);
}
} // namespace imageeditor::core
