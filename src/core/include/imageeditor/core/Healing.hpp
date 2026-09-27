#pragma once

#include "imageeditor/core/BlendCompositing.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace imageeditor::core {

// All arrays use the same row-major target-texel grid. Source pixels have already
// been mapped to their corresponding document-space source positions. Include
// at least six context texels around coverage where the images permit it.
// destination is a coherent pre-stroke raw or rendered reference, including the
// visible underlying image when painting on an empty Normal retouch layer.
struct HealingPatch {
    int width {0};
    int height {0};
    std::span<const PremultipliedColor> source;
    std::span<const PremultipliedColor> destination;
    // Accumulated write coverage, including selection holes. Positive coverage
    // defines the defect region; its magnitude is NOT applied by this operator.
    std::span<const float> coverage;
};

struct HealingOptions {
    // Fraction of the reconstructed spatial illumination correction to apply.
    // Zero is exactly Stamp; one requests full surrounding-tone adaptation.
    float adaptation {0.8F};
    unsigned maxIterations {400};
    std::size_t maxPixels {1024U * 1024U};
    float relativeTolerance {1.0e-4F};
    // True cancels. Called on the invoking thread, never from a worker owned by
    // this function. The callback must not mutate either input array.
    std::function<bool()> cancelled;
};

enum class HealingStatus {
    Converged,
    PartialFallback,
    StampFallback,
    Cancelled,
    InvalidInput,
    LimitExceeded,
};

struct HealingDiagnostics {
    std::size_t components {0};
    std::size_t healedComponents {0};
    std::size_t fallbackComponents {0};
    std::size_t affectedPixels {0};
    std::size_t healedPixels {0};
    std::size_t fallbackPixels {0};
    std::size_t contextSamples {0};
    std::size_t rejectedContextSamples {0};
    std::size_t unconvergedComponents {0};
    // Maximum iterations/residual among the channel solves, not a sum.
    unsigned iterations {0};
    float relativeResidual {0};
    // Maximum actual residual RMS. Relative residual can be large when the RHS
    // is nearly zero but this absolute value is already below the 1e-8 floor.
    float absoluteResidualRms {0};
    // Conservative operator-owned peak estimate; excludes caller input arrays.
    std::size_t estimatedScratchBytes {0};
    // Suitable for an interactive status message; fallback must remain visible.
    std::string message;
};

struct HealingResult {
    HealingStatus status {HealingStatus::InvalidInput};
    // Unmasked premultiplied linear source RGBA, including source alpha. The
    // caller applies its brush opacity/flow/selection and source-over ONCE.
    // Pixels outside coverage are unchanged source samples, not proposed writes.
    // Empty on invalid input, memory limit, or cancellation: never publish these.
    std::vector<PremultipliedColor> pixels;
    HealingDiagnostics diagnostics;
};

// Dependency-free, bounded, synchronous and cancellable. Components lacking
// valid surrounding context or numerical convergence retain Stamp pixels and
// are explicitly reported. No destination RGB under zero alpha is ever read.
[[nodiscard]] HealingResult healPatch(const HealingPatch&, const HealingOptions& = {});

} // namespace imageeditor::core
