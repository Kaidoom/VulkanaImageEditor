#pragma once

#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/SmartSelection.hpp"
#include <functional>
#include <stdexcept>

namespace imageeditor::core {

// Ephemeral, platform-neutral processing. The caller owns the immutable input
// and reference. Grid centers map to document centers through gridToDocument.
struct RefinementSettings {
  static constexpr double maximumRadius = 250;
  double radius{}, smooth{}, feather{}, contrast{}, shift{};
  friend bool operator==(const RefinementSettings &,
                         const RefinementSettings &) = default;
};
enum class RefinementBrush { Edge, Add, Subtract };
struct RefinementStroke {
  RefinementBrush kind{RefinementBrush::Edge};
  double hardness{.8};
  std::vector<BrushDab> dabs;
};
using RefinementStrokeState = std::shared_ptr<const RefinementStroke>;
struct RefinementState {
  RefinementSettings settings;
  std::vector<RefinementStrokeState> strokes;
  friend bool operator==(const RefinementState &,
                         const RefinementState &) = default;
};
class RefinementPath final : private BrushDabSink {
public:
  bool begin(RefinementBrush, const BrushSettings &,
             const NormalizedPointerSample &);
  bool append(const NormalizedPointerSample &);
  bool end(const NormalizedPointerSample &);
  void cancel() noexcept {
    engine_.cancelStroke();
    stroke_.dabs.clear();
  }
  [[nodiscard]] bool active() const noexcept { return engine_.active(); }
  [[nodiscard]] RefinementStrokeState stroke() const;

private:
  void emitDab(const BrushDab &dab) override {
    if (stroke_.dabs.size() >= 262144)
      throw std::length_error("Refinement stroke exceeds its memory limit.");
    stroke_.dabs.push_back(dab);
  }
  BasicPixelBrushEngine engine_;
  RefinementStroke stroke_;
};
struct RefinementAnalysis;
struct RefinementOptions {
  // This service works on a uniform document-aligned grid. Mask clients may
  // use a finer grid and map only modified coverage back to their native grid.
  Vec2d origin;
  double pixelsPerDocumentPixel{1};
  std::size_t maxWorkingBytes{1536ULL * 1024 * 1024};
  std::function<bool()> cancelled;
  // Optional exact-identity cache. Ownership prevents stale pointer reuse.
  std::shared_ptr<const SmartReferenceImage> referenceOwner;
  std::shared_ptr<const RefinementAnalysis> analysis;
};
struct RefinementAnalysis {
  SelectionState input, region;
  std::shared_ptr<const SmartReferenceImage> reference;
  double radius{}, scale{};
  Vec2d origin;
  std::vector<RefinementStrokeState> regions;
  std::vector<float> coverage;
  std::size_t analyzed{}, unresolved{};
};
struct RefinementResult {
  SelectionState coverage, region;
  std::size_t analyzedPixels{}, unresolvedPixels{}, workingBytes{};
  double analysisMs{}, globalMs{}, correctionMs{};
  bool cancelled{};
  std::shared_ptr<const RefinementAnalysis> analysis;
};
// Order: immutable input -> local color-pair matting -> median smoothing ->
// Euclidean grayscale offset -> Gaussian feather -> contrast -> manual strokes.
// A missing image disables only matting. No foreground color recovery occurs.
[[nodiscard]] RefinementResult
refineSelection(const SelectionState &input,
                const SmartReferenceImage *reference, const RefinementState &,
                const RefinementOptions & = {});

} // namespace imageeditor::core
