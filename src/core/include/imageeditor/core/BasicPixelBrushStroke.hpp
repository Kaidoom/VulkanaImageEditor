#pragma once

#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/BrushTip.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/BlendCompositing.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <functional>

namespace imageeditor::core {

// Paint, erase, and sampled color filters share one brush pipeline. The
// mode is intentionally not preset data: it is captured when a stroke begins
// and only changes how the resolved coverage is applied to RasterSurface.
enum class BrushCompositeMode : std::uint8_t {
    Paint,
    Erase,
    // Sampled filters replace linear-light color detail using the accumulated
    // brush influence, retaining the exact destination alpha byte.
    FilterColor,
};

enum class BrushStrokeFailure { None, RasterWorkLimitExceeded };
// More than the worst 1000px identity tip AABB (square bitmap rotated 45°).
// Deterministic per-dab admission, never dependent on input event grouping.
inline constexpr std::uint64_t kMaximumBrushDabCandidatePixels = 2'100'000;

struct BrushStrokeStats {
    std::uint64_t inputSamples {0};
    std::uint64_t emittedDabs {0};
    std::uint64_t evaluatedPixels {0};
    std::uint64_t changedPixels {0};
    std::uint64_t surfaceWriteBatches {0};
    std::uint64_t uploadedRegionBytes {0};
    std::size_t retainedStrokeTiles {0};
    std::uint64_t maximumCandidatePixels {0};
    std::uint64_t rejectedCandidatePixels {0};
    std::uint64_t parallelDabs {0};
    std::size_t coverageScratchBytes {0};
};

// Execution policy is not brush/preset data. One forces the reference serial
// path; zero uses the shared bounded pool for sufficiently large dabs.
struct BrushExecutionOptions {
    unsigned workers {0};
};

// Product vertical slice joining an arbitrary IBrushEngine to a raster edit
// transaction. The engine emits document-space dabs only; this class owns all
// layer mapping, tip rasterization, opacity/flow accumulation, clipping, live
// surface writes, and the single history commit.
class BasicPixelBrushStroke final : private BrushDabSink {
public:
    BasicPixelBrushStroke(Document& document, LayerId layerId,
        BrushSettings settings,
        std::unique_ptr<IBrushEngine> engine =
            std::make_unique<BasicPixelBrushEngine>(),
        std::unique_ptr<IBrushTip> tip = {},
        std::unique_ptr<IBrushGrain> grain = {},
        const IBrushAssetResolver* assetResolver = nullptr,
        RasterEditTransactionOptions transactionOptions = {},
        BrushExecutionOptions executionOptions = {});
    BasicPixelBrushStroke(Document& document, LayerId layerId,
        BrushSettings settings, BrushCompositeMode compositeMode,
        std::unique_ptr<IBrushEngine> engine =
            std::make_unique<BasicPixelBrushEngine>(),
        std::unique_ptr<IBrushTip> tip = {},
        std::unique_ptr<IBrushGrain> grain = {},
        const IBrushAssetResolver* assetResolver = nullptr,
        RasterEditTransactionOptions transactionOptions = {},
        std::function<PremultipliedColor(Vec2d)> sampledColor = {},
        std::string_view historyLabel = {}, bool deferSampledWrites = false,
        std::function<void(const BrushDab&)> dabObserver = {},
        BrushExecutionOptions executionOptions = {});
    ~BasicPixelBrushStroke();

    BasicPixelBrushStroke(const BasicPixelBrushStroke&) = delete;
    BasicPixelBrushStroke& operator=(const BasicPixelBrushStroke&) = delete;

    bool begin(const NormalizedPointerSample& sample);
    bool append(const NormalizedPointerSample& sample);
    [[nodiscard]] RasterEditCommitResult end(
        const NormalizedPointerSample& sample, History& history);
    // Sampled operators can replace a provisional result from the original
    // pixels before adopting the same transaction into history.
    bool finishInput(const NormalizedPointerSample& sample);
    [[nodiscard]] RasterEditCommitResult commit(History& history);
    [[nodiscard]] RectI coverageBounds() const noexcept;
    bool copyCoverage(RectI region, std::span<float> coverage,
        const std::function<bool()>& cancelled = {}) const;
    bool reapplySampledColor(const std::function<PremultipliedColor(Vec2d)>& source,
        const std::function<bool()>& cancelled = {});
    // Final deferred repair: include the pinned selection in floating-point
    // brush influence before encoding. The transaction still enforces zero
    // selection, document bounds and crop, but does not interpolate it again.
    bool reapplySampledColorSelectionResolved(const std::function<PremultipliedColor(Vec2d)>& source,
        const std::function<bool()>& cancelled = {});
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] BrushStrokeFailure failure() const noexcept { return failure_; }
    [[nodiscard]] const BrushSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] BrushCompositeMode compositeMode() const noexcept
    {
        return compositeMode_;
    }
    [[nodiscard]] const BrushStrokeStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const DirtySet& lastDirtySet() const noexcept { return lastDirty_; }
    [[nodiscard]] std::optional<Vec2d> constrainedPosition() const noexcept
    { return engine_->constrainedPosition(); }
    // Exact orientation of the most recently emitted/resolved dab. The canvas
    // cursor consumes this value so preview and paint never resolve direction
    // through separate paths.
    [[nodiscard]] std::optional<double> lastResolvedTipAngleDegrees() const noexcept
    {
        return lastResolvedTipAngleDegrees_;
    }

private:
    struct StrokeTile {
        RectI region;
        std::vector<std::byte> original;
        std::vector<std::byte> working;
        std::vector<float> flowCoverage;
        RectI pendingDirty;
        std::uint64_t changedPixels {0};
    };

    struct DabTileWork {
        RectI region;
        StrokeTile* tile {nullptr};
        std::uint64_t evaluatedPixels {0};
        bool hasCoverage {false};
    };

    using TileKey = std::pair<std::int32_t, std::int32_t>;

    void emitDab(const BrushDab& dab) override;
    bool emitParallelDab(const BrushDab&, RectI bounds);
    [[nodiscard]] StrokeTile* ensureTile(std::int32_t localX, std::int32_t localY);
    [[nodiscard]] RectI localDabBounds(
        const BrushTipBounds& bounds) const noexcept;
    void compositePixel(StrokeTile& tile, std::int32_t localX,
        std::int32_t localY, const BrushDab& dab, double tipAlpha);
    void resolvePixel(StrokeTile&, std::int32_t localX, std::int32_t localY,
        const BrushDab&, double accumulated);
    bool reapplySampledColorImpl(const std::function<PremultipliedColor(Vec2d)>& source,
        const std::function<bool()>& cancelled, bool selectionResolved);
    void flushPending();
    [[nodiscard]] RectI surfaceBounds() const noexcept;

    Document* document_ {nullptr};
    LayerId layerId_ {0};
    BrushSettings settings_;
    BrushCompositeMode compositeMode_ {BrushCompositeMode::Paint};
    std::function<PremultipliedColor(Vec2d)> sampledColor_;
    bool deferSampledWrites_ {false};
    bool sampledSelectionResolved_ {false};
    std::function<void(const BrushDab&)> dabObserver_;
    std::unique_ptr<IBrushEngine> engine_;
    std::unique_ptr<IBrushTip> tip_;
    std::unique_ptr<IBrushGrain> grain_;
    std::unique_ptr<RasterEditTransaction> transaction_;
    std::shared_ptr<RasterSurface> surface_;
    AffineTransform localToDocument_;
    AffineTransform documentToLocal_;
    Extent2u surfaceExtent_;
    Extent2u canvasExtent_;
    std::uint32_t tileSize_ {64};
    std::map<TileKey, StrokeTile> tiles_;
    std::size_t retainedPixels_ {0};
    RectI coverageBounds_;
    DirtySet lastDirty_;
    BrushStrokeStats stats_;
    BrushExecutionOptions executionOptions_;
    std::vector<double> coverageScratch_;
    std::vector<DabTileWork> dabTileWork_;
    std::optional<double> lastResolvedTipAngleDegrees_;
    double documentPixelFootprint_ {1.0};
    bool valid_ {false};
    bool active_ {false};
    BrushStrokeFailure failure_ {BrushStrokeFailure::None};
};

} // namespace imageeditor::core
