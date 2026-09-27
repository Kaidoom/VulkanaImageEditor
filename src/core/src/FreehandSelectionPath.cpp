#include "imageeditor/core/FreehandSelectionPath.hpp"
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
bool FreehandSelectionPath::append(Vec2d point)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > maximumCoordinate
        || std::abs(point.y) > maximumCoordinate)
        throw std::invalid_argument("Lasso coordinates are outside the supported numeric range");
    if (!points_.empty() && points_.back() == point)
        return false;
    if (points_.size() >= 2) {
        const auto a = points_[points_.size() - 2], b = points_.back();
        const long double ax = static_cast<long double>(b.x) - a.x;
        const long double ay = static_cast<long double>(b.y) - a.y;
        const long double bx = static_cast<long double>(point.x) - b.x;
        const long double by = static_cast<long double>(point.y) - b.y;
        // Backtracking/retracing affects even-odd topology: never remove it.
        if (ax * by == ay * bx && ax * bx + ay * by >= 0) {
            points_.back() = point;
            return true;
        }
    }
    if (points_.size() >= maximumPoints)
        throw std::length_error("Lasso is too complex (maximum 65,536 path points); selection unchanged");
    points_.push_back(point);
    return true;
}
}
