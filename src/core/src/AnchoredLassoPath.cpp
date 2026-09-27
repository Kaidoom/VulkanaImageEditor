#include "imageeditor/core/AnchoredLassoPath.hpp"
#include "imageeditor/core/FreehandSelectionPath.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace imageeditor::core {
namespace {
    void validate(Vec2d p)
    {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)
            || std::abs(p.x) > FreehandSelectionPath::maximumCoordinate
            || std::abs(p.y) > FreehandSelectionPath::maximumCoordinate)
            throw std::invalid_argument("Invalid lasso vertex");
    }
}
bool AnchoredLassoPath::addVertex(Vec2d point)
{
    validate(point);
    if (!points_.empty()) {
        const std::array segment { points_.back(), point };
        return addSegment(segment);
    }
    points_.reserve(8);
    ends_.reserve(8);
    points_.push_back(point);
    ends_.push_back(1);
    return true;
}
bool AnchoredLassoPath::addSegment(std::span<const Vec2d> segment)
{
    if (segment.empty())
        return false;
    if (points_.empty() || segment.front() != points_.back())
        throw std::invalid_argument("Lasso segment does not start at its anchor");
    std::size_t added = 0;
    auto last = points_.back();
    for (auto p : segment) {
        validate(p);
        if (p != last) {
            ++added;
            last = p;
        }
    }
    if (!added)
        return false;
    if (added > FreehandSelectionPath::maximumPoints - points_.size())
        throw std::length_error("Lasso exceeds 65,536 points");
    if (points_.capacity() < points_.size() + added)
        points_.reserve(std::max(points_.size() + added, points_.capacity() * 2));
    if (ends_.size() == ends_.capacity())
        ends_.reserve(ends_.size() * 2);
    for (auto p : segment)
        if (p != points_.back())
            points_.push_back(p);
    ends_.push_back(points_.size());
    return true;
}
bool AnchoredLassoPath::removeLastAnchor()
{
    if (ends_.empty())
        return false;
    ends_.pop_back();
    points_.resize(ends_.empty() ? 0 : ends_.back());
    return true;
}
std::vector<Vec2d> AnchoredLassoPath::anchors() const
{
    std::vector<Vec2d> result;
    result.reserve(ends_.size());
    for (auto end : ends_)
        result.push_back(points_[end - 1]);
    return result;
}
}
