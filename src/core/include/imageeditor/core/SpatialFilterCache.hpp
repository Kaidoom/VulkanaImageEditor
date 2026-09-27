#pragma once
#include "imageeditor/core/Document.hpp"
#include <functional>

namespace imageeditor::core {
// One disposable, bounds-aware source-resolution image. It includes color
// adjustments and the fixed spatial stack, but never crop, opacity or blend.
// Canonical raster/text/shape data are not changed or retained as baked pixels.
struct LayerSpatialFilterCache {
    std::shared_ptr<const RasterSurface> surface;
    AffineTransform pixelsToLocal;
    SurfaceId sourceId {0};
    Revision sourceRevision {0};
    AffineTransform sourcePixelsToLocal;
    AdjustmentState adjustments;
    SpatialFilterState filters;
    std::size_t workingBytes {0};
};
struct FilterPreparationOptions {
    std::size_t byteBudget {768ULL * 1024 * 1024};
    std::function<bool()> cancelled;
    std::function<void(double)> progress;
};
// Freeze on the document-owning thread before launching work. Canonical mutable
// raster pixels are copied in bounded row blocks; typed render caches are
// immutable and shared. No worker reads a concurrently editable surface.
struct FilterInputSnapshot {
    Layer layer;
    SurfaceId sourceId {0};
    Revision sourceRevision {0};
    std::size_t copiedBytes {0};
};
struct PreparedLayerSpatialEffects {
    std::shared_ptr<const LayerSpatialFilterCache> filters;
    std::shared_ptr<const LayerEffectCache> effects;
};
[[nodiscard]] PreparedLayerSpatialEffects prepareLayerSpatialEffects(const FilterInputSnapshot&,const FilterPreparationOptions& = {});
[[nodiscard]] FilterInputSnapshot snapshotFilterInput(const Layer&, const FilterPreparationOptions& = {});
[[nodiscard]] bool filterInputMatches(const Layer&,const FilterInputSnapshot&) noexcept;
// Cheap, allocation-free admission for parameter previews. Working bytes
// include a frozen raster source and float intermediates; no source is copied.
[[nodiscard]] std::size_t preflightLayerSpatialFilters(const Layer&,const FilterPreparationOptions& = {});
// Expensive: call only in a worker or an explicit output-preparation operation,
// NEVER from paint/render/scheduleFrame. Throws on cancellation or admission
// failure, leaving both document and existing cache unchanged.
[[nodiscard]] std::shared_ptr<const LayerSpatialFilterCache> prepareLayerSpatialFilters(
    const FilterInputSnapshot&, const FilterPreparationOptions& = {});
[[nodiscard]] std::shared_ptr<const LayerSpatialFilterCache> prepareLayerSpatialFilters(
    const Layer&, const FilterPreparationOptions& = {});
[[nodiscard]] bool layerSpatialFilterCacheValid(const Layer&) noexcept;
[[nodiscard]] bool layerSpatialFilterCacheValid(const LayerSnapshot&) noexcept;
// Only publishes if ALL source/parameter keys still agree; no document dirty,
// history, pixel revision or original-render-cache invalidation is performed.
bool publishLayerSpatialFilters(Document&, LayerId, std::shared_ptr<const LayerSpatialFilterCache>);
// Explicit CPU consumers use this to obtain a coherent effective layer. If a
// valid published cache already exists, this is just a shallow metadata copy.
[[nodiscard]] Layer prepareSpatialFilterLayer(const Layer&, const FilterPreparationOptions& = {});
[[nodiscard]] LayerSnapshot prepareSpatialFilterLayer(const LayerSnapshot&, const FilterPreparationOptions& = {});
} // namespace imageeditor::core
