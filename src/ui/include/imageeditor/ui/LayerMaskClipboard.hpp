#pragma once
#include "imageeditor/core/Layer.hpp"
#include <QByteArray>
#include <optional>

namespace imageeditor::ui {
inline constexpr auto layerMaskClipboardMime = "application/x-vulkana-layer-mask-v1";
struct LayerMaskClipboard {
    core::LayerMaskState mask;
    core::AffineTransform sourceToDocument;
};
// Bounded, versioned R8 data; no image decoding, hidden source pixels or layer IDs.
QByteArray encodeLayerMaskClipboard(const core::Layer&);
std::optional<LayerMaskClipboard> decodeLayerMaskClipboard(const QByteArray&);
// Preserve document-pixel placement and original bitmap resolution. Pad to the
// destination's local bounds using outside coverage; never scale the bitmap.
// A null result means the destination cannot represent the mapping safely.
core::LayerMaskState pastedLayerMask(const LayerMaskClipboard&, const core::AffineTransform&,
                                    core::RectD destinationBounds = {});
}
