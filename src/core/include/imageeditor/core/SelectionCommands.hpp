#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"
#include <optional>
#include <string>

namespace imageeditor::core {
class SetSelectionCommand final : public Command {
public:
    explicit SetSelectionCommand(SelectionState after, std::string label = "Selection",
        std::optional<SelectionEvidenceState> evidence = std::nullopt);
    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] bool affectsPersistentContent() const noexcept override { return false; }
private:
    SelectionState before_, after_;
    SelectionState rememberedBefore_;
    SelectionEvidenceState evidenceBefore_, evidenceAfter_;
    std::string label_;
    bool initialized_ {false};
    bool explicitEvidence_ {false};
};

class LayerViaCopyCommand final : public Command {
public:
    explicit LayerViaCopyCommand(LayerId source, std::optional<LayerId> previousActive);
    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Layer via copy"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] std::optional<std::uint64_t> activeLayerAfter(bool undo) const noexcept override;
    [[nodiscard]] std::optional<LayerId> createdLayerId() const noexcept
    { return result_ ? std::optional(result_->id) : std::nullopt; }
private:
    LayerId source_, previousActive_;
    std::optional<Layer> result_;
    std::size_t index_ {0};
    std::optional<ItemPlacement> placement_;
};
}
