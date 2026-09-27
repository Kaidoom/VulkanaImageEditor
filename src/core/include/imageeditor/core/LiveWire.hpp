#pragma once
#include "imageeditor/core/Geometry.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace imageeditor::core {
// All reference sampling happens on the document's owner thread. The caller
// pins a reference signature and cancels before advancing if it changes.
class MagneticEdgeCache {
public:
    struct Feature {
        float x { }, y { }, strength { };
    };
    struct Stats {
        std::size_t samples { }, features { }, evictions { };
    };
    using Sample = std::function<Rgba8(int, int)>;
    MagneticEdgeCache(Extent2u, Sample);
    ~MagneticEdgeCache();
    MagneticEdgeCache(const MagneticEdgeCache&) = delete;
    MagneticEdgeCache& operator=(const MagneticEdgeCache&) = delete;
    Feature feature(int x, int y);
    [[nodiscard]] Extent2u extent() const;
    [[nodiscard]] std::size_t memoryBytes() const;
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LiveWireSearch {
public:
    static constexpr std::size_t maximumNodes = 131072;
    static constexpr std::size_t maximumGuidePoints = 512;
    enum class Result { Searching, Edge, ManualFallback };
    struct Stats {
        std::size_t prepared { }, expanded { }, peakQueue { };
    };
    // Guide runs from the fixed anchor to the pointer (both exact subpixel
    // positions are retained). Radius is in document pixels, never zoom units.
    LiveWireSearch(MagneticEdgeCache&, std::span<const Vec2d> guide, double radius = 12);
    ~LiveWireSearch();
    bool step(std::size_t budget = 1024);
    [[nodiscard]] Result result() const;
    [[nodiscard]] std::span<const Vec2d> path() const;
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::size_t memoryBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
