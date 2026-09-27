#include "imageeditor/core/LayerCropSession.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
namespace imageeditor::core {
namespace {
    class CropCommand final : public Command {
    public:
        CropCommand(LayerId id, std::optional<LayerCrop> before,
            std::optional<LayerCrop> after)
            : id_(id)
            , before_(before)
            , after_(after)
        {
        }
        bool apply(Document& d) override
        {
            return d.layer(id_) && d.layer(id_)->crop == before_ && d.setLayerCrop(id_, after_);
        }
        bool undo(Document& d) override
        {
            return d.layer(id_) && d.layer(id_)->crop == after_ && d.setLayerCrop(id_, before_);
        }
        bool canAdoptApplied(const Document& d) const noexcept override
        {
            return d.layer(id_) && d.layer(id_)->crop == after_;
        }
        std::string_view label() const noexcept override
        {
            return after_ ? "Layer crop" : "Remove layer crop";
        }
        std::size_t memoryCost() const noexcept override { return sizeof(*this); }

    private:
        LayerId id_;
        std::optional<LayerCrop> before_, after_;
    };
    Revision contentRevision(const Layer& l)
    {
        if (const auto* r = std::get_if<RasterLayer>(&l.payload))
            return r->surface ? r->surface->revision() : 0;
        return std::holds_alternative<TextLayer>(l.payload) ? l.textRevision
                                                            : l.shapeRevision;
    }
    bool near(std::optional<LayerCrop> a, std::optional<LayerCrop> b)
    {
        if (a.has_value() != b.has_value())
            return false;
        if (a)
            for (std::size_t i = 0; i < 4; ++i)
                if (std::abs(a->corners[i] - b->corners[i]) >= 1e-8)
                    return false;
        return !a || (std::abs(a->x - b->x) < 1e-8 && std::abs(a->y - b->y) < 1e-8 && std::abs(a->width - b->width) < 1e-8 && std::abs(a->height - b->height) < 1e-8);
    }
} // namespace
LayerCropSession::LayerCropSession(Document& d, LayerId id)
    : document_(&d)
    , id_(id)
    , entryState_(d.contentState())
    , expectedState_(entryState_)
{
    const auto* layer = d.layer(id);
    if (!layer)
        return;
    const auto inverse = layer->localToDocument.inverted();
    sourceBounds_ = layerSourceBounds(*layer);
    if (!inverse || sourceBounds_.empty() || !validLayerCrop(sourceBounds_) || !layer->localToDocument.validOver(sourceBounds_))
        return;
    // Match the supported transform range; pathological near-singular matrices
    // must not turn a one-pixel pointer motion into unbounded local coordinates.
    if (std::max({ std::abs(inverse->m00), std::abs(inverse->m01),
            std::abs(inverse->m10), std::abs(inverse->m11) })
        > 1e6)
        return;
    kind_ = layer->payload.index();
    sourceRevision_ = contentRevision(*layer);
    if (const auto* raster = std::get_if<RasterLayer>(&layer->payload))
        surface_ = raster->surface->id();
    layerTransform_ = layer->localToDocument;
    inverse_ = *inverse;
    original_ = current_ = actionStart_ = layer->crop;
    active_ = true;
}
LayerCropSession::~LayerCropSession() { cancel(); }
bool LayerCropSession::targetAvailable() const noexcept
{
    const auto* layer = document_->layer(id_);
    if (!active_ || !layer || layer->payload.index() != kind_ || layer->crop != current_ || layer->localToDocument != layerTransform_ || contentRevision(*layer) != sourceRevision_)
        return false;
    return !surface_ || std::get<RasterLayer>(layer->payload).surface->id() == surface_;
}
RectD LayerCropSession::frame() const noexcept
{
    auto r = current_.value_or(sourceBounds_);
    // Empty crops are legitimate saved data. Keep their frame reachable and
    // widenable without changing the stored empty rectangle on tool entry.
    r.width = std::max(r.width, 1e-4);
    r.height = std::max(r.height, 1e-4);
    return r;
}
AffineTransform LayerCropSession::frameTransform() const noexcept
{
    const auto r = frame();
    return composeAffine(layerTransform_, { 1, 0, r.x, 0, 1, r.y });
}
bool LayerCropSession::set(std::optional<LayerCrop> r)
{
    if (!targetAvailable() || document_->contentState() != expectedState_ || (r && (!validLayerCrop(*r) || !layerTransform_.validOver(*r))))
        return false;
    if (current_ != r) {
        if (!document_->setLayerCrop(id_, r))
            return false;
        current_ = r;
    }
    return true;
}
bool LayerCropSession::setRect(RectD r)
{
    auto crop = current_.value_or(LayerCrop { r });
    static_cast<RectD&>(crop) = r;
    return set(crop);
}
bool LayerCropSession::removeCrop()
{
    (void)completeAction();
    return set({ }) && completeAction();
}
bool LayerCropSession::beginDrag(TransformHandle handle, Vec2d point, bool chamfer)
{
    if (handle < TransformHandle::TopLeft || handle > TransformHandle::Move || !std::isfinite(point.x) || !std::isfinite(point.y) || !targetAvailable())
        return false;
    (void)completeAction();
    dragStart_ = current_;
    dragFrame_ = frame();
    press_ = point;
    handle_ = handle;
    chamferDrag_ = chamfer && handle <= TransformHandle::Left && int(handle) % 2 == 0;
    return true;
}
bool LayerCropSession::dragTo(Vec2d point, TransformModifiers modifiers,
    bool locked)
{
    if (!dragging() || !std::isfinite(point.x) || !std::isfinite(point.y))
        return false;
    auto r = dragFrame_;
    if (chamferDrag_) {
        auto crop = dragStart_.value_or(LayerCrop { dragFrame_ });
        const auto index = std::size_t(handle_) / 2;
        const auto start = crop.resolvedCorners();
        const auto delta = inverse_.map(point) - inverse_.map(press_);
        const auto sx = (index == 0 || index == 3) ? 1.0 : -1.0, sy = index < 2 ? 1.0 : -1.0;
        const auto amount = sx * delta.x + sy * delta.y;
        const auto limit = std::min(r.width, r.height) * .5;
        const auto update = [&](std::size_t i) {const auto value=std::clamp(start[i]+amount,0.0,limit);
            if(std::abs(value-start[i])>=1e-8)crop.corners[i]=value; };
        update(index);
        if (modifiers.alt)
            update((index + 2) % 4);
        if (!dragStart_ && !crop.hasChamfer())
            return set({ });
        return set(crop);
    }
    if (handle_ == TransformHandle::Move) {
        const auto delta = inverse_.map(point) - inverse_.map(press_);
        r.x += delta.x;
        r.y += delta.y;
    } else {
        const auto transform = composeAffine(layerTransform_, { 1, 0, r.x, 0, 1, r.y });
        const auto box = resolveResizeGeometry({ r.width, r.height }, transform, handle_, press_,
            point, modifiers, locked);
        if (!box)
            return false;
        auto size = box->signedExtent;
        size.width = std::copysign(std::max(std::abs(size.width), 1e-4), size.width);
        size.height = std::copysign(std::max(std::abs(size.height), 1e-4), size.height);
        const Vec2d anchor { r.x + box->unitAnchor.x * r.width,
            r.y + box->unitAnchor.y * r.height };
        const Vec2d a { anchor.x - box->unitAnchor.x * size.width,
            anchor.y - box->unitAnchor.y * size.height };
        r = { std::min(a.x, a.x + size.width), std::min(a.y, a.y + size.height),
            std::abs(size.width), std::abs(size.height) };
    }
    // A click-only gesture over an uncropped frame must not add a restriction.
    if (!dragStart_ && near(r, dragFrame_))
        return set({ });
    auto crop = dragStart_.value_or(LayerCrop { r });
    static_cast<RectD&>(crop) = r;
    return set(crop);
}
void LayerCropSession::cancelDrag()
{
    if (dragging())
        (void)set(dragStart_);
    handle_ = TransformHandle::None;
}
void LayerCropSession::endDrag()
{
    handle_ = TransformHandle::None;
    (void)completeAction();
}
bool LayerCropSession::completeAction()
{
    if (!targetAvailable() || document_->contentState() != expectedState_)
        return false;
    if (near(current_, actionStart_)) {
        (void)set(actionStart_);
        return false;
    }
    std::unique_ptr<Command> command = std::make_unique<CropCommand>(id_, actionStart_, current_);
    if (!pending_.adoptApplied(*document_, command))
        return false;
    actionStart_ = current_;
    expectedState_ = document_->contentState();
    return true;
}
bool LayerCropSession::undo()
{
    if (!targetAvailable() || document_->contentState() != expectedState_)
        return false;
    cancelDrag();
    (void)completeAction();
    if (!pending_.undo(*document_))
        return false;
    current_ = actionStart_ = document_->layer(id_)->crop;
    expectedState_ = document_->contentState();
    return true;
}
bool LayerCropSession::redo()
{
    if (!targetAvailable() || document_->contentState() != expectedState_)
        return false;
    cancelDrag();
    (void)completeAction();
    if (!pending_.redo(*document_))
        return false;
    current_ = actionStart_ = document_->layer(id_)->crop;
    expectedState_ = document_->contentState();
    return true;
}
TransformCommitResult LayerCropSession::commit(History& history)
{
    if (!targetAvailable() || document_->contentState() != expectedState_) {
        cancel();
        return TransformCommitResult::TargetUnavailable;
    }
    endDrag();
    if (!pending_.canUndo()) {
        active_ = false;
        return TransformCommitResult::NoChange;
    }
    if (!history.publishAppliedBranch(*document_, pending_))
        return TransformCommitResult::TargetUnavailable;
    active_ = false;
    return TransformCommitResult::Committed;
}
void LayerCropSession::cancel()
{
    const bool available = targetAvailable();
    const auto* layer = document_->layer(id_);
    // Losing an unrelated source/transform revision must not strand our owned
    // crop preview. Never resurrect missing targets or overwrite a newer crop.
    if (active_ && layer && layer->payload.index() == kind_ && layer->crop == current_) {
        if (current_ != original_)
            (void)document_->setLayerCrop(id_, original_);
        if (available && document_->contentState() == expectedState_)
            document_->restoreContentState(entryState_);
    }
    active_ = false;
    handle_ = TransformHandle::None;
    pending_.clear();
}
} // namespace imageeditor::core
