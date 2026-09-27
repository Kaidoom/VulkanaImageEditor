#pragma once
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/CloneReference.hpp"
#include "imageeditor/core/Healing.hpp"

namespace imageeditor::core {
// Document-space anchor/offset, independent of target identity and brush-tip
// rotation. Only successful completed strokes publish a newly aligned offset.
struct CloneAnchor {
    LayerId layer { 0 };
    Vec2d point;
    AffineTransform sourceTransform;
    std::optional<Vec2d> alignedOffset;
    [[nodiscard]] Vec2d offsetFor(Vec2d destination, bool aligned) const noexcept
    {
        return aligned && alignedOffset ? *alignedOffset : point - destination;
    }
    void completed(Vec2d offset, bool aligned) noexcept
    {
        if (aligned)
            alignedOffset = offset;
    }
};

class CloneStroke final {
public:
    CloneStroke(Document&, LayerId, BrushSettings, CloneSettings, CloneReference, Vec2d offset,
        const IBrushAssetResolver* = nullptr);
    bool begin(const NormalizedPointerSample&);
    bool append(const NormalizedPointerSample&);
    RasterEditCommitResult end(
        const NormalizedPointerSample&, History&, const std::function<bool()>& cancelled = { });
    void cancel() noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const BrushStrokeStats& stats() const noexcept { return brush_.stats(); }
    [[nodiscard]] const HealingResult& healingResult() const noexcept { return healing_; }
    [[nodiscard]] std::size_t snapshotBytes() const noexcept { return reference_.snapshotBytes(); }
    [[nodiscard]] Vec2d offset() const noexcept { return offset_; }
    [[nodiscard]] const BasicPixelBrushStroke& brush() const noexcept { return brush_; }

private:
    bool targetMatches() const noexcept;
    void checkpoint() noexcept;
    Document& document_;
    LayerId target_;
    CloneSettings settings_;
    CloneReference reference_;
    Vec2d offset_;
    AffineTransform localToDocument_, documentToLocal_;
    Extent2u extent_;
    SurfaceId surface_ { 0 };
    Revision revision_ { 0 }, surfaceRevision_ { 0 };
    BasicPixelBrushStroke brush_;
    HealingResult healing_;
    std::string diagnostic_;
};
}
