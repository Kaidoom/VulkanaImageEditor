#pragma once

#include "imageeditor/core/Geometry.hpp"
#include <optional>

namespace imageeditor::core {

// Stateful document-space path filter, before distance resampling and tip
// generation. Unlike a shape preview, deposited ink cannot change direction
// retroactively: pick one 45-degree axis, then keep it until Shift is released.
class BrushLineConstraint {
public:
    void reset(Vec2d origin = {}, bool held = false) noexcept
    {
        anchor_ = position_ = origin;
        axis_.reset();
        held_ = held;
    }
    [[nodiscard]] Vec2d resolve(Vec2d point, bool held) noexcept
    {
        if (held != held_) {
            anchor_ = position_;
            axis_.reset();
            held_ = held;
        }
        if (!held_) return position_ = point;
        const auto delta = point - anchor_;
        if (!axis_) {
            // Ignore the first two document pixels of motion when choosing an
            // axis; no screen/DPI threshold or per-event allocation involved.
            const auto distance = std::hypot(delta.x, delta.y);
            if (distance < 2.0) return position_ = anchor_;
            const auto snapped = constrainLineEndpoint(anchor_, point) - anchor_;
            axis_ = snapped * (1.0 / distance);
        }
        // Projection (rather than radial distance) keeps off-axis wobble from
        // extending the line. Signed distance allows natural retracing.
        return position_ = anchor_ + *axis_ * (delta.x * axis_->x + delta.y * axis_->y);
    }
    [[nodiscard]] std::optional<Vec2d> constrainedPosition() const noexcept
    { return held_ ? std::optional(position_) : std::nullopt; }

private:
    Vec2d anchor_ {}, position_ {};
    std::optional<Vec2d> axis_;
    bool held_ {false};
};

} // namespace imageeditor::core
