#pragma once
#include "imageeditor/core/Geometry.hpp"
#include <span>
#include <vector>

namespace imageeditor::core {
enum class LassoMode { Freehand, Polygonal, Magnetic };

// Construction geometry only. Checkpoints never enter document history.
class AnchoredLassoPath {
public:
    bool addVertex(Vec2d);
    // Segment starts at the current anchor. Consecutive duplicate junctions
    // are removed, but intentional turns and retracing remain intact.
    bool addSegment(std::span<const Vec2d>);
    bool removeLastAnchor();
    [[nodiscard]] std::span<const Vec2d> points() const { return points_; }
    [[nodiscard]] std::vector<Vec2d> anchors() const;
    [[nodiscard]] std::size_t anchorCount() const { return ends_.size(); }

private:
    std::vector<Vec2d> points_;
    std::vector<std::size_t> ends_;
};
}
