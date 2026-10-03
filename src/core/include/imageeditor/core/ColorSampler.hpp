#pragma once

#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/core/CompositionPlan.hpp"
#include <unordered_map>

namespace imageeditor::core {

// Prepare geometry once for a bounded extraction; same filtering convention as
// Active Layer eyedropper, without opacity/visibility or per-pixel inversion.
// Explicit intrinsic source access: crop and adjustments are not evaluated.
class PreparedRasterSampler {
public:
    explicit PreparedRasterSampler(const Layer&);
    [[nodiscard]] Rgba8 sample(Vec2d documentPoint) const;
private:
    const RasterSurface* surface_ {nullptr};
    AffineTransform inverse_;
};

// One prepared source image and adjustment program, independent of visibility,
// layer opacity and blend mode. Document points may lie outside the canvas for
// off-canvas merge/thumbnail output. adjusted=false bypasses adjustments and spatial filters;
// crop is always an output restriction here. Use PreparedRasterSampler for raw
// intrinsic pixels, not this rendered-appearance sampler.
class PreparedLayerSampler {
public:
    explicit PreparedLayerSampler(const Layer&, bool adjusted = true);
    [[nodiscard]] PremultipliedColor sample(Vec2d documentPoint,
        std::size_t stopBefore = adjustmentCount) const;
private:
    std::shared_ptr<const RasterSurface> surface_;
    Revision revision_ {0};
    AffineTransform inverse_, documentToLocal_;
    std::optional<LayerCrop> crop_;
    LayerMaskState mask_;
    std::shared_ptr<const CompiledAdjustmentStack> adjustments_;
    std::optional<EffectParameters> effects_;
    std::shared_ptr<const LayerEffectCache> effectCache_;
};

enum class ColorSampleSource { MergedVisible, ActiveLayer };
// All content consumers use alpha-aware linear filtering. Retain the old name
// as an alias for callers; it no longer enables hidden-RGB edge contamination.
enum class SampleFiltering { AlphaAware, DisplayStraight = AlphaAware };
// Existing eyedropper/color-selection active sources intentionally stay raw.
// Smart Select explicitly requests the active layer's rendered appearance.
enum class ActiveReferenceAppearance { Intrinsic, Rendered };
struct SampleCacheOverride { LayerId id; std::shared_ptr<const LayerRenderCache> cache; };
enum class ColorSampleStatus { Available, OutsideCanvas, NoActiveRaster, UnsupportedLayer };

// Retains source lifetimes and freezes geometry/composition metadata, not a
// full-frame pixel copy. Caller checks matches() before each cooperative job
// step; sample() also rejects changed pixel revisions before reading them.
class PinnedDocumentSampler {
public:
    PinnedDocumentSampler(const Document&, std::optional<LayerId>, ColorSampleSource,
        SampleFiltering = SampleFiltering::AlphaAware, std::span<const SampleCacheOverride> = {},
        ActiveReferenceAppearance = ActiveReferenceAppearance::Intrinsic);
    // Prepared authoritative output/thumbnail sources. Captures just immutable
    // surfaces + matrices, never copies text payloads or an entire document.
    // sampleOrigin offsets sample coordinates without rebasing source matrices:
    // inverse-map evaluation remains identical at tight off-canvas merge edges.
    PinnedDocumentSampler(std::span<const Layer* const>, Extent2u sampleExtent,
        const AffineTransform& documentToSample = {}, Vec2d sampleOrigin = {}, const LayerTree* tree = nullptr,
        std::span<const LayerId> selectedItems = {});
    [[nodiscard]] bool matches(const Document&) const noexcept;
    [[nodiscard]] Rgba8 sample(Vec2d) const;
    // Continuous document coordinates, accumulated premultiplied linear RGBA.
    // Export reconstruction integrates these values before its single RGBA8
    // quantization; ordinary pixel sampling retains its center-snapped API.
    [[nodiscard]] PremultipliedColor sampleLinear(Vec2d) const;
    // Document-grid output. Validate revisions once per row; integer-aligned
    // sources are copied in one bounded span without filtering or texel reads.
    void sampleRow(std::int32_t x, std::int32_t y, std::span<PremultipliedColor>,
        std::span<std::byte> scratch = {}) const;
    // Unlike transparent coverage, outside an active source's transformed
    // render extent is not a valid sampling location. Merged uses the canvas.
    [[nodiscard]] bool validSample(Vec2d) const noexcept;
    [[nodiscard]] Extent2u extent() const { return snapshot_.canvas.extent; }
    [[nodiscard]] std::size_t sourceCount() const noexcept { return sources_.size(); }
private:
    struct Source {
        std::shared_ptr<const RasterSurface> surface;
        Revision revision;
        AffineTransform inverse;
        double opacity;
        BlendMode blendMode {BlendMode::Normal};
        AffineTransform documentToLocal;
        std::shared_ptr<const CompiledAdjustmentStack> adjustments;
        std::optional<LayerCrop> crop;
        std::optional<EffectParameters> effects;
        std::shared_ptr<const LayerEffectCache> effectCache;
        LayerMaskState mask;
        LayerId id{};
    };
    const Document* owner_ {nullptr};
    DocumentSnapshot snapshot_;
    std::vector<Source> sources_;
    Vec2d sampleOrigin_;
    bool activeOnly_ {false};
    SampleFiltering filtering_ {SampleFiltering::AlphaAware};
    std::optional<CompositionNode> composition_;
    std::unordered_map<LayerId,std::size_t> sourceById_;
    std::vector<std::shared_ptr<const PinnedDocumentSampler>> clippingGates_;
    [[nodiscard]] PremultipliedColor sampleComposition(Vec2d, std::span<const PremultipliedColor> = {}) const;
};

struct ColorSample {
    ColorSampleStatus status {ColorSampleStatus::OutsideCanvas};
    Rgba8 color;
    std::size_t layersVisited {0};
    std::size_t texelsRead {0};
    [[nodiscard]] bool available() const noexcept { return status == ColorSampleStatus::Available; }
};

// One document pixel, evaluated at its center, independent of viewport zoom.
// Reads <= 4 texels per contributing layer, with no snapshot/full-frame copy.
// Active Layer explicitly reads intrinsic uncropped/unadjusted source color,
// ignoring visibility and layer opacity. Merged Visible matches
// alpha-aware linear filtering and the shared layer blend/composite contract.
[[nodiscard]] ColorSample sampleDocumentColor(const Document& document,
    std::optional<LayerId> activeLayer, Vec2d documentPosition,
    ColorSampleSource source);

// Topmost visible raster coverage, text layout bounds, or true shape interior
// and rendered stroke at the continuous pointer position. Unlike Eyedropper
// this never quantizes to document pixel centers. UI shape targeting extends
// lines with a display-space tolerance through its path service.
[[nodiscard]] std::optional<LayerId> hitTestRasterLayer(const Document&, Vec2d);
[[nodiscard]] float clippingVisibility(const Document&, LayerId, Vec2d);

} // namespace imageeditor::core
