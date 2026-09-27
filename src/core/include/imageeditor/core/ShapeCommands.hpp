#pragma once

#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Layer.hpp"

namespace imageeditor::core {

class SetShapeCommand final : public Command {
public:
    SetShapeCommand(LayerId, ShapeLayer before, ShapeLayer after, std::uint64_t mergeKey = 0);
    bool apply(Document&) override;
    bool undo(Document&) override;
    bool mergeWith(const Command&) override;
    [[nodiscard]] bool canAdoptApplied(const Document&) const noexcept override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Edit shape"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] std::optional<std::uint64_t> activeLayerAfter(bool) const noexcept override
    { return id_; }

private:
    LayerId id_;
    ShapeLayer before_, after_;
    std::uint64_t mergeKey_;
};
} // namespace imageeditor::core
