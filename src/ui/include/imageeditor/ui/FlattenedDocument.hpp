#pragma once

#include "imageeditor/core/Document.hpp"

#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>

namespace imageeditor::ui {

struct FlattenedDocumentLimits {
    // Callers may lower these limits; increasing them does not bypass the
    // implementation's corresponding hard ceilings.
    std::uint64_t outputPixels { 64ULL * 1024 * 1024 };
    std::uint64_t derivedCacheBytes { 256ULL * 1024 * 1024 };
    std::uint64_t metadataBytes { 16ULL * 1024 * 1024 };
};
using FlattenedDocumentProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;
// Optional diagnostic timings. No clocks in per-row paths unless requested.
struct MergeProfile {
    double setupMs{}, typedRasterizationMs{}, spatialEffectsMs{}, boundsMs{};
    double allocationMs{}, samplingBlendMs{}, encodingMs{};
    std::uint64_t outputBytes{}, derivedBytes{};
};
struct FlattenedDocumentResult {
    QImage image; // Straight RGBA8888; merge is exactly one pixel/document pixel.
    QString error;
    bool cancelled { false };
    core::Vec2d origin; // Document-pixel top-left, including negative off-canvas coordinates.
    explicit operator bool() const noexcept { return !image.isNull(); }
};

// Disposable typed image at document-output resolution, independent of zoom
// or display DPI. Shared by flattened output and document-space color selection.
// Raster returns null; the caller retains its original RasterSurface directly.
[[nodiscard]] std::shared_ptr<const core::LayerRenderCache> prepareDocumentSampleCache(
    const core::Layer&, std::size_t availablePixels);

// Bounded CPU flattened output, not an export dialog or a destructive flatten
// command. Geometry/style snapshots share raster pixels (no full-layer copies).
// Text/shape caches regenerate independently of viewport caches at output scale
// 1, accounting for their layer affine scale. No source document/cache/history
// is mutated, and no partially rendered image is returned on failure/cancel.
// Call on the document-owning thread; background callers must own frozen
// sources and create their Qt layout/paint objects on that worker. A
// cooperative progress callback may cancel and service UI events; mutations
// detected after a callback fail explicitly instead of mixing document states.
[[nodiscard]] FlattenedDocumentResult flattenDocument(
    const core::Document&, FlattenedDocumentProgress = { }, FlattenedDocumentLimits = { }, MergeProfile* = nullptr);
// Same authoritative renderer. Identity is unchanged. Reduction integrates
// native document-pixel cells in premultiplied linear light; enlargement uses
// continuous sampling and independently higher-density text/shape caches.
[[nodiscard]] FlattenedDocumentResult flattenDocumentAtSize(const core::Document&,
    core::Extent2u output, FlattenedDocumentProgress = {}, FlattenedDocumentLimits = {},
    std::optional<core::Rgba8> matte = {});
// Isolated selected-only composition over transparent RGBA. Real blend modes
// are retained; no unselected backdrop participates or restricts admission.
// Selected normalized items only; preserve full visible bounds outside canvas.
// Empty/hidden-only input fails explicitly. Never samples a raster selection.
[[nodiscard]] FlattenedDocumentResult flattenLayerItems(const core::Document&,
    std::span<const core::LayerId>, FlattenedDocumentProgress = {}, FlattenedDocumentLimits = {},
    MergeProfile* = nullptr);
// One layer's internal source/effects/geometry, before final opacity/blending.
// Visibility is retained by the caller, not baked into these source pixels.
[[nodiscard]] FlattenedDocumentResult rasterizeLayerContent(const core::Document&, core::LayerId,
    FlattenedDocumentProgress = {}, FlattenedDocumentLimits = {});

struct EvaluatedDocumentBounds {
    core::RectI rect;
    QString error;
    bool cancelled {false};
};
// The same evaluated visual bounds used by native baking, without allocating
// or sampling an output image. Empty visible content returns an empty rect.
[[nodiscard]] EvaluatedDocumentBounds evaluatedLayerItemsBounds(const core::Document&,
    std::span<const core::LayerId>, FlattenedDocumentProgress = {}, FlattenedDocumentLimits = {});
// Explicit document-space page region. Preserves source coordinates and native
// pixels; output density affects reconstruction, never document geometry.
[[nodiscard]] FlattenedDocumentResult flattenDocumentRegion(const core::Document&, core::RectI,
    core::Extent2u output, FlattenedDocumentProgress = {}, FlattenedDocumentLimits = {},
    std::optional<core::Rgba8> matte = {});

} // namespace imageeditor::ui
