#pragma once

#include "imageeditor/core/Geometry.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace imageeditor::core {

enum class PointerType : std::uint8_t {
    Unknown,
    Mouse,
    Pen,
    Eraser,
    Touch,
};

enum PointerButton : std::uint32_t {
    PointerButtonNone = 0,
    PointerButtonPrimary = 1U << 0U,
    PointerButtonSecondary = 1U << 1U,
    PointerButtonMiddle = 1U << 2U,
    PointerButtonBarrel = 1U << 3U,
};

enum PointerModifier : std::uint32_t {
    PointerModifierNone = 0,
    PointerModifierShift = 1U << 0U,
    PointerModifierControl = 1U << 1U,
    PointerModifierAlt = 1U << 2U,
    PointerModifierMeta = 1U << 3U,
};

// UI adapters normalize every pointing device into this platform-neutral
// sample before tool code sees it. Coordinates are document pixels in the
// canonical top-left/Y-down convention; tilt is normalized to [-1, 1].
struct NormalizedPointerSample {
    Vec2d documentPosition;
    std::uint64_t timestampMicroseconds {0};
    double pressure {1.0};
    double tiltX {0.0};
    double tiltY {0.0};
    double rotationDegrees {0.0};
    double barrelRotationDegrees {0.0};
    PointerType pointerType {PointerType::Unknown};
    std::uint32_t buttons {PointerButtonNone};
    std::uint32_t modifiers {PointerModifierNone};
};

enum class BrushSmoothingMode : std::uint8_t {
    None,
    Weighted,
};

// Presets and project files refer to brush resources by stable data IDs. The
// IDs are deliberately not C++ type names: a future asset registry can resolve
// external bitmap tips and grains without adding one subclass per preset.
namespace BrushAssetIds {
inline constexpr std::string_view ProceduralRoundTip {
    "builtin.tip.procedural-round.v1"};
inline constexpr std::string_view ProceduralEllipseTip {
    "builtin.tip.procedural-ellipse.v1"};
inline constexpr std::string_view DryInkMaskTip {
    "builtin.tip.bitmap-dry-ink.v1"};
inline constexpr std::string_view NoGrain {"builtin.grain.none.v1"};
inline constexpr std::string_view DryInkPaperGrain {
    "builtin.grain.dry-ink-paper.v1"};
} // namespace BrushAssetIds

enum class BrushTipRotationMode : std::uint8_t {
    Fixed,
    FollowStrokeDirection,
};

// Plain data descriptors are suitable for preset/project serialization. Asset
// IDs identify immutable resources; geometry and dynamics remain preset data,
// so adding a preset never requires adding a preset-specific C++ class.
struct BrushTipDescriptor {
    std::string assetId {BrushAssetIds::ProceduralRoundTip};
    double aspectRatio {1.0};
    // Tip-local +X is the canonical forward axis for procedural and bitmap
    // tips. Positive angles rotate clockwise in top-left/Y-down document
    // space. In FollowStrokeDirection mode this is an offset from the resolved
    // path tangent rather than an absolute orientation.
    double angleDegrees {0.0};
    BrushTipRotationMode rotationMode {BrushTipRotationMode::FollowStrokeDirection};

    friend bool operator==(
        const BrushTipDescriptor&, const BrushTipDescriptor&) = default;
};

struct BrushGrainDescriptor {
    std::string assetId {BrushAssetIds::NoGrain};
    // Width of one repeated mask tile in document pixels.
    double scalePixels {96.0};
    double angleDegrees {0.0};
    double strength {0.0};
    bool invert {false};

    friend bool operator==(
        const BrushGrainDescriptor&, const BrushGrainDescriptor&) = default;
};

struct BrushSettings {
    BrushTipDescriptor tip;
    BrushGrainDescriptor grain;
    double sizePixels {32.0};
    double hardness {0.8};
    double opacity {1.0};
    double flow {1.0};
    double spacingPercent {10.0};
    Rgba8 foreground {79, 115, 255, 255};
    bool pressureToSize {true};
    bool pressureToFlow {true};
    BrushSmoothingMode smoothing {BrushSmoothingMode::None};
    double smoothingTimeMilliseconds {12.0};
    std::uint64_t deterministicSeed {1};

    friend bool operator==(
        const BrushSettings&, const BrushSettings&) = default;
};

struct BrushDab {
    Vec2d documentCenter;
    double diameterPixels {1.0};
    // Stroke opacity is a ceiling over cumulative flow. Flow is the amount
    // deposited by this dab before that ceiling is applied.
    double strokeOpacity {1.0};
    double flow {1.0};
    double tipAspectRatio {1.0};
    // Direction is resolved from the smoothed, distance-resampled document
    // path. It is absent for a fixed tip and until a stroke has moved far
    // enough to establish a stable tangent (including a stationary dot).
    std::optional<double> strokeDirectionDegrees;
    // Fully resolved orientation consumed by every IBrushTip implementation.
    double tipAngleDegrees {0.0};
    double grainScalePixels {96.0};
    double grainAngleDegrees {0.0};
    double grainStrength {0.0};
    bool grainInvert {false};
    std::uint64_t deterministicSeed {1};
    Rgba8 color;
    // Distance-resampled dab ordinal, not an input-event/frame counter.
    std::uint64_t sequenceIndex {0};
    NormalizedPointerSample sourceSample;
};

class BrushDabSink {
public:
    virtual ~BrushDabSink() = default;
    virtual void emitDab(const BrushDab& dab) = 0;
};

class IBrushEngine {
public:
    virtual ~IBrushEngine() = default;
    virtual bool beginStroke(const BrushSettings& settings,
        const NormalizedPointerSample& first, BrushDabSink& sink) = 0;
    virtual bool appendSample(const NormalizedPointerSample& sample,
        BrushDabSink& sink) = 0;
    virtual bool endStroke(const NormalizedPointerSample& finalSample,
        BrushDabSink& sink) = 0;
    virtual void cancelStroke() noexcept = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
    // Optional resolved cursor position for engines with a constrained path.
    // Presentation consumes it; raster edits still go exclusively through dabs.
    [[nodiscard]] virtual std::optional<Vec2d> constrainedPosition() const noexcept
    { return std::nullopt; }
};

enum class ProceduralBrushPreset : std::uint8_t {
    HardRound,
    SoftRound,
    PressureRound,
    InkPen,
    DryInk,
};

// Serialization-ready preset data. The rendering pipeline consumes settings
// plus referenced assets
// rather than a preset-specific implementation class.
struct BrushPresetRecord {
    std::string id;
    std::string displayName;
    BrushSettings settings;

    friend bool operator==(
        const BrushPresetRecord&, const BrushPresetRecord&) = default;
};

[[nodiscard]] std::span<const BrushPresetRecord> builtinBrushPresets() noexcept;
[[nodiscard]] const BrushPresetRecord* findBuiltinBrushPreset(
    std::string_view id) noexcept;
[[nodiscard]] BrushSettings proceduralBrushPreset(ProceduralBrushPreset preset);
[[nodiscard]] std::string serializeBrushPreset(
    const BrushPresetRecord& preset);
[[nodiscard]] std::optional<BrushPresetRecord> deserializeBrushPreset(
    std::string_view serialized, std::string* error = nullptr);

} // namespace imageeditor::core
