#pragma once

#include "imageeditor/core/RasterSurface.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace imageeditor::core {

class SelectionMaskInput {
public:
    virtual ~SelectionMaskInput() = default;
    [[nodiscard]] virtual Revision revision() const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t coverageAtDocumentPixel(
        std::int32_t x, std::int32_t y) const noexcept = 0;
};

enum class SelectionOperation { Replace, Add, Subtract, Intersect };

// Pixel-edge endpoints, top-left origin, +Y down. Boundaries use coverage >=128;
// editing and cached bounds retain every nonzero R8 coverage value.
struct SelectionEdge {
    Vec2d from, to;
    friend bool operator==(const SelectionEdge&, const SelectionEdge&) = default;
};

class SelectionMask;
using SelectionState = std::shared_ptr<const SelectionMask>; // null = unrestricted

// Immutable document-space R8 plane. Uniform tiles need no pixel allocation;
// unchanged tiles are shared across history, snapshots and pinned raster edits.
class SelectionMask final : public SelectionMaskInput {
public:
    static constexpr std::uint32_t tileSize = 128;
    [[nodiscard]] static SelectionState filled(Extent2u, std::uint8_t);
    [[nodiscard]] static SelectionState rectangle(Extent2u, RectI, std::uint8_t = 255);
    [[nodiscard]] static SelectionState fromR8(Extent2u, std::span<const std::uint8_t>, std::size_t stride);
    // Cropped producer output; pixels outside region are zero. No full-canvas
    // intermediate allocation, and only overlapping tiles receive pixel data.
    [[nodiscard]] static SelectionState fromR8Region(Extent2u, RectI region,
        std::span<const std::uint8_t>, std::size_t stride);
    [[nodiscard]] Extent2u extent() const noexcept { return extent_; }
    [[nodiscard]] RectI bounds() const noexcept { return bounds_; }
    [[nodiscard]] Revision revision() const noexcept override { return revision_; }
    [[nodiscard]] std::uint8_t coverageAtDocumentPixel(std::int32_t, std::int32_t) const noexcept override;
    // Conservative metadata-only uniform-region query. A value is exact;
    // nullopt means pixels may vary and must be read. Zero extends off-canvas.
    [[nodiscard]] std::optional<std::uint8_t> constantCoverage(RectI) const;
    [[nodiscard]] SelectionState combined(const SelectionMask&, SelectionOperation) const;
    [[nodiscard]] SelectionState inverted() const;
    [[nodiscard]] SelectionState resized(Extent2u) const;
    [[nodiscard]] SelectionState translated(int dx, int dy) const;
    // Signed document-axis radii, per side. X is applied before Y for mixed signs.
    [[nodiscard]] SelectionState adjusted(int horizontal, int vertical) const;
    [[nodiscard]] SelectionState rotated(double clockwiseDegrees, Vec2d pivot) const;
    // Maps original document coordinates to new document coordinates. Coverage
    // uses zero-extended float mip averages for minification and bilinear
    // reconstruction, quantized once to R8 and clipped to the unchanged canvas.
    [[nodiscard]] SelectionState transformed(const AffineTransform& documentMapping) const;
    [[nodiscard]] bool equivalent(const SelectionMask&) const noexcept;
    [[nodiscard]] std::size_t memoryCost() const noexcept;
    [[nodiscard]] const std::vector<SelectionEdge>& boundaryEdges() const;
    // Support contour for captured adjustment masks: includes every nonzero
    // coverage pixel, even when no pixel reaches the marching-ants threshold.
    // Separate lazy immutable cache; never changes ordinary selection outlines.
    [[nodiscard]] const std::vector<SelectionEdge>& nonzeroBoundaryEdges() const;

private:
    using Pixels = std::array<std::uint8_t, tileSize * tileSize>;
    struct Tile {
        std::uint8_t uniform {0};
        std::shared_ptr<const Pixels> pixels;
        RectI bounds; // tile-local nonzero coverage, clipped to the document
        [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t y) const noexcept
        { return pixels ? (*pixels)[y * tileSize + x] : uniform; }
    };
    explicit SelectionMask(Extent2u);
    [[nodiscard]] RectI tileRect(std::size_t) const noexcept;
    static Tile compress(std::shared_ptr<Pixels>, Extent2u);
    void updateBounds();
    [[nodiscard]] std::vector<SelectionEdge> buildEdges(std::uint8_t threshold) const;
    template<class Mapping> SelectionState resampled(Mapping map, bool affine=true) const;
    Extent2u extent_;
    std::uint32_t columns_;
    Revision revision_;
    RectI bounds_;
    std::vector<Tile> tiles_;
    mutable std::once_flag edgesOnce_;
    mutable std::vector<SelectionEdge> edges_;
    mutable std::atomic<std::size_t> edgeMemoryBytes_ {0};
    mutable std::once_flag nonzeroEdgesOnce_;
    mutable std::vector<SelectionEdge> nonzeroEdges_;
    mutable std::atomic<std::size_t> nonzeroEdgeMemoryBytes_ {0};
};

[[nodiscard]] SelectionState combineSelection(SelectionState, SelectionState, SelectionOperation);
[[nodiscard]] RectI alignedSelectionRectangle(Vec2d start, Vec2d end, Extent2u) noexcept;
// Analytic rectangle preview: O(existing boundary + rectangle perimeter), never
// a full document-mask copy per pointer event. The returned geometry also draws
// holes/disconnected regions, independent of viewport zoom.
[[nodiscard]] std::vector<SelectionEdge> rectangleSelectionPreviewEdges(
    const SelectionState&, RectI, SelectionOperation, Extent2u);
// Translated threshold contour, including fractional snap offsets and canvas
// perimeter clipping. No mask resampling/allocation during the drag.
[[nodiscard]] std::vector<SelectionEdge> translatedSelectionPreviewEdges(const SelectionState&, double dx, double dy);
[[nodiscard]] bool selectionMoveHit(const SelectionState&, Vec2d, SelectionOperation, bool shift, bool alt) noexcept;

} // namespace imageeditor::core
