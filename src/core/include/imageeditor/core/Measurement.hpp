#pragma once

#include "imageeditor/core/Document.hpp"

#include <optional>
#include <span>

namespace imageeditor::core {

// Session/view data only. Coordinates are document pixels, including positions
// outside the canvas, with +X right and +Y down.
struct MeasurementLine {
    Vec2d a, b;
    friend bool operator==(const MeasurementLine&, const MeasurementLine&) = default;
};
struct MeasurementValues {
    Vec2d delta;
    double distance {0};
    // Zero at +X, positive clockwise, signed [-180,180]. A zero-length line
    // has no direction rather than an invented/invalid angle.
    std::optional<double> angleDegrees;
};
[[nodiscard]] std::optional<MeasurementValues> measurementValues(MeasurementLine) noexcept;

struct DocumentBounds {
    Vec2d minimum, maximum;
    [[nodiscard]] Extent2d extent() const noexcept
    {
        return {maximum.x - minimum.x, maximum.y - minimum.y};
    }
    friend bool operator==(const DocumentBounds&, const DocumentBounds&) = default;
};

// Document-aligned union of the existing logical transform frames (not painted
// alpha/stroke bounds or local dimensions). Containers expand exactly once;
// hidden descendants participate. Reads existing text layout geometry only,
// never requests a raster cache or scans pixels. Unavailable/invalid geometry
// yields no readout, rather than a misleading partial bound.
[[nodiscard]] std::optional<DocumentBounds> selectedLayerBounds(
    const Document&, std::span<const LayerId>);
[[nodiscard]] std::optional<DocumentBounds> layerDocumentBounds(const Layer&);

} // namespace imageeditor::core
