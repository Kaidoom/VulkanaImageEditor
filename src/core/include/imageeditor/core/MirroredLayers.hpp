#pragma once
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"

namespace imageeditor::core {
inline bool isMirroredLayer(const Layer& layer) noexcept { return layer.mirror.has_value(); }
[[nodiscard]] bool canMirrorLayer(const Layer&) noexcept;
[[nodiscard]] bool validMirrorStructure(std::span<const Layer>) noexcept;
[[nodiscard]] Layer resolvedMirroredLayer(const Layer&, const Layer& source);
[[nodiscard]] bool mirrorNeedsResolution(const Layer&, const Layer& source);
[[nodiscard]] Layer promotedMirroredLayer(const Layer&);
[[nodiscard]] std::unique_ptr<LayerStructureCommand> createMirroredLayer(const Document&, const LayerSelectionState&);
[[nodiscard]] std::unique_ptr<LayerStructureCommand> promoteMirroredLayer(const Document&, const LayerSelectionState&);

enum class MirrorSync { All, Opacity, FillOpacity };
class SyncMirroredLayerCommand final : public Command {
public:
    SyncMirroredLayerCommand(LayerId id, MirrorSync field = MirrorSync::All) : id_(id), field_(field) {}
    bool apply(Document&) override;
    bool undo(Document&) override;
    std::string_view label() const noexcept override { return "Sync mirrored layer"; }
    std::size_t memoryCost() const noexcept override;
private:
    LayerId id_; MirrorSync field_;
    std::optional<Layer> before_, after_;
};
} // namespace imageeditor::core
