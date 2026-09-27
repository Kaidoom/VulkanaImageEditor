#include "imageeditor/core/LocalBlurStroke.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::core {
namespace {
constexpr int tileSide = 128;
struct Cancelled {};
BrushSettings influenceSettings(BrushSettings brush, const BlurSettings& settings)
{
    brush.opacity = std::clamp(settings.strength, 0.0, 1.0);
    return brush;
}
int tileCoordinate(int value) { return value / tileSide - (value % tileSide < 0 ? 1 : 0); }
}

LocalBlurStroke::LocalBlurStroke(Document& document, LayerId target, BrushSettings brush,
    BlurSettings settings, const IBrushAssetResolver* assets)
    : document_(document), target_(target), settings_(settings)
    , brush_(document, target, influenceSettings(brush, settings), BrushCompositeMode::FilterColor,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, assets, {},
        [this](Vec2d p) { return filteredSample(p); }, "Local Blur stroke")
{
    const auto* layer = document.layer(target);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    if (raster && raster->surface) {
        surface_ = raster->surface->id();
        localToDocument_ = intrinsicTransform(*layer);
    }
    checkpoint();
}

void LocalBlurStroke::checkpoint() noexcept
{
    revision_ = document_.revision();
    const auto* layer = document_.layer(target_);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    surfaceRevision_ = raster && raster->surface ? raster->surface->revision() : 0;
}
bool LocalBlurStroke::targetMatches() const noexcept
{
    const auto* layer = document_.layer(target_);
    const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
    return document_.revision() == revision_ && raster && raster->surface
        && raster->surface->id() == surface_ && raster->surface->revision() == surfaceRevision_
        && intrinsicTransform(*layer) == localToDocument_;
}
bool LocalBlurStroke::interrupted() const { return (cancelled_ && cancelled_()) || !targetMatches(); }

bool LocalBlurStroke::capture()
{
    if (!targetMatches() || !std::isfinite(settings_.radius) || settings_.radius < 0 || settings_.radius > 64
        || !std::isfinite(settings_.strength) || settings_.strength < 0 || settings_.strength > 1)
        throw std::invalid_argument("Invalid Local Blur target or settings.");
    if (settings_.radius == 0 || settings_.strength == 0
        || (document_.selection() && document_.selection()->bounds().empty())) return true;
    const auto* layer = document_.layer(target_);
    const auto surface = std::get<RasterLayer>(layer->payload).surface;
    const auto extent = surface->extent();
    if (extent.empty() || extent.width > std::uint32_t(std::numeric_limits<int>::max())
        || extent.height > std::uint32_t(std::numeric_limits<int>::max())
        || std::size_t(extent.width) > snapshotLimit / 4 / std::size_t(extent.height))
        throw std::length_error("Local Blur source exceeds the 256 MiB snapshot budget.");
    std::vector<std::byte> pixels(std::size_t(extent.width) * extent.height * 4);
    for (int y = 0; y < int(extent.height); y += 64) {
        if (interrupted()) throw Cancelled{};
        surface->copyRgba8({0, y, int(extent.width), std::min(64, int(extent.height) - y)},
            std::span<std::byte>(pixels).subspan(std::size_t(y) * extent.width * 4), std::size_t(extent.width) * 4);
    }
    if (interrupted()) throw Cancelled{};
    stats_.snapshotBytes = pixels.size();
    Layer frozen;
    frozen.payload = RasterLayer{std::make_shared<ContiguousRasterSurface>(extent, std::move(pixels))};
    frozen.localToDocument = layer->localToDocument;
    frozen.rasterOrigin=layer->rasterOrigin;frozen.rasterEffectFrame=layer->rasterEffectFrame;
    frozen.crop = layer->crop;
    // Deliberately do not copy adjustments, filters, blend mode, or opacity.
    source_.emplace(frozen, false);
    auto filter = defaultSpatialFilter(SpatialFilterType::Gaussian);
    filter.enabled = true;
    filter.parameters = GaussianBlurParameters{settings_.radius, settings_.radius};
    kernel_ = makeSpatialKernel(filter);
    return true;
}

PremultipliedColor LocalBlurStroke::filteredPixel(int x, int y)
{
    const auto key = std::make_pair(tileCoordinate(x), tileCoordinate(y));
    auto found = tiles_.find(key);
    if (found == tiles_.end()) {
        if (interrupted()) throw Cancelled{};
        const RectI output{key.first * tileSide, key.second * tileSide, tileSide, tileSide};
        auto filter = defaultSpatialFilter(SpatialFilterType::Gaussian);
        filter.enabled = true;
        filter.parameters = GaussianBlurParameters{settings_.radius, settings_.radius};
        const auto inputBounds = requiredSpatialInputBounds(filter, output);
        SpatialPlane input{inputBounds, 4, std::vector<float>(std::size_t(inputBounds.width) * std::size_t(inputBounds.height) * 4)};
        for (int row = 0; row < inputBounds.height; ++row) {
            if (interrupted()) throw Cancelled{};
            for (int column = 0; column < inputBounds.width; ++column) {
                const auto color = source_->sample({inputBounds.x + column + .5, inputBounds.y + row + .5});
                std::copy(color.begin(), color.end(), input.pixels.begin()
                    + std::ptrdiff_t((std::size_t(row) * std::size_t(inputBounds.width) + std::size_t(column)) * 4));
            }
        }
        SpatialFilterOptions options;
        options.maxWorkingBytes = 16U * 1024U * 1024U;
        options.cancelled = [this] { return interrupted(); };
        auto result = convolveSpatialRegion(input.view(), output, kernel_, options);
        if (result.status == SpatialFilterStatus::Cancelled) throw Cancelled{};
        if (!result) throw std::runtime_error(result.error.empty() ? "Local Blur filtering failed." : result.error);
        stats_.peakWorkingBytes = std::max(stats_.peakWorkingBytes, input.pixels.size() * sizeof(float) + result.peakWorkingBytes);
        const auto bytes = result.output.pixels.size() * sizeof(float);
        while (!tiles_.empty() && stats_.cachedBytes + bytes > cacheLimit) {
            const auto oldest = std::min_element(tiles_.begin(), tiles_.end(), [](const auto& a, const auto& b) {
                return a.second.used < b.second.used;
            });
            stats_.cachedBytes -= oldest->second.pixels.size() * sizeof(float);
            tiles_.erase(oldest);
        }
        found = tiles_.emplace(key, CachedTile{std::move(result.output.pixels), ++clock_}).first;
        stats_.cachedBytes += bytes;
        ++stats_.filteredTiles;
    } else {
        ++stats_.cacheHits;
        found->second.used = ++clock_;
    }
    const auto offset = (std::size_t(y - key.second * tileSide) * tileSide
        + std::size_t(x - key.first * tileSide)) * 4;
    const auto& pixel = found->second.pixels;
    return {pixel[offset], pixel[offset + 1], pixel[offset + 2], pixel[offset + 3]};
}

PremultipliedColor LocalBlurStroke::filteredSample(Vec2d p)
{
    if (!source_ || settings_.strength == 0 || settings_.radius == 0) return {};
    const auto x = std::floor(p.x - .5), y = std::floor(p.y - .5);
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<int>::max() - 512.0
        || std::abs(y) > std::numeric_limits<int>::max() - 512.0)
        throw std::length_error("Local Blur coordinates exceed the supported range.");
    const double fx = p.x - .5 - x, fy = p.y - .5 - y;
    PremultipliedColor result{}, baseline{};
    for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
        const auto weight = float((i ? fx : 1 - fx) * (j ? fy : 1 - fy));
        if (weight <= 0) continue;
        const auto pixel = filteredPixel(int(x) + i, int(y) + j);
        const auto original = source_->sample({x + i + .5, y + j + .5});
        for (std::size_t channel = 0; channel < 4; ++channel) {
            result[channel] += pixel[channel] * weight;
            baseline[channel] += original[channel] * weight;
        }
    }
    const auto original = source_->sample(p);
    if (result[3] <= 0 || baseline[3] <= 0 || original[3] <= 0) return {};
    // Apply only the Gaussian color residual. Returning the reconstructed grid
    // outright would resample transformed artwork even for an identity kernel.
    // Subtracting its identically reconstructed baseline retains exact source
    // detail where the filter is neutral, including fractional transforms.
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto delta = double(result[channel]) / result[3] - double(baseline[channel]) / baseline[3];
        result[channel] = float(std::clamp(double(original[channel]) / original[3] + delta, 0.0, 1.0) * original[3]);
    }
    result[3] = original[3];
    return result;
}

bool LocalBlurStroke::begin(const NormalizedPointerSample& sample, const std::function<bool()>& cancelled)
{
    cancelled_ = cancelled;
    try {
        if (!capture() || !brush_.begin(sample)) { cancel(); return false; }
        checkpoint(); cancelled_ = {};
        return true;
    } catch (const Cancelled&) { diagnostic_ = "Local Blur cancelled."; }
      catch (const std::exception& error) { diagnostic_ = error.what(); }
    cancel(); return false;
}
bool LocalBlurStroke::append(const NormalizedPointerSample& sample, const std::function<bool()>& cancelled)
{
    cancelled_ = cancelled;
    try {
        if (interrupted() || !brush_.append(sample)) { cancel(); return false; }
        checkpoint(); cancelled_ = {};
        return true;
    } catch (const Cancelled&) { diagnostic_ = "Local Blur cancelled."; }
      catch (const std::exception& error) { diagnostic_ = error.what(); }
    cancel(); return false;
}
RasterEditCommitResult LocalBlurStroke::end(const NormalizedPointerSample& sample, History& history,
    const std::function<bool()>& cancelled)
{
    cancelled_ = cancelled;
    try {
        if (interrupted() || !brush_.finishInput(sample)) { cancel(); return RasterEditCommitResult::TargetUnavailable; }
        checkpoint();
        if (interrupted()) throw Cancelled{};
        const auto result = brush_.commit(history);
        cancelled_ = {}; source_.reset(); tiles_.clear();
        return result;
    } catch (const Cancelled&) { diagnostic_ = "Local Blur cancelled."; }
      catch (const std::exception& error) { diagnostic_ = error.what(); }
    cancel(); return RasterEditCommitResult::TargetUnavailable;
}
void LocalBlurStroke::cancel() noexcept
{
    brush_.cancel(); source_.reset(); tiles_.clear(); cancelled_ = {};
}
}
