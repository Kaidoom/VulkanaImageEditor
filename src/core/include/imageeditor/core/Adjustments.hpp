#pragma once

#include "imageeditor/core/Geometry.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/core/BlendCompositing.hpp"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace imageeditor::core {

enum class AdjustmentType : std::uint32_t {
    Exposure, BrightnessContrast, Levels, Curves, HueSaturation,
    Vibrance, ColorBalance, WarmthTint, BlackWhite, Invert
};
inline constexpr std::size_t adjustmentCount = 10;
inline constexpr std::uint32_t adjustmentAlgorithmVersion = 1;
inline constexpr std::array<AdjustmentType, adjustmentCount> allAdjustmentTypes {
    AdjustmentType::Exposure, AdjustmentType::BrightnessContrast, AdjustmentType::Levels,
    AdjustmentType::Curves, AdjustmentType::HueSaturation, AdjustmentType::Vibrance,
    AdjustmentType::ColorBalance, AdjustmentType::WarmthTint, AdjustmentType::BlackWhite,
    AdjustmentType::Invert
};
[[nodiscard]] std::string_view adjustmentName(AdjustmentType) noexcept;
[[nodiscard]] std::string_view adjustmentIdentifier(AdjustmentType) noexcept;
[[nodiscard]] std::optional<AdjustmentType> adjustmentFromIdentifier(std::string_view) noexcept;

struct ExposureParameters { double stops {0}; friend bool operator==(const ExposureParameters&, const ExposureParameters&) = default; };
struct BrightnessContrastParameters {
    double brightness {0}, contrast {0}; // Both [-1,1], sRGB contrast pivot .5.
    friend bool operator==(const BrightnessContrastParameters&, const BrightnessContrastParameters&) = default;
};
struct LevelsChannel {
    double inputBlack {0}, gamma {1}, inputWhite {1}, outputBlack {0}, outputWhite {1};
    friend bool operator==(const LevelsChannel&, const LevelsChannel&) = default;
};
struct LevelsParameters {
    std::array<LevelsChannel,4> channels {}; // Composite, Red, Green, Blue; composite first.
    friend bool operator==(const LevelsParameters&, const LevelsParameters&) = default;
};
struct CurvePoint { double input {0}, output {0}; friend bool operator==(const CurvePoint&, const CurvePoint&) = default; };
struct CurveChannel {
    std::vector<CurvePoint> points {{0,0},{1,1}}; // Strictly increasing X; <=16 points, arbitrary Y order.
    friend bool operator==(const CurveChannel&, const CurveChannel&) = default;
};
struct CurvesParameters {
    std::array<CurveChannel,4> channels {};
    friend bool operator==(const CurvesParameters&, const CurvesParameters&) = default;
};
struct HueRangeParameters {
    double hue {0}, saturation {0}, lightness {0}; // Degrees [-180,180], other two [-1,1].
    friend bool operator==(const HueRangeParameters&, const HueRangeParameters&) = default;
};
struct HueSaturationParameters {
    // Master, Reds, Yellows, Greens, Cyans, Blues, Magentas.
    std::array<HueRangeParameters,7> ranges {};
    bool colorize {false};
    double colorizeHue {0}, colorizeSaturation {.5}; // Degrees [0,360], [0,1].
    friend bool operator==(const HueSaturationParameters&, const HueSaturationParameters&) = default;
};
struct VibranceParameters {
    double amount {0}; // [-1,1]; saturation-aware, without a skin-color heuristic.
    friend bool operator==(const VibranceParameters&, const VibranceParameters&) = default;
};
struct ColorBalanceParameters {
    std::array<std::array<double,3>,3> tones {}; // Shadows/midtones/highlights × red/green/blue [-1,1].
    bool preserveLuminosity {true};
    friend bool operator==(const ColorBalanceParameters&, const ColorBalanceParameters&) = default;
};
struct WarmthTintParameters {
    double warmth {0}, tint {0}; // Relative [-1,1], not camera Kelvin.
    friend bool operator==(const WarmthTintParameters&, const WarmthTintParameters&) = default;
};
struct BlackWhiteParameters {
    std::array<double,6> contributions {0.0,0.0,0.0,0.0,0.0,0.0}; // Hue-range luminance offsets [-1,1].
    Rgba8 tintColor {194,160,115,255};
    double tintStrength {0}; // [0,1], tint alpha intentionally ignored; source alpha unchanged.
    friend bool operator==(const BlackWhiteParameters&, const BlackWhiteParameters&) = default;
};
struct InvertParameters { friend bool operator==(const InvertParameters&, const InvertParameters&) = default; };
using AdjustmentParameters = std::variant<ExposureParameters, BrightnessContrastParameters,
    LevelsParameters, CurvesParameters, HueSaturationParameters, VibranceParameters,
    ColorBalanceParameters, WarmthTintParameters, BlackWhiteParameters, InvertParameters>;
struct AdjustmentMask {
    SelectionState coverage; // Non-null, owned immutable snapshot; active empty remains empty.
    AffineTransform localToMask; // Capture-time local→document; frozen, never normalized to layer bounds.
};
struct Adjustment {
    AdjustmentType type {AdjustmentType::Exposure};
    bool enabled {false};
    AdjustmentParameters parameters {ExposureParameters {}};
    std::optional<AdjustmentMask> mask;
};
struct AdjustmentStack {
    std::uint32_t algorithmVersion {adjustmentAlgorithmVersion};
    std::array<Adjustment,adjustmentCount> items;
    AdjustmentStack();
};
using AdjustmentState = std::shared_ptr<const AdjustmentStack>;
[[nodiscard]] Adjustment defaultAdjustment(AdjustmentType);
[[nodiscard]] bool validAdjustments(const AdjustmentStack&, std::string* reason = nullptr);
[[nodiscard]] bool equivalentAdjustments(const AdjustmentState&, const AdjustmentState&) noexcept;
[[nodiscard]] bool adjustmentIsNeutral(const Adjustment&) noexcept;
[[nodiscard]] std::size_t adjustmentMemoryCost(const AdjustmentState&) noexcept;
[[nodiscard]] std::size_t adjustmentMemoryCost(const AdjustmentState&, const AdjustmentState&) noexcept;
[[nodiscard]] AdjustmentMask captureAdjustmentMask(SelectionState, const AffineTransform& localToDocument);
[[nodiscard]] float adjustmentMaskCoverage(const AdjustmentMask&, Vec2d localPoint) noexcept;

// Packed shader ABI: ten fixed records of 256 floats. Slot 0 = active (0/1);
// slot 1 = stable type ID; parameters start at slot 8. Curves use 4×49 floats
// containing count then (x,y,tangent)×16; count=0 means exact channel identity.
// Exact piecewise Hermite otherwise, no LUT.
// Mask textures/mappings are bound separately by the renderer by stage index.
inline constexpr std::size_t adjustmentParameterStride = 256;
struct CompiledAdjustmentStack {
    std::array<float,adjustmentCount*adjustmentParameterStride> parameters {};
    std::array<std::optional<AdjustmentMask>,adjustmentCount> masks;
    bool active {false};
};
[[nodiscard]] CompiledAdjustmentStack compileAdjustmentStack(const AdjustmentState&);
// Premultiplied LINEAR input, after source filtering, before layer opacity/blend.
// stopBefore is the stable stage index for Levels/Curves input histograms.
[[nodiscard]] PremultipliedColor evaluateAdjustments(const CompiledAdjustmentStack&,
    PremultipliedColor, Vec2d localPoint, std::size_t stopBefore = adjustmentCount) noexcept;
[[nodiscard]] float evaluateCurve(const CurveChannel&, float input);

} // namespace imageeditor::core
