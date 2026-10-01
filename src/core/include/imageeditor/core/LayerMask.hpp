#pragma once
#include "imageeditor/core/SelectionMask.hpp"
#include <memory>

namespace imageeditor::core {
// Immutable coverage in a stable layer-local frame. Outside coverage is
// explicit so Reveal All remains neutral when text, geometry or style extents
// grow.
struct LayerMask {
  SelectionState coverage;
  AffineTransform localToMask;
  std::uint8_t outside{255};
  bool enabled{true};
};
using LayerMaskState = std::shared_ptr<const LayerMask>;
[[nodiscard]] bool validLayerMask(const LayerMaskState &) noexcept;
[[nodiscard]] bool equivalentLayerMasks(const LayerMaskState &,
                                        const LayerMaskState &) noexcept;
[[nodiscard]] float layerMaskCoverage(const LayerMaskState &,
                                      Vec2d local) noexcept;
[[nodiscard]] std::uint8_t maskGray(Rgba8) noexcept;
} // namespace imageeditor::core
