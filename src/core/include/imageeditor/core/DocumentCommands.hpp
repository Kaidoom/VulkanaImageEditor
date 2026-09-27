#pragma once

#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace imageeditor::core {

// Resizes the document boundary without resampling, cropping, translating, or
// otherwise mutating any layer. Rendering is responsible for clipping content
// to the current boundary.
class ChangeCanvasSpecCommand final : public Command {
public:
    explicit ChangeCanvasSpecCommand(CanvasSpec after);

    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override
    {
        return "Change canvas size";
    }
    [[nodiscard]] std::size_t memoryCost() const noexcept override
    { return sizeof(*this) + (beforeSelection_ ? beforeSelection_->memoryCost() : 0)
        + (beforeRemembered_ ? beforeRemembered_->memoryCost() : 0); }

private:
    CanvasSpec after_;
    std::optional<CanvasSpec> before_;
    SelectionState beforeSelection_;
    SelectionState beforeRemembered_;
};

} // namespace imageeditor::core
