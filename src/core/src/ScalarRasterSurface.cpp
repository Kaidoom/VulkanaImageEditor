#include "imageeditor/core/ScalarRasterSurface.hpp"

#include <algorithm>
#include <stdexcept>

namespace imageeditor::core {
namespace {

void validateBuffer(Extent2u e, RectI r, std::size_t count, std::size_t stride, std::size_t bpp)
{
    const auto row = r.width > 0 ? std::size_t(r.width) * bpp : 0;
    if (r.empty() || r.x < 0 || r.y < 0
        || std::int64_t(r.x) + r.width > e.width || std::int64_t(r.y) + r.height > e.height
        || stride < row || count < row
        || (r.height > 1 && stride > (count - row) / std::size_t(r.height - 1)))
        throw std::invalid_argument("Invalid scalar pixel region or buffer");
}

std::vector<std::byte> scalarValues(Extent2u e, const RasterPatch& p)
{
    validateBuffer(e, p.region, p.rgbaBytes.size(), p.stride, 4);
    std::vector<std::byte> values(std::size_t(p.region.width) * std::size_t(p.region.height));
    for (int y = 0; y < p.region.height; ++y)
        for (int x = 0; x < p.region.width; ++x) {
            const auto* rgba = p.rgbaBytes.data() + std::size_t(y) * p.stride + std::size_t(x) * 4;
            if (rgba[0] != rgba[1] || rgba[0] != rgba[2] || rgba[3] != std::byte {255})
                throw std::invalid_argument("Scalar writes require opaque grayscale pixels");
            values[std::size_t(y) * std::size_t(p.region.width) + std::size_t(x)] = rgba[0];
        }
    return values;
}

} // namespace

ScalarRasterSurface::ScalarRasterSurface(Extent2u extent, std::uint8_t fill)
    : state_(ScalarPlane::filled(extent, fill)) {}

ScalarRasterSurface::ScalarRasterSurface(Extent2u extent, std::span<const std::byte> values, std::size_t stride)
    : state_(ScalarPlane::fromR8(extent, values, stride)) {}

ScalarRasterSurface::ScalarRasterSurface(ScalarPlaneState state) : state_(std::move(state))
{
    if (!state_) throw std::invalid_argument("Scalar raster requires a pixel plane");
}

std::shared_ptr<ScalarRasterSurface> ScalarRasterSurface::clone() const
{ return std::make_shared<ScalarRasterSurface>(state_); }

std::size_t ScalarRasterSurface::memoryCost() const noexcept
{
    std::size_t result = sizeof(*this) + state_->memoryCost();
    for (const auto& r : dirtyJournal_) result += sizeof(r) + r.regions.capacity() * sizeof(RectI);
    return result;
}

std::size_t ScalarRasterSurface::memoryCost(std::unordered_set<const void*>& allocations) const
{
    if (!allocations.insert(this).second) return 0;
    std::size_t result = sizeof(*this) + state_->memoryCost(allocations);
    for (const auto& r : dirtyJournal_) result += sizeof(r) + r.regions.capacity() * sizeof(RectI);
    return result;
}

DirtySet ScalarRasterSurface::dirtySince(Revision uploaded) const
{
    DirtySet result {.revision = revision_, .fullRefresh = false, .regions = {}};
    if (uploaded == revision_) return result;
    if (uploaded == 0 || uploaded > revision_ || dirtyJournal_.empty()
        || uploaded < dirtyJournal_.front().revision - 1) {
        result.fullRefresh = true;
        return result;
    }
    for (const auto& record : dirtyJournal_)
        if (record.revision > uploaded)
            result.regions.insert(result.regions.end(), record.regions.begin(), record.regions.end());
    return result;
}

void ScalarRasterSurface::copyNative(RectI r, std::span<std::byte> output, std::size_t stride) const
{ state_->copyR8(r, output, stride); }

DirtySet ScalarRasterSurface::publish(ScalarPlaneState next, std::vector<RectI> regions)
{
    if (!next) return {.revision = revision_, .fullRefresh = false, .regions = {}};
    // Prepare potentially allocating metadata before replacing the live state.
    dirtyJournal_.push_back({revision_ + 1, regions});
    if (dirtyJournal_.size() > 256) dirtyJournal_.pop_front();
    state_ = std::move(next);
    ++revision_;
    return {.revision = revision_, .fullRefresh = false, .regions = std::move(regions)};
}

DirtySet ScalarRasterSurface::replaceNativeBatch(std::span<const RasterPatch> patches)
{
    auto next = state_->replacedR8(patches);
    if (!next) return {.revision = revision_, .fullRefresh = false, .regions = {}};
    std::vector<RectI> regions;
    regions.reserve(patches.size());
    for (const auto& p : patches) regions.push_back(p.region);
    return publish(std::move(next), std::move(regions));
}

DirtySet ScalarRasterSurface::swapNativeBatch(std::span<MutableRasterPatch> patches)
{
    for (const auto& p : patches)
        validateBuffer(extent(), p.region, p.rgbaBytes.size(), p.stride, 1);
    // Retain originals until the entire swap has succeeded. A rejected patch
    // cannot partially change either the live plane or the caller's history.
    std::vector<std::vector<std::byte>> originals;
    originals.reserve(patches.size());
    std::vector<RasterPatch> replacements;
    replacements.reserve(patches.size());
    std::vector<RectI> regions;
    regions.reserve(patches.size());
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        auto& bytes = originals.emplace_back(std::size_t(p.region.width) * std::size_t(p.region.height));
        state_->copyR8(p.region, bytes, std::size_t(p.region.width));
        // Preserve ordered-swap semantics for overlapping regions, while
        // copying the plane's tile table only once for the complete batch.
        for (std::size_t j = 0; j < i; ++j) {
            const auto overlap = p.region.clippedTo(patches[j].region);
            for (int y = overlap.y; y < overlap.bottom(); ++y)
                std::copy_n(patches[j].rgbaBytes.data() + std::size_t(y - patches[j].region.y) * patches[j].stride
                    + std::size_t(overlap.x - patches[j].region.x), overlap.width,
                    bytes.data() + std::size_t(y - p.region.y) * std::size_t(p.region.width)
                    + std::size_t(overlap.x - p.region.x));
        }
        replacements.push_back({p.region, p.rgbaBytes, p.stride});
        regions.push_back(p.region);
    }
    const auto result = publish(state_->replacedR8(replacements), std::move(regions));
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        for (int y = 0; y < p.region.height; ++y)
            std::copy_n(originals[i].data() + std::size_t(y) * std::size_t(p.region.width), p.region.width,
                p.rgbaBytes.data() + std::size_t(y) * p.stride);
    }
    return result;
}

void ScalarRasterSurface::copyRgba8(RectI r, std::span<std::byte> output, std::size_t stride) const
{
    validateBuffer(extent(), r, output.size(), stride, 4);
    // One scanline is sufficient; RGBA expansion never becomes stored state.
    std::vector<std::byte> row(std::size_t(r.width));
    for (int y = 0; y < r.height; ++y) {
        state_->copyR8({r.x, r.y + y, r.width, 1}, row, row.size());
        for (int x = 0; x < r.width; ++x) {
            auto* rgba = output.data() + std::size_t(y) * stride + std::size_t(x) * 4;
            rgba[0] = rgba[1] = rgba[2] = row[std::size_t(x)];
            rgba[3] = std::byte {255};
        }
    }
}

DirtySet ScalarRasterSurface::replaceRgba8Batch(std::span<const RasterPatch> patches)
{
    std::vector<std::vector<std::byte>> values;
    std::vector<RasterPatch> native;
    values.reserve(patches.size()); native.reserve(patches.size());
    for (const auto& p : patches) {
        values.push_back(scalarValues(extent(), p));
        native.push_back({p.region, values.back(), std::size_t(p.region.width)});
    }
    return replaceNativeBatch(native);
}

DirtySet ScalarRasterSurface::swapRgba8Batch(std::span<MutableRasterPatch> patches)
{
    std::vector<std::vector<std::byte>> values;
    std::vector<MutableRasterPatch> native;
    values.reserve(patches.size()); native.reserve(patches.size());
    for (const auto& p : patches) {
        values.push_back(scalarValues(extent(), {p.region, p.rgbaBytes, p.stride}));
        native.push_back({p.region, values.back(), std::size_t(p.region.width)});
    }
    const auto result = swapNativeBatch(native);
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        for (int y = 0; y < p.region.height; ++y)
            for (int x = 0; x < p.region.width; ++x) {
                auto* rgba = p.rgbaBytes.data() + std::size_t(y) * p.stride + std::size_t(x) * 4;
                rgba[0] = rgba[1] = rgba[2] = values[i][std::size_t(y) * std::size_t(p.region.width) + std::size_t(x)];
                rgba[3] = std::byte {255};
            }
    }
    return result;
}

} // namespace imageeditor::core
