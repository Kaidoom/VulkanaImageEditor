#pragma once

#include "imageeditor/core/Geometry.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace imageeditor::core {

// Geometry producers share cooperative completion and cropped R8 output, not
// document/history ownership. The UI publishes and combines the finished mask once.
class SelectionCoverageRasterizer {
public:
    virtual ~SelectionCoverageRasterizer() = default;
    virtual bool step(std::size_t workBudget = 65536) = 0;
    [[nodiscard]] virtual bool finished() const noexcept = 0;
    [[nodiscard]] virtual RectI region() const noexcept = 0;
    [[nodiscard]] virtual std::span<const std::uint8_t> coverage() const noexcept = 0;
    [[nodiscard]] virtual std::size_t stride() const noexcept = 0;
};

} // namespace imageeditor::core
