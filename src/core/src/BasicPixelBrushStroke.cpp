#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/BoundedParallel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace imageeditor::core {
namespace {

constexpr std::size_t kBytesPerPixel = 4;
// Exceptional admission failure unwinds resampling immediately. No further
// dabs are generated, and the enclosing stroke method rolls back its journal.
struct RasterWorkLimit {};

bool finitePoint(Vec2d point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

double maximumTransformScale(const AffineTransform& transform) noexcept
{
    // Largest singular value of the linear 2x2 portion. Unlike a maximum of
    // the two column lengths, this remains conservative for sheared layers;
    // identity and pure rotations still resolve exactly to one.
    const auto squaredNorm = transform.m00 * transform.m00
        + transform.m01 * transform.m01
        + transform.m10 * transform.m10
        + transform.m11 * transform.m11;
    const auto determinant = transform.m00 * transform.m11
        - transform.m01 * transform.m10;
    const auto discriminant = std::max(0.0,
        squaredNorm * squaredNorm
            - 4.0 * determinant * determinant);
    return std::sqrt(std::max(0.0,
        0.5 * (squaredNorm + std::sqrt(discriminant))));
}

} // namespace

BasicPixelBrushStroke::BasicPixelBrushStroke(Document& document, LayerId layerId,
    BrushSettings settings, std::unique_ptr<IBrushEngine> engine,
    std::unique_ptr<IBrushTip> tip, std::unique_ptr<IBrushGrain> grain,
    const IBrushAssetResolver* assetResolver,
    RasterEditTransactionOptions transactionOptions, BrushExecutionOptions executionOptions)
    : BasicPixelBrushStroke(document, layerId, std::move(settings),
          BrushCompositeMode::Paint, std::move(engine), std::move(tip),
          std::move(grain), assetResolver, transactionOptions, {}, {}, false, {}, executionOptions)
{
}

BasicPixelBrushStroke::BasicPixelBrushStroke(Document& document, LayerId layerId,
    BrushSettings settings, BrushCompositeMode compositeMode,
    std::unique_ptr<IBrushEngine> engine, std::unique_ptr<IBrushTip> tip,
    std::unique_ptr<IBrushGrain> grain,
    const IBrushAssetResolver* assetResolver,
    RasterEditTransactionOptions transactionOptions,
    std::function<PremultipliedColor(Vec2d)> sampledColor,
    std::string_view historyLabel, bool deferSampledWrites,
    std::function<void(const BrushDab&)> dabObserver, BrushExecutionOptions executionOptions)
    : document_(&document)
    , layerId_(layerId)
    , settings_(settings)
    , compositeMode_(compositeMode)
    , sampledColor_(std::move(sampledColor))
    , deferSampledWrites_(deferSampledWrites)
    , dabObserver_(std::move(dabObserver))
    , engine_(std::move(engine))
    , tip_(std::move(tip))
    , grain_(std::move(grain))
    , canvasExtent_(document.canvas().extent)
    , tileSize_(transactionOptions.journalTileSize)
    , executionOptions_(executionOptions)
{
    if (!assetResolver) {
        assetResolver = &builtinBrushAssetResolver();
    }
    if (!tip_) {
        tip_ = assetResolver->createTip(settings_.tip);
    }
    if (!grain_) {
        grain_ = assetResolver->createGrain(settings_.grain);
    }
    auto* layer = document.layer(layerId);
    if (!layer || !engine_ || !tip_ || !grain_
        || !std::holds_alternative<RasterLayer>(layer->payload)) {
        return;
    }
    auto& raster = std::get<RasterLayer>(layer->payload);
    if (!raster.surface) {
        return;
    }
    surface_ = raster.surface;
    const auto inverse = intrinsicTransform(*layer).inverted();
    if (!inverse) {
        return;
    }
    localToDocument_ = intrinsicTransform(*layer);
    documentToLocal_ = *inverse;
    surfaceExtent_ = raster.surface->extent();
    if (surfaceExtent_.width > std::uint32_t(std::numeric_limits<std::int32_t>::max())
        || surfaceExtent_.height > std::uint32_t(std::numeric_limits<std::int32_t>::max())) return;
    documentPixelFootprint_ = std::max(
        1.0e-6, localToDocument_.isAffine()?maximumTransformScale(localToDocument_):localToDocument_.maximumScaleOver({0,0,double(surfaceExtent_.width),double(surfaceExtent_.height)}));
    transaction_ = std::make_unique<RasterEditTransaction>(
        document, layerId,
        !historyLabel.empty() ? historyLabel : compositeMode_ == BrushCompositeMode::Erase
            ? "Eraser stroke" : "Brush stroke",
        transactionOptions);
    valid_ = transaction_->active();
}

BasicPixelBrushStroke::~BasicPixelBrushStroke()
{
    cancel();
}

bool BasicPixelBrushStroke::begin(const NormalizedPointerSample& sample)
{
    if (!valid_ || active_ || !transaction_->targetAvailable()) {
        return false;
    }
    active_ = true;
    ++stats_.inputSamples;
    try {
        if (!engine_->beginStroke(settings_, sample, *this)) { cancel(); return false; }
    } catch (const RasterWorkLimit&) { cancel(); return false; }
    flushPending();
    return true;
}

bool BasicPixelBrushStroke::append(const NormalizedPointerSample& sample)
{
    if (!active_ || !transaction_->targetAvailable()) {
        cancel();
        return false;
    }
    ++stats_.inputSamples;
    try {
        if (!engine_->appendSample(sample, *this)) { cancel(); return false; }
    } catch (const RasterWorkLimit&) { cancel(); return false; }
    flushPending();
    return true;
}

RasterEditCommitResult BasicPixelBrushStroke::end(
    const NormalizedPointerSample& sample, History& history)
{
    if (!finishInput(sample)) return RasterEditCommitResult::TargetUnavailable;
    return commit(history);
}

bool BasicPixelBrushStroke::finishInput(const NormalizedPointerSample& sample)
{
    if (!active_) {
        return false;
    }
    ++stats_.inputSamples;
    try {
        if (!engine_->endStroke(sample, *this)) {
            cancel(); return false;
        }
    } catch (const RasterWorkLimit&) {
        cancel();
        return false;
    }
    flushPending();
    return true;
}

RasterEditCommitResult BasicPixelBrushStroke::commit(History& history)
{
    if (!active_ || !transaction_->targetAvailable()) return RasterEditCommitResult::TargetUnavailable;
    active_ = false;
    const auto result = transaction_->commit(history);
    valid_ = false;
    surface_.reset();
    return result;
}

void BasicPixelBrushStroke::cancel() noexcept
{
    if (engine_) {
        engine_->cancelStroke();
    }
    if (transaction_) {
        transaction_->cancel();
    }
    active_ = false;
    valid_ = false;
    lastResolvedTipAngleDegrees_.reset();
    dabTileWork_.clear();
    std::vector<double>().swap(coverageScratch_);
    tiles_.clear();
    retainedPixels_=0;
    coverageBounds_={};
    surface_.reset();
}

void BasicPixelBrushStroke::emitDab(const BrushDab& dab)
{
    lastResolvedTipAngleDegrees_ = dab.tipAngleDegrees;
    ++stats_.emittedDabs;
    // Reconstruction consumers need the complete marked unknown footprint,
    // before selection/crop/opacity restrict the independently stored writes.
    if (dabObserver_) dabObserver_(dab);
    if (transaction_->selectionIsEmpty()) return;
    const auto preparedBounds = tip_->prepareDab(
        dab, settings_.hardness, documentPixelFootprint_);
    if (!std::isfinite(preparedBounds.left) || !std::isfinite(preparedBounds.top)
        || !std::isfinite(preparedBounds.right) || !std::isfinite(preparedBounds.bottom)) {
        failure_=BrushStrokeFailure::RasterWorkLimitExceeded;
        throw RasterWorkLimit {};
    }
    const auto bounds = transaction_->clipToCrop(localDabBounds(preparedBounds).clippedTo(surfaceBounds()));
    const auto candidates=std::uint64_t(bounds.width)*std::uint64_t(bounds.height);
    stats_.maximumCandidatePixels=std::max(stats_.maximumCandidatePixels,candidates);
    if (candidates>kMaximumBrushDabCandidatePixels) {
        stats_.rejectedCandidatePixels=candidates;
        failure_=BrushStrokeFailure::RasterWorkLimitExceeded;
        throw RasterWorkLimit {};
    }
    if (bounds.empty()) {
        return;
    }
    grain_->prepareDab(dab, documentPixelFootprint_);
    if (candidates >= 16'384 && emitParallelDab(dab, bounds)) return;
    for (std::int32_t localY = bounds.y; localY < bounds.bottom(); ++localY) {
        for (std::int32_t localX = bounds.x; localX < bounds.right(); ++localX) {
            if(!transaction_->cropAllows(localX,localY))continue;
            const auto documentCenter = localToDocument_.map(
                {static_cast<double>(localX) + 0.5,
                    static_cast<double>(localY) + 0.5});
            ++stats_.evaluatedPixels;
            if (documentCenter.x < 0.0 || documentCenter.y < 0.0
                || documentCenter.x >= static_cast<double>(canvasExtent_.width)
                || documentCenter.y >= static_cast<double>(canvasExtent_.height)) {
                continue;
            }
            // Admission is read-only and uses the same pinned input as the
            // transaction. Zero coverage must not allocate/copy journal tiles.
            if (transaction_->hasSelection() && transaction_->selectionCoverageAtDocumentPoint(documentCenter) == 0)
                continue;
            const auto tipCoverage = tip_->coverage(documentCenter);
            // Rotated/textured tip bounds include empty pixels. Grain cannot
            // add coverage there, so don't sample it outside the actual tip.
            if (tipCoverage <= 0.0) continue;
            const auto coverage = std::clamp(tipCoverage * grain_->modulation(documentCenter), 0.0, 1.0);
            if (coverage <= 0.0) {
                continue;
            }
            auto* tile = ensureTile(localX, localY);
            if (tile) {
                compositePixel(*tile, localX, localY, dab, coverage);
            }
        }
    }
}

bool BasicPixelBrushStroke::emitParallelDab(const BrushDab& dab, RectI bounds)
{
    if (executionOptions_.workers == 1 || sampledColor_ || deferSampledWrites_
        || !tip_->supportsConcurrentSampling() || !grain_->supportsConcurrentSampling()
        || !transaction_->supportsConcurrentAdmission()) return false;
    const auto pixels = std::size_t(bounds.width) * std::size_t(bounds.height);
    const auto requested = executionOptions_.workers ? executionOptions_.workers
        : static_cast<unsigned>(std::clamp(pixels / 8'192, std::size_t(2), std::size_t(8)));
    const auto workers = boundedParallelWorkerCount(requested);
    if (workers == 1) return false;

    // Reuse bounded scratch without vector's geometric growth exceeding the
    // per-dab work limit. This is temporary coverage, not another raster copy.
    if (coverageScratch_.capacity() < pixels) coverageScratch_.reserve(pixels);
    coverageScratch_.resize(pixels);
    stats_.coverageScratchBytes = coverageScratch_.capacity() * sizeof(double);
    dabTileWork_.clear();
    const auto tileSize = static_cast<int>(tileSize_);
    for (int y = bounds.y / tileSize * tileSize; y < bounds.bottom(); y += tileSize)
        for (int x = bounds.x / tileSize * tileSize; x < bounds.right(); x += tileSize)
            dabTileWork_.push_back({RectI{x, y, tileSize, tileSize}.clippedTo(bounds)});

    // Prepare every sampler on the owner, then only read immutable geometry /
    // masks on workers. Each tile owns disjoint scratch and counters.
    const bool started = tryBoundedParallel(workers, [&](unsigned rank, unsigned count) {
        for (std::size_t i = rank; i < dabTileWork_.size(); i += count) {
            auto& work = dabTileWork_[i];
            for (int y = work.region.y; y < work.region.bottom(); ++y)
                for (int x = work.region.x; x < work.region.right(); ++x) {
                    auto& coverage = coverageScratch_[std::size_t(y - bounds.y) * std::size_t(bounds.width) + std::size_t(x - bounds.x)];
                    coverage = 0;
                    if (!transaction_->cropAllows(x, y)) continue;
                    const auto point = localToDocument_.map({double(x) + .5, double(y) + .5});
                    ++work.evaluatedPixels;
                    if (point.x < 0 || point.y < 0 || point.x >= double(canvasExtent_.width)
                        || point.y >= double(canvasExtent_.height)) continue;
                    if (transaction_->hasSelection() && !transaction_->selectionCoverageAtDocumentPoint(point)) continue;
                    const auto tip = tip_->coverage(point);
                    if (tip <= 0) continue;
                    coverage = std::clamp(tip * grain_->modulation(point), 0.0, 1.0);
                    work.hasCoverage |= coverage > 0;
                }
        }
    });
    if (!started) return false; // Never queue interactive painting behind a repair job.

    // Surface reads and lazy allocations are still owner-thread-only. Empty
    // tips/selection/crop regions never capture or retain a raster tile.
    for (auto& work : dabTileWork_) {
        stats_.evaluatedPixels += work.evaluatedPixels;
        if (work.hasCoverage) work.tile = ensureTile(work.region.x, work.region.y);
    }
    const auto accumulate = [&](unsigned rank, unsigned count) {
        for (std::size_t i = rank; i < dabTileWork_.size(); i += count) {
            const auto& work = dabTileWork_[i];
            if (!work.tile) continue;
            for (int y = work.region.y; y < work.region.bottom(); ++y)
                for (int x = work.region.x; x < work.region.right(); ++x) {
                    const auto coverage = coverageScratch_[std::size_t(y - bounds.y) * std::size_t(bounds.width) + std::size_t(x - bounds.x)];
                    if (coverage > 0) compositePixel(*work.tile, x, y, dab, coverage);
                }
        }
    };
    // Tile ownership makes accumulation independent; do not parallelize dabs,
    // sampled-color callbacks, authoritative surface writes or history.
    if (!tryBoundedParallel(workers, accumulate)) accumulate(0, 1);
    ++stats_.parallelDabs;
    return true;
}

BasicPixelBrushStroke::StrokeTile* BasicPixelBrushStroke::ensureTile(
    std::int32_t localX, std::int32_t localY)
{
    const auto tileX = localX / static_cast<std::int32_t>(tileSize_);
    const auto tileY = localY / static_cast<std::int32_t>(tileSize_);
    const TileKey key {tileX, tileY};
    const auto found = tiles_.find(key);
    if (found != tiles_.end()) {
        return &found->second;
    }
    const RectI gridTile {
        tileX * static_cast<std::int32_t>(tileSize_),
        tileY * static_cast<std::int32_t>(tileSize_),
        static_cast<std::int32_t>(tileSize_),
        static_cast<std::int32_t>(tileSize_),
    };
    const auto region = gridTile.clippedTo(surfaceBounds());
    if (region.empty() || !surface_) {
        return nullptr;
    }
    const auto pixelCount = static_cast<std::size_t>(region.width)
        * static_cast<std::size_t>(region.height);
    // 192 MiB of accumulation, regardless of journal tile dimensions.
    if(sampledColor_ && pixelCount>16U*1024U*1024U-retainedPixels_) {
        failure_=BrushStrokeFailure::RasterWorkLimitExceeded;
        throw RasterWorkLimit{};
    }
    StrokeTile tile {
        .region = region,
        .original = std::vector<std::byte>(pixelCount * kBytesPerPixel),
        .working = std::vector<std::byte>(pixelCount * kBytesPerPixel),
        .flowCoverage = std::vector<float>(pixelCount, 0.0F),
        .pendingDirty = {},
    };
    // Stroke-local accumulation needs a stable source tile, but the history
    // journal remains lazy: RasterEditTransaction captures this region only
    // when flushPending performs the first byte-changing write.
    surface_->copyRgba8(region, tile.original,
        static_cast<std::size_t>(region.width) * kBytesPerPixel);
    tile.working = tile.original;
    auto [inserted, wasInserted] = tiles_.emplace(key, std::move(tile));
    retainedPixels_+=pixelCount;
    (void)wasInserted;
    stats_.retainedStrokeTiles = tiles_.size();
    return &inserted->second;
}

RectI BasicPixelBrushStroke::localDabBounds(
    const BrushTipBounds& bounds) const noexcept
{
    const auto docLeft=std::max(0.0,bounds.left), docTop=std::max(0.0,bounds.top);
    const auto docRight=std::min(double(canvasExtent_.width),bounds.right);
    const auto docBottom=std::min(double(canvasExtent_.height),bounds.bottom);
    if (docLeft>=docRight || docTop>=docBottom) {
        return {};
    }
    // The inverse horizon can cross an off-layer brush box even though the
    // finite layer quad is valid. Scan the bounded source in that uncommon case;
    // ordinary per-texel forward admission and the existing work budget apply.
    if(!documentToLocal_.isAffine()&&!documentToLocal_.validOver({docLeft,docTop,docRight-docLeft,docBottom-docTop}))
        return surfaceBounds();
    const std::array<Vec2d, 4> corners {
        documentToLocal_.map({docLeft, docTop}),
        documentToLocal_.map({docRight, docTop}),
        documentToLocal_.map({docRight, docBottom}),
        documentToLocal_.map({docLeft, docBottom}),
    };
    if (!std::all_of(corners.begin(), corners.end(), finitePoint)) {
        return {};
    }
    const auto [minimumX, maximumX] = std::minmax_element(corners.begin(), corners.end(),
        [](Vec2d left, Vec2d right) { return left.x < right.x; });
    const auto [minimumY, maximumY] = std::minmax_element(corners.begin(), corners.end(),
        [](Vec2d left, Vec2d right) { return left.y < right.y; });
    // Clip in double precision before converting. A legal tiny layer scale
    // can otherwise produce inverse-mapped coordinates far beyond INT32_MAX.
    const auto left = static_cast<std::int32_t>(std::clamp(std::floor(minimumX->x),0.0,double(surfaceExtent_.width)));
    const auto top = static_cast<std::int32_t>(std::clamp(std::floor(minimumY->y),0.0,double(surfaceExtent_.height)));
    const auto right = static_cast<std::int32_t>(std::clamp(std::ceil(maximumX->x),0.0,double(surfaceExtent_.width)));
    const auto bottom = static_cast<std::int32_t>(std::clamp(std::ceil(maximumY->y),0.0,double(surfaceExtent_.height)));
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

void BasicPixelBrushStroke::compositePixel(StrokeTile& tile,
    std::int32_t localX, std::int32_t localY, const BrushDab& dab,
    double tipAlpha)
{
    const auto column = static_cast<std::size_t>(localX - tile.region.x);
    const auto row = static_cast<std::size_t>(localY - tile.region.y);
    const auto pixelIndex = row * static_cast<std::size_t>(tile.region.width) + column;
    const auto previousCoverage = static_cast<double>(tile.flowCoverage[pixelIndex]);
    const auto deposit = std::clamp(dab.flow * tipAlpha, 0.0, 1.0);
    const auto accumulated = 1.0 - (1.0 - previousCoverage) * (1.0 - deposit);
    if (accumulated <= previousCoverage + 1.0e-9) {
        return;
    }
    tile.flowCoverage[pixelIndex] = static_cast<float>(accumulated);
    if(sampledColor_) {
        const RectI pixel{localX,localY,1,1};
        coverageBounds_=coverageBounds_.empty()?pixel:coverageBounds_.united(pixel);
    }

    if(!deferSampledWrites_)resolvePixel(tile, localX, localY, dab, accumulated);
}

void BasicPixelBrushStroke::resolvePixel(StrokeTile& tile,
    std::int32_t localX, std::int32_t localY, const BrushDab& dab, double accumulated)
{
    const auto offset = (std::size_t(localY-tile.region.y)*std::size_t(tile.region.width)
        +std::size_t(localX-tile.region.x))*kBytesPerPixel;

    std::array<std::byte, kBytesPerPixel> output {};
    const auto destinationAlpha = static_cast<double>(
        std::to_integer<std::uint8_t>(tile.original[offset + 3])) / 255.0;
    if (sampledSelectionResolved_) {
        const auto point = localToDocument_.map({double(localX) + .5, double(localY) + .5});
        accumulated *= double(transaction_->selectionCoverageAtDocumentPoint(point)) / 255.0;
    }
    if (compositeMode_ == BrushCompositeMode::Erase) {
        // RasterSurface stores straight RGBA. Porter-Duff destination-out
        // scales premultiplied color and alpha equally, so the canonical
        // straight RGB bytes remain unchanged while alpha is reduced.
        std::copy_n(tile.original.data() + offset, 3, output.data());
        const auto eraseCoverage = std::clamp(
            dab.strokeOpacity * accumulated, 0.0, 1.0);
        output[3] = static_cast<std::byte>(alphaToByte(
            destinationAlpha * (1.0 - eraseCoverage)));
    } else if (compositeMode_ == BrushCompositeMode::FilterColor && destinationAlpha <= 0) {
        std::copy_n(tile.original.data() + offset, kBytesPerPixel, output.data());
    } else if (sampledColor_) {
        const auto sampled = sampledColor_(localToDocument_.map({double(localX)+.5,double(localY)+.5}));
        if (sampled[3] <= 0) {
            // Invalid/transparent samples leave the original intact, including
            // hidden RGB. A Heal replacement may retract its provisional paint.
            std::copy_n(tile.original.data()+offset,kBytesPerPixel,output.data());
        } else if (compositeMode_ == BrushCompositeMode::FilterColor) {
            const double coverage = std::clamp(dab.strokeOpacity * accumulated, 0.0, 1.0);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto original = srgbToLinear(std::to_integer<std::uint8_t>(tile.original[offset + channel]));
                const auto filtered = std::clamp(double(sampled[channel]) / double(sampled[3]), 0.0, 1.0);
                output[channel] = std::byte(linearToSrgb(original + (filtered - original) * coverage));
            }
            output[3] = tile.original[offset + 3];
        } else {
            const double coverage=std::clamp(dab.strokeOpacity*accumulated,0.0,1.0);
            const double sourceAlpha=std::clamp(sampled[3]*coverage,0.0,1.0);
            const double outputAlpha=sourceAlpha+destinationAlpha*(1-sourceAlpha);
            const std::array<double,3> channels{sampled[0],sampled[1],sampled[2]};
            for (std::size_t channel=0;channel<3;++channel) {
                const double linear=channels[channel]*coverage
                    +srgbToLinear(std::to_integer<std::uint8_t>(tile.original[offset+channel]))
                        *destinationAlpha*(1-sourceAlpha);
                output[channel]=std::byte(linearToSrgb(outputAlpha>0?linear/outputAlpha:0));
            }
            output[3]=std::byte(alphaToByte(outputAlpha));
        }
    } else {
        const auto sourceAlpha = std::clamp(dab.strokeOpacity * accumulated
                * static_cast<double>(dab.color.alpha) / 255.0,
            0.0, 1.0);
        const auto outputAlpha = sourceAlpha
            + destinationAlpha * (1.0 - sourceAlpha);
        const std::array<std::uint8_t, 3> sourceChannels {
            dab.color.red, dab.color.green, dab.color.blue};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto destinationChannel = std::to_integer<std::uint8_t>(
                tile.original[offset + channel]);
            const auto premultiplied = srgbToLinear(sourceChannels[channel]) * sourceAlpha
                + srgbToLinear(destinationChannel) * destinationAlpha
                    * (1.0 - sourceAlpha);
            const auto channelValue = outputAlpha > 1.0e-12
                ? premultiplied / outputAlpha : 0.0;
            output[channel] = static_cast<std::byte>(linearToSrgb(channelValue));
        }
        output[3] = static_cast<std::byte>(alphaToByte(outputAlpha));
    }

    if (std::equal(output.begin(), output.end(),
            tile.working.begin() + static_cast<std::ptrdiff_t>(offset))) {
        return;
    }
    std::copy(output.begin(), output.end(),
        tile.working.begin() + static_cast<std::ptrdiff_t>(offset));

    const RectI changedPixel {localX, localY, 1, 1};
    tile.pendingDirty = tile.pendingDirty.empty()
        ? changedPixel : tile.pendingDirty.united(changedPixel);
    ++tile.changedPixels;
}

RectI BasicPixelBrushStroke::coverageBounds() const noexcept
{
    return coverageBounds_;
}

bool BasicPixelBrushStroke::copyCoverage(RectI region,std::span<float> coverage,
    const std::function<bool()>& cancelled) const
{
    if(coverage.size()!=std::size_t(region.width)*std::size_t(region.height))
        throw std::invalid_argument("Brush coverage buffer size mismatch");
    std::fill(coverage.begin(),coverage.end(),0);
    for(const auto& [key,tile]:tiles_) {
        (void)key;
        if(cancelled&&cancelled())return false;
        const auto intersection=tile.region.clippedTo(region);
        for(int y=intersection.y;y<intersection.bottom();++y)for(int x=intersection.x;x<intersection.right();++x) {
            const auto point=localToDocument_.map({double(x)+.5,double(y)+.5});
            const auto mask=transaction_->selectionCoverageAtDocumentPoint(point)/255.0;
            coverage[std::size_t(y-region.y)*std::size_t(region.width)+std::size_t(x-region.x)]
                =float(tile.flowCoverage[std::size_t(y-tile.region.y)*std::size_t(tile.region.width)
                    +std::size_t(x-tile.region.x)]*mask);
        }
    }
    return true;
}

bool BasicPixelBrushStroke::reapplySampledColor(const std::function<PremultipliedColor(Vec2d)>& source,
    const std::function<bool()>& cancelled)
{
    return reapplySampledColorImpl(source, cancelled, false);
}

bool BasicPixelBrushStroke::reapplySampledColorSelectionResolved(const std::function<PremultipliedColor(Vec2d)>& source,
    const std::function<bool()>& cancelled)
{
    return reapplySampledColorImpl(source, cancelled, true);
}

bool BasicPixelBrushStroke::reapplySampledColorImpl(const std::function<PremultipliedColor(Vec2d)>& source,
    const std::function<bool()>& cancelled, bool selectionResolved)
{
    if(!active_ || !transaction_->targetAvailable() || !sampledColor_ || !source)return false;
    sampledColor_=source;
    sampledSelectionResolved_=selectionResolved;
    deferSampledWrites_=false;
    BrushDab dab;
    dab.strokeOpacity=settings_.opacity;
    for(auto& [key,tile]:tiles_) {
        (void)key;
        if(cancelled&&cancelled())return false;
        for(int y=0;y<tile.region.height;++y)for(int x=0;x<tile.region.width;++x) {
            const auto coverage=tile.flowCoverage[std::size_t(y)*std::size_t(tile.region.width)+std::size_t(x)];
            if(coverage>0)resolvePixel(tile,tile.region.x+x,tile.region.y+y,dab,coverage);
        }
    }
    flushPending();
    return true;
}

void BasicPixelBrushStroke::flushPending()
{
    if(deferSampledWrites_)return;
    std::vector<RasterPatch> patches;
    patches.reserve(tiles_.size());
    for (auto& [key, tile] : tiles_) {
        (void)key;
        stats_.changedPixels += tile.changedPixels;
        tile.changedPixels = 0;
        if (tile.pendingDirty.empty()) {
            continue;
        }
        const auto stride = static_cast<std::size_t>(tile.region.width) * kBytesPerPixel;
        const auto offset = static_cast<std::size_t>(
                tile.pendingDirty.y - tile.region.y)
                * stride
            + static_cast<std::size_t>(tile.pendingDirty.x - tile.region.x)
                * kBytesPerPixel;
        patches.push_back({tile.pendingDirty,
            std::span<const std::byte>(tile.working).subspan(offset), stride});
        stats_.uploadedRegionBytes += static_cast<std::uint64_t>(
            tile.pendingDirty.width) * static_cast<std::uint64_t>(
            tile.pendingDirty.height) * kBytesPerPixel;
    }
    if (patches.empty()) {
        lastDirty_.fullRefresh = false;
        lastDirty_.regions.clear();
        return;
    }
    lastDirty_ = sampledSelectionResolved_ ? transaction_->writeSelectionResolvedRgba8Batch(patches)
        : transaction_->writeRgba8Batch(patches);
    ++stats_.surfaceWriteBatches;
    for (auto& [key, tile] : tiles_) {
        (void)key;
        tile.pendingDirty = {};
    }
}

RectI BasicPixelBrushStroke::surfaceBounds() const noexcept
{
    return {0, 0, static_cast<std::int32_t>(surfaceExtent_.width),
        static_cast<std::int32_t>(surfaceExtent_.height)};
}

} // namespace imageeditor::core
