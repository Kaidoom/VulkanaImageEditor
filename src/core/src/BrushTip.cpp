#include "imageeditor/core/BrushTip.hpp"
#include "imageeditor/core/CreativeBrushes.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

double smoothstep(double edge0, double edge1, double value) noexcept
{
    if (edge1 <= edge0) {
        return value < edge0 ? 0.0 : 1.0;
    }
    const auto amount = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return amount * amount * (3.0 - 2.0 * amount);
}

double radians(double degrees) noexcept
{
    return degrees * kPi / 180.0;
}

std::uint64_t mix64(std::uint64_t value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

double hashUnit(std::uint64_t value) noexcept
{
    constexpr auto mask = (std::uint64_t {1} << 53U) - 1U;
    return static_cast<double>(mix64(value) & mask)
        / static_cast<double>(mask);
}

std::vector<std::uint8_t> makeDryInkTipPixels()
{
    constexpr std::uint32_t extent = 64;
    std::vector<std::uint8_t> pixels(extent * extent);
    for (std::uint32_t y = 0; y < extent; ++y) {
        for (std::uint32_t x = 0; x < extent; ++x) {
            const auto px = (static_cast<double>(x) + 0.5) / extent * 2.0 - 1.0;
            const auto py = (static_cast<double>(y) + 0.5) / extent * 2.0 - 1.0;
            const auto angle = std::atan2(py, px);
            const auto radius = std::hypot(px, py);
            const auto irregularEdge = 0.88
                + 0.045 * std::sin(angle * 5.0 + 0.7)
                + 0.025 * std::sin(angle * 11.0 - 0.35);
            const auto edge = 1.0 - smoothstep(
                irregularEdge - 0.065, irregularEdge + 0.025, radius);
            const auto fiber = 0.82
                + 0.18 * hashUnit(static_cast<std::uint64_t>(x)
                    | (static_cast<std::uint64_t>(y) << 32U));
            const auto coverage = std::clamp(edge * fiber, 0.0, 1.0);
            pixels[static_cast<std::size_t>(y) * extent + x]
                = static_cast<std::uint8_t>(std::lround(coverage * 255.0));
        }
    }
    return pixels;
}

std::vector<std::uint8_t> makePaperGrainPixels()
{
    constexpr std::uint32_t extent = 64;
    std::vector<double> working(extent * extent);
    for (std::uint32_t y = 0; y < extent; ++y) {
        for (std::uint32_t x = 0; x < extent; ++x) {
            const auto key = static_cast<std::uint64_t>(x)
                | (static_cast<std::uint64_t>(y) << 32U);
            const auto fine = hashUnit(key ^ 0xD17A6B4C2E9015F3ULL);
            const auto fiber = hashUnit(key ^ 0x5A71C39E842DB60FULL);
            working[static_cast<std::size_t>(y) * extent + x]
                = 0.22 + 0.78 * fine * fine
                - (fiber > 0.965 ? 0.18 : 0.0);
        }
    }
    // A small periodic blur creates paper-like clusters while retaining dry
    // holes. Wrapping here makes the built-in tile seamless by construction.
    for (int pass = 0; pass < 2; ++pass) {
        auto filtered = working;
        for (std::uint32_t y = 0; y < extent; ++y) {
            for (std::uint32_t x = 0; x < extent; ++x) {
                double sum = 0.0;
                for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        const auto sourceX = static_cast<std::uint32_t>(
                            (static_cast<int>(x) + offsetX
                                + static_cast<int>(extent))
                            % static_cast<int>(extent));
                        const auto sourceY = static_cast<std::uint32_t>(
                            (static_cast<int>(y) + offsetY
                                + static_cast<int>(extent))
                            % static_cast<int>(extent));
                        sum += working[static_cast<std::size_t>(sourceY)
                                * extent
                            + sourceX];
                    }
                }
                filtered[static_cast<std::size_t>(y) * extent + x] = sum / 9.0;
            }
        }
        working = std::move(filtered);
    }
    std::vector<std::uint8_t> pixels(working.size());
    std::transform(working.begin(), working.end(), pixels.begin(),
        [](double value) {
            const auto normalized = std::clamp(
                (value - 0.16) / 0.58, 0.04, 1.0);
            return static_cast<std::uint8_t>(std::lround(normalized * 255.0));
        });
    return pixels;
}

class BuiltinBrushAssetResolver final : public IBrushAssetResolver {
public:
    BuiltinBrushAssetResolver()
    {
        const auto dryTip = makeDryInkTipPixels();
        dryInkTip_ = std::make_shared<GrayscaleMaskAsset>(
            std::string(BrushAssetIds::DryInkMaskTip), 1, 64, 64, dryTip);
        const auto paper = makePaperGrainPixels();
        paperGrain_ = std::make_shared<GrayscaleMaskAsset>(
            std::string(BrushAssetIds::DryInkPaperGrain), 1, 64, 64, paper);
    }

    std::unique_ptr<IBrushTip> createTip(
        const BrushTipDescriptor& descriptor) const override
    {
        if (descriptor.assetId == BrushAssetIds::ProceduralRoundTip) {
            return std::make_unique<ProceduralRoundTip>();
        }
        if (descriptor.assetId == BrushAssetIds::ProceduralEllipseTip) {
            return std::make_unique<ProceduralEllipseTip>();
        }
        if (descriptor.assetId == BrushAssetIds::DryInkMaskTip) {
            return std::make_unique<BitmapMaskTip>(dryInkTip_);
        }
        return makeCreativeBrushTip(descriptor.assetId);
    }

    std::unique_ptr<IBrushGrain> createGrain(
        const BrushGrainDescriptor& descriptor) const override
    {
        if (descriptor.assetId == BrushAssetIds::NoGrain) {
            return std::make_unique<NoBrushGrain>();
        }
        if (descriptor.assetId == BrushAssetIds::DryInkPaperGrain) {
            return std::make_unique<DocumentAnchoredMaskGrain>(paperGrain_);
        }
        return {};
    }

    BrushAssetCacheStats cacheStats() const noexcept override
    {
        const auto creative = creativeBrushCacheStats();
        return {
            .grayscaleMaskCount = 2 + creative.grayscaleMaskCount,
            .retainedBytes = dryInkTip_->retainedBytes()
                + paperGrain_->retainedBytes() + creative.retainedBytes,
        };
    }

private:
    std::shared_ptr<const GrayscaleMaskAsset> dryInkTip_;
    std::shared_ptr<const GrayscaleMaskAsset> paperGrain_;
};

} // namespace

BrushTipBounds ProceduralRoundTip::prepareDab(const BrushDab& dab,
    double hardness, double documentPixelFootprint) noexcept
{
    center_ = dab.documentCenter;
    halfWidth_ = std::max(0.0, dab.diameterPixels * 0.5);
    halfHeight_ = halfWidth_ * std::clamp(dab.tipAspectRatio, 0.02, 1.0);
    hardness_ = std::clamp(hardness, 0.0, 1.0);
    const auto angle = radians(dab.tipAngleDegrees);
    cosine_ = std::cos(angle);
    sine_ = std::sin(angle);
    if (halfWidth_ <= 0.0 || halfHeight_ <= 0.0) {
        return {};
    }
    const auto antialias = std::max(0.25, documentPixelFootprint * 0.5);
    normalizedAntialiasWidth_ = antialias / std::min(halfWidth_, halfHeight_);
    const auto extentX = std::hypot(halfWidth_ * cosine_, halfHeight_ * sine_)
        + std::max(0.0, documentPixelFootprint);
    const auto extentY = std::hypot(halfWidth_ * sine_, halfHeight_ * cosine_)
        + std::max(0.0, documentPixelFootprint);
    return {center_.x - extentX, center_.y - extentY,
        center_.x + extentX, center_.y + extentY};
}

double ProceduralRoundTip::coverage(Vec2d point) const noexcept
{
    if (halfWidth_ <= 0.0 || halfHeight_ <= 0.0) {
        return 0.0;
    }
    const auto deltaX = point.x - center_.x;
    const auto deltaY = point.y - center_.y;
    const auto localX = cosine_ * deltaX + sine_ * deltaY;
    const auto localY = -sine_ * deltaX + cosine_ * deltaY;
    const auto normalizedDistance = std::hypot(
        localX / halfWidth_, localY / halfHeight_);
    if (hardness_ >= 0.999) {
        return 1.0 - smoothstep(1.0 - normalizedAntialiasWidth_,
            1.0 + normalizedAntialiasWidth_, normalizedDistance);
    }
    if (normalizedDistance <= hardness_) {
        return 1.0;
    }
    if (normalizedDistance >= 1.0 + normalizedAntialiasWidth_) {
        return 0.0;
    }
    return 1.0 - smoothstep(hardness_,
        1.0 + normalizedAntialiasWidth_, normalizedDistance);
}

BrushTipBounds ProceduralEllipseTip::prepareDab(const BrushDab& dab,
    double hardness, double documentPixelFootprint) noexcept
{
    center_ = dab.documentCenter;
    halfWidth_ = std::max(0.0, dab.diameterPixels * 0.5);
    halfHeight_ = halfWidth_ * std::clamp(dab.tipAspectRatio, 0.02, 1.0);
    hardness_ = std::clamp(hardness, 0.0, 1.0);
    const auto angle = radians(dab.tipAngleDegrees);
    cosine_ = std::cos(angle);
    sine_ = std::sin(angle);
    if (halfWidth_ <= 0.0 || halfHeight_ <= 0.0) {
        return {};
    }
    const auto antialias = std::max(0.25, documentPixelFootprint * 0.5);
    normalizedAntialiasWidth_ = antialias / std::min(halfWidth_, halfHeight_);
    const auto extentX = std::hypot(halfWidth_ * cosine_, halfHeight_ * sine_)
        + std::max(0.0, documentPixelFootprint);
    const auto extentY = std::hypot(halfWidth_ * sine_, halfHeight_ * cosine_)
        + std::max(0.0, documentPixelFootprint);
    return {center_.x - extentX, center_.y - extentY,
        center_.x + extentX, center_.y + extentY};
}

double ProceduralEllipseTip::coverage(Vec2d point) const noexcept
{
    if (halfWidth_ <= 0.0 || halfHeight_ <= 0.0) {
        return 0.0;
    }
    const auto deltaX = point.x - center_.x;
    const auto deltaY = point.y - center_.y;
    const auto localX = cosine_ * deltaX + sine_ * deltaY;
    const auto localY = -sine_ * deltaX + cosine_ * deltaY;
    const auto normalizedDistance = std::hypot(
        localX / halfWidth_, localY / halfHeight_);
    if (hardness_ >= 0.999) {
        return 1.0 - smoothstep(1.0 - normalizedAntialiasWidth_,
            1.0 + normalizedAntialiasWidth_, normalizedDistance);
    }
    if (normalizedDistance <= hardness_) {
        return 1.0;
    }
    if (normalizedDistance >= 1.0 + normalizedAntialiasWidth_) {
        return 0.0;
    }
    return 1.0 - smoothstep(hardness_,
        1.0 + normalizedAntialiasWidth_, normalizedDistance);
}

GrayscaleMaskAsset::GrayscaleMaskAsset(std::string id,
    std::uint64_t revision, std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> pixels)
    : id_(std::move(id))
    , revision_(revision)
{
    if (id_.empty() || width == 0 || height == 0
        || width > std::numeric_limits<std::uint32_t>::max() / height
        || pixels.size() != static_cast<std::size_t>(width) * height) {
        throw std::invalid_argument("Invalid grayscale brush mask");
    }
    Level base {.width = width, .height = height, .texels = {}};
    base.texels.reserve(pixels.size());
    std::transform(pixels.begin(), pixels.end(),
        std::back_inserter(base.texels), [](std::uint8_t value) {
            return static_cast<float>(value) / 255.0F;
        });
    levels_.push_back(std::move(base));
    while (levels_.back().width > 1 || levels_.back().height > 1) {
        const auto& source = levels_.back();
        Level next {
            .width = std::max(1U,
                source.width / 2U + source.width % 2U),
            .height = std::max(1U,
                source.height / 2U + source.height % 2U),
            .texels = {},
        };
        next.texels.resize(
            static_cast<std::size_t>(next.width) * next.height);
        for (std::uint32_t y = 0; y < next.height; ++y) {
            for (std::uint32_t x = 0; x < next.width; ++x) {
                double sum = 0.0;
                for (std::uint32_t offsetY = 0; offsetY < 2; ++offsetY) {
                    for (std::uint32_t offsetX = 0; offsetX < 2; ++offsetX) {
                        const auto sourceX = std::min(
                            source.width - 1U, x * 2U + offsetX);
                        const auto sourceY = std::min(
                            source.height - 1U, y * 2U + offsetY);
                        sum += source.texels[static_cast<std::size_t>(sourceY)
                                * source.width
                            + sourceX];
                    }
                }
                next.texels[static_cast<std::size_t>(y) * next.width + x]
                    = static_cast<float>(sum * 0.25);
            }
        }
        levels_.push_back(std::move(next));
    }
}

std::uint32_t GrayscaleMaskAsset::width() const noexcept
{
    return levels_.empty() ? 0U : levels_.front().width;
}

std::uint32_t GrayscaleMaskAsset::height() const noexcept
{
    return levels_.empty() ? 0U : levels_.front().height;
}

std::size_t GrayscaleMaskAsset::retainedBytes() const noexcept
{
    std::size_t result = id_.capacity() + levels_.capacity() * sizeof(Level);
    for (const auto& level : levels_) {
        result += level.texels.capacity() * sizeof(float);
    }
    return result;
}

double GrayscaleMaskAsset::sample(
    double u, double v, double lod, bool repeat) const noexcept
{
    if (levels_.empty() || !std::isfinite(u) || !std::isfinite(v)) {
        return 0.0;
    }
    lod = std::clamp(std::isfinite(lod) ? lod : 0.0, 0.0,
        static_cast<double>(levels_.size() - 1U));
    const auto first = static_cast<std::size_t>(std::floor(lod));
    const auto second = std::min(first + 1U, levels_.size() - 1U);
    const auto blend = lod - static_cast<double>(first);
    const auto firstValue = sampleLevel(first, u, v, repeat);
    const auto secondValue = sampleLevel(second, u, v, repeat);
    return std::clamp(firstValue
            + (secondValue - firstValue) * blend,
        0.0, 1.0);
}

double GrayscaleMaskAsset::sampleLevel(std::size_t levelIndex,
    double u, double v, bool repeat) const noexcept
{
    const auto& level = levels_[levelIndex];
    const auto x = u * static_cast<double>(level.width) - 0.5;
    const auto y = v * static_cast<double>(level.height) - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(x));
    const auto y0 = static_cast<std::int64_t>(std::floor(y));
    const auto amountX = x - static_cast<double>(x0);
    const auto amountY = y - static_cast<double>(y0);
    const auto fetch = [&level, repeat](std::int64_t column,
                           std::int64_t row) noexcept {
        const auto width = static_cast<std::int64_t>(level.width);
        const auto height = static_cast<std::int64_t>(level.height);
        if (repeat) {
            column = ((column % width) + width) % width;
            row = ((row % height) + height) % height;
        } else if (column < 0 || row < 0
            || column >= width || row >= height) {
            return 0.0;
        }
        return static_cast<double>(level.texels[
            static_cast<std::size_t>(row) * level.width
            + static_cast<std::size_t>(column)]);
    };
    const auto top = fetch(x0, y0)
        + (fetch(x0 + 1, y0) - fetch(x0, y0)) * amountX;
    const auto bottom = fetch(x0, y0 + 1)
        + (fetch(x0 + 1, y0 + 1) - fetch(x0, y0 + 1)) * amountX;
    return top + (bottom - top) * amountY;
}

BitmapMaskTip::BitmapMaskTip(
    std::shared_ptr<const GrayscaleMaskAsset> mask, Filtering filtering)
    : mask_(std::move(mask))
    , filtering_(filtering)
{
}

BrushTipBounds BitmapMaskTip::prepareDab(const BrushDab& dab,
    double hardness, double documentPixelFootprint) noexcept
{
    center_ = dab.documentCenter;
    halfWidth_ = std::max(0.0, dab.diameterPixels * 0.5);
    halfHeight_ = halfWidth_ * std::clamp(dab.tipAspectRatio, 0.02, 1.0);
    const auto angle = radians(dab.tipAngleDegrees);
    cosine_ = std::cos(angle);
    sine_ = std::sin(angle);
    coverageExponent_ = std::exp2(
        (0.5 - std::clamp(hardness, 0.0, 1.0)) * 2.0);
    filterTaps_ = 1;
    filterStepU_ = filterStepV_ = 0.0;
    if (!mask_ || halfWidth_ <= 0.0 || halfHeight_ <= 0.0) {
        return {};
    }
    const auto sourceTexelsPerPixel = std::max(
        static_cast<double>(mask_->width()) / (halfWidth_ * 2.0),
        static_cast<double>(mask_->height()) / (halfHeight_ * 2.0))
        * std::max(1.0e-6, documentPixelFootprint);
    lod_ = std::max(0.0, std::log2(std::max(1.0, sourceTexelsPerPixel)));
    if (filtering_ == Filtering::Anisotropic) {
        const auto footprint = std::max(1.0e-6, documentPixelFootprint);
        const auto texelsX = double(mask_->width()) * footprint / (halfWidth_ * 2);
        const auto texelsY = double(mask_->height()) * footprint / (halfHeight_ * 2);
        const auto major = std::max(texelsX, texelsY);
        // At most sixteen taps, with a coarser mip if the requested footprint
        // is more elongated. Integrate coverage along the compressed axis,
        // rather than choosing its coarse mip in BOTH axes and losing fibers.
        const auto minor = std::max({1.0, std::min(texelsX, texelsY), major / 16.0});
        filterTaps_ = std::clamp(int(std::ceil(major / minor)), 1, 16);
        lod_ = std::log2(minor);
        if (texelsX > texelsY) filterStepU_ = footprint / (halfWidth_ * 2 * filterTaps_);
        else filterStepV_ = footprint / (halfHeight_ * 2 * filterTaps_);
    }
    const auto extentX = std::abs(cosine_) * halfWidth_
        + std::abs(sine_) * halfHeight_
        + std::max(0.0, documentPixelFootprint);
    const auto extentY = std::abs(sine_) * halfWidth_
        + std::abs(cosine_) * halfHeight_
        + std::max(0.0, documentPixelFootprint);
    return {center_.x - extentX, center_.y - extentY,
        center_.x + extentX, center_.y + extentY};
}

double BitmapMaskTip::coverage(Vec2d point) const noexcept
{
    if (!mask_ || halfWidth_ <= 0.0 || halfHeight_ <= 0.0) {
        return 0.0;
    }
    const auto deltaX = point.x - center_.x;
    const auto deltaY = point.y - center_.y;
    const auto localX = cosine_ * deltaX + sine_ * deltaY;
    const auto localY = -sine_ * deltaX + cosine_ * deltaY;
    const auto u = localX / (halfWidth_ * 2.0) + 0.5;
    const auto v = localY / (halfHeight_ * 2.0) + 0.5;
    if (filterTaps_ == 1)
        return std::pow(mask_->sample(u, v, lod_, false), coverageExponent_);
    double coverage = 0;
    for (int i = 0; i < filterTaps_; ++i) {
        const auto offset = double(i) - (filterTaps_ - 1) * .5;
        coverage += mask_->sample(u + offset * filterStepU_, v + offset * filterStepV_, lod_, false);
    }
    // Hardness acts on the reconstructed coverage once, not on individual taps.
    return std::pow(coverage / filterTaps_, coverageExponent_);
}

DocumentAnchoredMaskGrain::DocumentAnchoredMaskGrain(
    std::shared_ptr<const GrayscaleMaskAsset> mask)
    : mask_(std::move(mask))
{
}

void DocumentAnchoredMaskGrain::prepareDab(const BrushDab& dab,
    double documentPixelFootprint) noexcept
{
    scaleX_ = std::max(1.0e-6, dab.grainScalePixels);
    scaleY_ = mask_ && mask_->width() > 0
        ? scaleX_ * static_cast<double>(mask_->height()) / mask_->width()
        : scaleX_;
    const auto angle = radians(dab.grainAngleDegrees);
    cosine_ = std::cos(angle);
    sine_ = std::sin(angle);
    offsetU_ = hashUnit(dab.deterministicSeed ^ 0x68F42A19B7C35D01ULL);
    offsetV_ = hashUnit(dab.deterministicSeed ^ 0xB41E9037D625AC8FULL);
    strength_ = std::clamp(dab.grainStrength, 0.0, 1.0);
    invert_ = dab.grainInvert;
    const auto sourceTexelsPerPixel = mask_
        ? std::max(static_cast<double>(mask_->width()) / scaleX_,
              static_cast<double>(mask_->height()) / scaleY_)
            * std::max(1.0e-6, documentPixelFootprint)
        : 1.0;
    lod_ = std::max(0.0, std::log2(std::max(1.0, sourceTexelsPerPixel)));
}

double DocumentAnchoredMaskGrain::modulation(Vec2d point) const noexcept
{
    if (!mask_ || strength_ <= 0.0) {
        return 1.0;
    }
    // The rotation is around document origin, never around the dab. Therefore
    // separate strokes and transformed layers encounter the same paper grain.
    const auto grainX = cosine_ * point.x + sine_ * point.y;
    const auto grainY = -sine_ * point.x + cosine_ * point.y;
    const auto u = grainX / scaleX_ + offsetU_;
    const auto v = grainY / scaleY_ + offsetV_;
    auto value = mask_->sample(u, v, lod_, true);
    if (invert_) {
        value = 1.0 - value;
    }
    return std::clamp(1.0 + (value - 1.0) * strength_, 0.0, 1.0);
}

const IBrushAssetResolver& builtinBrushAssetResolver() noexcept
{
    static const BuiltinBrushAssetResolver resolver;
    return resolver;
}

std::unique_ptr<IBrushTip> makeBuiltinBrushTip(std::string_view assetId)
{
    BrushTipDescriptor descriptor;
    descriptor.assetId = assetId;
    return builtinBrushAssetResolver().createTip(descriptor);
}

std::unique_ptr<IBrushGrain> makeBuiltinBrushGrain(std::string_view assetId)
{
    BrushGrainDescriptor descriptor;
    descriptor.assetId = assetId;
    return builtinBrushAssetResolver().createGrain(descriptor);
}

} // namespace imageeditor::core
