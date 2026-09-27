#pragma once

#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/CloneReference.hpp"
#include "imageeditor/core/SpotHealRepair.hpp"

namespace imageeditor::core {

// Immutable solve ownership. These contain only frozen source data, geometry,
// brush marks and masks, never the live Document or a writable target surface.
struct SpotHealWork;
// Empty means admissible. Cheap metadata-only check before source capture.
[[nodiscard]] std::string spotHealTargetDiagnostic(const Document&, LayerId, CloneSampleSource);
struct SpotHealSolved {
    SpotHealResult repair;
    RectI gridBounds;
    AffineTransform gridToDocument;
    std::shared_ptr<const SpotHealWork> input;
    std::size_t preparedBytes {0};
    // Opt-in worker wall timings, populated with captureProfile only. Preparation
    // includes coherent reference/cache preparation and evaluation-grid creation;
    // solve accumulates all reconstruction attempts. Neither includes publication.
    double workerPreparationMilliseconds {0};
    double solveMilliseconds {0};
};

class SpotHealStroke final {
public:
    SpotHealStroke(Document&, LayerId, BrushSettings, CloneSettings, CloneReference,
        const IBrushAssetResolver* = nullptr);
    ~SpotHealStroke();
    bool begin(const NormalizedPointerSample&);
    bool append(const NormalizedPointerSample&);
    // Owner thread: finalize the complete mask without pixel writes. A null
    // result with an empty diagnostic is a no-effect stroke; commitNoop exits
    // its transaction without clearing redo. A diagnostic denotes failure.
    [[nodiscard]] std::shared_ptr<const SpotHealWork> finishInput(const NormalizedPointerSample&);
    // Worker-safe. Includes rendered-filter preparation and reference/unknown
    // rasterization; all callbacks belong to the scheduling caller.
    [[nodiscard]] static SpotHealSolved solve(std::shared_ptr<const SpotHealWork>, const SpotHealOptions& = {});
    // Owner thread only. Revalidates every pinned source/target revision and
    // applies final soft coverage once through the existing raster transaction.
    [[nodiscard]] RasterEditCommitResult publish(const SpotHealSolved&, History&,
        const std::function<bool()>& cancelled = {});
    [[nodiscard]] RasterEditCommitResult commitNoop(History&);
    void cancel() noexcept;
    [[nodiscard]] bool targetMatches() const noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const BasicPixelBrushStroke& brush() const noexcept { return brush_; }
    [[nodiscard]] const BrushStrokeStats& stats() const noexcept { return brush_.stats(); }
    [[nodiscard]] std::size_t snapshotBytes() const noexcept { return reference_.snapshotBytes(); }
    [[nodiscard]] Revision markRevision() const noexcept { return markRevision_; }
    // Cached until more dabs arrive; never modifies the raster selection.
    [[nodiscard]] SelectionState previewMask() const;

private:
    void observeDab(const BrushDab&);
    Document& document_;
    LayerId target_;
    BrushSettings settings_;
    CloneSettings cloneSettings_;
    CloneReference reference_;
    AffineTransform localToDocument_, documentToLocal_;
    Extent2u extent_, canvas_;
    SurfaceId surface_ {0};
    Revision revision_ {0}, surfaceRevision_ {0};
    std::vector<std::pair<std::shared_ptr<const RasterSurface>, Revision>> sourceRevisions_;
    std::unique_ptr<IBrushTip> hardTip_;
    using MarkTile = std::array<std::uint8_t, 128 * 128>;
    std::map<std::pair<int, int>, MarkTile> marks_;
    std::map<std::pair<int, int>, MarkTile> nativeMarks_;
    RectI markBounds_;
    RectI nativeMarkBounds_;
    Revision markRevision_ {0};
    mutable SelectionState preview_;
    mutable Revision previewRevision_ {0};
    std::shared_ptr<const SpotHealWork> work_;
    bool finished_ {false};
    BasicPixelBrushStroke brush_;
    std::string diagnostic_;
};
} // namespace imageeditor::core
