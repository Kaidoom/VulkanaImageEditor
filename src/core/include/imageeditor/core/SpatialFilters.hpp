#pragma once

#include "imageeditor/core/Adjustments.hpp"
#include <functional>
#include <span>

namespace imageeditor::core {

enum class SpatialFilterType : std::uint32_t { Gaussian, Motion, Lens };
inline constexpr std::size_t spatialFilterCount = 3;
inline constexpr std::uint32_t spatialFilterAlgorithmVersion = 1;
inline constexpr double maximumSpatialRadius = 256.0;
inline constexpr double maximumMotionDistance = 512.0;
inline constexpr std::array<SpatialFilterType, spatialFilterCount> allSpatialFilterTypes {
    SpatialFilterType::Gaussian, SpatialFilterType::Motion, SpatialFilterType::Lens
};
struct GaussianBlurParameters {
    // Stable layer-local pixel units. Radius = 3 sigma; finite, pixel-integrated
    // support [-radius,+radius]. Integer support is ceil(radius). Zero is identity.
    double radiusX {0}, radiusY {0};
    friend bool operator==(const GaussianBlurParameters&, const GaussianBlurParameters&) = default;
};
struct MotionBlurParameters {
    double distance {0}; // Full centered trail length, in layer-local pixels.
    double angle {0}; // Degrees clockwise: 0 = right, 90 = down (document axes).
    friend bool operator==(const MotionBlurParameters&, const MotionBlurParameters&) = default;
};
struct LensBlurParameters {
    double radius {0}; // Circumradius in layer-local pixels; uniform aperture, not depth-aware.
    std::uint32_t blades {0}; // 0 = circle, otherwise 3..12 regular polygon vertices.
    double rotation {0}; // Degrees clockwise; first polygon vertex points right at zero.
    friend bool operator==(const LensBlurParameters&, const LensBlurParameters&) = default;
};
using SpatialFilterParameters = std::variant<GaussianBlurParameters, MotionBlurParameters, LensBlurParameters>;
struct LayerSpatialFilter {
    SpatialFilterType type {SpatialFilterType::Gaussian};
    bool enabled {false};
    bool preserveAlpha {false};
    SpatialFilterParameters parameters {GaussianBlurParameters {}};
    std::optional<AdjustmentMask> mask; // Restricts output contribution, never input neighbors.
};
struct SpatialFilterStack {
    std::uint32_t algorithmVersion {spatialFilterAlgorithmVersion};
    std::array<LayerSpatialFilter, spatialFilterCount> items; // Gaussian -> Motion -> Lens, never UI order.
    SpatialFilterStack();
};
using SpatialFilterState = std::shared_ptr<const SpatialFilterStack>;
[[nodiscard]] std::string_view spatialFilterName(SpatialFilterType) noexcept;
[[nodiscard]] std::string_view spatialFilterIdentifier(SpatialFilterType) noexcept;
[[nodiscard]] std::optional<SpatialFilterType> spatialFilterFromIdentifier(std::string_view) noexcept;
[[nodiscard]] LayerSpatialFilter defaultSpatialFilter(SpatialFilterType);
[[nodiscard]] bool validSpatialFilters(const SpatialFilterStack&, std::string* reason = nullptr);
[[nodiscard]] bool equivalentSpatialFilters(const SpatialFilterState&, const SpatialFilterState&) noexcept;
[[nodiscard]] bool spatialFilterIsNeutral(const LayerSpatialFilter&) noexcept;
[[nodiscard]] bool hasActiveSpatialFilters(const SpatialFilterState&) noexcept;
[[nodiscard]] std::size_t spatialFilterMemoryCost(const SpatialFilterState&) noexcept;
[[nodiscard]] std::size_t spatialFilterMemoryCost(const SpatialFilterState&, const SpatialFilterState&) noexcept;

// Pixel centers are (x+.5,y+.5) in bounds coordinates. Four-channel planes are
// LINEAR premultiplied RGBA; one-channel planes are linear scalar masks. Pixels
// outside the provided source bounds are transparent zero, never edge-extended.
// For a source patch rather than a whole image, callers MUST include the entire
// required input neighborhood: internal tile boundaries are not image borders.
struct SpatialPlaneView {
    RectI bounds;
    std::uint32_t channels {4};
    std::span<const float> pixels;
    std::size_t rowStride {0}; // In floats; zero means tightly packed.
};
struct SpatialPlane {
    RectI bounds;
    std::uint32_t channels {4};
    std::vector<float> pixels;
    [[nodiscard]] SpatialPlaneView view() const noexcept { return {bounds, channels, pixels, 0}; }
};
struct SpatialKernel {
    RectI bounds; // Integer sample offsets. Application is convolution: source(output-offset).
    std::vector<double> weights; // Normalized row-major 2D taps for Motion/Lens; empty if separable.
    std::vector<double> horizontal, vertical; // Normalized Gaussian factors.
    [[nodiscard]] bool separable() const noexcept { return !horizontal.empty(); }
};
// Gaussian integrates exp(-x*x/(2*sigma*sigma)) over each pixel cell, clipped
// to the continuous radius, then normalizes. This avoids finite-support jumps.
[[nodiscard]] std::vector<double> makeGaussianKernel(double radius);
[[nodiscard]] SpatialKernel makeSpatialKernel(const LayerSpatialFilter&, double pixelScale = 1.0,
    const std::function<bool()>& cancelled = {});
[[nodiscard]] RectI requiredSpatialInputBounds(const LayerSpatialFilter&, RectI output, double pixelScale = 1.0);
[[nodiscard]] RectI expandedSpatialOutputBounds(const LayerSpatialFilter&, RectI input, double pixelScale = 1.0);
[[nodiscard]] RectI requiredSpatialInputBounds(const SpatialFilterState&, RectI output, double pixelScale = 1.0);
[[nodiscard]] RectI expandedSpatialOutputBounds(const SpatialFilterState&, RectI input, double pixelScale = 1.0);

struct SpatialFilterOptions {
    std::size_t maxWorkingBytes {512ULL * 1024 * 1024}; // Output + scratch; input owned by caller.
    std::uint64_t maxSampleOperations {64ULL * 1024 * 1024 * 1024};
    double pixelScale {1.0}; // Cache pixels per parameter unit, (0,8]; does not mutate document parameters.
    std::function<bool()> cancelled;
    std::function<void(double)> progress; // Monotonic [0,1]; bounded scanline/tile cadence.
};
enum class SpatialFilterStatus { Complete, Cancelled, InvalidInput, BudgetExceeded };
struct SpatialFilterResult {
    SpatialFilterStatus status {SpatialFilterStatus::InvalidInput};
    SpatialPlane output; // Only populated on Complete; cancellation never publishes partial output.
    std::string error;
    std::size_t peakWorkingBytes {0};
    std::uint64_t sampleOperations {0};
    [[nodiscard]] explicit operator bool() const noexcept { return status == SpatialFilterStatus::Complete; }
};
// Reusable scalar/RGBA primitive. Kernel compilation can be cached independently
// of source revisions; no alpha preservation or captured mask at this lower level.
[[nodiscard]] SpatialFilterResult convolveSpatialRegion(SpatialPlaneView, RectI output,
    const SpatialKernel&, const SpatialFilterOptions& = {});
// Applies convolution, optional alpha preservation, then captured coverage once
// in premultiplied linear space. For preserveAlpha, recover alpha-weighted neighbor
// color and re-premultiply by ORIGINAL destination alpha (zero stays zero).
[[nodiscard]] SpatialFilterResult filterSpatialRegion(SpatialPlaneView, RectI output,
    const LayerSpatialFilter&, const SpatialFilterOptions& = {});
// Shared no-allocation point finishing helper for callers caching raw convolutions.
[[nodiscard]] PremultipliedColor finishSpatialFilterPixel(PremultipliedColor original,
    PremultipliedColor filtered, bool preserveAlpha, float coverage = 1.0F) noexcept;
// Final storage boundary only: unassociate, encode sRGB, quantize RGBA8. Do not
// quantize between stages. Scalar output is clamped/quantized directly to R8.
// Cancellation/invalid input throws std::runtime_error / std::invalid_argument;
// no partially encoded byte vector escapes. The callback can be the same worker
// cancellation token used for convolution.
[[nodiscard]] std::vector<std::byte> quantizeSpatialPlane(SpatialPlaneView,
    const SpatialFilterOptions& = {});

} // namespace imageeditor::core
