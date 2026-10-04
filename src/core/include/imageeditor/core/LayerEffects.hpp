#pragma once
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include <functional>
#include <string_view>

namespace imageeditor::core {
enum class LayerEffectType : std::uint32_t {
  Stroke,
  DropShadow,
  InnerShadow,
  OuterGlow,
  InnerGlow,
  ColorOverlay,
  GradientOverlay,
  BevelEmboss
};
enum class StrokePosition : std::uint32_t { Inside, Center, Outside };
enum class GradientType : std::uint32_t { Linear, Radial };
inline constexpr std::size_t layerEffectCount = 8;
inline constexpr std::size_t layerEffectMaskCount = 10;
inline constexpr std::uint32_t layerEffectVersion = 1;
inline constexpr double maximumEffectSize = 256, maximumEffectDistance = 512;
enum class BevelStyle : std::uint32_t { Inner, Outer, Emboss };
struct EffectContourPoint {
  double input{}, output{};
  bool corner{false};
  friend bool operator==(const EffectContourPoint&, const EffectContourPoint&) = default;
};
enum class EffectContourInterpolation : std::uint32_t { Linear, Smooth };
struct EffectContour {
  bool enabled{false};
  EffectContourInterpolation interpolation{EffectContourInterpolation::Smooth};
  std::vector<EffectContourPoint> points{{0,0,false},{1,1,false}};
  friend bool operator==(const EffectContour&, const EffectContour&) = default;
};
[[nodiscard]] bool validEffectContour(const EffectContour&) noexcept;
[[nodiscard]] double evaluateEffectContour(const EffectContour&, double) noexcept;
// Locally generated preset data. Index zero is identity; no preset identifier
// is authoritative or needed to reconstruct a saved curve.
[[nodiscard]] EffectContour effectContourPreset(bool gloss, int index);
struct BevelParameters {
  std::uint32_t version{1};
  BevelStyle style{BevelStyle::Inner};
  double depth{1}, soften{0}, altitude{30}, shadowOpacity{.55};
  bool down{false};
  BlendMode shadowBlend{BlendMode::Multiply};
  EffectContour surface, gloss;
  friend bool operator==(const BevelParameters&, const BevelParameters&) = default;
};
struct LayerEffect {
  bool enabled{false};
  BlendMode blendMode{BlendMode::Normal};
  double opacity{1};
  Rgba8 color{255, 255, 255, 255}, secondColor{0, 0, 0, 255};
  double size{6}, angle{45}, distance{8}, spread{0}; // spread/choke in [0,1]
  StrokePosition position{StrokePosition::Outside};
  GradientType gradient{GradientType::Linear};
  double scale{1};
  bool reverse{false};
  BevelParameters bevel;
  friend bool operator==(const LayerEffect &, const LayerEffect &) = default;
};
struct LayerEffectStack {
  std::uint32_t algorithmVersion{layerEffectVersion};
  std::array<LayerEffect, layerEffectCount> items;
  LayerEffectStack();
  friend bool operator==(const LayerEffectStack &,
                         const LayerEffectStack &) = default;
};
using LayerEffectState = std::shared_ptr<const LayerEffectStack>;
[[nodiscard]] std::size_t layerEffectMemoryCost(const LayerEffectState&) noexcept;
[[nodiscard]] LayerEffect defaultLayerEffect(LayerEffectType);
[[nodiscard]] std::string_view layerEffectName(LayerEffectType) noexcept;
[[nodiscard]] std::string_view layerEffectIdentifier(LayerEffectType) noexcept;
[[nodiscard]] std::optional<LayerEffectType>
    layerEffectFromIdentifier(std::string_view) noexcept;
[[nodiscard]] bool validLayerEffects(const LayerEffectStack &) noexcept;
[[nodiscard]] bool equivalentLayerEffects(const LayerEffectState &,
                                          const LayerEffectState &) noexcept;
[[nodiscard]] bool hasActiveLayerEffects(const LayerEffectState &) noexcept;
[[nodiscard]] bool equivalentLayerEffectGeometry(const LayerEffectState &,
                                               const LayerEffectState &) noexcept;

// Geometry is independent of effect color/blending, the viewport and backdrop.
// The R8 storage boundary matches existing selection/filter coverage;
// morphology and Gaussian convolution use floats before this one final mask
// quantization.
struct LayerEffectMask {
  SelectionState coverage;
  AffineTransform localToMask;
  RectD localBounds;
};
struct BevelDistanceCache {
  RectI bounds;
  std::vector<float> distance;
  std::vector<std::uint8_t> alpha, edgeAlpha;
};
struct BevelNormalCache {
  // Unit normal XY in signed normalized 16 bit; positive Z reconstructed.
  std::vector<std::array<std::int16_t,2>> xy;
};
struct LayerEffectCache {
  SurfaceId sourceId{};
  Revision sourceRevision{};
  AffineTransform pixelsToLocal;
  LayerEffectState geometry;
  // Stroke outside, stroke inside, drop shadow, inner shadow, outer glow, inner
  // glow.
  // Followed by bevel interior highlight/shadow, exterior highlight/shadow.
  std::array<std::shared_ptr<const LayerEffectMask>, layerEffectMaskCount> masks;
  std::shared_ptr<const BevelDistanceCache> bevelDistance;
  std::shared_ptr<const BevelNormalCache> bevelNormals;
  LayerEffectState bevelPrepared;
  // Per-preparation diagnostics. Reused stages record zero work.
  double bevelDistanceMilliseconds{}, bevelNormalMilliseconds{}, bevelLightingMilliseconds{};
  [[nodiscard]] std::size_t bevelMemoryCost() const noexcept {
    return (bevelDistance ? bevelDistance->distance.size()*6 : 0) +
           (bevelNormals ? bevelNormals->xy.size()*4 : 0);
  }
  RectD visualBounds;
  std::size_t workingBytes{};
};
struct Layer;
struct LayerSnapshot;
struct FilterPreparationOptions;
[[nodiscard]] bool layerEffectCacheValid(const Layer &) noexcept;
[[nodiscard]] bool layerEffectCacheValid(const LayerSnapshot &) noexcept;
[[nodiscard]] std::shared_ptr<const LayerEffectCache>
prepareLayerEffects(const Layer &, const FilterPreparationOptions &);
[[nodiscard]] float sampleEffectMask(const LayerEffectMask &, Vec2d) noexcept;
inline constexpr std::size_t effectParameterStride = 32;
inline constexpr std::size_t effectParameterCount =
    layerEffectCount * effectParameterStride + 4;
using EffectParameters = std::array<float, effectParameterCount>;
[[nodiscard]] EffectParameters compileLayerEffects(const LayerEffectState &,
                                                   RectD referenceFrame);
[[nodiscard]] PremultipliedColor
compositeLayerEffects(PremultipliedColor backdrop, PremultipliedColor base,
                      const EffectParameters &, const LayerEffectCache *,
                      Vec2d local, float layerOpacity, BlendMode,
                      float cropCoverage = 1) noexcept;
} // namespace imageeditor::core
