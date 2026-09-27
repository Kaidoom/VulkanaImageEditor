#include "imageeditor/core/ShapeResize.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
namespace {
    bool matches(const Document& doc, LayerId id, const ShapeGeometryState& state)
    {
        const auto* layer = doc.layer(id);
        const auto* shape = layer ? std::get_if<ShapeLayer>(&layer->payload) : nullptr;
        return shape && *shape == state.shape && layer->localToDocument == state.transform;
    }
}
ShapeResizeGesture::ShapeResizeGesture(ShapeLayer shape, AffineTransform transform,
    TransformHandle handle, Vec2d press)
    : before_ { std::move(shape), transform }
    , handle_(handle)
    , press_(press)
{
    if (!validShape(before_.shape) || !transform.inverted()
        || handle < TransformHandle::TopLeft || handle > TransformHandle::Left)
        throw std::invalid_argument("Invalid shape resize gesture");
}

std::optional<ShapeGeometryState> ShapeResizeGesture::resolve(Vec2d point, TransformModifiers modifiers)
{
    const auto& original = before_.shape;
    auto box = resolveResizeGeometry(original.size, before_.transform, handle_, press_, point, modifiers, false);
    if (!box || std::abs(box->signedExtent.width) > maximumShapeDimension
        || std::abs(box->signedExtent.height) > maximumShapeDimension)
        return { };
    auto width = box->signedExtent.width, height = box->signedExtent.height;
    // Snap only numerical dust. Zero is real geometry, not a singular affine.
    if (std::abs(width) < 1e-9)
        width = 0;
    if (std::abs(height) < 1e-9)
        height = 0;
    if (width != 0)
        signX_ = std::copysign(1.0, width);
    if (height != 0)
        signY_ = std::copysign(1.0, height);
    if (std::abs(width - original.size.width) < 1e-8
        && std::abs(height - original.size.height) < 1e-8)
        return before_;

    auto result = before_;
    result.shape.size = { std::abs(width), std::abs(height) };
    for (std::size_t i = 0; i < result.shape.points.size(); ++i) {
        const auto source = original.points[i];
        // A horizontal/vertical line can acquire a span without dividing by
        // zero. Its ordered endpoints define the missing normalized axis.
        const auto fallback = original.kind == ShapeKind::Line ? double(i) : 0.0;
        const double x = original.size.width > 0 ? source.x / original.size.width : fallback;
        const double y = original.size.height > 0 ? source.y / original.size.height : fallback;
        result.shape.points[i] = { std::clamp(x * result.shape.size.width, 0.0, result.shape.size.width),
            std::clamp(y * result.shape.size.height, 0.0, result.shape.size.height) };
    }
    // Do not add resize magnitudes to the matrix or compensate stroke with an
    // average scale. Existing Ctrl+T linear geometry remains exact, except for
    // deliberate sign flips. Requested radius and stroke width stay unchanged.
    auto& t = result.transform;
    t=composeTransform(t,{signX_,0,0,0,signY_,0});
    const auto anchor = before_.transform.map({ box->unitAnchor.x * original.size.width,
        box->unitAnchor.y * original.size.height });
    const Vec2d local { box->unitAnchor.x * result.shape.size.width,
        box->unitAnchor.y * result.shape.size.height };
    const auto delta=anchor-t.map(local);
    t=composeTransform({1,0,delta.x,0,1,delta.y},t);
    if (!validShape(result.shape) || !t.validOver({0,0,std::max(1.0,result.shape.size.width),std::max(1.0,result.shape.size.height)}))
        return { };
    return result;
}

ResizeShapeCommand::ResizeShapeCommand(LayerId id, ShapeGeometryState before, ShapeGeometryState after)
    : id_(id)
    , before_(std::move(before))
    , after_(std::move(after))
{
    if (!validShape(before_.shape) || !validShape(after_.shape)
        || !before_.transform.inverted() || !after_.transform.inverted())
        throw std::invalid_argument("Invalid shape resize command");
}
bool ResizeShapeCommand::apply(Document& doc)
{
    return before_ != after_ && matches(doc, id_, before_)
        && doc.setLayerShapeGeometry(id_, after_.shape, after_.transform);
}
bool ResizeShapeCommand::undo(Document& doc)
{
    return matches(doc, id_, after_) && doc.setLayerShapeGeometry(id_, before_.shape, before_.transform);
}
bool ResizeShapeCommand::canAdoptApplied(const Document& doc) const noexcept
{
    return before_ != after_ && matches(doc, id_, after_);
}
std::size_t ResizeShapeCommand::memoryCost() const noexcept
{
    return sizeof(*this) + shapeMemoryCost(before_.shape) + shapeMemoryCost(after_.shape);
}
}
