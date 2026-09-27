#pragma once

#include "imageeditor/core/Geometry.hpp"

#include <cstddef>
#include <vector>

namespace imageeditor::core {

enum class ShapeKind { Rectangle, RoundedRectangle, Ellipse, Triangle, Line, Polygon };

// Authoritative geometry is measured in layer-local document pixels (+X right,
// +Y down). The canonical frame starts at (0,0); viewport zoom never changes it.
// Rectangle/ellipse/triangle use size. Line has two points; polygon has >=3,
// with points inside that frame. Polygon fill is even-odd, including crossings.
struct ShapeLayer {
    ShapeKind kind {ShapeKind::Rectangle};
    Extent2d size {100, 100};
    std::vector<Vec2d> points;
    // Radius is retained verbatim; rendering clamps to half the smaller axis.
    double cornerRadius {12};
    bool fillEnabled {true};
    bool strokeEnabled {false};
    Rgba8 fillColor {255, 255, 255, 255};
    Rgba8 strokeColor {0, 0, 0, 255};
    // Centered, non-cosmetic stroke, round joins and round line caps in V1.
    // Zero width paints no stroke, never a device-pixel hairline. Geometry and
    // stroke transform together (including anisotropic scaling and flips).
    double strokeWidth {2};

    friend bool operator==(const ShapeLayer& a, const ShapeLayer& b)
    {
        return a.kind == b.kind && a.size.width == b.size.width && a.size.height == b.size.height
            && a.points == b.points && a.cornerRadius == b.cornerRadius
            && a.fillEnabled == b.fillEnabled && a.strokeEnabled == b.strokeEnabled
            && a.fillColor == b.fillColor && a.strokeColor == b.strokeColor
            && a.strokeWidth == b.strokeWidth;
    }
};

inline constexpr double maximumShapeDimension = 1'000'000;
inline constexpr double maximumShapeStyleDimension = 100'000;
inline constexpr std::size_t maximumShapePoints = 100'000;

// Admission and creation are distinct: degenerate but structurally valid data
// can be reopened/edited safely, while an empty gesture creates no layer.
[[nodiscard]] bool validShape(const ShapeLayer&) noexcept;
[[nodiscard]] bool hasShapeGeometry(const ShapeLayer&) noexcept;
// Enclosed interior independent of paint alpha/fill enablement, for picking
// hollow closed shapes. Line proximity is a screen-space concern at the UI
// path-service boundary; a line has no enclosed interior here.
[[nodiscard]] bool shapeContainsPoint(const ShapeLayer&, Vec2d localPoint) noexcept;
// Logical transform frame excludes stroke/AA outsets. Disposable render caches
// contain those outsets and supply an independent pixelsToLocal offset.
[[nodiscard]] Extent2u shapeGeometryExtent(const ShapeLayer&) noexcept;
[[nodiscard]] std::size_t shapeMemoryCost(const ShapeLayer&) noexcept;

} // namespace imageeditor::core
