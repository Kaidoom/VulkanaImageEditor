#pragma once

#include "imageeditor/core/SelectionCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionMask.hpp"

namespace imageeditor::core {

// Unclipped, subpixel document geometry. Circle constraints retain the press
// corner and use the larger drag-axis magnitude, with each axis's drag sign.
class EllipseGeometry {
public:
    EllipseGeometry(Vec2d start, Vec2d end, bool circle = false);
    [[nodiscard]] Vec2d minimum() const noexcept { return minimum_; }
    [[nodiscard]] Vec2d maximum() const noexcept { return maximum_; }
    [[nodiscard]] Vec2d center() const noexcept { return { double(cx_), double(cy_) }; }
    [[nodiscard]] bool empty() const noexcept { return rx_ <= 0 || ry_ <= 0; }
    [[nodiscard]] RectI region(Extent2u canvas) const;
    [[nodiscard]] std::uint8_t coverageAt(int x, int y) const noexcept;
    // Exact >=128 pixel-edge contour, using monotone row searches over only
    // the AA band. No mask allocation/rasterization during pointer movement.
    [[nodiscard]] std::vector<SelectionEdge> previewEdges(Extent2u canvas) const;
    friend bool operator==(const EllipseGeometry&, const EllipseGeometry&) = default;

private:
    friend class EllipseCoverageRasterizer;
    struct RowBand {
        int outerLeft, outerRight, innerLeft, innerRight;
    };
    [[nodiscard]] RowBand rowBand(int y, RectI clip) const noexcept;
    Vec2d minimum_, maximum_;
    long double cx_, cy_, rx_, ry_;
};

class EllipseCoverageRasterizer final : public SelectionCoverageRasterizer {
public:
    static constexpr std::uint64_t maximumPixels = 64000000;
    struct Stats {
        std::uint64_t writtenPixels { 0 }, boundaryPixels { 0 };
    };
    EllipseCoverageRasterizer(Extent2u canvas, EllipseGeometry geometry);
    bool step(std::size_t workBudget = 65536) override;
    [[nodiscard]] bool finished() const noexcept override { return nextRow_ >= region_.bottom(); }
    [[nodiscard]] RectI region() const noexcept override { return region_; }
    [[nodiscard]] std::span<const std::uint8_t> coverage() const noexcept override { return pixels_; }
    [[nodiscard]] std::size_t stride() const noexcept override { return std::size_t(region_.width); }
    [[nodiscard]] std::size_t memoryBytes() const noexcept { return pixels_.capacity(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    EllipseGeometry geometry_;
    RectI region_;
    int nextRow_;
    std::vector<std::uint8_t> pixels_;
    Stats stats_;
};

} // namespace imageeditor::core
