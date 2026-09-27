#pragma once
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/RegionalRasterSurface.hpp"

namespace imageeditor::core
{
// Cut + transform from a pinned source. Geometry is document-space; storage is
// native raster-space. Preview changes only raw pixels before the ordinary
// effect stack. A completed session retains ordinary pixel tiles, not a float.
class SelectedPixelTransformSession final
{
  public:
    SelectedPixelTransformSession(Document &, LayerId, std::size_t byteBudget = 512ULL * 1024 * 1024);
    ~SelectedPixelTransformSession();
    [[nodiscard]] bool targetAvailable() const noexcept;
    [[nodiscard]] LayerId layerId() const noexcept { return layer_; }
    [[nodiscard]] const SelectionState &originalSelection() const noexcept { return selection_; }
    [[nodiscard]] const ProjectiveTransform &mapping() const noexcept { return mapping_; }
    [[nodiscard]] const History &pendingHistory() const noexcept { return pending_; }
    // False means invalid/unrepresentable geometry; the last valid state stays.
    bool preview(const ProjectiveTransform &documentMapping);
    bool completeAction();
    bool undo();
    bool redo();
    TransformCommitResult commit(History &);
    void cancel() noexcept;
    [[nodiscard]] std::size_t temporaryBytes() const noexcept;
    [[nodiscard]] std::uint64_t evaluatedPixels() const noexcept { return evaluatedPixels_; }

  private:
    struct Sample {
        PremultipliedColor color{};
        float coverage{0};
    };
    struct Mip {
        Extent2u extent;
        std::vector<Sample> pixels;
    };
    [[nodiscard]] Sample texel(int x, int y, int level) const;
    [[nodiscard]] Sample sample(Vec2d, const ProjectiveTransform &, double affineLod) const;
    void prepareMips();
    [[nodiscard]] bool identity(const ProjectiveTransform &) const noexcept;
    void rememberTarget() noexcept;
    Document *document_;
    LayerId layer_;
    std::shared_ptr<RasterSurface> source_;
    Revision sourceRevision_;
    Revision workingRevision_{};
    Extent2u canvasExtent_{};
    Vec2d expectedOrigin_{};
    std::optional<RectD> expectedEffectFrame_;
    std::uint64_t expectedContentState_{};
    SelectionState selection_, expectedSelection_, originalLastSelection_;
    SelectionEvidenceState evidence_;
    ProjectiveTransform external_, inverseExternal_, mapping_, actionMapping_;
    Vec2d originalOrigin_;
    std::optional<RectD> originalEffectFrame_;
    std::optional<LayerCrop> crop_;
    AdjustmentState adjustments_;
    SpatialFilterState filters_;
    LayerEffectState effects_;
    RectI originalBounds_, fragmentBounds_;
    std::vector<std::byte> fragment_;
    std::vector<float> coverage_;
    std::vector<Mip> mips_;
    std::shared_ptr<RegionalRasterSurface> working_;
    RegionalRasterSurface::State actionState_, initialState_;
    bool actionWasOriginal_{true}, active_{true};
    std::uint64_t contentState_, evaluatedPixels_{0};
    std::size_t budget_;
    History pending_{std::numeric_limits<std::size_t>::max()};
    std::vector<ProjectiveTransform> checkpoints_{{}};
    std::vector<RegionalRasterSurface::State> storageCheckpoints_;
};
} // namespace imageeditor::core
