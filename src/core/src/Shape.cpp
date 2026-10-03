#include "imageeditor/core/Shape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace imageeditor::core {
namespace {
bool bounded(double value, double maximum) noexcept
{
    return std::isfinite(value) && value >= 0 && value <= maximum;
}

bool polygonContainsPoint(std::span<const Vec2d> points, Vec2d point) noexcept
{
    bool inside = false;
    auto previous = points.back();
    for (const auto current : points) {
        const auto segment = current - previous;
        const auto delta = point - previous;
        const auto cross = segment.x * delta.y - segment.y * delta.x;
        if (cross == 0 && point.x >= std::min(current.x, previous.x)
            && point.x <= std::max(current.x, previous.x)
            && point.y >= std::min(current.y, previous.y)
            && point.y <= std::max(current.y, previous.y))
            return true;
        if ((current.y > point.y) != (previous.y > point.y)
            && point.x < previous.x + (point.y - previous.y) * segment.x / segment.y)
            inside = !inside;
        previous = current;
    }
    return inside;
}
}

bool validShape(const ShapeLayer& shape) noexcept
{
    if (!bounded(shape.size.width, maximumShapeDimension)
        || !bounded(shape.size.height, maximumShapeDimension)
        || !bounded(shape.cornerRadius, maximumShapeStyleDimension)
        || !bounded(shape.strokeWidth, maximumShapeStyleDimension)
        || !bounded(shape.strokeMiterLimit, 1000) || shape.strokeMiterLimit < 0.5
        || static_cast<unsigned>(shape.strokeJoin)>2 || static_cast<unsigned>(shape.strokeCap)>2
        || shape.points.size() > maximumShapePoints)
        return false;
    switch (shape.kind) {
    case ShapeKind::Rectangle:
    case ShapeKind::RoundedRectangle:
    case ShapeKind::Ellipse:
    case ShapeKind::Triangle:
        if (!shape.points.empty()) return false;
        break;
    case ShapeKind::Line:
        if (shape.points.size() != 2) return false;
        break;
    case ShapeKind::Polygon:
        if (shape.points.size() < 3) return false;
        break;
    default:
        return false;
    }
    return std::all_of(shape.points.begin(), shape.points.end(), [&shape](Vec2d point) {
        return bounded(point.x, shape.size.width) && bounded(point.y, shape.size.height);
    });
}

bool hasShapeGeometry(const ShapeLayer& shape) noexcept
{
    if (!validShape(shape)) return false;
    if (shape.kind == ShapeKind::Line) return shape.points[0] != shape.points[1];
    if (shape.size.width <= 0 || shape.size.height <= 0) return false;
    if (shape.kind != ShapeKind::Polygon) return true;

    // Do not use signed area: valid even-odd figure-eights can sum to zero.
    const auto first = shape.points.front();
    const auto second = std::find_if(shape.points.begin() + 1, shape.points.end(),
        [first](Vec2d point) { return point != first; });
    if (second == shape.points.end()) return false;
    const auto axis = *second - first;
    const auto norm = std::max(std::abs(axis.x), std::abs(axis.y));
    const auto unit = axis * (1 / norm);
    return std::any_of(second + 1, shape.points.end(), [first, unit](Vec2d point) {
        const auto delta = point - first;
        const auto length = std::max(std::abs(delta.x), std::abs(delta.y));
        if (length == 0) return false;
        return std::abs(unit.x * (delta.y / length) - unit.y * (delta.x / length)) > 1e-12;
    });
}

bool shapeContainsPoint(const ShapeLayer& shape, Vec2d point) noexcept
{
    if (!hasShapeGeometry(shape) || shape.kind == ShapeKind::Line
        || !bounded(point.x, shape.size.width) || !bounded(point.y, shape.size.height))
        return false;
    switch (shape.kind) {
    case ShapeKind::Rectangle:
        return true;
    case ShapeKind::RoundedRectangle: {
        const auto radius = std::min({shape.cornerRadius, shape.size.width * 0.5, shape.size.height * 0.5});
        if (radius == 0) return true;
        const auto dx = (point.x - std::clamp(point.x, radius, shape.size.width - radius)) / radius;
        const auto dy = (point.y - std::clamp(point.y, radius, shape.size.height - radius)) / radius;
        return dx * dx + dy * dy <= 1;
    }
    case ShapeKind::Ellipse: {
        const auto x = 2 * point.x / shape.size.width - 1;
        const auto y = 2 * point.y / shape.size.height - 1;
        return x * x + y * y <= 1;
    }
    case ShapeKind::Triangle: {
        const std::array<Vec2d, 3> points {{{shape.size.width * 0.5, 0},
            {shape.size.width, shape.size.height}, {0, shape.size.height}}};
        return polygonContainsPoint(points, point);
    }
    case ShapeKind::Polygon:
        return polygonContainsPoint(shape.points, point);
    case ShapeKind::Line:
        return false;
    }
    return false;
}

Extent2u shapeGeometryExtent(const ShapeLayer& shape) noexcept
{
    if (!validShape(shape)) return {};
    const auto minimum = shape.kind == ShapeKind::Line ? 1.0 : 0.0;
    return {static_cast<std::uint32_t>(std::max(minimum, std::ceil(shape.size.width))),
        static_cast<std::uint32_t>(std::max(minimum, std::ceil(shape.size.height)))};
}

std::size_t shapeMemoryCost(const ShapeLayer& shape) noexcept
{
    return shape.points.capacity() * sizeof(Vec2d);
}
} // namespace imageeditor::core
