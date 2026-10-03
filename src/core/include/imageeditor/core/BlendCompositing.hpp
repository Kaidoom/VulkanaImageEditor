#pragma once
#include "imageeditor/core/BlendMode.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/RasterSurface.hpp"

namespace imageeditor::core {
// Straight sRGB RGBA8 is authoritative storage. Consumers filter decoded,
// premultiplied linear colors and accumulate without intermediate RGBA8 rounds.
using PremultipliedColor = std::array<float, 4>;
namespace blend_detail {
struct BVec3 { float x{}, y{}, z{}; };
struct BVec4 { float x{}, y{}, z{}, w{}; };
inline BVec3 operator+(BVec3 a, BVec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline BVec3 operator-(BVec3 a, BVec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline BVec3 operator*(BVec3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
inline float bMin(float a,float b) { return std::min(a,b); }
inline float bMax(float a,float b) { return std::max(a,b); }
inline float bClamp(float v,float a,float b) { return std::clamp(v,a,b); }
inline float bPow(float a,float b) { return std::pow(a,b); }
inline float bSqrt(float a) { return std::sqrt(a); }
inline float bAbs(float a) { return std::abs(a); }
#define B_INLINE inline
#include "imageeditor/core/detail/BlendMath.inc"
#include "imageeditor/core/detail/ClippingMath.inc"
#undef B_INLINE
}
[[nodiscard]] inline PremultipliedColor compositeLayer(PremultipliedColor backdrop,
    PremultipliedColor source, float opacity, BlendMode mode) noexcept
{
    const auto c = blend_detail::bComposite({backdrop[0],backdrop[1],backdrop[2],backdrop[3]},
        {source[0],source[1],source[2],source[3]},opacity,static_cast<int>(mode));
    return {c.x,c.y,c.z,c.w};
}
[[nodiscard]] inline PremultipliedColor decodeColor(Rgba8 c) noexcept
{
    const float a = float(c.alpha)/255.0F;
    return {float(srgbToLinear(c.red))*a,float(srgbToLinear(c.green))*a,float(srgbToLinear(c.blue))*a,a};
}
[[nodiscard]] inline Rgba8 encodeColor(PremultipliedColor c) noexcept
{
    if (c[3] <= 0) return {};
    return {linearToSrgb(double(c[0])/c[3]),linearToSrgb(double(c[1])/c[3]),
        linearToSrgb(double(c[2])/c[3]),alphaToByte(c[3])};
}
}
