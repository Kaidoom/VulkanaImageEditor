#pragma once

#include "imageeditor/core/BlendCompositing.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace imageeditor::core {

// Immutable row-major evaluation grid. Direct raw repair uses raster-local
// texels; a rendered-reference consumer may choose a document-pixel grid.
// Colors are premultiplied LINEAR RGBA, not stored sRGB bytes. Unknown pixels
// are never read as appearance evidence, at any resolution. Nonzero means
// unknown/valid respectively. A missing valid span means all grid pixels are
// geometrically valid; zero-alpha samples still supply no color evidence.
// The consumer excludes interpolation support touching marked damage from
// valid; this solver additionally validates entire patches/descriptor support.
// Soft brush strength, opacity, selection and final write coverage are NOT
// inputs: apply them once to the returned candidate against pre-stroke pixels.
struct SpotHealPatch {
    int width {0};
    int height {0};
    std::span<const PremultipliedColor> pixels;
    std::span<const std::uint8_t> unknown;
    std::span<const std::uint8_t> valid;
};

struct SpotHealOptions {
    std::uint64_t seed {0x564b53504f544831ULL};
    std::size_t maxPixels {16U * 1024U * 1024U};
    std::size_t maxWorkingBytes {768U * 1024U * 1024U};
    std::uint64_t maxComparisons {256ULL * 1024ULL * 1024ULL};
    // Surrounding-tone adaptation, not opacity. Diagnostic comparisons can
    // disable this to inspect the texture-preserving reconstruction directly.
    float adaptation {0.8F};
    bool captureDiagnostics {false};
    // Development profiling is opt-in; final performance runs leave it off.
    bool captureProfile {false};
    // 0 chooses a bounded automatic count; 1 is the serial reference schedule.
    unsigned workerCount {0};
    std::function<bool()> cancelled;
    std::function<void(double)> progress;
};

enum class SpotHealStatus {
    Complete,
    NoUnknownPixels,
    InsufficientContext,
    Cancelled,
    InvalidInput,
    LimitExceeded,
};

struct SpotHealDonor {
    // Center of a wholly valid original source patch in the input grid.
    // -1 denotes no assignment (known/outside the reconstruction region).
    int x {-1};
    int y {-1};
    friend bool operator==(const SpotHealDonor&, const SpotHealDonor&) = default;
};

struct SpotHealCounters {
    std::uint64_t proposals {}, invalidProposals {}, duplicateProposals {};
    std::uint64_t scoredCandidates {}, earlyRejectedCandidates {}, acceptedCandidates {}, patchSamples {};
    double acceptedImprovement {};
};

struct SpotHealIterationProfile {
    unsigned iteration {};
    double searchMilliseconds {}, reconstructionMilliseconds {}, objectiveSum {};
    std::uint64_t changedAssignments {};
    SpotHealCounters counters;
};

struct SpotHealLevelProfile {
    unsigned level {};
    int width {}, height {};
    unsigned radius {}, thickness {};
    std::size_t unknownPixels {}, activePixels {}, validDonors {};
    double initializationMilliseconds {};
    double initializationUnknownMilliseconds {}, initializationContextMilliseconds {};
    SpotHealCounters initialization;
    std::vector<SpotHealIterationProfile> iterations;
};

struct SpotHealProfile {
    unsigned workersUsed {1};
    double preparationMilliseconds {}, smoothMilliseconds {}, adaptationMilliseconds {}, totalMilliseconds {};
    std::vector<SpotHealLevelProfile> levels;
};

struct SpotHealDiagnostics {
    std::size_t unknownPixels {0};
    std::size_t validDonorCenters {0};
    std::size_t estimatedPeakWorkingBytes {0};
    std::uint64_t comparisons {0};
    unsigned pyramidLevels {0};
    unsigned patchRadius {0};
    unsigned refinementIterations {0};
    // Independently validated near-affine context can reconstruct a smooth
    // component without exemplar quantization/offset noise. Never a blanket
    // diffusion pass over textured output.
    std::size_t smoothModelPixels {0};
    bool adaptationApplied {false};
    SpotHealProfile profile;
    std::string message;
    // Optional full-resolution grids, populated only when requested. Donors
    // are patch assignments; invalidDonors is 255 when a center is forbidden.
    std::vector<SpotHealDonor> donors;
    std::vector<std::uint8_t> invalidDonors;
    std::vector<PremultipliedColor> beforeAdaptation;
};

struct SpotHealResult {
    SpotHealStatus status {SpotHealStatus::InvalidInput};
    // Full input-sized candidate. Known pixels are unchanged input samples;
    // only unknown pixels are proposed reconstruction. No soft blending or
    // final storage quantization has happened. Empty on failure/cancellation.
    std::vector<PremultipliedColor> pixels;
    SpotHealDiagnostics diagnostics;
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == SpotHealStatus::Complete || status == SpotHealStatus::NoUnknownPixels;
    }
};

// Original paper-based implementation. Synchronous, deterministic, bounded and
// cancellable; scheduling and stale-source/target checks belong to the caller.
// No document, surface, Qt or history ownership is retained. Independent work
// borrows a bounded reusable executor; every task joins before this call returns.
[[nodiscard]] SpotHealResult repairSpotHeal(const SpotHealPatch&, const SpotHealOptions& = {});

} // namespace imageeditor::core
