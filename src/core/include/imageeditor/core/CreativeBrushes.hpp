#pragma once

#include "imageeditor/core/BrushTip.hpp"

namespace imageeditor::core {
// Immutable masks/mips are shared across previews and strokes. The existing
// serialized seed controls variation; no extra preset/project format is needed.
[[nodiscard]] std::span<const BrushPresetRecord> creativeBrushPresets() noexcept;
[[nodiscard]] std::unique_ptr<IBrushTip> makeCreativeBrushTip(std::string_view id);
[[nodiscard]] BrushAssetCacheStats creativeBrushCacheStats() noexcept;
// Decorative tips keep the distance lattice at release instead of stamping
// a second, overlapping copy just to touch the pointer-up location.
[[nodiscard]] bool isCreativeStampTip(std::string_view id) noexcept;
} // namespace imageeditor::core
