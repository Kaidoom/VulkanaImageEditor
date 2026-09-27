#pragma once

#include "imageeditor/core/Geometry.hpp"
#include <span>
#include <vector>

namespace imageeditor::core {

// Document-space input only. No smoothing, resampling or vertex clamping.
// Consecutive duplicates and exactly collinear forward middle points are the
// only points removed: retained geometry has zero simplification error.
class FreehandSelectionPath {
public:
    static constexpr std::size_t maximumPoints = 65536;
    static constexpr double maximumCoordinate = 1.0e9;
    bool append(Vec2d);
    [[nodiscard]] std::span<const Vec2d> points() const noexcept { return points_; }

private:
    std::vector<Vec2d> points_;
};

} // namespace imageeditor::core
