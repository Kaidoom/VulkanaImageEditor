#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace imageeditor::core {
// Binary four-neighbour Potts energy. Costs are label costs, not confidences or
// alpha. right/down own each undirected edge; image borders add no constraints.
// Result is 1 for foreground. Cancellation returns no result.
std::vector<std::uint8_t> selectionGraphCut(unsigned width, unsigned height,
    std::span<const float> foregroundCost, std::span<const float> backgroundCost,
    std::span<const float> right, std::span<const float> down, const std::atomic_bool& cancelled,
    std::span<const std::uint8_t> active = { });
}
