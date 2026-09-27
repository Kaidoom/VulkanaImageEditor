#include "imageeditor/core/SpotHealStroke.hpp"

#include "imageeditor/core/LayerGeometry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::core {
namespace {
constexpr std::size_t markTileLimit = 4096;
constexpr std::size_t maskAreaLimit = 16U * 1024U * 1024U;
bool stopped(const SpotHealOptions& options) { return options.cancelled && options.cancelled(); }
std::size_t area(RectI r) { return std::size_t(r.width) * std::size_t(r.height); }

RectI mappedBounds(const BrushTipBounds& b, const AffineTransform& mapping, Extent2u extent)
{
    if(!mapping.isAffine()&&!mapping.validOver({b.left,b.top,b.right-b.left,b.bottom-b.top}))
        return {0,0,int(extent.width),int(extent.height)};
    const std::array points{mapping.map({b.left, b.top}), mapping.map({b.right, b.top}),
        mapping.map({b.right, b.bottom}), mapping.map({b.left, b.bottom})};
    double left = std::numeric_limits<double>::infinity(), top = left;
    double right = -left, bottom = right;
    for (auto p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw std::length_error("Spot Heal footprint has an invalid transform.");
        left = std::min(left, p.x); right = std::max(right, p.x);
        top = std::min(top, p.y); bottom = std::max(bottom, p.y);
    }
    const int x = int(std::clamp(std::floor(left), 0.0, double(extent.width)));
    const int y = int(std::clamp(std::floor(top), 0.0, double(extent.height)));
    const int x1 = int(std::clamp(std::ceil(right), 0.0, double(extent.width)));
    const int y1 = int(std::clamp(std::ceil(bottom), 0.0, double(extent.height)));
    return {x, y, x1 - x, y1 - y};
}

template<class Tiles>
void mark(Tiles& tiles, RectI& bounds, int x, int y)
{
    const auto key = std::make_pair(x / 128, y / 128);
    auto found = tiles.find(key);
    if (found == tiles.end()) {
        if (tiles.size() >= markTileLimit) throw std::length_error("Spot Heal marked region exceeds its memory budget. Use a shorter stroke.");
        found = tiles.try_emplace(key).first;
    }
    found->second[std::size_t(y % 128) * 128 + std::size_t(x % 128)] = 255;
    bounds = bounds.united({x, y, 1, 1});
}

template<class Tiles>
SelectionState snapshotMask(const Tiles& tiles, RectI bounds, Extent2u extent)
{
    if (bounds.empty()) return SelectionMask::filled(extent, 0);
    if (area(bounds) > maskAreaLimit) throw std::length_error("Spot Heal marked bounds exceed the mask budget. Use a shorter stroke.");
    std::vector<std::uint8_t> pixels(area(bounds));
    for (const auto& [key, tile] : tiles) {
        const auto intersection = RectI{key.first * 128, key.second * 128, 128, 128}.clippedTo(bounds);
        for (int y = intersection.y; y < intersection.bottom(); ++y)
            for (int x = intersection.x; x < intersection.right(); ++x)
                pixels[std::size_t(y - bounds.y) * std::size_t(bounds.width) + std::size_t(x - bounds.x)]
                    = tile[std::size_t(y % 128) * 128 + std::size_t(x % 128)];
    }
    return SelectionMask::fromR8Region(extent, bounds, pixels, std::size_t(bounds.width));
}

RectI padded(RectI bounds, int margin, Extent2u extent)
{
    const auto x = std::max(0, bounds.x - margin), y = std::max(0, bounds.y - margin);
    const auto right = std::min(std::int64_t(extent.width), std::int64_t(bounds.right()) + margin);
    const auto bottom = std::min(std::int64_t(extent.height), std::int64_t(bounds.bottom()) + margin);
    return {x, y, int(right - x), int(bottom - y)};
}

PremultipliedColor candidateAt(const SpotHealSolved& solved, Vec2d gridPoint)
{
    const auto& b = solved.gridBounds;
    const double x = std::clamp(gridPoint.x - .5 - b.x, 0.0, double(b.width - 1));
    const double y = std::clamp(gridPoint.y - .5 - b.y, 0.0, double(b.height - 1));
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    PremultipliedColor output{};
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
        const int ix = std::min(x0 + dx, b.width - 1), iy = std::min(y0 + dy, b.height - 1);
        const float weight = float((dx ? x - x0 : 1 - (x - x0)) * (dy ? y - y0 : 1 - (y - y0)));
        const auto& color = solved.repair.pixels[std::size_t(iy) * std::size_t(b.width) + std::size_t(ix)];
        for (std::size_t channel = 0; channel < 4; ++channel) output[channel] += color[channel] * weight;
    }
    return output;
}
} // namespace

struct SpotHealWork {
    explicit SpotHealWork(CloneReference source) : reference(std::move(source)) {}
    CloneReference reference;
    SelectionState unknown;
    Extent2u gridExtent;
    AffineTransform gridToDocument;
    AffineTransform localToDocument;
    RectI writeBounds;
    std::vector<float> writeCoverage;
    bool direct {true};
    std::uint64_t seed {1};
    float adaptation {.8F};
};

std::string spotHealTargetDiagnostic(const Document& document, LayerId target, CloneSampleSource source)
{
    const auto* layer = document.layer(target);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    if (!raster || !raster->surface) return "Spot Heal needs a raster destination; text and shapes remain editable.";
    if (!document.isEffectivelyVisible(target)) return "Reveal the destination layer before using Spot Heal.";
    const auto inverse = intrinsicTransform(*layer).inverted();
    if (!inverse || !std::isfinite(inverse->m00) || !std::isfinite(inverse->m01)
        || !std::isfinite(inverse->m10) || !std::isfinite(inverse->m11))
        return "Spot Heal needs a nonsingular destination transform.";
    const auto e = raster->surface->extent();
    if (e.empty() || e.width > std::uint32_t(std::numeric_limits<int>::max())
        || e.height > std::uint32_t(std::numeric_limits<int>::max())) return "Spot Heal destination dimensions are unsupported.";
    if (source == CloneSampleSource::SourceLayer) return {};
    const bool adjusted = layer->adjustments && std::any_of(layer->adjustments->items.begin(), layer->adjustments->items.end(),
        [](const Adjustment& a) { return a.enabled && !adjustmentIsNeutral(a); });
    if (layer->blendMode != BlendMode::Normal || layer->opacity != 1.0F || adjusted || hasActiveSpatialFilters(layer->filters)||hasActiveLayerEffects(layer->effects))
        return "Rendered Spot Heal needs a Normal, 100% opacity retouch layer without active effects. Use Source Layer for direct repair.";
    if (source == CloneSampleSource::AllVisible) {
        bool above = false;
        for (const auto& item : document.layers()) {
            if (above && document.isEffectivelyVisible(item.id) && item.opacity > 0)
                return "Place the retouch layer above all visible layers, or choose Current & Below.";
            if (item.id == target) above = true;
        }
    }
    return {};
}

SpotHealStroke::SpotHealStroke(Document& document, LayerId target, BrushSettings brush, CloneSettings settings,
    CloneReference reference, const IBrushAssetResolver* assets)
    : document_(document), target_(target), settings_(brush), cloneSettings_(settings), reference_(std::move(reference))
    , canvas_(document.canvas().extent)
    , hardTip_((assets ? assets : &builtinBrushAssetResolver())->createTip(brush.tip))
    , brush_(document, target, brush,
        settings.source == CloneSampleSource::SourceLayer ? BrushCompositeMode::FilterColor : BrushCompositeMode::Paint,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, assets, {}, [](Vec2d) { return PremultipliedColor{}; },
        "Spot Heal stroke", true, [this](const BrushDab& dab) { observeDab(dab); })
{
    diagnostic_ = spotHealTargetDiagnostic(document, target, settings.source);
    const auto* layer = document.layer(target);
    if (layer) if (const auto* raster = std::get_if<RasterLayer>(&layer->payload); raster && raster->surface) {
        surface_ = raster->surface->id(); surfaceRevision_ = raster->surface->revision(); extent_ = raster->surface->extent();
        localToDocument_ = intrinsicTransform(*layer);
        if (const auto inverse = localToDocument_.inverted()) documentToLocal_ = *inverse;
    }
    revision_ = document.revision();
    for (const auto& source : document.layers()) if (const auto* raster = std::get_if<RasterLayer>(&source.payload); raster && raster->surface)
        sourceRevisions_.emplace_back(raster->surface, raster->surface->revision());
}
SpotHealStroke::~SpotHealStroke() { cancel(); }

bool SpotHealStroke::targetMatches() const noexcept
{
    const auto* layer = document_.layer(target_);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    return document_.revision() == revision_ && raster && raster->surface && raster->surface->id() == surface_
        && raster->surface->revision() == surfaceRevision_ && intrinsicTransform(*layer) == localToDocument_
        && std::all_of(sourceRevisions_.begin(), sourceRevisions_.end(), [](const auto& source) { return source.first->revision() == source.second; });
}

void SpotHealStroke::observeDab(const BrushDab& dab)
{
    if (!hardTip_) throw std::runtime_error("Spot Heal brush tip is unavailable.");
    auto bounds = hardTip_->prepareDab(dab, 1.0, 1.0);
    auto pixels = mappedBounds(bounds, {}, canvas_);
    if (area(pixels) > kMaximumBrushDabCandidatePixels) throw std::length_error("Spot Heal brush footprint exceeds its work budget.");
    for (int y = pixels.y; y < pixels.bottom(); ++y) for (int x = pixels.x; x < pixels.right(); ++x)
        if (hardTip_->coverage({x + .5, y + .5}) > 0) mark(marks_, markBounds_, x, y);
    // The native mask additionally accounts for the transformed texel's
    // footprint. This is independent of crop, selection, grain or flow.
    const double footprint = localToDocument_.maximumScaleOver({0,0,double(extent_.width),double(extent_.height)});
    bounds = hardTip_->prepareDab(dab, 1.0, std::max(1e-6, footprint));
    pixels = mappedBounds(bounds, documentToLocal_, extent_);
    if (area(pixels) > kMaximumBrushDabCandidatePixels) throw std::length_error("Spot Heal transformed footprint exceeds its work budget.");
    for (int y = pixels.y; y < pixels.bottom(); ++y) for (int x = pixels.x; x < pixels.right(); ++x)
        if (hardTip_->coverage(localToDocument_.map({x + .5, y + .5})) > 0) mark(nativeMarks_, nativeMarkBounds_, x, y);
    ++markRevision_;
}

SelectionState SpotHealStroke::previewMask() const
{
    if (!preview_ || previewRevision_ != markRevision_) {
        preview_ = snapshotMask(marks_, markBounds_, canvas_); previewRevision_ = markRevision_;
    }
    return preview_;
}

bool SpotHealStroke::begin(const NormalizedPointerSample& sample)
{
    try {
        if (!diagnostic_.empty() || !targetMatches() || !brush_.begin(sample)) { cancel(); return false; }
        return true;
    } catch (const std::exception& error) { diagnostic_ = error.what(); cancel(); return false; }
}
bool SpotHealStroke::append(const NormalizedPointerSample& sample)
{
    try {
        if (finished_ || !targetMatches() || !brush_.append(sample)) { cancel(); return false; }
        return true;
    } catch (const std::exception& error) { diagnostic_ = error.what(); cancel(); return false; }
}

std::shared_ptr<const SpotHealWork> SpotHealStroke::finishInput(const NormalizedPointerSample& sample)
{
    try {
        if (finished_ || !targetMatches() || !brush_.finishInput(sample)) throw std::runtime_error("Spot Heal source or destination changed; stroke cancelled.");
        finished_ = true;
        const auto writes = brush_.coverageBounds();
        if (writes.empty() || settings_.opacity <= 0) return {};
        if (area(writes) > maskAreaLimit) throw std::length_error("Spot Heal write region exceeds its mask budget.");
        auto result = std::make_shared<SpotHealWork>(reference_);
        result->direct = cloneSettings_.source == CloneSampleSource::SourceLayer;
        result->unknown = result->direct ? snapshotMask(nativeMarks_, nativeMarkBounds_, extent_) : previewMask();
        result->gridExtent = result->direct ? extent_ : canvas_;
        result->gridToDocument = result->direct ? localToDocument_ : AffineTransform{};
        result->localToDocument = localToDocument_;
        result->writeBounds = writes; result->writeCoverage.resize(area(writes));
        if (!brush_.copyCoverage(writes, result->writeCoverage)) throw std::runtime_error("Spot Heal write mask is unavailable.");
        if (std::none_of(result->writeCoverage.begin(), result->writeCoverage.end(), [](float c) { return c > 0; })) return {};
        result->seed = settings_.deterministicSeed; result->adaptation = float(cloneSettings_.adaptation);
        work_ = result; return result;
    } catch (const std::exception& error) { diagnostic_ = error.what(); cancel(); return {}; }
}

SpotHealSolved SpotHealStroke::solve(std::shared_ptr<const SpotHealWork> work, const SpotHealOptions& options)
{
    SpotHealSolved solved; solved.input = work;
    using Clock = std::chrono::steady_clock;
    const auto started = options.captureProfile ? Clock::now() : Clock::time_point{};
    const auto finish = [&]() {
        if (options.captureProfile)
            solved.workerPreparationMilliseconds = std::max(0.0,
                std::chrono::duration<double, std::milli>(Clock::now() - started).count()
                    - solved.solveMilliseconds);
        return std::move(solved);
    };
    if (!work) { solved.repair.status = SpotHealStatus::InvalidInput; return finish(); }
    try {
        if (stopped(options)) { solved.repair.status = SpotHealStatus::Cancelled; return finish(); }
        const auto reference = work->reference.prepared(options.cancelled, options.maxWorkingBytes);
        const auto remainingBytes = options.maxWorkingBytes - reference.snapshotBytes();
        auto marked = work->unknown->bounds();
        if (marked.empty()) { solved.repair.status = SpotHealStatus::NoUnknownPixels; return finish(); }
        if (!work->direct) {
            // A Normal retouch layer can safely replace an opaque visible
            // backdrop. Arbitrary RGBA inverse compositing is not attempted.
            for (int y = 0; y < work->writeBounds.height; ++y) {
                if (stopped(options)) { solved.repair.status = SpotHealStatus::Cancelled; return finish(); }
                for (int x = 0; x < work->writeBounds.width; ++x) {
                    if (work->writeCoverage[std::size_t(y) * std::size_t(work->writeBounds.width) + std::size_t(x)] <= 0) continue;
                    const auto p = work->localToDocument.map({work->writeBounds.x + x + .5, work->writeBounds.y + y + .5});
                    if (reference.sample(p)[3] < 1.0F - 1e-6F) {
                        solved.repair.status = SpotHealStatus::InvalidInput;
                        solved.repair.diagnostics.message = "Rendered Spot Heal requires opaque reference under the writable repair. Use Source Layer to preserve partial alpha.";
                        return finish();
                    }
                }
            }
        }
        const auto support = work->direct ? Vec2d{} : reference.readSupport();
        if (!std::isfinite(support.x) || !std::isfinite(support.y) || support.x > 4096 || support.y > 4096)
            throw std::length_error("Spot Heal reference filtering support is too large for safe donor exclusion.");
        const auto excluded = work->direct ? work->unknown
            : work->unknown->adjusted(int(std::ceil(support.x)) + 1, int(std::ceil(support.y)) + 1);
        // Filter-contaminated support is reconstructed as unknown too, not
        // left as a zero-colored gap between the repair and trusted boundary.
        // This expands inference only; the original soft write mask is fixed.
        marked = excluded->bounds();
        // Locality is automatic context admission, never a brush-sized donor
        // radius. Retry with wider trusted context when the solver requests it.
        const auto pixelLimit = std::min(options.maxPixels, remainingBytes / (options.captureDiagnostics ? 320U : 288U));
        int margin = 256;
        auto bounds = padded(marked, margin, work->gridExtent);
        while (area(bounds) > pixelLimit && margin > 16) { margin /= 2; bounds = padded(marked, margin, work->gridExtent); }
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (area(bounds) > pixelLimit) throw std::length_error("Spot Heal repair and trustworthy context exceed the solve budget. Use a shorter stroke.");
            std::vector<PremultipliedColor> colors(area(bounds));
            std::vector<std::uint8_t> unknown(area(bounds)), valid(area(bounds));
            for (int y = 0; y < bounds.height; ++y) {
                if (stopped(options)) { solved.repair.status = SpotHealStatus::Cancelled; return finish(); }
                for (int x = 0; x < bounds.width; ++x) {
                    const auto i = std::size_t(y) * std::size_t(bounds.width) + std::size_t(x);
                    const int gx = bounds.x + x, gy = bounds.y + y;
                    unknown[i] = excluded->coverageAtDocumentPixel(gx, gy);
                    // Do not even request contaminated pixels. The solver
                    // cannot see original blemish RGB through gradients,
                    // descriptors or pyramid construction.
                    if (excluded->coverageAtDocumentPixel(gx, gy)) continue;
                    colors[i] = work->direct ? reference.rawTexel(gx, gy) : reference.sample({gx + .5, gy + .5});
                    valid[i] = colors[i][3] > 0 ? 255 : 0;
                }
            }
            auto effective = options; effective.seed ^= work->seed; effective.adaptation = work->adaptation;
            solved.gridBounds = bounds; solved.gridToDocument = work->gridToDocument;
            const auto inputBytes = colors.size() * sizeof(PremultipliedColor) + unknown.size() + valid.size();
            solved.preparedBytes = reference.snapshotBytes() + inputBytes;
            effective.maxWorkingBytes = remainingBytes - inputBytes;
            const auto solveStarted = options.captureProfile ? Clock::now() : Clock::time_point{};
            solved.repair = repairSpotHeal({bounds.width, bounds.height, colors, unknown, valid}, effective);
            if (options.captureProfile)
                solved.solveMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - solveStarted).count();
            if (solved.repair.status != SpotHealStatus::InsufficientContext) return finish();
            const auto wider = padded(marked, margin * 2, work->gridExtent);
            if (wider == bounds || area(wider) > pixelLimit) return finish();
            margin *= 2; bounds = wider;
        }
    } catch (const std::length_error& error) {
        solved.repair.status = SpotHealStatus::LimitExceeded; solved.repair.diagnostics.message = error.what();
    } catch (const std::exception& error) {
        solved.repair.status = stopped(options) ? SpotHealStatus::Cancelled : SpotHealStatus::InvalidInput;
        solved.repair.diagnostics.message = error.what();
    }
    return finish();
}

RasterEditCommitResult SpotHealStroke::publish(const SpotHealSolved& solved, History& history,
    const std::function<bool()>& cancelled)
{
    try {
        if (!work_ || solved.input != work_ || !targetMatches() || (cancelled && cancelled()))
            throw std::runtime_error("Spot Heal source changed or operation cancelled; repair discarded.");
        if (!solved.repair || solved.repair.status == SpotHealStatus::NoUnknownPixels) {
            diagnostic_ = solved.repair.diagnostics.message;
            if (diagnostic_.empty()) diagnostic_ = "Spot Heal could not find sufficient trustworthy context; repair discarded.";
            cancel(); return RasterEditCommitResult::TargetUnavailable;
        }
        if (solved.gridBounds.empty() || solved.repair.pixels.size() != area(solved.gridBounds))
            throw std::runtime_error("Spot Heal returned an incomplete reconstruction; repair discarded.");
        const bool direct = work_->direct;
        const auto sampler = [&](Vec2d p) {
            if (direct) {
                const auto local = documentToLocal_.map(p);
                const int x = int(std::lround(local.x - .5)) - solved.gridBounds.x;
                const int y = int(std::lround(local.y - .5)) - solved.gridBounds.y;
                if (x < 0 || y < 0 || x >= solved.gridBounds.width || y >= solved.gridBounds.height) return PremultipliedColor{};
                return solved.repair.pixels[std::size_t(y) * std::size_t(solved.gridBounds.width) + std::size_t(x)];
            }
            auto value = candidateAt(solved, p);
            if (value[3] > 0) { for (int c = 0; c < 3; ++c) value[std::size_t(c)] /= value[3]; value[3] = 1; }
            return value;
        };
        if (!brush_.reapplySampledColorSelectionResolved(sampler, cancelled) || (cancelled && cancelled()))
            throw std::runtime_error("Spot Heal publication cancelled; original pixels restored.");
        const auto result = brush_.commit(history);
        if (result == RasterEditCommitResult::NoChanges) diagnostic_ = "Spot Heal produced no pixel changes.";
        return result;
    } catch (const std::exception& error) { diagnostic_ = error.what(); cancel(); return RasterEditCommitResult::TargetUnavailable; }
}

RasterEditCommitResult SpotHealStroke::commitNoop(History& history)
{
    if (!finished_ || work_ || !targetMatches()) { cancel(); return RasterEditCommitResult::TargetUnavailable; }
    diagnostic_ = "Spot Heal has no writable coverage.";
    return brush_.commit(history);
}
void SpotHealStroke::cancel() noexcept
{
    brush_.cancel(); work_.reset(); marks_.clear(); nativeMarks_.clear(); preview_.reset();
}
} // namespace imageeditor::core
