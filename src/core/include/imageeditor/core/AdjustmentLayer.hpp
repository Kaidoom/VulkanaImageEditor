#pragma once
#include "imageeditor/core/Adjustments.hpp"
#include <algorithm>

namespace imageeditor::core {
enum class AdjustmentScope : std::uint8_t { AllBelow, ThisGroup };
// A stack operator, never an image surface. ThisGroup establishes a local
// compositing domain on its immediate container, including while bypassed.
// The scope owns that boundary: moving/removing it cannot leave orphan state.
struct AdjustmentLayer {
    AdjustmentScope scope {AdjustmentScope::AllBelow};
    static constexpr std::uint32_t domainPolicyVersion = 1;
    friend bool operator==(const AdjustmentLayer&, const AdjustmentLayer&) = default;
};
inline PremultipliedColor compositeAdjustment(const CompiledAdjustmentStack* program,
    PremultipliedColor input, Vec2d local, float strength,
    std::size_t stopBefore = adjustmentCount) noexcept
{
    if (!program || !program->active || input[3] <= 0 || strength <= 0) return input;
    const auto corrected = evaluateAdjustments(*program, input, local, stopBefore);
    strength = std::clamp(strength, 0.0F, 1.0F);
    for (std::size_t i=0; i<3; ++i)
        input[i] = strength == 1 ? corrected[i] : input[i] + (corrected[i]-input[i])*strength;
    return input;
}
} // namespace imageeditor::core
