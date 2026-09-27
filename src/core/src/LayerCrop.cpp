#include "imageeditor/core/LayerCrop.hpp"
namespace imageeditor::core {
namespace {
    struct V2 {
        float x, y;
    };
    struct V4 {
        float x, y, z, w;
    };
    V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
    V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.y - b.y }; }
    V2 operator*(V2 a, float b) { return { a.x * b, a.y * b }; }
    V2 cAbs(V2 a) { return { std::abs(a.x), std::abs(a.y) }; }
    float cAbs(float a) { return std::abs(a); }
    float cClamp(float a, float low, float high)
    {
        return std::clamp(a, low, high);
    }
#define C_INLINE inline
#define CVec2 V2
#define CVec4 V4
#include "imageeditor/core/detail/CropMath.inc"
#undef C_INLINE
#undef CVec2
#undef CVec4
} // namespace
float layerCropCoverage(const LayerCrop& r, Vec2d center, Vec2d dx, Vec2d dy) noexcept
{
    if (!validLayerCrop(r) || r.empty())
        return 0;
    for (double v : { center.x, center.y, dx.x, dx.y, dy.x, dy.y })
        if (!std::isfinite(v))
            return 0;
    const auto c = r.resolvedCorners();
    return cCoverage({ float(dx.x), float(dx.y) }, { float(dy.x), float(dy.y) },
        { float(r.x - center.x), float(r.y - center.y),
            float(r.right() - center.x), float(r.bottom() - center.y) },
        { float(c[0]), float(c[1]), float(c[2]), float(c[3]) });
}
PremultipliedColor applyLayerCrop(PremultipliedColor color,
    const std::optional<LayerCrop>& crop,
    const AffineTransform& inverse,
    Vec2d point) noexcept
{
    if (!crop)
        return color;
    const auto derivatives=inverse.derivatives(point);
    const float coverage = layerCropCoverage(*crop, inverse.map(point), derivatives[0], derivatives[1]);
    for (auto& channel : color)
        channel *= coverage;
    return color;
}
} // namespace imageeditor::core
