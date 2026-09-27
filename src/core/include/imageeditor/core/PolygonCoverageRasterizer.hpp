#pragma once

#include "imageeditor/core/SelectionCoverageRasterizer.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace imageeditor::core {

// Independent even-odd polygon -> cropped R8 producer. No document, selection,
// history, UI, or renderer ownership. Coverage is analytic along X and sampled
// at 256 deterministic midpoint subrows along Y (not exact area integration).
class PolygonCoverageRasterizer final : public SelectionCoverageRasterizer {
public:
    static constexpr int subrowsPerPixel = 256;
    static constexpr std::uint64_t maximumEdgeSamples = 64000000;
    static constexpr std::uint64_t maximumPixels = 64000000;
    struct Stats {
        std::uint64_t edgeSamples { 0 }, subrows { 0 }, writtenPixels { 0 };
    };
    PolygonCoverageRasterizer(Extent2u canvas, std::span<const Vec2d> polygon);
    // Cooperative budget; a single subrow and its row finalization are atomic.
    bool step(std::size_t workBudget = 65536) override;
    [[nodiscard]] bool finished() const noexcept override { return nextSample_ >= endSample_; }
    [[nodiscard]] RectI region() const noexcept override { return region_; }
    [[nodiscard]] std::span<const std::uint8_t> coverage() const noexcept override { return pixels_; }
    [[nodiscard]] std::size_t stride() const noexcept override { return std::size_t(region_.width); }
    [[nodiscard]] std::size_t memoryBytes() const noexcept;
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Edge {
        Vec2d low, high;
        int start, end;
    };
    void addInterval(double left, double right);
    void finishRow(int y);
    RectI region_;
    std::vector<Edge> edges_;
    std::vector<std::size_t> active_;
    std::vector<double> crossings_, differences_, partials_;
    std::vector<std::uint8_t> pixels_;
    std::size_t nextEdge_ { 0 };
    int nextSample_ { 0 }, endSample_ { 0 };
    Stats stats_;
};

} // namespace imageeditor::core
