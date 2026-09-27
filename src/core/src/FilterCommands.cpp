#include "imageeditor/core/FilterCommands.hpp"

namespace imageeditor::core {
SetLayerFiltersCommand::SetLayerFiltersCommand(LayerId id, SpatialFilterState before, SpatialFilterState after)
    : id_(id), before_(std::move(before)), after_(std::move(after)) {}
bool SetLayerFiltersCommand::apply(Document& document)
{
    const auto* layer = document.layer(id_);
    return layer && equivalentSpatialFilters(layer->filters, before_)
        && document.setLayerFilters(id_, after_);
}
bool SetLayerFiltersCommand::undo(Document& document)
{
    const auto* layer = document.layer(id_);
    return layer && equivalentSpatialFilters(layer->filters, after_)
        && document.setLayerFilters(id_, before_);
}
bool SetLayerFiltersCommand::canAdoptApplied(const Document& document) const noexcept
{
    const auto* layer = document.layer(id_);
    return layer && !equivalentSpatialFilters(before_, after_)
        && equivalentSpatialFilters(layer->filters, after_);
}
std::size_t SetLayerFiltersCommand::memoryCost() const noexcept
{ return sizeof(*this) + spatialFilterMemoryCost(before_, after_); }
FilterEditTransaction::FilterEditTransaction(Document& document, LayerId id)
    : document_(document), id_(id), contentState_(document.contentState())
{
    if (const auto* layer = document.layer(id)) {
        before_ = after_ = layer->filters;
        revision_ = layer->filterRevision;
        active_ = true;
    }
}
FilterEditTransaction::~FilterEditTransaction() { (void)cancel(); }
bool FilterEditTransaction::ownsPreview() const noexcept
{
    const auto* layer = document_.layer(id_);
    return active_ && layer && layer->filterRevision == revision_
        && equivalentSpatialFilters(layer->filters, after_);
}
bool FilterEditTransaction::update(SpatialFilterState state)
{
    if (!ownsPreview() || document_.contentState() != contentState_) {
        (void)cancel(); return false;
    }
    if (equivalentSpatialFilters(after_, state)) return false;
    if (!document_.setLayerFilters(id_, state)) return false;
    after_ = std::move(state);
    revision_ = document_.layer(id_)->filterRevision;
    return true;
}
bool FilterEditTransaction::commit(History& history)
{
    if (!ownsPreview() || document_.contentState() != contentState_) {
        (void)cancel(); return false;
    }
    if (equivalentSpatialFilters(before_, after_)) { active_ = false; return false; }
    try {
        std::unique_ptr<Command> command = std::make_unique<SetLayerFiltersCommand>(id_, before_, after_);
        if (!history.adoptApplied(document_, command)) { (void)cancel(); return false; }
    } catch (...) {
        (void)cancel(); // Admission allocation failure must not strand a preview.
        throw;
    }
    active_ = false;
    return true;
}
bool FilterEditTransaction::cancel()
{
    // An unrelated command can invalidate admission without replacing our
    // target's preview. Roll that still-owned metadata back even then, while
    // never overwriting a newer filter on the target itself.
    const bool owns = ownsPreview();
    active_ = false;
    return owns && document_.setLayerFilters(id_, before_);
}
} // namespace imageeditor::core
