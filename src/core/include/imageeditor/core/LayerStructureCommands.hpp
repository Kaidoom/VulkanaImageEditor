#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"

namespace imageeditor::core {

// One atomic structural replacement, retaining only removed/inserted typed
// content (raster ownership is shared, never copied) and small order metadata.
class LayerStructureCommand final : public Command {
public:
    LayerStructureCommand(std::string label, const Document&, LayerTree after,
        std::vector<LayerId> removed = { }, std::vector<Layer> added = { },
        std::optional<LayerSelectionState> beforeSelection = { },
        std::optional<LayerSelectionState> afterSelection = { },
        std::vector<ItemVisibilityUpdate> retainedLeafVisibility = { });
    bool apply(Document&) override;
    bool undo(Document&) override;
    // A staged insertion can use the shared transform gesture, then retain its
    // final matrices before rolling back and committing one structural action.
    bool captureAddedLayerTransforms(const Document&);
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] const LayerSelectionState* layerSelectionAfter(bool undo) const noexcept override;

private:
    std::string label_;
    LayerTree before_, after_;
    std::vector<LayerId> removedIds_, addedIds_;
    std::vector<Layer> removed_, added_;
    std::optional<LayerSelectionState> beforeSelection_, afterSelection_;
    std::vector<ItemVisibilityUpdate> forwardVisibility_, reverseVisibility_;
};

class SetItemMetadataCommand final : public Command {
public:
    SetItemMetadataCommand(LayerId id, std::string name, ColorLabel label)
        : id_(id)
        , afterName_(std::move(name))
        , afterLabel_(label)
    {
    }
    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] std::string_view label() const noexcept override { return "item name / color label"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override { return sizeof(*this) + beforeName_.capacity() + afterName_.capacity(); }

private:
    LayerId id_;
    std::string beforeName_, afterName_;
    ColorLabel beforeLabel_ { }, afterLabel_ { };
    bool captured_ { false };
};

[[nodiscard]] std::size_t retainedLayerMemory(const Layer&) noexcept;
struct LayerTransfer {
    LayerTree tree;
    std::vector<Layer> layers;
};
// Capture once, before UI activation changes the source model. Pixels are
// independent; immutable effect/adjustment/mask values may be shared safely.
[[nodiscard]] LayerTransfer captureLayerTransfer(const Document&, std::span<const LayerId>);
[[nodiscard]] std::unique_ptr<LayerStructureCommand> insertLayerTransfer(
    const Document&, LayerTransfer, ItemPlacement, LayerSelectionState, Vec2d translation = {});
// Deep-copy mutable raster pixels, retain editable typed content, and normalize
// parent/child selections. Construction completes before any document mutation.
[[nodiscard]] std::unique_ptr<LayerStructureCommand> duplicateLayerItems(
    const Document&, const LayerSelectionState&);
// Caller renders/preflights the raster before constructing this atomic command.
[[nodiscard]] std::unique_ptr<LayerStructureCommand> consolidateLayerItems(
    const Document&, const LayerSelectionState&, Layer raster);
} // namespace imageeditor::core
