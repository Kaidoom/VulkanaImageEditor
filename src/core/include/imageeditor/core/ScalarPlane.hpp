#pragma once

#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/detail/ScalarTile.hpp"

#include <memory>
#include <optional>
#include <unordered_set>

namespace imageeditor::core {

class ScalarPlane;
using ScalarPlaneState = std::shared_ptr<const ScalarPlane>;

// Immutable numeric R8 pixels. Zero is ordinary data. Coordinates are local to
// this plane; callers define whether it represents coverage or channel values.
class ScalarPlane final {
public:
    static constexpr auto tileSize = detail::scalarTileSize;
    [[nodiscard]] static ScalarPlaneState filled(Extent2u, std::uint8_t value = 0);
    [[nodiscard]] static ScalarPlaneState fromR8(Extent2u, std::span<const std::byte>, std::size_t stride);
    [[nodiscard]] Extent2u extent() const noexcept { return extent_; }
    // Out-of-bounds access is rejected; it must not turn missing samples into
    // a valid zero value in cloning or repair tools.
    [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t y) const;
    void copyR8(RectI, std::span<std::byte>, std::size_t stride) const;
    // All patches are validated before any allocation or changes. Patches are
    // applied in order, and untouched pixel tiles remain shared. Null = no change.
    [[nodiscard]] ScalarPlaneState replacedR8(std::span<const RasterPatch>) const;
    [[nodiscard]] std::optional<std::uint8_t> constantValue(RectI) const;
    [[nodiscard]] std::size_t allocatedPixelBytes() const noexcept;
    [[nodiscard]] std::size_t memoryCost() const noexcept;
    // Account shared immutable allocations once across a set of live planes.
    [[nodiscard]] std::size_t memoryCost(std::unordered_set<const void*>& allocations) const;
    [[nodiscard]] std::size_t sharedPixelBytes(const ScalarPlane&) const noexcept;

private:
    explicit ScalarPlane(Extent2u);
    [[nodiscard]] RectI tileRect(std::size_t) const noexcept;
    void validate(RectI, std::size_t byteCount, std::size_t stride) const;
    Extent2u extent_;
    std::uint32_t columns_;
    std::vector<detail::ScalarTile> tiles_;
};

} // namespace imageeditor::core
