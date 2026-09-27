#pragma once

#include "imageeditor/core/Geometry.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace imageeditor::core {
// Reusable four-connected, seed-relative discovery. The source is immutable
// until discovery completes; nullopt is an impassable pixel (clip/selection).
// Coordinates and connectivity are in the source raster's pixel grid.
class RegionFinder final {
public:
    using Source = std::function<std::optional<Rgba8>(int, int)>;
    RegionFinder(Extent2u extent, int seedX, int seedY, std::uint8_t tolerance, Source source);
    bool step(std::size_t budget = 65536); // true when complete; bounded by budget + one row
    [[nodiscard]] bool contains(int x, int y) const noexcept;
    [[nodiscard]] std::size_t pixelCount() const noexcept { return count_; }
    [[nodiscard]] RectI bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;
    [[nodiscard]] static bool matches(Rgba8 seed, Rgba8 pixel, std::uint8_t tolerance) noexcept;

private:
    struct Span {
        int y, left, right;
    };
    bool test(int x, int y);
    void enqueue(Span span);
    Extent2u extent_;
    Source source_;
    Rgba8 seed_;
    std::uint8_t tolerance_;
    std::vector<std::uint64_t> visited_, region_;
    std::vector<Span> pending_;
    std::optional<Span> current_;
    std::size_t count_ { 0 }, work_ { 0 };
    RectI bounds_;
};
}
