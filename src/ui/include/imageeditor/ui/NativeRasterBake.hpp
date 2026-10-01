#pragma once
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"

namespace imageeditor::ui {
// Copies already quantized native RGBA bytes; no sampling/color conversion.
[[nodiscard]] std::shared_ptr<core::RasterSurface> surfaceFromNativeImage(const QImage&);
[[nodiscard]] bool needsRasterization(const core::Layer&);
struct RasterizeResult {
    std::unique_ptr<core::LayerStructureCommand> command;
    QString error;
    bool cancelled {false};
    // A null command with no error is a successful no-op (preserves redo).
};
[[nodiscard]] RasterizeResult prepareRasterizeLayers(const core::Document&,
    const core::LayerSelectionState&, std::size_t historyBudget,
    FlattenedDocumentProgress = {}, std::optional<core::LayerId> onlyLayer = {});
}
