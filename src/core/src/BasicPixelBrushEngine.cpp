#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/CreativeBrushes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace imageeditor::core {
namespace {

constexpr double kMinimumDabStep = 0.25;
constexpr double kPositionEpsilon = 1.0e-9;
// A half document pixel rejects stationary hand noise without tying direction
// stability to viewport zoom or incoming event frequency. Dabs farther than
// one pixel apart use the new tangent immediately; only subpixel movement gets
// lightweight shortest-arc damping.
constexpr double kDirectionMovementThreshold = 0.5;
constexpr double kMinimumDirectionBlend = 0.65;
constexpr double kPi = 3.14159265358979323846;

double clampedFinite(double value, double minimum, double maximum,
    double fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

double distance(Vec2d first, Vec2d second) noexcept
{
    return std::hypot(second.x - first.x, second.y - first.y);
}

double lerp(double from, double to, double amount) noexcept
{
    return from + (to - from) * amount;
}

double normalizedDegrees(double value, double fallback = 0.0) noexcept
{
    return std::isfinite(value) ? std::remainder(value, 360.0) : fallback;
}

double lerpDegrees(double from, double to, double amount) noexcept
{
    from = normalizedDegrees(from);
    const auto shortestDelta = std::remainder(
        normalizedDegrees(to) - from, 360.0);
    return normalizedDegrees(from + shortestDelta * amount);
}

} // namespace

bool BasicPixelBrushEngine::beginStroke(const BrushSettings& settings,
    const NormalizedPointerSample& first, BrushDabSink& sink)
{
    cancelStroke();
    settings_ = settings;
    settings_.sizePixels = clampedFinite(settings_.sizePixels, 0.25, 4096.0, 32.0);
    settings_.hardness = clampedFinite(settings_.hardness, 0.0, 1.0, 0.8);
    settings_.opacity = clampedFinite(settings_.opacity, 0.0, 1.0, 1.0);
    settings_.flow = clampedFinite(settings_.flow, 0.0, 1.0, 1.0);
    settings_.spacingPercent = clampedFinite(
        settings_.spacingPercent, 1.0, 200.0, 10.0);
    settings_.tip.aspectRatio = clampedFinite(
        settings_.tip.aspectRatio, 0.05, 1.0, 1.0);
    settings_.tip.angleDegrees = normalizedDegrees(
        settings_.tip.angleDegrees);
    settings_.grain.scalePixels = clampedFinite(
        settings_.grain.scalePixels, 1.0, 4096.0, 96.0);
    settings_.grain.angleDegrees = normalizedDegrees(
        settings_.grain.angleDegrees);
    settings_.grain.strength = clampedFinite(
        settings_.grain.strength, 0.0, 1.0, 0.0);
    settings_.smoothingTimeMilliseconds = clampedFinite(
        settings_.smoothingTimeMilliseconds, 1.0, 250.0, 12.0);

    const auto normalized = sanitize(first);
    constraint_.reset(normalized.documentPosition,
        (normalized.modifiers & PointerModifierShift) != 0);
    previousRaw_ = normalized;
    previousFiltered_ = normalized;
    pathCursor_ = normalized;
    active_ = true;
    emitDabAt(normalized, sink);
    distanceUntilNextDab_ = spacingFor(makeDab(normalized));
    return true;
}

bool BasicPixelBrushEngine::appendSample(const NormalizedPointerSample& sample,
    BrushDabSink& sink)
{
    if (!active_) {
        return false;
    }
    const auto normalized = sanitize(sample);
    auto filtered = smooth(normalized);
    filtered.documentPosition = constraint_.resolve(filtered.documentPosition,
        (normalized.modifiers & PointerModifierShift) != 0);
    previousRaw_ = normalized;
    return appendFiltered(filtered, sink);
}

bool BasicPixelBrushEngine::endStroke(const NormalizedPointerSample& finalSample,
    BrushDabSink& sink)
{
    if (!active_) {
        return false;
    }
    auto normalized = sanitize(finalSample);
    // Releasing Shift before the button, without another pointer movement,
    // must not append an unconstrained hook to an otherwise straight stroke.
    if (previousRaw_ && distance(previousRaw_->documentPosition,
            normalized.documentPosition) <= kPositionEpsilon) {
        normalized.modifiers = (normalized.modifiers & ~PointerModifierShift)
            | (previousRaw_->modifiers & PointerModifierShift);
    }
    // Weighted smoothing is intentionally lightweight during motion, but the
    // pointer-up path always reaches the real endpoint so a stroke never ends
    // visibly short of the cursor.
    if (!previousRaw_ || distance(previousRaw_->documentPosition,
            normalized.documentPosition) > kPositionEpsilon
        || previousRaw_->timestampMicroseconds != normalized.timestampMicroseconds) {
        (void)appendSample(normalized, sink);
    }
    normalized.documentPosition = constraint_.resolve(normalized.documentPosition,
        (normalized.modifiers & PointerModifierShift) != 0);
    if (settings_.smoothing != BrushSmoothingMode::None && pathCursor_
        && distance(pathCursor_->documentPosition, normalized.documentPosition)
            > kPositionEpsilon) {
        (void)appendFiltered(normalized, sink);
    }
    if ((!lastDabCenter_
        || distance(*lastDabCenter_, normalized.documentPosition) > kPositionEpsilon)
        && !(settings_.spacingPercent >= 50 && isCreativeStampTip(settings_.tip.assetId))) {
        emitDabAt(normalized, sink);
    }
    cancelStroke();
    return true;
}

void BasicPixelBrushEngine::cancelStroke() noexcept
{
    previousRaw_.reset();
    previousFiltered_.reset();
    pathCursor_.reset();
    lastDabCenter_.reset();
    directionAnchor_.reset();
    resolvedDirectionDegrees_.reset();
    constraint_.reset();
    distanceUntilNextDab_ = 0.0;
    sequenceIndex_ = 0;
    active_ = false;
}

NormalizedPointerSample BasicPixelBrushEngine::sanitize(
    const NormalizedPointerSample& sample) const noexcept
{
    auto result = sample;
    result.documentPosition.x = clampedFinite(result.documentPosition.x,
        -1.0e9, 1.0e9, 0.0);
    result.documentPosition.y = clampedFinite(result.documentPosition.y,
        -1.0e9, 1.0e9, 0.0);
    result.pressure = clampedFinite(result.pressure, 0.0, 1.0,
        result.pointerType == PointerType::Mouse ? 1.0 : 0.0);
    result.tiltX = clampedFinite(result.tiltX, -1.0, 1.0, 0.0);
    result.tiltY = clampedFinite(result.tiltY, -1.0, 1.0, 0.0);
    result.rotationDegrees = clampedFinite(result.rotationDegrees, -360.0, 360.0, 0.0);
    result.barrelRotationDegrees = clampedFinite(
        result.barrelRotationDegrees, -360.0, 360.0, 0.0);
    return result;
}

NormalizedPointerSample BasicPixelBrushEngine::smooth(
    const NormalizedPointerSample& sample) noexcept
{
    if (settings_.smoothing == BrushSmoothingMode::None || !previousFiltered_
        || !previousRaw_) {
        previousFiltered_ = sample;
        return sample;
    }
    const auto elapsedMicroseconds = sample.timestampMicroseconds
            > previousRaw_->timestampMicroseconds
        ? sample.timestampMicroseconds - previousRaw_->timestampMicroseconds
        : 0;
    const auto elapsedMilliseconds = static_cast<double>(elapsedMicroseconds) / 1000.0;
    const auto weight = 1.0 - std::exp(
        -elapsedMilliseconds / settings_.smoothingTimeMilliseconds);
    auto result = sample;
    result.documentPosition.x = lerp(previousFiltered_->documentPosition.x,
        sample.documentPosition.x, weight);
    result.documentPosition.y = lerp(previousFiltered_->documentPosition.y,
        sample.documentPosition.y, weight);
    previousFiltered_ = result;
    return result;
}

NormalizedPointerSample BasicPixelBrushEngine::interpolate(
    const NormalizedPointerSample& from, const NormalizedPointerSample& to,
    double amount) noexcept
{
    NormalizedPointerSample result = to;
    result.documentPosition = {
        lerp(from.documentPosition.x, to.documentPosition.x, amount),
        lerp(from.documentPosition.y, to.documentPosition.y, amount),
    };
    result.pressure = lerp(from.pressure, to.pressure, amount);
    result.tiltX = lerp(from.tiltX, to.tiltX, amount);
    result.tiltY = lerp(from.tiltY, to.tiltY, amount);
    result.rotationDegrees = lerpDegrees(
        from.rotationDegrees, to.rotationDegrees, amount);
    result.barrelRotationDegrees = lerpDegrees(
        from.barrelRotationDegrees, to.barrelRotationDegrees, amount);
    const auto timeDelta = to.timestampMicroseconds >= from.timestampMicroseconds
        ? to.timestampMicroseconds - from.timestampMicroseconds : 0;
    result.timestampMicroseconds = from.timestampMicroseconds
        + static_cast<std::uint64_t>(std::llround(
            static_cast<double>(timeDelta) * amount));
    return result;
}

BrushDab BasicPixelBrushEngine::makeDab(
    const NormalizedPointerSample& sample,
    std::optional<double> strokeDirectionDegrees) const noexcept
{
    const auto sizePressure = settings_.pressureToSize ? sample.pressure : 1.0;
    const auto flowPressure = settings_.pressureToFlow ? sample.pressure : 1.0;
    const auto dynamicAngle = settings_.tip.rotationMode
            == BrushTipRotationMode::FollowStrokeDirection
        ? strokeDirectionDegrees.value_or(0.0)
        : 0.0;
    return {
        .documentCenter = sample.documentPosition,
        .diameterPixels = settings_.sizePixels * sizePressure,
        .strokeOpacity = settings_.opacity,
        .flow = settings_.flow * flowPressure,
        .tipAspectRatio = settings_.tip.aspectRatio,
        .strokeDirectionDegrees = strokeDirectionDegrees,
        .tipAngleDegrees = normalizedDegrees(
            settings_.tip.angleDegrees + dynamicAngle),
        .grainScalePixels = settings_.grain.scalePixels,
        .grainAngleDegrees = settings_.grain.angleDegrees,
        .grainStrength = settings_.grain.strength,
        .grainInvert = settings_.grain.invert,
        .deterministicSeed = settings_.deterministicSeed,
        .color = settings_.foreground,
        .sourceSample = sample,
    };
}

std::optional<double> BasicPixelBrushEngine::resolveStrokeDirection(
    Vec2d documentCenter) noexcept
{
    if (settings_.tip.rotationMode
        != BrushTipRotationMode::FollowStrokeDirection) {
        return std::nullopt;
    }
    if (!directionAnchor_) {
        directionAnchor_ = documentCenter;
        return std::nullopt;
    }

    const auto deltaX = documentCenter.x - directionAnchor_->x;
    const auto deltaY = documentCenter.y - directionAnchor_->y;
    const auto movement = std::hypot(deltaX, deltaY);
    if (!std::isfinite(movement) || movement < kDirectionMovementThreshold) {
        return resolvedDirectionDegrees_;
    }

    const auto candidate = normalizedDegrees(
        std::atan2(deltaY, deltaX) * 180.0 / kPi);
    directionAnchor_ = documentCenter;
    if (!resolvedDirectionDegrees_) {
        resolvedDirectionDegrees_ = candidate;
        return resolvedDirectionDegrees_;
    }

    const auto blend = std::clamp(
        movement / (2.0 * kDirectionMovementThreshold),
        kMinimumDirectionBlend, 1.0);
    resolvedDirectionDegrees_ = lerpDegrees(
        *resolvedDirectionDegrees_, candidate, blend);
    return resolvedDirectionDegrees_;
}

double BasicPixelBrushEngine::spacingFor(const BrushDab& dab) const noexcept
{
    return std::max(kMinimumDabStep,
        dab.diameterPixels * settings_.spacingPercent / 100.0);
}

void BasicPixelBrushEngine::emitDabAt(const NormalizedPointerSample& sample,
    BrushDabSink& sink)
{
    const auto direction = resolveStrokeDirection(sample.documentPosition);
    auto dab = makeDab(sample, direction);
    dab.sequenceIndex = sequenceIndex_++;
    if (dab.diameterPixels > 0.0 && dab.flow > 0.0
        && dab.strokeOpacity > 0.0) {
        sink.emitDab(dab);
    }
    lastDabCenter_ = sample.documentPosition;
}

bool BasicPixelBrushEngine::appendFiltered(
    const NormalizedPointerSample& sample, BrushDabSink& sink)
{
    if (!pathCursor_) {
        pathCursor_ = sample;
        return true;
    }
    auto segmentStart = *pathCursor_;
    const auto segmentLength = distance(
        segmentStart.documentPosition, sample.documentPosition);
    if (segmentLength <= kPositionEpsilon) {
        pathCursor_ = sample;
        return true;
    }

    double consumed = 0.0;
    while (distanceUntilNextDab_ <= segmentLength - consumed + kPositionEpsilon) {
        const auto amount = std::clamp(
            (consumed + distanceUntilNextDab_) / segmentLength, 0.0, 1.0);
        const auto dabSample = interpolate(segmentStart, sample, amount);
        emitDabAt(dabSample, sink);
        consumed += distanceUntilNextDab_;
        distanceUntilNextDab_ = spacingFor(makeDab(dabSample));
    }
    distanceUntilNextDab_ -= std::max(0.0, segmentLength - consumed);
    distanceUntilNextDab_ = std::max(kPositionEpsilon, distanceUntilNextDab_);
    pathCursor_ = sample;
    return true;
}

BrushSettings proceduralBrushPreset(ProceduralBrushPreset preset)
{
    const auto presets = builtinBrushPresets();
    const auto index = static_cast<std::size_t>(preset);
    return index < presets.size() ? presets[index].settings : BrushSettings {};
}

std::span<const BrushPresetRecord> builtinBrushPresets() noexcept
{
    static const std::vector<BrushPresetRecord> records = [] {
        BrushSettings hard;
        hard.hardness = 1.0;
        hard.pressureToSize = false;
        hard.pressureToFlow = false;

        BrushSettings soft;
        soft.hardness = 0.0;
        soft.flow = 0.35;
        soft.pressureToSize = false;
        soft.pressureToFlow = false;

        BrushSettings pressure;
        pressure.hardness = 0.8;
        pressure.pressureToSize = true;
        pressure.pressureToFlow = true;

        BrushSettings ink;
        ink.tip.assetId = BrushAssetIds::ProceduralEllipseTip;
        ink.tip.aspectRatio = 0.24;
        ink.tip.angleDegrees = -38.0;
        ink.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
        ink.sizePixels = 24.0;
        ink.hardness = 0.94;
        ink.opacity = 1.0;
        ink.flow = 1.0;
        ink.spacingPercent = 7.0;
        ink.pressureToSize = true;
        ink.pressureToFlow = false;
        ink.smoothing = BrushSmoothingMode::Weighted;
        ink.smoothingTimeMilliseconds = 10.0;

        BrushSettings dryInk;
        dryInk.tip.assetId = BrushAssetIds::DryInkMaskTip;
        dryInk.tip.aspectRatio = 0.38;
        dryInk.tip.angleDegrees = -34.0;
        dryInk.grain.assetId = BrushAssetIds::DryInkPaperGrain;
        dryInk.grain.scalePixels = 72.0;
        dryInk.grain.angleDegrees = 7.0;
        dryInk.grain.strength = 0.68;
        dryInk.sizePixels = 46.0;
        dryInk.hardness = 0.58;
        dryInk.opacity = 0.96;
        dryInk.flow = 0.72;
        dryInk.spacingPercent = 8.0;
        dryInk.pressureToSize = true;
        dryInk.pressureToFlow = true;
        dryInk.smoothing = BrushSmoothingMode::Weighted;
        dryInk.smoothingTimeMilliseconds = 10.0;
        dryInk.deterministicSeed = 0xD12A5EEDULL;

        std::vector<BrushPresetRecord> result {
            {"builtin.preset.hard-round.v1", "Hard Round", std::move(hard)},
            {"builtin.preset.soft-round.v1", "Soft Round", std::move(soft)},
            {"builtin.preset.pressure-round.v1", "Pressure Round",
                std::move(pressure)},
            {"builtin.preset.ink-pen.v1", "Ink Pen", std::move(ink)},
            {"builtin.preset.dry-ink.v1", "Dry Ink", std::move(dryInk)},
        };
        const auto creative = creativeBrushPresets();
        result.insert(result.end(), creative.begin(), creative.end());
        return result;
    }();
    return records;
}

const BrushPresetRecord* findBuiltinBrushPreset(std::string_view id) noexcept
{
    const auto presets = builtinBrushPresets();
    const auto found = std::find_if(presets.begin(), presets.end(),
        [id](const BrushPresetRecord& preset) { return preset.id == id; });
    return found == presets.end() ? nullptr : &*found;
}

} // namespace imageeditor::core
