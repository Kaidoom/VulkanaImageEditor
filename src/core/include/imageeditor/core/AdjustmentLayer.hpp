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
    std::size_t stopBefore = adjustmentCount, BlendMode mode = BlendMode::Normal,
    std::uint32_t seed = defaultBlendSeed) noexcept
{
    if (!program || !program->active || input[3] <= 0 || strength <= 0) return input;
    const auto corrected = evaluateAdjustments(*program, input, local, stopBefore);
    if(mode==BlendMode::Normal) {
        strength = std::clamp(strength, 0.0F, 1.0F);
        for (std::size_t i=0; i<3; ++i)
            input[i] = strength == 1 ? corrected[i] : input[i] + (corrected[i]-input[i])*strength;
        return input;
    }
    const auto c=blend_detail::bAdjustmentBlend({input[0],input[1],input[2],input[3]},
        {corrected[0],corrected[1],corrected[2],corrected[3]},strength,int(mode),float(local.x),float(local.y),seed);
    return {c.x,c.y,c.z,c.w};
}
} // namespace imageeditor::core
