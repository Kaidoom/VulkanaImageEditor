#pragma once
#include "imageeditor/core/ColorSampler.hpp"
#include <atomic>

namespace imageeditor::core {
// Bucket's maximum premultiplied sRGB/alpha channel metric, retaining the
// fractional byte precision of c*a/255. Zero/nonzero alpha are separate classes.
// 65535 means ineligible, otherwise distance is measured in 1/255 byte units.
[[nodiscard]] std::uint16_t colorSelectionDistance(Rgba8, Rgba8) noexcept;
[[nodiscard]] std::uint8_t colorSelectionCoverage(std::uint16_t distance, int fuzziness) noexcept;
struct ColorSelectionField {
    Extent2u extent;
    Rgba8 sampled;
    std::vector<std::uint16_t> distances;
};

// Cooperative sampling on the document owner thread. No mutable surface is
// read by a worker. Only immutable, seed-relative comparison data is retained.
class ColorSelectionReference final {
public:
    static constexpr std::uint64_t maximumPixels = 64'000'000;
    ColorSelectionReference(const Document&, std::optional<LayerId>, ColorSampleSource, Vec2d seed,
        std::span<const SampleCacheOverride> = {});
    bool step(std::size_t pixelBudget = 4096);
    [[nodiscard]] bool matches(const Document& doc) const noexcept { return sampler_.matches(doc); }
    [[nodiscard]] Rgba8 sampledColor() const noexcept { return field_->sampled; }
    [[nodiscard]] std::size_t sampledPixels() const noexcept { return next_; }
    [[nodiscard]] std::size_t sourcesPerPixel() const noexcept { return sampler_.sourceCount(); }
    [[nodiscard]] std::shared_ptr<const ColorSelectionField> field() const noexcept;
private:
    PinnedDocumentSampler sampler_;
    std::shared_ptr<ColorSelectionField> field_;
    std::size_t next_ {0};
};
struct ColorSelectionResult { SelectionState incoming, combined; };
// Shared one-pixel geometric contour integration and bounded overlay preflight.
[[nodiscard]] std::uint8_t colorSelectionPixelCoverage(const ColorSelectionField&, std::size_t index, int fuzziness);
[[nodiscard]] bool prepareSelectionBoundary(const SelectionState&, const std::atomic_bool& cancelled);
// Safe on a worker: all inputs immutable; empty result struct means cancelled.
// All-zero coverage still produces a non-null active-empty SelectionMask.
// Threshold edges use a one-document-pixel analytic distance-contour integral,
// preserving fractional coverage without a spatial feather radius.
[[nodiscard]] ColorSelectionResult buildColorSelection(const ColorSelectionField&, int fuzziness,
    SelectionState original, SelectionOperation, const std::atomic_bool& cancelled);
}
