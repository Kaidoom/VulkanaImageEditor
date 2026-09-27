#pragma once

#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/LayerTree.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace imageeditor::core {

enum class VisibilityOperation { Hide, Show, Isolate, ShowAll };
class SetItemsVisibilityCommand final : public Command {
public:
    SetItemsVisibilityCommand(const Document&, std::span<const LayerId>, VisibilityOperation);
    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] std::string_view label() const noexcept override;
    [[nodiscard]] std::size_t memoryCost() const noexcept override
    { return sizeof(*this) + (forward_.capacity() + reverse_.capacity()) * sizeof(ItemVisibilityUpdate); }
private:
    VisibilityOperation operation_;
    std::vector<ItemVisibilityUpdate> forward_, reverse_;
};

class SetLayerVisibilityCommand final : public Command {
public:
    SetLayerVisibilityCommand(LayerId layerId, bool visible);
    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Layer visibility"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override { return sizeof(*this); }

private:
    LayerId layerId_;
    bool after_;
    std::optional<bool> before_;
};

class SetLayerOpacityCommand final : public Command {
public:
    SetLayerOpacityCommand(LayerId layerId, float opacity, std::uint64_t mergeKey = 0);
    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Layer opacity"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override { return sizeof(*this); }
    bool mergeWith(const Command& other) override;

private:
    LayerId layerId_;
    float after_;
    std::uint64_t mergeKey_ {0};
    std::optional<float> before_;
};

class SetLayerBlendModeCommand final : public Command {
public:
    SetLayerBlendModeCommand(LayerId layerId, BlendMode mode);
    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Layer blend mode"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override { return sizeof(*this); }
private:
    LayerId layerId_;
    BlendMode after_;
    std::optional<BlendMode> before_;
};

class AddLayerCommand final : public Command {
public:
    // Optional selection consequence stays inside the same add-layer action.
    // Existing callers without a hint retain their normal selection behavior.
    AddLayerCommand(Layer layer, std::size_t index, std::optional<LayerId> previousActive = {});
    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Add layer"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] std::optional<LayerId> activeLayerAfter(bool undo) const noexcept override
    { return previousActive_ ? (undo ? previousActive_ : std::optional<LayerId>(layer_.id)) : std::nullopt; }

private:
    Layer layer_;
    std::size_t index_;
    std::optional<LayerId> previousActive_;
    std::optional<ItemPlacement> placement_;
};

class RemoveLayerCommand final : public Command {
public:
    explicit RemoveLayerCommand(LayerId layerId);
    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Delete layer"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;

private:
    LayerId layerId_;
    std::optional<Layer> removedLayer_;
    std::optional<ItemPlacement> placement_;
    std::size_t index_ {0};
};

class MoveLayerCommand final : public Command {
public:
    MoveLayerCommand(LayerId layerId, std::size_t destinationIndex);
    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Reorder layer"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override { return sizeof(*this)
        +(beforeTree_?beforeTree_->memoryCost():0)+(afterTree_?afterTree_->memoryCost():0); }

private:
    LayerId layerId_;
    std::size_t afterIndex_;
    std::optional<std::size_t> beforeIndex_;
    std::optional<LayerTree> beforeTree_,afterTree_;
};

} // namespace imageeditor::core
