#pragma once
#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/LayerTransform.hpp"
namespace imageeditor::core {
// Same retractable, per-action history model as Ctrl+T; source data is never
// retained by crop commands. Frame coordinates remain canonical layer-local.
class LayerCropSession final {
public:
    LayerCropSession(Document&, LayerId);
    ~LayerCropSession();
    LayerCropSession(const LayerCropSession&) = delete;
    LayerCropSession& operator=(const LayerCropSession&) = delete;
    bool active() const noexcept { return active_; }
    bool targetAvailable() const noexcept;
    LayerId layerId() const noexcept { return id_; }
    RectD frame() const noexcept;
    AffineTransform frameTransform() const noexcept;
    bool dragging() const noexcept { return handle_ != TransformHandle::None; }
    bool setRect(RectD);
    bool removeCrop();
    bool beginDrag(TransformHandle, Vec2d, bool chamfer=false);
    bool dragTo(Vec2d, TransformModifiers, bool aspectLocked);
    void endDrag();
    void cancelDrag();
    bool completeAction();
    bool undo();
    bool redo();
    TransformCommitResult commit(History&);
    void cancel();
    const History& pendingHistory() const noexcept { return pending_; }

private:
    bool set(std::optional<LayerCrop>);
    Document* document_;
    LayerId id_;
    std::size_t kind_ { 0 };
    SurfaceId surface_ { 0 };
    Revision sourceRevision_ { 0 };
    std::uint64_t entryState_, expectedState_;
    AffineTransform layerTransform_, inverse_;
    RectD sourceBounds_;
    std::optional<LayerCrop> original_, current_, actionStart_, dragStart_;
    Vec2d press_;
    RectD dragFrame_;
    bool active_ { false };
    bool chamferDrag_ { false };
    TransformHandle handle_ { TransformHandle::None };
    History pending_ { std::numeric_limits<std::size_t>::max() };
};
} // namespace imageeditor::core
