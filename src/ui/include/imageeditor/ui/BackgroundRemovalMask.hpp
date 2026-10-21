#pragma once

#include "imageeditor/core/Layer.hpp"

#include <QImage>
#include <functional>

namespace imageeditor::ui {

// Owned, straight RGBA pixels in the raster's native grid. No document
// transform, user mask, crop, opacity, adjustments or effects are baked in.
struct BackgroundRemovalSource {
    QImage rgba;
    core::LayerId layerId {};
    core::SurfaceId sourceId {};
    core::Revision sourceRevision {};
    core::Vec2d rasterOrigin {};
    core::LayerMaskState originalMask;
    std::optional<core::LayerCrop> crop;
};

// RasterSurface is mutable, including through DocumentSnapshot. Drive step()
// on the UI thread under an editing barrier; only the completed owned source
// may be passed to a worker. Row batches let startup/cancellation stay live.
class BackgroundRemovalSourceBuilder final {
public:
    explicit BackgroundRemovalSourceBuilder(const core::Layer&);
    [[nodiscard]] bool step(int maximumRows = 128);
    [[nodiscard]] int rowsCopied() const noexcept { return copiedRows_; }
    [[nodiscard]] int totalRows() const noexcept { return source_.rgba.height(); }
    [[nodiscard]] BackgroundRemovalSource source() const;

private:
    std::shared_ptr<const core::RasterSurface> surface_;
    BackgroundRemovalSource source_;
    int copiedRows_ {};
    bool hasVisibleSource_ {};
};

[[nodiscard]] BackgroundRemovalSource backgroundRemovalSource(const core::Layer&);

// Combines the generated foreground coverage with the existing enabled mask
// in layer-local coordinates. Original alpha is not multiplied into the mask.
// No document state is changed; publication belongs to one LayerMaskCommand.
// Throws for invalid inputs, disabled existing masks, cancellation or results
// retaining no visible source pixels.
[[nodiscard]] core::LayerMaskState backgroundRemovalMask(
    const BackgroundRemovalSource&, const QImage& nativeGrayscaleMask,
    const std::function<bool()>& cancelled = {});

} // namespace imageeditor::ui
