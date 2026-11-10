#pragma once

#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"

namespace imageeditor::core {

[[nodiscard]] bool isScalarLayer(const Layer&) noexcept;
[[nodiscard]] bool isPackedDataLayer(const Layer&) noexcept;
[[nodiscard]] const LayerContainer* channelGroupForLayer(const Document&, LayerId) noexcept;
[[nodiscard]] bool numericLayerPropertiesSupported(const Layer&, RasterDataKind) noexcept;
[[nodiscard]] bool validChannelStructure(const LayerTree&, std::span<const Layer>) noexcept;
[[nodiscard]] bool channelTransformsCompatible(const Document&, std::span<const LayerTransformUpdate>) noexcept;
[[nodiscard]] bool canConvertToChannelGroup(const Document&, LayerId, std::string* reason = nullptr);
[[nodiscard]] std::unique_ptr<LayerStructureCommand> convertToChannelGroup(const Document&, const LayerSelectionState&);
[[nodiscard]] std::unique_ptr<LayerStructureCommand> convertChannelGroupToRaster(const Document&, const LayerSelectionState&);
// Exact native-grid packing, independent of display, visibility, selection and
// alpha compositing. Missing alpha is 255. Source channels remain unmodified.
[[nodiscard]] std::shared_ptr<ContiguousRasterSurface> packChannelGroup(const Document&, LayerId);

} // namespace imageeditor::core
