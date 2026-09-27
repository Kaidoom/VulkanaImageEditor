#include "imageeditor/core/ShapeCommands.hpp"

#include "imageeditor/core/Document.hpp"

#include <new>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
SetShapeCommand::SetShapeCommand(LayerId id, ShapeLayer before, ShapeLayer after, std::uint64_t mergeKey)
    : id_(id), before_(std::move(before)), after_(std::move(after)), mergeKey_(mergeKey)
{
    if (!validShape(before_) || !validShape(after_))
        throw std::invalid_argument("Shape command requires valid geometry and style");
}

bool SetShapeCommand::apply(Document& document)
{
    const auto* layer = document.layer(id_);
    const auto* shape = layer ? std::get_if<ShapeLayer>(&layer->payload) : nullptr;
    return shape && *shape == before_ && before_ != after_ && document.setLayerShape(id_, after_);
}

bool SetShapeCommand::undo(Document& document)
{
    const auto* layer = document.layer(id_);
    const auto* shape = layer ? std::get_if<ShapeLayer>(&layer->payload) : nullptr;
    return shape && *shape == after_ && document.setLayerShape(id_, before_);
}

bool SetShapeCommand::mergeWith(const Command& command)
{
    const auto* next = dynamic_cast<const SetShapeCommand*>(&command);
    if (!next || !mergeKey_ || mergeKey_ != next->mergeKey_ || id_ != next->id_
        || after_ != next->before_ || next->after_ == before_)
        return false;
    // Like text commands: allocation failure after admission keeps two valid
    // undo steps instead of throwing after the document was already changed.
    try {
        auto after = next->after_;
        after_ = std::move(after);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool SetShapeCommand::canAdoptApplied(const Document& document) const noexcept
{
    const auto* layer = document.layer(id_);
    const auto* shape = layer ? std::get_if<ShapeLayer>(&layer->payload) : nullptr;
    return shape && before_ != after_ && *shape == after_;
}

std::size_t SetShapeCommand::memoryCost() const noexcept
{
    return sizeof(*this) + shapeMemoryCost(before_) + shapeMemoryCost(after_);
}
} // namespace imageeditor::core
