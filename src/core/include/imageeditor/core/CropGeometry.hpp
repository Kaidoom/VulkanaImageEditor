#pragma once
#include "imageeditor/core/Geometry.hpp"
#include <array>

namespace imageeditor::core {
// RectD is the stable frame, not the complete visibility shape. Coverage and
// containment must use this descriptor, never slice it to a frame first.
struct LayerCrop : RectD {
    std::array<double, 4> corners { }; // TL, TR, BR, BL; equal-axis local-pixel cuts.
    LayerCrop() = default;
    LayerCrop(RectD frame)
        : RectD(frame)
    {
    }
    LayerCrop(double left, double top, double w, double h)
        : RectD { left, top, w, h }
    {
    }
    [[nodiscard]] bool hasChamfer() const noexcept
    {
        return std::ranges::any_of(corners, [](double v) { return v > 0; });
    }
    [[nodiscard]] std::array<double, 4> resolvedCorners() const noexcept
    {
        auto result = corners;
        const auto limit = std::max(0.0, std::min(width, height) * .5);
        for (auto& value : result)
            value = std::clamp(value, 0.0, limit);
        return result;
    }
    [[nodiscard]] bool contains(Vec2d p) const noexcept
    {
        if (!RectD::contains(p))
            return false;
        const auto c = resolvedCorners();
        const auto u = p.x - x, v = p.y - y;
        return u + v >= c[0] && width - u + v >= c[1]
            && width - u + height - v >= c[2] && u + height - v >= c[3];
    }
    friend bool operator==(const LayerCrop&, const LayerCrop&) = default;
    friend bool operator==(const LayerCrop& a, const RectD& b)
    {
        return !a.hasChamfer() && static_cast<const RectD&>(a) == b;
    }
};
struct CropPolygon {
    std::array<Vec2d, 12> vertices { };
    std::size_t size { 0 };
};
// Clockwise boundary, starting at the top edge next to TL. No allocation.
[[nodiscard]] inline CropPolygon cropPolygon(const LayerCrop& r) noexcept
{
    const auto c = r.resolvedCorners();
    return { { { { r.x + c[0], r.y }, { r.right() - c[1], r.y }, { r.right(), r.y + c[1] },
                 { r.right(), r.bottom() - c[2] }, { r.right() - c[2], r.bottom() },
                 { r.x + c[3], r.bottom() }, { r.x, r.bottom() - c[3] }, { r.x, r.y + c[0] } } },
        8 };
}
[[nodiscard]] inline CropPolygon clippedCropPolygon(const LayerCrop& r, RectD clip) noexcept
{
    if (r.empty() || clip.empty())
        return { };
    auto polygon = cropPolygon(r);
    for (int side = 0; side < 4; ++side) {
        CropPolygon next;
        const auto boundary = side == 0 ? clip.x : side == 1 ? clip.right()
            : side == 2                                      ? clip.y
                                                             : clip.bottom();
        for (std::size_t i = 0; i < polygon.size; ++i) {
            const auto a = polygon.vertices[i], b = polygon.vertices[(i + 1) % polygon.size];
            const auto av = side < 2 ? a.x : a.y, bv = side < 2 ? b.x : b.y;
            const bool ia = (side == 0 || side == 2) ? av >= boundary : av <= boundary;
            const bool ib = (side == 0 || side == 2) ? bv >= boundary : bv <= boundary;
            if (ia)
                next.vertices[next.size++] = a;
            if (ia != ib)
                next.vertices[next.size++] = a + (b - a) * ((boundary - av) / (bv - av));
        }
        polygon = next;
        if (polygon.size < 3)
            return { };
    }
    return polygon;
}
[[nodiscard]] inline RectD croppedBounds(const LayerCrop& r, RectD source) noexcept
{
    if (!r.hasChamfer())
        return source.clippedTo(r);
    const auto polygon = clippedCropPolygon(r, source);
    if (!polygon.size)
        return { };
    auto low = polygon.vertices[0], high = low;
    for (std::size_t i = 1; i < polygon.size; ++i) {
        const auto p = polygon.vertices[i];
        low.x = std::min(low.x, p.x);
        low.y = std::min(low.y, p.y);
        high.x = std::max(high.x, p.x);
        high.y = std::max(high.y, p.y);
    }
    return { low.x, low.y, high.x - low.x, high.y - low.y };
}
} // namespace imageeditor::core
