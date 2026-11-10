#include "imageeditor/core/ScalarPlane.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace imageeditor::core {

ScalarPlane::ScalarPlane(Extent2u extent)
    : extent_(extent), columns_((extent.width + tileSize - 1) / tileSize)
{
    // Matches the existing coverage storage limit; public import/document
    // limits remain separately enforced and are not widened by scalar storage.
    if (extent.empty() || extent.width > 32768 || extent.height > 32768)
        throw std::invalid_argument("Scalar extent must be between 1 and 32768 pixels per axis");
    tiles_.resize(std::size_t(columns_) * ((extent.height + tileSize - 1) / tileSize));
}

RectI ScalarPlane::tileRect(std::size_t i) const noexcept
{
    return RectI {int(i % columns_ * tileSize), int(i / columns_ * tileSize), int(tileSize), int(tileSize)}
        .clippedTo({0, 0, int(extent_.width), int(extent_.height)});
}

void ScalarPlane::validate(RectI r, std::size_t count, std::size_t stride) const
{
    if (r.empty() || r.x < 0 || r.y < 0
        || std::int64_t(r.x) + r.width > extent_.width
        || std::int64_t(r.y) + r.height > extent_.height
        || stride < std::size_t(r.width) || count < std::size_t(r.width)
        || (r.height > 1 && stride > (count - std::size_t(r.width)) / std::size_t(r.height - 1)))
        throw std::invalid_argument("Invalid scalar pixel region or buffer");
}

ScalarPlaneState ScalarPlane::filled(Extent2u extent, std::uint8_t value)
{
    auto result = std::shared_ptr<ScalarPlane>(new ScalarPlane(extent));
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        auto& tile = result->tiles_[i];
        tile.uniform = value;
        const auto r = result->tileRect(i);
        if (value) tile.bounds = {0, 0, r.width, r.height};
    }
    return result;
}

ScalarPlaneState ScalarPlane::fromR8(Extent2u extent, std::span<const std::byte> bytes, std::size_t stride)
{
    auto result = std::shared_ptr<ScalarPlane>(new ScalarPlane(extent));
    result->validate({0, 0, int(extent.width), int(extent.height)}, bytes.size(), stride);
    for (std::size_t i = 0; i < result->tiles_.size(); ++i) {
        const auto r = result->tileRect(i);
        auto pixels = std::make_shared<detail::ScalarTilePixels>();
        for (int y = 0; y < r.height; ++y)
            std::memcpy(pixels->data() + std::size_t(y) * tileSize,
                bytes.data() + std::size_t(r.y + y) * stride + std::size_t(r.x), std::size_t(r.width));
        result->tiles_[i] = detail::compressScalarTile(std::move(pixels),
            {std::uint32_t(r.width), std::uint32_t(r.height)});
    }
    return result;
}

std::uint8_t ScalarPlane::at(std::uint32_t x, std::uint32_t y) const
{
    if (x >= extent_.width || y >= extent_.height)
        throw std::out_of_range("Scalar sample is outside the pixel grid");
    return tiles_[std::size_t(y / tileSize) * columns_ + x / tileSize].at(x % tileSize, y % tileSize);
}

void ScalarPlane::copyR8(RectI r, std::span<std::byte> bytes, std::size_t stride) const
{
    validate(r, bytes.size(), stride);
    for (int ty = r.y / int(tileSize); ty <= (r.bottom() - 1) / int(tileSize); ++ty)
        for (int tx = r.x / int(tileSize); tx <= (r.right() - 1) / int(tileSize); ++tx) {
            const auto index = std::size_t(ty) * columns_ + std::size_t(tx);
            const auto tile = tileRect(index), overlap = tile.clippedTo(r);
            const auto& source = tiles_[index];
            for (int y = overlap.y; y < overlap.bottom(); ++y) {
                auto* out = bytes.data() + std::size_t(y - r.y) * stride + std::size_t(overlap.x - r.x);
                if (source.pixels)
                    std::memcpy(out, source.pixels->data() + std::size_t(y - tile.y) * tileSize
                        + std::size_t(overlap.x - tile.x), std::size_t(overlap.width));
                else std::fill_n(out, overlap.width, std::byte(source.uniform));
            }
        }
}

ScalarPlaneState ScalarPlane::replacedR8(std::span<const RasterPatch> patches) const
{
    for (const auto& p : patches) validate(p.region, p.rgbaBytes.size(), p.stride);
    std::map<std::size_t, std::shared_ptr<detail::ScalarTilePixels>> changed;
    for (const auto& p : patches) {
        const auto r = p.region;
        for (int ty = r.y / int(tileSize); ty <= (r.bottom() - 1) / int(tileSize); ++ty)
            for (int tx = r.x / int(tileSize); tx <= (r.right() - 1) / int(tileSize); ++tx) {
                const auto index = std::size_t(ty) * columns_ + std::size_t(tx);
                const auto tile = tileRect(index), overlap = tile.clippedTo(r);
                auto found = changed.find(index);
                for (int y = overlap.y; y < overlap.bottom(); ++y) {
                    const auto* input = p.rgbaBytes.data() + std::size_t(y - r.y) * p.stride
                        + std::size_t(overlap.x - r.x);
                    const auto offset = std::size_t(y - tile.y) * tileSize + std::size_t(overlap.x - tile.x);
                    bool differs = false;
                    if (found != changed.end())
                        differs = std::memcmp(found->second->data() + offset, input, std::size_t(overlap.width)) != 0;
                    else if (tiles_[index].pixels)
                        differs = std::memcmp(tiles_[index].pixels->data() + offset, input, std::size_t(overlap.width)) != 0;
                    else differs = std::any_of(input, input + overlap.width,
                        [&](auto v) { return v != std::byte(tiles_[index].uniform); });
                    if (!differs) continue;
                    if (found == changed.end()) {
                        auto pixels = std::make_shared<detail::ScalarTilePixels>();
                        if (tiles_[index].pixels) *pixels = *tiles_[index].pixels;
                        else pixels->fill(tiles_[index].uniform);
                        found = changed.emplace(index, std::move(pixels)).first;
                    }
                    std::memcpy(found->second->data() + offset, input, std::size_t(overlap.width));
                }
            }
    }
    if (changed.empty()) return {};
    auto result = std::shared_ptr<ScalarPlane>(new ScalarPlane(extent_));
    result->tiles_ = tiles_;
    bool differs = false;
    for (auto& [index, pixels] : changed) {
        const auto r = tileRect(index);
        auto tile = detail::compressScalarTile(std::move(pixels),
            {std::uint32_t(r.width), std::uint32_t(r.height)});
        const auto& old = tiles_[index];
        if ((!old.pixels && !tile.pixels && old.uniform == tile.uniform)
            || (old.pixels && tile.pixels && *old.pixels == *tile.pixels)) continue;
        result->tiles_[index] = std::move(tile);
        differs = true;
    }
    return differs ? result : ScalarPlaneState {};
}

std::optional<std::uint8_t> ScalarPlane::constantValue(RectI r) const
{
    // This deliberately does not scan non-uniform tiles.
    validate(r, std::size_t(r.width > 0 ? r.width : 0) * std::size_t(r.height > 0 ? r.height : 0),
        std::size_t(r.width > 0 ? r.width : 0));
    std::optional<std::uint8_t> result;
    for (int ty = r.y / int(tileSize); ty <= (r.bottom() - 1) / int(tileSize); ++ty)
        for (int tx = r.x / int(tileSize); tx <= (r.right() - 1) / int(tileSize); ++tx) {
            const auto& tile = tiles_[std::size_t(ty) * columns_ + std::size_t(tx)];
            if (tile.pixels || (result && *result != tile.uniform)) return {};
            result = tile.uniform;
        }
    return result;
}

std::size_t ScalarPlane::allocatedPixelBytes() const noexcept
{
    return std::size_t(std::count_if(tiles_.begin(), tiles_.end(), [](const auto& t) { return bool(t.pixels); }))
        * sizeof(detail::ScalarTilePixels);
}

std::size_t ScalarPlane::memoryCost() const noexcept
{ return sizeof(*this) + tiles_.capacity() * sizeof(detail::ScalarTile) + allocatedPixelBytes(); }

std::size_t ScalarPlane::memoryCost(std::unordered_set<const void*>& allocations) const
{
    if (!allocations.insert(this).second) return 0;
    std::size_t result = sizeof(*this) + tiles_.capacity() * sizeof(detail::ScalarTile);
    for (const auto& tile : tiles_)
        if (tile.pixels && allocations.insert(tile.pixels.get()).second)
            result += sizeof(detail::ScalarTilePixels);
    return result;
}

std::size_t ScalarPlane::sharedPixelBytes(const ScalarPlane& other) const noexcept
{
    if (extent_ != other.extent_) return 0;
    std::size_t result = 0;
    for (std::size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].pixels && tiles_[i].pixels == other.tiles_[i].pixels)
            result += sizeof(detail::ScalarTilePixels);
    return result;
}

} // namespace imageeditor::core
