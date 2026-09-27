#pragma once
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/Shape.hpp"
#include <utility>

namespace imageeditor::core {
struct ShapeGeometryState {
    ShapeLayer shape;
    AffineTransform transform;
    friend bool operator==(const ShapeGeometryState&, const ShapeGeometryState&) = default;
};

// One gesture resolves every preview from its original local geometry. The
// same handle/anchor/projection math serves Ctrl+T, but only shape coordinates
// take on resize magnitudes. The existing affine's scale/shear is preserved.
class ShapeResizeGesture final {
public:
    ShapeResizeGesture(ShapeLayer, AffineTransform, TransformHandle, Vec2d press);
    [[nodiscard]] const ShapeGeometryState& before() const { return before_; }
    [[nodiscard]] ShapeGeometryState releaseBefore() && noexcept { return std::move(before_); }
    [[nodiscard]] std::optional<ShapeGeometryState> resolve(Vec2d, TransformModifiers);

private:
    ShapeGeometryState before_;
    TransformHandle handle_;
    Vec2d press_;
    double signX_ { 1 }, signY_ { 1 };
};

class ResizeShapeCommand final : public Command {
public:
    ResizeShapeCommand(LayerId, ShapeGeometryState before, ShapeGeometryState after);
    bool apply(Document&) override;
    bool undo(Document&) override;
    bool canAdoptApplied(const Document&) const noexcept override;
    std::string_view label() const noexcept override { return "Resize shape"; }
    std::size_t memoryCost() const noexcept override;
    std::optional<std::uint64_t> activeLayerAfter(bool) const noexcept override { return id_; }

private:
    LayerId id_;
    ShapeGeometryState before_, after_;
};
}
