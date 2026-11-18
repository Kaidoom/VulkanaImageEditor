#pragma once

#include "imageeditor/core/BlendMode.hpp"
#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/core/AdjustmentLayer.hpp"
#include "imageeditor/core/SpatialFilters.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/LayerMask.hpp"
#include "imageeditor/core/Geometry.hpp"
#include "imageeditor/core/CropGeometry.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/Shape.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace imageeditor::core {

struct LayerSpatialFilterCache;
struct ProjectLayerSource;

using LayerId = std::uint64_t;

[[nodiscard]] LayerId makeLayerId();
// Loader reserves IDs only after the entire project has been validated.
void reserveLayerId(LayerId id);

enum class MirrorEffectMode : std::uint8_t { Inherit, Override, Disabled };
struct LayerMirror {
    LayerId source {};
    bool opacityOverride {}, fillOpacityOverride {}, blendModeOverride {};
    bool pixelAlphaShapesEffectsOverride {}, maskEffectsAfterRenderingOverride {};
    std::array<MirrorEffectMode, layerEffectCount> effects {};
    friend bool operator==(const LayerMirror&, const LayerMirror&) = default;
};

struct FontDescriptor {
    std::string family;
    std::string style;
    int weight {400};
    bool italic {false};
    // Original imported face request; family/style above are the chosen face.
    std::string originalFace {};
    friend bool operator==(const FontDescriptor&, const FontDescriptor&) = default;
};

enum class TextAlignment {
    Left,
    Center,
    Right,
};

struct RasterLayer {
    std::shared_ptr<RasterSurface> surface;
};

struct TextStyle {
    FontDescriptor font {"Sans Serif", "", 400, false};
    // One em in document pixels, not screen points or monitor-dependent DPI.
    double sizePixels {24.0};
    Rgba8 color {255, 255, 255, 255};
    friend bool operator==(const TextStyle&, const TextStyle&) = default;
};
struct TextFormatRun {
    std::size_t start {0}, length {0}; // UTF-8 byte offsets on scalar boundaries.
    TextStyle style;
    friend bool operator==(const TextFormatRun&, const TextFormatRun&) = default;
};
struct TextParagraph {
    std::size_t start {0}; // UTF-8 start, including a trailing empty paragraph.
    TextAlignment alignment {TextAlignment::Left};
    friend bool operator==(const TextParagraph&, const TextParagraph&) = default;
};
struct TextLayer {
    std::string utf8;
    TextStyle defaultStyle;
    std::vector<TextFormatRun> runs;
    std::vector<TextParagraph> paragraphs {{}};
    friend bool operator==(const TextLayer&, const TextLayer&) = default;
};

// Disposable, platform-neutral presentation data. Not canonical text and not
// serialized/history state. A density change never changes logical geometry.
struct LayerRenderCache {
    std::shared_ptr<const RasterSurface> surface;
    AffineTransform pixelsToLocal;
    Extent2u logicalExtent;
    Revision contentRevision {0};
    double density {1};
    double requestedDensity {1};
    // A final-resolution typed raster can live directly on the document grid.
    // Its transform is part of this disposable cache's key, never model state.
    std::optional<AffineTransform> rasterizedDocumentTransform;
    Vec2d documentOrigin;
    // Crop/interaction bounds remain in the model's local frame; inverse-
    // mapping a rotated document raster's empty corners would enlarge them.
    std::optional<RectD> localSourceBounds;
};

using LayerPayload = std::variant<RasterLayer, TextLayer, ShapeLayer, AdjustmentLayer>;

struct Layer {
    LayerId id {0};
    std::string name;
    std::uint8_t colorLabel {0}; // Palette index; zero means no organizational label.
    // Linked layers retain ordinary resolved fields for existing consumers.
    // Inherited fields are disposable; source identity and overrides are canonical.
    std::optional<LayerMirror> mirror;
    std::shared_ptr<const ProjectLayerSource> projectSource;
    bool visible {true};
    float opacity {1.0F};
    float fillOpacity {1.0F}; // Content only; never changes source alpha or effect geometry.
    BlendMode blendMode {BlendMode::Normal};
    std::uint32_t blendSeed {defaultBlendSeed};
    AdjustmentState adjustments;
    Revision adjustmentRevision {1};
    SpatialFilterState filters;
    Revision filterRevision {1};
    std::shared_ptr<const LayerSpatialFilterCache> filterCache;
    LayerEffectState effects;
    Revision effectRevision {1};
    std::shared_ptr<const LayerEffectCache> effectCache;
    AffineTransform localToDocument;
    // Raster storage may expand left/up without rebasing canonical layer-local
    // coordinates. Crops, captured masks and effect anchors stay in that frame.
    Vec2d rasterOrigin {};
    std::optional<RectD> rasterEffectFrame;
    std::optional<LayerCrop> crop; // Stable layer-local output visibility; never a source rebase.
    LayerMaskState mask;
    LayerPayload payload;
    Revision textRevision {1};
    Revision shapeRevision {1};
    std::shared_ptr<const LayerRenderCache> renderCache;

    [[nodiscard]] static Layer raster(std::string name, std::shared_ptr<RasterSurface> surface);
    [[nodiscard]] static Layer adjustment(std::string name, AdjustmentScope = AdjustmentScope::AllBelow);
    [[nodiscard]] static Layer text(std::string name, TextLayer textData);
    [[nodiscard]] static Layer shape(std::string name, ShapeLayer shapeData);
};

// Parent-owned masks, effects and transforms remain editable on project layers.
[[nodiscard]] inline bool layerPixelsReadOnly(const Layer& layer) noexcept
{
    return layer.projectSource || layer.mirror.has_value();
}
[[nodiscard]] inline const char* readOnlyPixelsReason(const Layer& layer) noexcept
{
    return layer.projectSource
        ? "Project layer pixels are linked. Edit the source project or convert the layer to raster."
        : "Mirrored layers are read-only. Promote to a full layer before editing content.";
}

} // namespace imageeditor::core
