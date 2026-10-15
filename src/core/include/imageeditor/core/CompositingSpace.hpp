#pragma once
#include <cstdint>

namespace imageeditor::core {
// Document appearance policy, independent of storage/profile and geometric
// filtering. Stable IDs are shared with the GPU; projects serialize names.
enum class CompositingSpace : std::uint8_t { LinearSRgb = 0, EncodedSRgb = 1 };
constexpr bool validCompositingSpace(CompositingSpace space) noexcept
{
    return space == CompositingSpace::LinearSRgb || space == CompositingSpace::EncodedSRgb;
}
}
