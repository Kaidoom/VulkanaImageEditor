#pragma once
#include "imageeditor/core/LayerTransform.hpp"

namespace imageeditor::core {
// A view-only, pixel-aligned frame. The document is untouched until Apply;
// local undo/redo never consumes or clears the document's history branch.
class CanvasResizeFrame {
public:
    CanvasResizeFrame(Extent2u extent, int limit, std::uint64_t pixelLimit = UINT64_MAX)
        : original_ { 0, 0, int(extent.width), int(extent.height) }
        , current_(original_)
        , action_(original_)
        , limit_(limit)
        , pixelLimit_(pixelLimit)
    {
    }
    RectI frame() const { return current_; }
    bool dragging() const { return handle_ != TransformHandle::None; }
    bool canUndo() const { return dragging() || current_ != action_ || !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool set(RectD r)
    {
        for (double v : { r.x, r.y, r.width, r.height, r.right(), r.bottom() })
            if (!std::isfinite(v) || std::abs(v) > 1e9)
                return false;
        if (r.width <= 0 || r.height <= 0)
            return false;
        const int x = int(std::round(r.x)), y = int(std::round(r.y));
        const int w = std::max(1, int(std::round(r.right())) - x),
                  h = std::max(1, int(std::round(r.bottom())) - y);
        if (w > limit_ || h > limit_ || std::uint64_t(w) * std::uint64_t(h) > pixelLimit_)
            return false;
        current_ = { x, y, w, h };
        return true;
    }
    void complete()
    {
        if (current_ == action_)
            return;
        undo_.push_back(action_);
        redo_.clear();
        action_ = current_;
    }
    void reset()
    {
        complete();
        current_ = original_;
        complete();
    }
    bool begin(TransformHandle handle, Vec2d p)
    {
        if (handle < TransformHandle::TopLeft || handle > TransformHandle::Move || !std::isfinite(p.x)
            || !std::isfinite(p.y))
            return false;
        complete();
        start_ = current_;
        press_ = p;
        handle_ = handle;
        return true;
    }
    bool drag(Vec2d p, TransformModifiers mods, bool locked)
    {
        if (!dragging() || !std::isfinite(p.x) || !std::isfinite(p.y))
            return false;
        if (handle_ == TransformHandle::Move)
            return set({ start_.x + p.x - press_.x, start_.y + p.y - press_.y, double(start_.width),
                double(start_.height) });
        auto box = resolveResizeGeometry({ double(start_.width), double(start_.height) },
            AffineTransform { 1, 0, double(start_.x), 0, 1, double(start_.y) }, handle_, press_, p, mods,
            locked);
        if (!box)
            return false;
        const auto size = box->signedExtent;
        const Vec2d anchor { start_.x + box->unitAnchor.x * start_.width,
            start_.y + box->unitAnchor.y * start_.height };
        const Vec2d a { anchor.x - box->unitAnchor.x * size.width,
            anchor.y - box->unitAnchor.y * size.height };
        return set({ std::min(a.x, a.x + size.width), std::min(a.y, a.y + size.height),
            std::max(1., std::abs(size.width)), std::max(1., std::abs(size.height)) });
    }
    void end()
    {
        if (dragging()) {
            handle_ = TransformHandle::None;
            complete();
        }
    }
    void cancelDrag()
    {
        if (dragging()) {
            current_ = start_;
            handle_ = TransformHandle::None;
        }
    }
    void step(bool redo)
    {
        if (dragging()) {
            cancelDrag();
            return;
        }
        complete();
        auto& from = redo ? redo_ : undo_;
        auto& to = redo ? undo_ : redo_;
        if (from.empty())
            return;
        to.push_back(current_);
        current_ = action_ = from.back();
        from.pop_back();
    }

private:
    RectI original_, current_, action_, start_;
    Vec2d press_;
    TransformHandle handle_ { TransformHandle::None };
    int limit_;
    std::uint64_t pixelLimit_;
    std::vector<RectI> undo_, redo_;
};
} // namespace imageeditor::core
