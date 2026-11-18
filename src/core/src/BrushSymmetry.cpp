#include "imageeditor/core/BrushSymmetry.hpp"
#include "imageeditor/core/Document.hpp"
#include <cmath>
#include <utility>

namespace imageeditor::core {

bool validBrushSymmetry(const BrushSymmetry& symmetry) noexcept
{
    if (symmetry.axis != BrushSymmetryAxis::Vertical && symmetry.axis != BrushSymmetryAxis::Horizontal)
        return false;
    if (!symmetry.center)
        return !symmetry.enabled;
    return std::isfinite(symmetry.center->x) && std::isfinite(symmetry.center->y)
        && std::abs(symmetry.center->x) <= 1e9 && std::abs(symmetry.center->y) <= 1e9;
}

Vec2d reflectedBrushPoint(Vec2d point, const BrushSymmetry& symmetry) noexcept
{
    if (!symmetry.enabled || !symmetry.center)
        return point;
    if (symmetry.axis == BrushSymmetryAxis::Vertical)
        point.x = 2 * symmetry.center->x - point.x;
    else
        point.y = 2 * symmetry.center->y - point.y;
    return point;
}

ChangeBrushSymmetryCommand::ChangeBrushSymmetryCommand(
    BrushSymmetry before, BrushSymmetry after, std::string label)
    : before_(before), after_(after), label_(std::move(label))
{
}

bool ChangeBrushSymmetryCommand::apply(Document& document)
{
    return document.setBrushSymmetry(before_, after_);
}

bool ChangeBrushSymmetryCommand::undo(Document& document)
{
    return document.setBrushSymmetry(after_, before_);
}

} // namespace imageeditor::core
