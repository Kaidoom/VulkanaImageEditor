#pragma once

#include "imageeditor/core/LayerStructureCommands.hpp"
#include <functional>

namespace imageeditor::core {

enum class LayerPixelCleanup { RemoveLowAlpha, TrimToCanvas };
inline constexpr std::uint8_t defaultCleanupAlpha = 4;

constexpr bool cleanupTargetsAlpha(std::uint8_t alpha, std::uint8_t threshold) noexcept
{
    return alpha > 0 && alpha <= threshold;
}

bool canCleanLayerPixels(const Document&, std::span<const LayerId> selected);
std::unique_ptr<Command> makeLayerPixelCleanupCommand(const Document&, const LayerSelectionState&,
    LayerPixelCleanup, std::uint8_t threshold = defaultCleanupAlpha, const std::function<bool()>& keepGoing = { });

} // namespace imageeditor::core
