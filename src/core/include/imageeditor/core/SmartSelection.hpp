#pragma once
#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/ColorSelection.hpp"

namespace imageeditor::core {
enum class SmartSelectMode { QuickSelection, MagicWand };
struct SmartReferenceImage {
    Extent2u extent;
    std::vector<Rgba8> pixels; // Canonical visible straight RGBA8; zero alpha has zero RGB.
    std::vector<std::uint8_t> valid;
};
// Owner-thread, cooperative snapshot. Workers only see the completed immutable
// image, never live RasterSurface/Qt layout data.
class SmartSelectionReference {
public:
    static constexpr std::uint64_t maximumPixels = 64'000'000;
    SmartSelectionReference(const Document&, std::optional<LayerId>, ColorSampleSource,
        std::span<const SampleCacheOverride> = { });
    bool step(std::size_t budget = 4096);
    [[nodiscard]] bool matches(const Document& doc) const noexcept { return sampler_.matches(doc); }
    [[nodiscard]] std::size_t sourcesPerPixel() const noexcept { return sampler_.sourceCount(); }
    [[nodiscard]] std::shared_ptr<const SmartReferenceImage> image() const noexcept;

private:
    PinnedDocumentSampler sampler_;
    std::shared_ptr<SmartReferenceImage> image_;
};
struct QuickSelectionHints {
    SelectionState foreground, background;
    // Accepted inferred negative corrections, NOT appearance-training samples.
    // Coverage c limits later Add coverage to 255-c until explicitly brushed over.
    SelectionState rejected;
};
struct SmartSelectionStats {
    std::uint64_t evaluatedPixels { 0 }, queuePops { 0 };
    std::size_t workspaceBytes { 0 };
    RectI workRegion;
};
struct SmartSelectionResult {
    SelectionState incoming, combined;
    QuickSelectionHints hints;
    SmartSelectionStats stats;
};
[[nodiscard]] SmartSelectionResult buildMagicWand(const SmartReferenceImage&, Vec2d seed, int tolerance,
    SelectionState original, SelectionOperation, const std::atomic_bool& cancelled);

struct QuickHintDab {
    Vec2d center;
    double radius;
};
struct QuickSelectionSettings {
    // Strength of the wider, monotone edge cue; not a spatial growth radius.
    double edgeSensitivity { 0.4 };
};
// Hint-only sink: reuses normalized samples and distance resampling, without
// creating a raster mutation/transaction or borrowing paint-color state.
class QuickSelectionPath final : private BrushDabSink {
public:
    bool begin(double diameter, NormalizedPointerSample);
    bool append(NormalizedPointerSample);
    bool end(NormalizedPointerSample);
    [[nodiscard]] const std::vector<QuickHintDab>& dabs() const noexcept { return dabs_; }

private:
    void emitDab(const BrushDab&) override;
    BasicPixelBrushEngine engine_;
    std::vector<QuickHintDab> dabs_;
};
[[nodiscard]] SmartSelectionResult buildQuickSelection(const SmartReferenceImage&,
    std::span<const QuickHintDab>, QuickSelectionHints previousHints, SelectionState original,
    SelectionOperation, const std::atomic_bool& cancelled, QuickSelectionSettings = {});
}
