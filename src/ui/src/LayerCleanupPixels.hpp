#pragma once

#include "imageeditor/core/LayerPixelCleanup.hpp"
#include <QImage>
#include <QRect>
#include <array>
#include <cstdint>
#include <vector>

namespace imageeditor::ui {

// A before/after pair on the same layer-local grid. Apply the threshold before
// averaging: clearing the faintest pixel must never erase its opaque neighbors.
struct CleanupPixelPatch {
    QRect region;
    int level{};
    int threshold{};
    QImage original, cleaned;

    bool covers(QRect visible, int lod) const { return level == lod && region.contains(visible); }
    void rethresholdNative(int value);
};

class CleanupPixelScan {
public:
    CleanupPixelScan(QRect region, int lod, int threshold);
    bool advance(const core::RasterSurface&);
    CleanupPixelPatch patch;

private:
    struct Sum {
        std::uint64_t red{}, green{}, blue{}, alpha{};
        void add(core::Rgba8 pixel);
        QRgb average(int count) const;
    };
    std::vector<std::array<Sum,2>> sums_;
    std::vector<core::Rgba8> band_;
    int rowsPerBand_{}, nextRow_{};
};
} // namespace imageeditor::ui
