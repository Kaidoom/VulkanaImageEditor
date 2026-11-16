#pragma once
#include "imageeditor/core/LayerEffects.hpp"
#include <QByteArray>
#include <optional>

namespace imageeditor::ui {
inline constexpr auto layerEffectClipboardMime = "application/x-vulkana-layer-effects";
// Effects only: layer pixels, masks, transforms, opacity and Fill are excluded.
// An absent stack is copied as the default (empty) stack, allowing Paste to
// clear effects without treating empty-but-valid clipboard content as failure.
QByteArray encodeLayerEffectClipboard(const core::LayerEffectState&);
std::optional<core::LayerEffectState> decodeLayerEffectClipboard(const QByteArray&);
} // namespace imageeditor::ui
