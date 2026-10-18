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

// A pixel-aligned boundary in the old document coordinate system. Translating
// the origin rebases every leaf once; it never scales or discards layer data.
class ReframeCanvasCommand final : public Command {
public:
    explicit ReframeCanvasCommand(RectI frame) : frame_(frame) {}
    bool apply(Document&) override;
    bool undo(Document&) override;
    std::string_view label() const noexcept override { return "Change canvas size"; }
    std::size_t memoryCost() const noexcept override;
    std::optional<CanvasOriginHint> canvasOriginAfter(bool undo) const noexcept override {
        return CanvasOriginHint{undo ? -frame_.x : frame_.x, undo ? -frame_.y : frame_.y};
    }
private:
    RectI frame_;
    std::optional<CanvasSpec> before_;
    CanvasSpec after_;
    std::vector<LayerTransformUpdate> forward_, reverse_;
    SelectionState beforeSelection_, afterSelection_, beforeRemembered_, afterRemembered_;
    SelectionEvidenceState beforeEvidence_;
};

} // namespace imageeditor::core
