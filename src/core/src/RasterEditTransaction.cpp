#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace imageeditor::core {
RectI RasterEditTransaction::clipToCrop(RectI bounds) const noexcept
{
    if(ignoreCrop_ || !crop_)return bounds;
    if(crop_->empty())return {};
    // Admitted crop coordinates/endpoints fit int32. Include partially visible
    // texels, leaving fractional output coverage solely to the compositor.
    const auto x=int(std::floor(crop_->x)),y=int(std::floor(crop_->y));
    const auto right=int(std::ceil(crop_->right())),bottom=int(std::ceil(crop_->bottom()));
    return bounds.clippedTo({x,y,right-x,bottom-y});
}
namespace {

constexpr std::size_t kBytesPerPixel = 4;

std::size_t checkedPixelBytes(RectI region)
{
    if (region.empty()) {
        return 0;
    }
    const auto width = static_cast<std::size_t>(region.width);
    const auto height = static_cast<std::size_t>(region.height);
    if (width > std::numeric_limits<std::size_t>::max() / height / kBytesPerPixel) {
        throw std::overflow_error("Raster journal tile is too large");
    }
    return width * height * kBytesPerPixel;
}

template <std::size_t Capacity>
std::uint8_t copyLabel(std::array<char, Capacity>& destination, std::string_view source)
{
    static_assert(Capacity > 1);
    const auto length = std::min(source.size(), Capacity - 1);
    std::copy_n(source.data(), length, destination.data());
    destination[length] = '\0';
    return static_cast<std::uint8_t>(length);
}

const RasterSurface* rasterSurfaceForLayer(const Document& document,
    LayerId layerId, SurfaceId expectedSurfaceId, Extent2u expectedExtent) noexcept
{
    const auto* layer = document.layer(layerId);
    if (!layer || !std::holds_alternative<RasterLayer>(layer->payload)) {
        return nullptr;
    }
    const auto& raster = std::get<RasterLayer>(layer->payload);
    if (!raster.surface || raster.surface->id() != expectedSurfaceId
        || raster.surface->extent() != expectedExtent) {
        return nullptr;
    }
    return raster.surface.get();
}

RasterSurface* rasterSurfaceForLayer(Document& document,
    LayerId layerId, SurfaceId expectedSurfaceId, Extent2u expectedExtent) noexcept
{
    return const_cast<RasterSurface*>(rasterSurfaceForLayer(
        std::as_const(document), layerId, expectedSurfaceId, expectedExtent));
}

bool samePixel(std::span<const std::byte> left, std::span<const std::byte> right,
    std::size_t leftOffset, std::size_t rightOffset) noexcept
{
    return std::memcmp(left.data() + leftOffset, right.data() + rightOffset,
               kBytesPerPixel)
        == 0;
}

// Storage growth and pixel edits are one history action. Subsequent strokes
// can mutate the regional surface; retain its COW tile state for exact redo.
class GrowingRasterCommand final : public Command {
public:
    GrowingRasterCommand(LayerId id, std::shared_ptr<RasterSurface> before,
        std::shared_ptr<RegionalRasterSurface> after, Vec2d origin,
        std::optional<RectD> frame, std::optional<RectD> nextFrame,
        AffineTransform external, std::string label)
        : id_(id), before_(std::move(before)), after_(std::move(after)),
          state_(after_->state()), origin_(origin), frame_(frame), nextFrame_(nextFrame),
          external_(external), label_(std::move(label)) {
        cost_=sizeof(*this)+state_.tiles.size()*96;
        const auto* old=dynamic_cast<const RegionalRasterSurface*>(before_.get());
        for(const auto& [key,tile]:state_.tiles)
            if(!old||!old->state().tiles.contains(key)||old->state().tiles.at(key)!=tile)
                cost_+=sizeof(RegionalRasterSurface::Tile);
    }
    bool apply(Document& d) override {
        if (!matches(d, before_, origin_)) return false;
        after_->restore(state_);
        return d.setLayerRasterStorage(id_,after_,after_->origin(),nextFrame_);
    }
    bool undo(Document& d) override {
        if (!canAdoptApplied(d)) return false;
        return d.setLayerRasterStorage(id_,before_,origin_,frame_);
    }
    bool canAdoptApplied(const Document& d) const noexcept override {
        return matches(d,after_,{double(state_.bounds.x),double(state_.bounds.y)});
    }
    std::string_view label() const noexcept override { return label_; }
    std::size_t memoryCost() const noexcept override {return cost_;}
private:
    bool matches(const Document& d,const std::shared_ptr<RasterSurface>& surface,Vec2d origin) const noexcept {
        const auto* l=d.layer(id_);const auto* r=l?std::get_if<RasterLayer>(&l->payload):nullptr;
        return r&&r->surface==surface&&l->rasterOrigin==origin&&l->localToDocument==external_;
    }
    LayerId id_;
    std::shared_ptr<RasterSurface> before_;
    std::shared_ptr<RegionalRasterSurface> after_;
    RegionalRasterSurface::State state_;
    Vec2d origin_;
    std::optional<RectD> frame_,nextFrame_;
    AffineTransform external_;
    std::string label_;
    std::size_t cost_{};
};

} // namespace

RasterEditCommand::RasterEditCommand(LayerId layerId, SurfaceId surfaceId,
    Extent2u surfaceExtent, std::string_view label, std::vector<TileSnapshot> tiles)
    : layerId_(layerId)
    , surfaceId_(surfaceId)
    , surfaceExtent_(surfaceExtent)
    , labelLength_(copyLabel(label_, label))
    , tiles_(std::move(tiles))
{
    for (auto& tile : tiles_) {
        tile.pixels.shrink_to_fit();
        tile.changedRegions.shrink_to_fit();
    }
    tiles_.shrink_to_fit();
}

bool RasterEditCommand::apply(Document& document)
{
    return swapWithSurface(document);
}

bool RasterEditCommand::undo(Document& document)
{
    return swapWithSurface(document);
}

std::string_view RasterEditCommand::label() const noexcept
{
    return {label_.data(), labelLength_};
}

std::size_t RasterEditCommand::memoryCost() const noexcept
{
    auto result = sizeof(*this) + tiles_.capacity() * sizeof(TileSnapshot);
    for (const auto& tile : tiles_) {
        result += tile.pixels.capacity() * sizeof(std::byte);
        result += tile.changedRegions.capacity() * sizeof(RectI);
    }
    return result;
}

std::size_t RasterEditCommand::journalPixelBytes() const noexcept
{
    std::size_t result = 0;
    for (const auto& tile : tiles_) {
        result += tile.pixels.size();
    }
    return result;
}

bool RasterEditCommand::canAdoptApplied(const Document& document) const noexcept
{
    return !tiles_.empty() && resolve(document) != nullptr;
}

RasterSurface* RasterEditCommand::resolve(Document& document) const noexcept
{
    return rasterSurfaceForLayer(document, layerId_, surfaceId_, surfaceExtent_);
}

const RasterSurface* RasterEditCommand::resolve(const Document& document) const noexcept
{
    return rasterSurfaceForLayer(document, layerId_, surfaceId_, surfaceExtent_);
}

bool RasterEditCommand::swapWithSurface(Document& document)
{
    auto* surface = resolve(document);
    if (!surface) {
        return false;
    }
    return swapWithSurface(*surface);
}

bool RasterEditCommand::swapWithSurface(RasterSurface& surface)
{
    std::size_t patchCount = 0;
    for (const auto& tile : tiles_) {
        patchCount += tile.changedRegions.size();
    }
    std::vector<MutableRasterPatch> patches;
    patches.reserve(patchCount);
    for (auto& tile : tiles_) {
        const auto tileStride = static_cast<std::size_t>(tile.tileRegion.width)
            * kBytesPerPixel;
        for (const auto region : tile.changedRegions) {
            const auto x = static_cast<std::size_t>(region.x - tile.tileRegion.x);
            const auto y = static_cast<std::size_t>(region.y - tile.tileRegion.y);
            const auto offset = y * tileStride + x * kBytesPerPixel;
            patches.push_back({region,
                std::span<std::byte>(tile.pixels).subspan(offset), tileStride});
        }
    }
    const auto dirty = surface.swapRgba8Batch(patches);
    return !dirty.empty();
}

RasterEditTransaction::RasterEditTransaction(Document& document, LayerId layerId,
    std::string_view historyLabel, RasterEditTransactionOptions options)
    : document_(&document)
    , layerId_(layerId)
    , canvasExtent_(document.canvas().extent)
    , tileSize_(options.journalTileSize)
    , pinnedSelection_(document.selection())
    , selectionMask_(options.selectionMask ? options.selectionMask : pinnedSelection_.get())
    , selectionRevision_(selectionMask_ ? selectionMask_->revision() : 0)
    , historyLabelLength_(copyLabel(historyLabel_, historyLabel))
{
    if (tileSize_ == 0 || tileSize_ > 1024) {
        throw std::invalid_argument("Raster journal tile size must be between 1 and 1024");
    }
    auto* layer = document.layer(layerId_);
    if (!layer || !std::holds_alternative<RasterLayer>(layer->payload)) {
        return;
    }
    auto& raster = std::get<RasterLayer>(layer->payload);
    if (!raster.surface) {
        return;
    }
    liveSurface_ = raster.surface;
    surfaceId_ = liveSurface_->id();
    surfaceExtent_ = liveSurface_->extent();
    localToDocument_ = intrinsicTransform(*layer);
    layerCrop_=layer->crop;
    crop_=layerCrop_;
    if(crop_){crop_->x-=layer->rasterOrigin.x;crop_->y-=layer->rasterOrigin.y;}
    ignoreCrop_=options.ignoreCrop;
    coverageValues_=options.coverageValues;
    allowGrowth_=options.allowGrowth && !coverageValues_;
    originalSurface_=liveSurface_;
    originalOrigin_=layer->rasterOrigin;
    externalTransform_=layer->localToDocument;
    originalFrame_=layer->rasterEffectFrame;
    growingFrame_=layerEffectReferenceFrame(*layer);
    originalContentState_=document.contentState();
    active_ = true;
}

RasterEditTransaction::~RasterEditTransaction()
{
    cancel();
}

bool RasterEditTransaction::targetAvailable() const noexcept
{
    if (!active_ || !document_ || !liveSurface_ || !resolveCurrentTarget()) {
        return false;
    }
    const auto* layer=document_->layer(layerId_);
    return layer->crop==layerCrop_ && layer->localToDocument==externalTransform_
        && layer->rasterOrigin==(growingSurface_?growingSurface_->origin():originalOrigin_)
        && (!selectionMask_ || selectionMask_->revision() == selectionRevision_);
}

std::size_t RasterEditTransaction::capturedPixelBytes() const noexcept
{
    std::size_t result = 0;
    for (const auto& [key, tile] : journal_) {
        (void)key;
        result += tile.before.size();
    }
    return result;
}

bool RasterEditTransaction::capture(RectI localRegion)
{
    if (!targetAvailable()) {
        return false;
    }
    if (selectionIsEmpty()) return true;
    if (growingSurface_) return true; // Original is now an immutable retained surface.
    const auto clipped = localRegion.clippedTo(surfaceBounds());
    if (clipped.empty()) {
        return true;
    }

    const auto firstTileX = clipped.x / static_cast<std::int32_t>(tileSize_);
    const auto firstTileY = clipped.y / static_cast<std::int32_t>(tileSize_);
    const auto lastTileX = (clipped.right() - 1) / static_cast<std::int32_t>(tileSize_);
    const auto lastTileY = (clipped.bottom() - 1) / static_cast<std::int32_t>(tileSize_);
    for (auto tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (auto tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const TileKey key {tileX, tileY};
            if (journal_.contains(key)) {
                continue;
            }
            const RectI gridTile {
                tileX * static_cast<std::int32_t>(tileSize_),
                tileY * static_cast<std::int32_t>(tileSize_),
                static_cast<std::int32_t>(tileSize_),
                static_cast<std::int32_t>(tileSize_),
            };
            JournalTile tile {
                .region = gridTile.clippedTo(surfaceBounds()),
                .before = {},
            };
            tile.before.resize(checkedPixelBytes(tile.region));
            liveSurface_->copyRgba8(tile.region, tile.before,
                static_cast<std::size_t>(tile.region.width) * kBytesPerPixel);
            journal_.emplace(key, std::move(tile));
        }
    }
    return true;
}

void RasterEditTransaction::copyOriginalRgba8(RectI localRegion,
    std::span<std::byte> destination, std::size_t destinationStride) const
{
    if (!active_ || localRegion.empty()
        || (!allowGrowth_ && localRegion.clippedTo(surfaceBounds()) != localRegion)) {
        throw std::invalid_argument("Original raster copy must be inside the transaction surface");
    }
    const auto rowBytes = static_cast<std::size_t>(localRegion.width) * kBytesPerPixel;
    const auto required = destinationStride * static_cast<std::size_t>(localRegion.height - 1)
        + rowBytes;
    if (destinationStride < rowBytes || destination.size() < required) {
        throw std::invalid_argument("Original raster destination is too small");
    }

    if (allowGrowth_) {
        for(int y=0;y<localRegion.height;++y)
            std::memset(destination.data()+std::size_t(y)*destinationStride,0,rowBytes);
        const auto r=localRegion.clippedTo(surfaceBounds());
        if(!r.empty()) originalSurface_->copyRgba8(r,destination.subspan(
            std::size_t(r.y-localRegion.y)*destinationStride+std::size_t(r.x-localRegion.x)*4),destinationStride);
        if(growingSurface_)return;
    }

    const auto tileSize = static_cast<std::int32_t>(tileSize_);
    for (auto ty = int(std::floor(double(localRegion.y) / tileSize)); ty <= int(std::floor(double(localRegion.bottom()-1) / tileSize)); ++ty)
    for (auto tx = int(std::floor(double(localRegion.x) / tileSize)); tx <= int(std::floor(double(localRegion.right()-1) / tileSize)); ++tx) {
        const auto found = journal_.find({tx, ty});
        if (found == journal_.end()) continue;
        const auto& tile = found->second;
        const auto intersection = tile.region.clippedTo(localRegion);
        if (intersection.empty()) {
            continue;
        }
        const auto tileStride = static_cast<std::size_t>(tile.region.width) * kBytesPerPixel;
        const auto copyBytes = static_cast<std::size_t>(intersection.width) * kBytesPerPixel;
        for (std::int32_t row = 0; row < intersection.height; ++row) {
            const auto sourceOffset = static_cast<std::size_t>(intersection.y - tile.region.y + row)
                    * tileStride
                + static_cast<std::size_t>(intersection.x - tile.region.x) * kBytesPerPixel;
            const auto destinationOffset = static_cast<std::size_t>(
                    intersection.y - localRegion.y + row)
                    * destinationStride
                + static_cast<std::size_t>(intersection.x - localRegion.x) * kBytesPerPixel;
            std::memcpy(destination.data() + destinationOffset,
                tile.before.data() + sourceOffset, copyBytes);
        }
    }
}

void RasterEditTransaction::copyCurrentRgba8(RectI r,std::span<std::byte> bytes,std::size_t stride) const
{
    if(r.empty()||stride<std::size_t(r.width)*4||bytes.size()<(std::size_t(r.height)-1)*stride+std::size_t(r.width)*4)
        throw std::invalid_argument("Invalid raster read buffer");
    for(int y=0;y<r.height;++y)std::memset(bytes.data()+std::size_t(y)*stride,0,std::size_t(r.width)*4);
    auto bounds=surfaceBounds();
    if(growingSurface_) {bounds=growingSurface_->state().bounds;bounds.x-=int(originalOrigin_.x);bounds.y-=int(originalOrigin_.y);}
    auto part=r.clippedTo(bounds);
    if(part.empty())return;
    auto destination=bytes.subspan(std::size_t(part.y-r.y)*stride+std::size_t(part.x-r.x)*4);
    part.x-=bounds.x;part.y-=bounds.y;
    liveSurface_->copyRgba8(part,destination,stride);
}

DirtySet RasterEditTransaction::writeRgba8(RectI localRegion,
    std::span<const std::byte> source, std::size_t sourceStride)
{
    const RasterPatch patch {localRegion, source, sourceStride};
    return writeRgba8Batch(std::span<const RasterPatch>(&patch, 1));
}

DirtySet RasterEditTransaction::writeRgba8Batch(std::span<const RasterPatch> patches)
{
    return writeRgba8BatchImpl(patches, false);
}

DirtySet RasterEditTransaction::writeSelectionResolvedRgba8Batch(std::span<const RasterPatch> patches)
{
    return writeRgba8BatchImpl(patches, true);
}

DirtySet RasterEditTransaction::writeRgba8BatchImpl(std::span<const RasterPatch> patches, bool selectionResolved)
{
    if (!targetAvailable() || selectionIsEmpty()) {
        return {
            .revision = liveSurface_ ? liveSurface_->revision() : 0,
            .fullRefresh = false,
            .regions = {},
        };
    }
    for (const auto& patch : patches) {
        if (!capture(patch.region)) {
            return {
                .revision = liveSurface_->revision(),
                .fullRefresh = false,
                .regions = {},
            };
        }
    }
    const bool mayExtend=allowGrowth_ && std::ranges::any_of(patches,[&](const auto& p) {
        return p.region.clippedTo(surfaceBounds())!=p.region;
    });
    if (selectionMask_ || (crop_ && !ignoreCrop_) || selectionResolved || mayExtend) {
        return writeMasked(patches, selectionResolved);
    }
    return writeAccepted(patches);
}

RasterEditCommitResult RasterEditTransaction::commit(History& history)
{
    if (!active_) {
        return RasterEditCommitResult::TargetUnavailable;
    }
    if (!targetAvailable()) {
        cancel();
        return RasterEditCommitResult::TargetUnavailable;
    }

    if(growingSurface_) {
        std::unique_ptr<Command> command=std::make_unique<GrowingRasterCommand>(layerId_,originalSurface_,
            growingSurface_,originalOrigin_,originalFrame_,growingFrame_,externalTransform_,
            std::string(historyLabel_.data(),historyLabelLength_));
        if(!history.adoptApplied(*document_,command)) {cancel();return RasterEditCommitResult::HistoryRejected;}
        active_=false;journal_.clear();liveSurface_.reset();
        return RasterEditCommitResult::Committed;
    }
    auto tiles = finalizeTiles();
    if (tiles.empty()) {
        active_ = false;
        journal_.clear();
        liveSurface_.reset();
        return RasterEditCommitResult::NoChanges;
    }

    auto rasterCommand = std::unique_ptr<RasterEditCommand>(new RasterEditCommand(
        layerId_, surfaceId_, surfaceExtent_,
        std::string_view(historyLabel_.data(), historyLabelLength_), std::move(tiles)));
    // All metadata/object allocation has succeeded. Only now transfer original
    // bytes; allocation failure above can still cancel from the intact journal.
    for (auto& tile : rasterCommand->tiles_)
        tile.pixels = std::move(journal_.at({tile.tileRegion.x / int(tileSize_),
            tile.tileRegion.y / int(tileSize_)}).before);
    auto* rasterCommandPointer = rasterCommand.get();
    std::unique_ptr<Command> command = std::move(rasterCommand);
    bool adopted = false;
    try { adopted = history.adoptApplied(*document_, command); }
    catch (...) {
        // adoptApplied reserves before taking ownership. Recover the journal
        // without allocating; rollback stays possible even under memory pressure.
        for (auto& tile : rasterCommandPointer->tiles_)
            journal_.at({tile.tileRegion.x / int(tileSize_), tile.tileRegion.y / int(tileSize_)}).before
                = std::move(tile.pixels);
        throw;
    }
    if (!adopted) {
        // The command still owns the original tile bytes. Restore through the
        // retained surface pointer even if the document target disappeared
        // during defensive validation, then discard the command.
        (void)rasterCommandPointer->swapWithSurface(*liveSurface_);
        active_ = false;
        journal_.clear();
        liveSurface_.reset();
        return RasterEditCommitResult::HistoryRejected;
    }

    active_ = false;
    journal_.clear();
    liveSurface_.reset();
    return RasterEditCommitResult::Committed;
}

void RasterEditTransaction::cancel() noexcept
{
    if (!active_) {
        return;
    }
    try {
        if(growingSurface_) {
            if(resolveCurrentTarget()) {
                document_->setLayerRasterStorage(layerId_,originalSurface_,originalOrigin_,originalFrame_);
                document_->restoreContentState(originalContentState_);
            }
        } else restoreOriginalPixels();
    } catch (...) {
        // Destructors and focus-loss cancellation must never throw. The live
        // surface is retained until this point, so only an invariant violation
        // in a concrete surface can reach this guard.
    }
    active_ = false;
    journal_.clear();
    liveSurface_.reset();
}

RectI RasterEditTransaction::surfaceBounds() const noexcept
{
    return {0, 0, static_cast<std::int32_t>(surfaceExtent_.width),
        static_cast<std::int32_t>(surfaceExtent_.height)};
}

RasterSurface* RasterEditTransaction::resolveCurrentTarget() const noexcept
{
    return document_ ? rasterSurfaceForLayer(
        *document_, layerId_, liveSurface_->id(), liveSurface_->extent()) : nullptr;
}

std::vector<RasterEditCommand::TileSnapshot> RasterEditTransaction::finalizeTiles()
{
    std::vector<RasterEditCommand::TileSnapshot> result;
    result.reserve(journal_.size());
    for (auto& [key, tile] : journal_) {
        (void)key;
        std::vector<std::byte> current(tile.before.size());
        liveSurface_->copyRgba8(tile.region, current,
            static_cast<std::size_t>(tile.region.width) * kBytesPerPixel);
        auto regions = changedRegions(tile, current);
        if (regions.empty()) {
            continue;
        }
        result.push_back({tile.region, {}, std::move(regions)});
    }
    return result;
}

std::vector<RectI> RasterEditTransaction::changedRegions(
    const JournalTile& tile, std::span<const std::byte> current) const
{
    std::vector<RectI> result;
    const auto stride = static_cast<std::size_t>(tile.region.width) * kBytesPerPixel;
    // The previous row's run -> output rectangle. Identical runs extend down;
    // otherwise a new one-row rectangle starts. This produces a disjoint exact
    // cover of pixels whose RGBA value actually changed.
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> previousRuns;
    for (std::int32_t row = 0; row < tile.region.height; ++row) {
        std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> currentRuns;
        std::int32_t column = 0;
        while (column < tile.region.width) {
            const auto offset = static_cast<std::size_t>(row) * stride
                + static_cast<std::size_t>(column) * kBytesPerPixel;
            if (samePixel(tile.before, current, offset, offset)) {
                ++column;
                continue;
            }
            const auto start = column;
            do {
                ++column;
                if (column >= tile.region.width) {
                    break;
                }
                const auto nextOffset = static_cast<std::size_t>(row) * stride
                    + static_cast<std::size_t>(column) * kBytesPerPixel;
                if (samePixel(tile.before, current, nextOffset, nextOffset)) {
                    break;
                }
            } while (true);
            const auto width = column - start;
            const auto key = std::pair {start, width};
            const auto previous = previousRuns.find(key);
            if (previous != previousRuns.end()) {
                ++result[previous->second].height;
                currentRuns.emplace(key, previous->second);
            } else {
                result.push_back({tile.region.x + start, tile.region.y + row, width, 1});
                currentRuns.emplace(key, result.size() - 1);
            }
        }
        previousRuns = std::move(currentRuns);
    }
    return result;
}

DirtySet RasterEditTransaction::writeMasked(std::span<const RasterPatch> patches, bool selectionResolved)
{
    struct OwnedPatch {
        RectI region;
        std::vector<std::byte> bytes;
    };
    std::vector<OwnedPatch> owned;
    owned.reserve(patches.size());
    for (const auto& patch : patches) {
        const auto clipped = allowGrowth_ ? patch.region : patch.region.clippedTo(surfaceBounds());
        if (clipped.empty()) {
            continue;
        }
        const auto sourceRowBytes = static_cast<std::size_t>(patch.region.width)
            * kBytesPerPixel;
        if (patch.region.empty() || patch.stride < sourceRowBytes) {
            throw std::invalid_argument("Masked raster patch has an invalid stride");
        }
        OwnedPatch output {.region = clipped, .bytes = {}};
        output.bytes.resize(checkedPixelBytes(clipped));
        const auto outputStride = static_cast<std::size_t>(clipped.width) * kBytesPerPixel;
        // Always retain the original for rejected pixels. Ordinary candidates
        // need once-per-stroke selection interpolation; final selection-resolved
        // candidates already include it before their sole RGBA8 encoding.
        copyOriginalRgba8(clipped, output.bytes, outputStride);
        for (std::int32_t row = 0; row < clipped.height; ++row) {
            for (std::int32_t column = 0; column < clipped.width; ++column) {
                const auto localX = clipped.x + column;
                const auto localY = clipped.y + row;
                const auto documentPoint = localToDocument_.map(
                    {static_cast<double>(localX) + 0.5,
                        static_cast<double>(localY) + 0.5});
                const bool representable=std::isfinite(documentPoint.x) && std::isfinite(documentPoint.y)
                    && documentPoint.x>=std::numeric_limits<std::int32_t>::min()
                    && documentPoint.y>=std::numeric_limits<std::int32_t>::min()
                    && documentPoint.x<double(std::numeric_limits<std::int32_t>::max())+1.0
                    && documentPoint.y<double(std::numeric_limits<std::int32_t>::max())+1.0;
                const bool outsideDocument = !std::isfinite(documentPoint.x) || !std::isfinite(documentPoint.y)
                    || documentPoint.x < 0 || documentPoint.y < 0
                    || documentPoint.x >= double(canvasExtent_.width) || documentPoint.y >= double(canvasExtent_.height);
                const auto coverage = !cropAllows(localX,localY) || (selectionResolved && outsideDocument)
                    ? std::uint8_t{0} : !selectionMask_ ? std::uint8_t{255} : representable ? selectionMask_->coverageAtDocumentPixel(
                    static_cast<std::int32_t>(std::floor(documentPoint.x)),
                    static_cast<std::int32_t>(std::floor(documentPoint.y))) : std::uint8_t {0};
                const auto outputOffset = static_cast<std::size_t>(row) * outputStride
                    + static_cast<std::size_t>(column) * kBytesPerPixel;
                const auto sourceOffset = static_cast<std::size_t>(
                        clipped.y - patch.region.y + row)
                        * patch.stride
                    + static_cast<std::size_t>(clipped.x - patch.region.x + column)
                        * kBytesPerPixel;
                if (patch.rgbaBytes.size() < sourceOffset + kBytesPerPixel) {
                    throw std::invalid_argument("Masked raster patch source is too small");
                }
                if (!coverage) continue;
                if (coverage == 255 || selectionResolved) {
                    std::copy_n(patch.rgbaBytes.begin() + std::ptrdiff_t(sourceOffset), kBytesPerPixel,
                        output.bytes.begin() + std::ptrdiff_t(outputOffset));
                    continue;
                }
                const auto weight = double(coverage) / 255.0;
                if(coverageValues_) {
                    const double before=std::to_integer<std::uint8_t>(output.bytes[outputOffset]);
                    const double after=std::to_integer<std::uint8_t>(patch.rgbaBytes[sourceOffset]);
                    const auto value=std::byte(std::uint8_t(std::lround(before+(after-before)*weight)));
                    output.bytes[outputOffset]=output.bytes[outputOffset+1]=output.bytes[outputOffset+2]=value;
                    output.bytes[outputOffset+3]=std::byte{255};
                    continue;
                }
                const auto beforeAlpha = double(std::to_integer<std::uint8_t>(output.bytes[outputOffset+3])) / 255.0;
                const auto afterAlpha = double(std::to_integer<std::uint8_t>(patch.rgbaBytes[sourceOffset+3])) / 255.0;
                const auto alpha = beforeAlpha * (1.0-weight) + afterAlpha * weight;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const auto before = srgbToLinear(std::to_integer<std::uint8_t>(output.bytes[outputOffset+channel]));
                    const auto after = srgbToLinear(std::to_integer<std::uint8_t>(patch.rgbaBytes[sourceOffset+channel]));
                    if (alpha > 0) output.bytes[outputOffset+channel] = std::byte(linearToSrgb(
                        (before * beforeAlpha * (1.0-weight) + after * afterAlpha * weight) / alpha));
                }
                output.bytes[outputOffset+3] = std::byte(alphaToByte(alpha));
            }
        }
        owned.push_back(std::move(output));
    }

    std::vector<RasterPatch> views;
    views.reserve(owned.size());
    for (std::size_t patchIndex = 0; patchIndex < owned.size(); ++patchIndex) {
        const auto& patch = owned[patchIndex];
        const auto stride = std::size_t(patch.region.width) * kBytesPerPixel;
        std::vector<std::byte> current(patch.bytes.size());
        copyCurrentRgba8(patch.region, current, stride);
        // Preserve ordered/last-write-wins batch semantics even when a future
        // raster producer submits overlapping patches. Compare against prior
        // candidates in this batch, not only the pre-batch live surface.
        for (std::size_t previous = 0; previous < patchIndex; ++previous) {
            const auto& earlier = owned[previous];
            const auto overlap = patch.region.clippedTo(earlier.region);
            const auto earlierStride = std::size_t(earlier.region.width) * kBytesPerPixel;
            for (int y = overlap.y; !overlap.empty() && y < overlap.bottom(); ++y) {
                const auto to = std::size_t(y-patch.region.y) * stride + std::size_t(overlap.x-patch.region.x) * kBytesPerPixel;
                const auto from = std::size_t(y-earlier.region.y) * earlierStride + std::size_t(overlap.x-earlier.region.x) * kBytesPerPixel;
                std::memcpy(current.data()+to, earlier.bytes.data()+from, std::size_t(overlap.width)*kBytesPerPixel);
            }
        }
        std::map<std::pair<int,int>, std::size_t> previousRuns;
        for (int y = 0; y < patch.region.height; ++y) {
            std::map<std::pair<int,int>, std::size_t> runs;
            for (int x = 0; x < patch.region.width;) {
                const auto offset = std::size_t(y) * stride + std::size_t(x) * kBytesPerPixel;
                if (samePixel(current, patch.bytes, offset, offset)) { ++x; continue; }
                const auto start = x++;
                while (x < patch.region.width) {
                    const auto next = std::size_t(y) * stride + std::size_t(x) * kBytesPerPixel;
                    if (samePixel(current, patch.bytes, next, next)) break;
                    ++x;
                }
                const auto key = std::pair(start, x-start);
                const auto previous = previousRuns.find(key);
                if (previous != previousRuns.end()) {
                    ++views[previous->second].region.height;
                    runs.emplace(key, previous->second);
                } else {
                    runs.emplace(key, views.size());
                    views.push_back({{patch.region.x+start, patch.region.y+y, x-start, 1},
                        std::span<const std::byte>(patch.bytes).subspan(offset), stride});
                }
            }
            previousRuns = std::move(runs);
        }
    }
    return writeAccepted(views);
}

void RasterEditTransaction::beginGrowth()
{
    // Preserve preceding in-bounds dabs without copying the whole layer. Roll
    // the old surface back before using it as an immutable COW base.
    std::vector<std::vector<std::byte>> after;
    std::vector<RasterPatch> beforePatches,afterPatches;
    after.reserve(journal_.size());
    for(const auto& [key,tile]:journal_) {
        (void)key;
        after.emplace_back(tile.before.size());
        const auto stride=std::size_t(tile.region.width)*4;
        liveSurface_->copyRgba8(tile.region,after.back(),stride);
        beforePatches.push_back({tile.region,tile.before,stride});
        afterPatches.push_back({tile.region,after.back(),stride});
    }
    (void)liveSurface_->replaceRgba8Batch(beforePatches);
    auto candidate=std::make_shared<RegionalRasterSurface>(originalSurface_,originalOrigin_);
    (void)candidate->replaceRgba8Batch(afterPatches);
    growingSurface_=std::move(candidate);
    liveSurface_=growingSurface_;
    document_->setLayerRasterStorage(layerId_,liveSurface_,originalOrigin_,growingFrame_);
}

DirtySet RasterEditTransaction::writeAccepted(std::span<const RasterPatch> patches)
{
    if(patches.empty())return {liveSurface_->revision(),false,{}};
    RectI required=surfaceBounds();
    if(growingSurface_) {required=growingSurface_->state().bounds;required.x-=int(originalOrigin_.x);required.y-=int(originalOrigin_.y);}
    for(const auto& patch:patches)if(!patch.region.empty())required=required.united(patch.region);
    const bool grow=allowGrowth_ && (growingSurface_ || required!=surfaceBounds());
    if(!grow)return liveSurface_->replaceRgba8Batch(patches);
    // Amortize texture/storage resizing at tile boundaries, not every dab.
    const auto oldBounds=growingSurface_?growingSurface_->state().bounds:
        RectI{int(originalOrigin_.x),int(originalOrigin_.y),int(surfaceExtent_.width),int(surfaceExtent_.height)};
    required.x+=int(originalOrigin_.x);required.y+=int(originalOrigin_.y);
    const int left=required.x<oldBounds.x?int(std::floor(double(required.x)/64))*64:oldBounds.x;
    const int top=required.y<oldBounds.y?int(std::floor(double(required.y)/64))*64:oldBounds.y;
    const int right=required.right()>oldBounds.right()?int(std::ceil(double(required.right())/64))*64:oldBounds.right();
    const int bottom=required.bottom()>oldBounds.bottom()?int(std::ceil(double(required.bottom())/64))*64:oldBounds.bottom();
    const RectI expanded{left,top,right-left,bottom-top};
    RegionalRasterSurface::validateBounds(expanded);
    if(!externalTransform_.validOver({double(left),double(top),double(expanded.width),double(expanded.height)}))
        throw std::length_error("Painting would cross the layer's projective horizon");
    if(!growingSurface_)beginGrowth();
    const auto previous=growingSurface_->revision();
    if(expanded!=growingSurface_->state().bounds) {
        auto next=growingSurface_->state();next.bounds=expanded;growingSurface_->restore(std::move(next));
        document_->setLayerRasterStorage(layerId_,liveSurface_,growingSurface_->origin(),growingFrame_);
    }
    std::vector<RasterPatch> mapped; mapped.reserve(patches.size());
    for(auto patch:patches) {patch.region.x+=int(originalOrigin_.x)-left;patch.region.y+=int(originalOrigin_.y)-top;mapped.push_back(patch);}
    (void)growingSurface_->replaceRgba8Batch(mapped);
    return growingSurface_->dirtySince(previous);
}

void RasterEditTransaction::restoreOriginalPixels()
{
    if (!liveSurface_ || journal_.empty()) {
        return;
    }
    std::vector<RasterEditCommand::TileSnapshot> changed;
    changed.reserve(journal_.size());
    for (auto& [key, tile] : journal_) {
        (void)key;
        std::vector<std::byte> current(tile.before.size());
        liveSurface_->copyRgba8(tile.region, current,
            static_cast<std::size_t>(tile.region.width) * kBytesPerPixel);
        auto regions = changedRegions(tile, current);
        if (!regions.empty()) {
            changed.push_back({tile.region, std::move(tile.before), std::move(regions)});
        }
    }

    std::vector<MutableRasterPatch> patches;
    for (auto& tile : changed) {
        const auto stride = static_cast<std::size_t>(tile.tileRegion.width) * kBytesPerPixel;
        for (const auto region : tile.changedRegions) {
            const auto offset = static_cast<std::size_t>(region.y - tile.tileRegion.y) * stride
                + static_cast<std::size_t>(region.x - tile.tileRegion.x) * kBytesPerPixel;
            patches.push_back({region,
                std::span<std::byte>(tile.pixels).subspan(offset), stride});
        }
    }
    if (!patches.empty()) {
        (void)liveSurface_->swapRgba8Batch(patches);
    }
}

} // namespace imageeditor::core
