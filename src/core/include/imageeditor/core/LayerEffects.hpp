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
  GradientOverlay
};
enum class StrokePosition : std::uint32_t { Inside, Center, Outside };
enum class GradientType : std::uint32_t { Linear, Radial };
inline constexpr std::size_t layerEffectCount = 7;
inline constexpr std::uint32_t layerEffectVersion = 1;
inline constexpr double maximumEffectSize = 256, maximumEffectDistance = 512;
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
struct LayerEffectCache {
  SurfaceId sourceId{};
  Revision sourceRevision{};
  AffineTransform pixelsToLocal;
  LayerEffectState geometry;
  // Stroke outside, stroke inside, drop shadow, inner shadow, outer glow, inner
  // glow.
  std::array<std::shared_ptr<const LayerEffectMask>, 6> masks;
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
