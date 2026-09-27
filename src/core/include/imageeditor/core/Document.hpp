#pragma once

#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/LayerTree.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/core/SelectionEvidence.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace imageeditor::core {

struct CanvasSpec {
    Extent2u extent;
    double dotsPerInch {96.0};

    friend constexpr bool operator==(const CanvasSpec&, const CanvasSpec&) = default;
};

struct RasterLayerSnapshot {
    std::shared_ptr<const RasterSurface> surface;
};

using LayerSnapshotPayload = std::variant<RasterLayerSnapshot, TextLayer, ShapeLayer>;

struct LayerSnapshot {
    LayerId id {0};
    std::string name;
    bool visible {true};
    float opacity {1.0F};
    BlendMode blendMode {BlendMode::Normal};
    AdjustmentState adjustments;
    Revision adjustmentRevision {1};
    SpatialFilterState filters;
    Revision filterRevision {1};
    std::shared_ptr<const LayerSpatialFilterCache> filterCache;
    LayerEffectState effects;
    Revision effectRevision {1};
    std::shared_ptr<const LayerEffectCache> effectCache;
    AffineTransform localToDocument;
    Vec2d rasterOrigin {};
    std::optional<RectD> rasterEffectFrame;
    std::optional<LayerCrop> crop;
    LayerSnapshotPayload payload;
    Revision textRevision {1};
    Revision shapeRevision {1};
    std::shared_ptr<const LayerRenderCache> renderCache;
};

struct DocumentSnapshot {
    Revision documentRevision {0};
    CanvasSpec canvas;
    std::vector<LayerSnapshot> layersBottomToTop;
    SelectionState selection;
    Revision selectionRevision {0};
};

struct RemovedLayer {
    Layer layer;
    std::size_t index {0};
    ItemPlacement placement;
};

// A guarded batch is validated in full before any geometry is published.
struct LayerTransformUpdate {
    LayerId id;
    AffineTransform before, after;
};
struct ItemVisibilityUpdate {
    LayerId id;
    bool before, after;
};

class Document {
public:
    explicit Document(CanvasSpec canvas);

    [[nodiscard]] const CanvasSpec& canvas() const noexcept { return canvas_; }
    [[nodiscard]] Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint64_t contentState() const noexcept { return contentState_; }
    [[nodiscard]] bool isModified() const noexcept { return contentState_ != savedState_; }
    void markSaved() noexcept { savedState_ = contentState_; }
    void markUnsaved() noexcept { savedState_ = 0; }
    // History and retractable edit branches alone advance/restore these IDs.
    void advanceContentState() noexcept;
    void restoreContentState(std::uint64_t state) noexcept { contentState_ = state; }
    [[nodiscard]] const std::vector<Layer>& layers() const noexcept { return layers_; }
    // Materialized leaf traversal of tree(), shared by all content consumers.
    [[nodiscard]] const LayerTree& tree() const noexcept { return tree_; }
    [[nodiscard]] bool containsItem(LayerId id) const noexcept { return containsLayer(id) || tree_.container(id); }
    [[nodiscard]] std::vector<LayerId> expandedLayers(std::span<const LayerId>) const;
    [[nodiscard]] LayerId canvasTarget(LayerId leaf) const noexcept;
    [[nodiscard]] std::optional<bool> itemVisibility(LayerId) const noexcept;
    [[nodiscard]] bool isEffectivelyVisible(LayerId) const noexcept;
    bool setItemVisibilities(std::span<const ItemVisibilityUpdate>);
    // Atomically validate/reorder and add/remove leaves. No pixel copies. The
    // loader uses the same admission path, so persisted cycles cannot leak in.
    bool replaceStructure(const LayerTree& expected, LayerTree replacement,
        std::span<const LayerId> removed = {}, std::span<const Layer> added = {},
        std::span<const ItemVisibilityUpdate> retainedLeafVisibility = {});
    bool insertLayerAt(ItemPlacement, Layer);
    bool setItemMetadata(LayerId, std::string name, ColorLabel);
    [[nodiscard]] const SelectionState& selection() const noexcept { return selection_; }
    [[nodiscard]] const SelectionState& lastSelection() const noexcept { return lastSelection_; }
    void setLastSelection(SelectionState);
    [[nodiscard]] Revision selectionRevision() const noexcept { return selectionRevision_; }
    [[nodiscard]] const SelectionEvidenceState& selectionEvidence() const noexcept { return selectionEvidence_; }
    // Ordinary mask changes rebase tool inference. A no-op leaves its evidence
    // intact; explicit evidence changes can be undoable even with identical R8.
    bool setSelection(SelectionState);
    bool setSelection(SelectionState, SelectionEvidenceState);

    [[nodiscard]] const Layer* layer(LayerId id) const noexcept;
    [[nodiscard]] Layer* layer(LayerId id) noexcept;
    [[nodiscard]] bool containsLayer(LayerId id) const noexcept;

    bool insertLayer(std::size_t index, Layer layer);
    [[nodiscard]] std::optional<RemovedLayer> takeLayer(LayerId id);
    // Moves a layer to its final bottom-to-top index without recreating it.
    bool moveLayer(LayerId id, std::size_t destinationIndex);
    bool setLayerVisibility(LayerId id, bool visible);
    bool setLayerOpacity(LayerId id, float opacity);
    bool setLayerBlendMode(LayerId id, BlendMode mode);
    bool setLayerAdjustments(LayerId id, AdjustmentState);
    bool setLayerFilters(LayerId id, SpatialFilterState);
    bool setLayerEffects(LayerId id, LayerEffectState);
    bool setLayerCrop(LayerId, std::optional<LayerCrop>);
    bool renameLayer(LayerId id, std::string name);
    bool setLayerTransform(LayerId id, const AffineTransform& transform);
    bool setLayerTransforms(std::span<const LayerTransformUpdate> updates);
    bool setLayerText(LayerId id, TextLayer text);
    bool setLayerShape(LayerId id, ShapeLayer shape);
    bool setLayerShapeGeometry(LayerId id, ShapeLayer shape, const AffineTransform&);
    // Raw native storage only; preserves layer identity, external geometry,
    // crop/masks/effects. Call after a regional storage publication to invalidate
    // bounds/reference observers without marking a persistent history checkpoint.
    bool setLayerRasterStorage(LayerId, std::shared_ptr<RasterSurface>,Vec2d origin,std::optional<RectD> effectFrame);
    // Changes only the document boundary and metadata. Layer surfaces and
    // local-to-document transforms deliberately remain untouched, so content
    // clipped by a smaller canvas can reappear after a later resize.
    bool setCanvas(CanvasSpec canvas);

    [[nodiscard]] DocumentSnapshot snapshot() const;

private:
    void touch() noexcept { ++revision_; }

    CanvasSpec canvas_;
    Revision revision_ {1};
    std::uint64_t contentState_ {0}, savedState_ {0};
    std::vector<Layer> layers_;
    LayerTree tree_;
    SelectionState selection_;
    SelectionState lastSelection_; // Last completed mask, never an interaction preview.
    SelectionEvidenceState selectionEvidence_;
    Revision selectionRevision_ {1};
};

} // namespace imageeditor::core
