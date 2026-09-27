#include "imageeditor/core/RegionalRasterSurface.hpp"
#include <cstring>
#include <set>
#include <stdexcept>
namespace imageeditor::core
{
namespace
{
int tileIndex(int p) { return int(std::floor(double(p) / RegionalRasterSurface::tileSide)); }
RectI tileRect(RegionalRasterSurface::Key key) { return {key.first * 64, key.second * 64, 64, 64}; }
void validateBuffer(RectI r, std::size_t bytes, std::size_t stride)
{
    if (r.empty() || stride < std::size_t(r.width) * 4 ||
        bytes < (std::size_t(r.height) - 1) * stride + std::size_t(r.width) * 4)
        throw std::invalid_argument("Invalid regional raster buffer");
}
} // namespace
void RegionalRasterSurface::validateBounds(RectI r)
{
    if (r.empty() || r.width > 32768 || r.height > 32768 || std::abs(double(r.x)) > 1e9 ||
        std::abs(double(r.y)) > 1e9 || std::uint64_t(r.width) * std::uint64_t(r.height) > 64ULL * 1024 * 1024)
        throw std::length_error("Transformed pixels exceed the 64 megapixel / "
                                "32768 pixel surface limit");
}
RegionalRasterSurface::RegionalRasterSurface(std::shared_ptr<const RasterSurface> base, Vec2d origin)
    : base_(std::move(base))
{
    if (!base_ || !std::isfinite(origin.x) || !std::isfinite(origin.y) || std::floor(origin.x) != origin.x ||
        std::floor(origin.y) != origin.y || std::abs(origin.x) > 1e9 || std::abs(origin.y) > 1e9)
        throw std::invalid_argument("Invalid raster storage origin");
    baseBounds_ = {int(origin.x), int(origin.y), int(base_->extent().width), int(base_->extent().height)};
    validateBounds(baseBounds_);
    state_.bounds = baseBounds_;
    if (const auto *regional = dynamic_cast<const RegionalRasterSurface *>(base_.get());
        regional && regional->state_.bounds == baseBounds_) {
        // Flatten storage ancestry, not pixels. Repeated edits share immutable
        // tile bytes directly instead of retaining a chain of whole surfaces.
        state_ = regional->state_;
        baseBounds_ = regional->baseBounds_;
        base_ = regional->base_;
    }
}
void RegionalRasterSurface::copyLocal(RectI r, std::span<std::byte> bytes, std::size_t stride) const
{
    validateBuffer(r, bytes.size(), stride);
    for (int y = 0; y < r.height; ++y)
        std::memset(bytes.data() + std::size_t(y) * stride, 0, std::size_t(r.width) * 4);
    const auto base = r.clippedTo(baseBounds_);
    if (!base.empty())
        base_->copyRgba8({base.x - baseBounds_.x, base.y - baseBounds_.y, base.width, base.height},
                         bytes.subspan(std::size_t(base.y - r.y) * stride + std::size_t(base.x - r.x) * 4),
                         stride);
    for (int y = tileIndex(r.y); y <= tileIndex(r.bottom() - 1); ++y)
        for (int x = tileIndex(r.x); x <= tileIndex(r.right() - 1); ++x) {
            const auto found = state_.tiles.find({x, y});
            if (found == state_.tiles.end())
                continue;
            const auto tile = tileRect({x, y}), part = r.clippedTo(tile);
            for (int row = part.y; row < part.bottom(); ++row)
                std::memcpy(bytes.data() + std::size_t(row - r.y) * stride + std::size_t(part.x - r.x) * 4,
                            found->second->data() + std::size_t(row - tile.y) * 64 * 4 +
                                std::size_t(part.x - tile.x) * 4,
                            std::size_t(part.width) * 4);
        }
}
void RegionalRasterSurface::copyRgba8(RectI r, std::span<std::byte> bytes, std::size_t stride) const
{
    if (r.clippedTo({0, 0, state_.bounds.width, state_.bounds.height}) != r)
        throw std::out_of_range("Regional raster read outside storage");
    r.x += state_.bounds.x;
    r.y += state_.bounds.y;
    copyLocal(r, bytes, stride);
}
void RegionalRasterSurface::publish(State next, DirtySet dirty)
{
    if (dirty.empty())
        return;
    dirty.revision = revision_ + 1;
    dirty_.push_back(std::move(dirty));
    state_ = std::move(next);
    ++revision_;
    if (dirty_.size() > 256)
        dirty_.pop_front();
}
void RegionalRasterSurface::restore(State next)
{
    validateBounds(next.bounds);
    DirtySet dirty;
    dirty.fullRefresh = next.bounds != state_.bounds;
    if (!dirty.fullRefresh) {
        std::set<Key> changed;
        for (const auto &[key, tile] : next.tiles) {
            const auto old = state_.tiles.find(key);
            if (old == state_.tiles.end() || old->second != tile)
                changed.insert(key);
        }
        for (const auto &[key, tile] : state_.tiles) {
            (void)tile;
            if (!next.tiles.contains(key))
                changed.insert(key);
        }
        for (auto key : changed) {
            auto r = tileRect(key).clippedTo(next.bounds);
            if (r.empty())
                continue;
            r.x -= next.bounds.x;
            r.y -= next.bounds.y;
            dirty.regions.push_back(r);
        }
    }
    publish(std::move(next), std::move(dirty));
}
DirtySet RegionalRasterSurface::dirtySince(Revision revision) const
{
    if (revision == revision_)
        return {revision_, false, {}};
    DirtySet result{revision_, revision == 0 || dirty_.empty() || revision + 1 < dirty_.front().revision, {}};
    for (const auto &record : dirty_)
        if (record.revision > revision) {
            result.fullRefresh |= record.fullRefresh;
            result.regions.insert(result.regions.end(), record.regions.begin(), record.regions.end());
        }
    if (result.fullRefresh)
        result.regions.clear();
    return result;
}
DirtySet RegionalRasterSurface::replaceRgba8Batch(std::span<const RasterPatch> patches)
{
    auto next = state_;
    std::map<Key, std::shared_ptr<Tile>> edits;
    DirtySet dirty;
    for (const auto &patch : patches) {
        if (patch.region.empty())
            continue;
        validateBuffer(patch.region, patch.rgbaBytes.size(), patch.stride);
        if (patch.region.clippedTo({0, 0, state_.bounds.width, state_.bounds.height}) != patch.region)
            throw std::out_of_range("Regional write outside storage");
        auto r = patch.region;
        r.x += state_.bounds.x;
        r.y += state_.bounds.y;
        for (int ty = tileIndex(r.y); ty <= tileIndex(r.bottom() - 1); ++ty)
            for (int tx = tileIndex(r.x); tx <= tileIndex(r.right() - 1); ++tx) {
                const Key key{tx, ty};
                auto found = edits.find(key);
                if (found == edits.end()) {
                    auto tile = std::make_shared<Tile>();
                    copyLocal(tileRect(key), *tile, 64 * 4);
                    found = edits.emplace(key, std::move(tile)).first;
                }
                const auto part = r.clippedTo(tileRect(key));
                for (int y = part.y; y < part.bottom(); ++y) {
                    auto *destination =
                        found->second->data() +
                        (std::size_t(y - key.second * 64) * 64 + std::size_t(part.x - key.first * 64)) * 4;
                    const auto *source = patch.rgbaBytes.data() + std::size_t(y - r.y) * patch.stride +
                                         std::size_t(part.x - r.x) * 4;
                    std::memcpy(destination, source, std::size_t(part.width) * 4);
                }
            }
    }
    for (auto &[key, tile] : edits) {
        Tile before;
        copyLocal(tileRect(key), before, 64 * 4);
        if (before == *tile)
            continue;
        next.tiles[key] = std::move(tile);
        auto r = tileRect(key).clippedTo(state_.bounds);
        r.x -= state_.bounds.x;
        r.y -= state_.bounds.y;
        dirty.regions.push_back(r);
    }
    const auto before = revision_;
    publish(std::move(next), std::move(dirty));
    return dirtySince(before);
}
DirtySet RegionalRasterSurface::swapRgba8Batch(std::span<MutableRasterPatch> patches)
{
    std::vector<std::vector<std::byte>> before;
    before.reserve(patches.size());
    std::vector<RasterPatch> writes;
    writes.reserve(patches.size());
    for (auto &patch : patches) {
        before.emplace_back(std::size_t(patch.region.width) * std::size_t(patch.region.height) * 4);
        copyRgba8(patch.region, before.back(), std::size_t(patch.region.width) * 4);
        writes.push_back({patch.region, patch.rgbaBytes, patch.stride});
    }
    const auto dirty = replaceRgba8Batch(writes);
    for (std::size_t i = 0; i < patches.size(); ++i)
        for (int y = 0; y < patches[i].region.height; ++y)
            std::memcpy(patches[i].rgbaBytes.data() + std::size_t(y) * patches[i].stride,
                        before[i].data() + std::size_t(y) * std::size_t(patches[i].region.width) * 4,
                        std::size_t(patches[i].region.width) * 4);
    return dirty;
}
} // namespace imageeditor::core
