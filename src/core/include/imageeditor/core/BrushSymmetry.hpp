#pragma once

#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Geometry.hpp"
#include <optional>
#include <string>

namespace imageeditor::core {

enum class BrushSymmetryAxis { Vertical, Horizontal };

// Document-space painting setup. A disabled setup can retain its last center;
// an unset center means the user has not positioned the axis yet.
struct BrushSymmetry {
    bool enabled {false};
    BrushSymmetryAxis axis {BrushSymmetryAxis::Vertical};
    std::optional<Vec2d> center;
    friend bool operator==(const BrushSymmetry&, const BrushSymmetry&) = default;
};

[[nodiscard]] bool validBrushSymmetry(const BrushSymmetry&) noexcept;
[[nodiscard]] Vec2d reflectedBrushPoint(Vec2d, const BrushSymmetry&) noexcept;

class ChangeBrushSymmetryCommand final : public Command {
public:
    ChangeBrushSymmetryCommand(BrushSymmetry before, BrushSymmetry after,
        std::string label = "Brush symmetry");
    bool apply(Document&) override;
    bool undo(Document&) override;
    std::string_view label() const noexcept override { return label_; }
    std::size_t memoryCost() const noexcept override { return sizeof(*this) + label_.capacity(); }

private:
    BrushSymmetry before_, after_;
    std::string label_;
};

} // namespace imageeditor::core
