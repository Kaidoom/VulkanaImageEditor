#include "imageeditor/core/ColorSelection.hpp"
#include <algorithm>
#include <stdexcept>

namespace imageeditor::core {
std::uint16_t colorSelectionDistance(Rgba8 a, Rgba8 b) noexcept
{
    if ((a.alpha == 0) != (b.alpha == 0)) return 65535;
    return std::uint16_t(std::max({std::abs(int(a.alpha) - b.alpha) * 255,
        std::abs(int(a.red) * a.alpha - int(b.red) * b.alpha),
        std::abs(int(a.green) * a.alpha - int(b.green) * b.alpha),
        std::abs(int(a.blue) * a.alpha - int(b.blue) * b.alpha)}));
}
std::uint8_t colorSelectionCoverage(std::uint16_t distance, int fuzziness) noexcept
{
    // One quantization-unit transition, not a spatial blur/feather radius.
    // Exact matches have full coverage; each pixel is monotone in fuzziness.
    return std::uint8_t(std::clamp((std::clamp(fuzziness, 0, 255) + 1) * 255 - int(distance), 0, 255));
}
ColorSelectionReference::ColorSelectionReference(const Document& doc, std::optional<LayerId> layer,
    ColorSampleSource source, Vec2d seed, std::span<const SampleCacheOverride> prepared)
    : sampler_(doc, layer, source, SampleFiltering::AlphaAware, prepared), field_(std::make_shared<ColorSelectionField>())
{
    const auto extent = sampler_.extent();
    const auto pixels = std::uint64_t(extent.width) * extent.height;
    if (!pixels || pixels > maximumPixels)
        throw std::length_error("Select by Color supports up to 64 megapixels per operation");
    if (!sampler_.validSample(seed))
        throw std::invalid_argument("Click inside the canvas and the active layer's valid image extent");
    field_->extent = extent;
    field_->sampled = sampler_.sample(seed);
    // Grow incrementally to avoid touching a full high-resolution plane in the
    // pointer-down handler. Reserve imposes the memory bound before any edit.
    field_->distances.reserve(std::size_t(pixels));
}
bool ColorSelectionReference::step(std::size_t budget)
{
    const auto extent = field_->extent;
    const auto total = std::size_t(extent.width) * extent.height;
    const auto end = next_ + std::min(total - next_, std::max(std::size_t(1), budget));
    for (; next_ < end; ++next_) {
        const Vec2d point {double(next_ % extent.width) + .5, double(next_ / extent.width) + .5};
        field_->distances.push_back(sampler_.validSample(point)
            ? colorSelectionDistance(field_->sampled, sampler_.sample(point)) : 65535);
    }
    return next_ == total;
}
std::shared_ptr<const ColorSelectionField> ColorSelectionReference::field() const noexcept
{
    return next_ == std::size_t(field_->extent.width) * field_->extent.height ? field_ : nullptr;
}
std::uint8_t colorSelectionPixelCoverage(const ColorSelectionField& field, std::size_t index, int fuzziness)
{
    const auto d = field.distances[index];
    if (d == 65535 || d == 0 || fuzziness == 255) return colorSelectionCoverage(d,fuzziness);
    const auto width = field.extent.width;
    const auto neighbor = [&](std::size_t i) { return field.distances[i] == 65535 ? double(d) : double(field.distances[i]); };
    const auto x = index % width, y = index / width;
    const double dx = ((x+1 < width ? neighbor(index+1) : d) - (x ? neighbor(index-1) : d)) * .5;
    const double dy = ((y+1 < field.extent.height ? neighbor(index+width) : d) - (y ? neighbor(index-width) : d)) * .5;
    const double a = std::abs(dx), b = std::abs(dy);
    if (a+b <= 255) return colorSelectionCoverage(d,fuzziness);
    // Antialias the local color-distance contour over ONE document pixel.
    // Analytic box integral of a linear reconstruction; never blur/feather a
    // binary mask, sample a viewport, or interpolate across invalid alpha classes.
    const double z = (std::clamp(fuzziness,0,255)+.5)*255 - d + (a+b)*.5;
    if (z <= 0) return 0;
    if (z >= a+b) return 255;
    double area;
    if (a < 1e-9 || b < 1e-9) area = z / std::max(a,b);
    else {
        const auto square = [](double v) { return std::max(0.0,v)*std::max(0.0,v); };
        area = (square(z)-square(z-a)-square(z-b)+square(z-a-b))/(2*a*b);
    }
    return std::uint8_t(std::lround(std::clamp(area,0.0,1.0)*255));
}
bool prepareSelectionBoundary(const SelectionState& mask, const std::atomic_bool& cancelled)
{
    // Preflight pathological noise before the existing boundary cache allocates
    // its pixel-edge vector. Refuse safely rather than exhaust UI/GPU memory.
    std::size_t count = 0;
    const auto bounds = mask->bounds();
    const auto selected = [&](int x, int y) { return mask->coverageAtDocumentPixel(x,y) >= 128; };
    for (int y = bounds.y; y < bounds.bottom(); ++y) {
        if (cancelled) return false;
        for (int x = bounds.x; x < bounds.right(); ++x) if (selected(x,y)) {
            count += !selected(x-1,y); count += !selected(x+1,y);
            count += !selected(x,y-1); count += !selected(x,y+1);
            if (count > 1'000'000)
                throw std::length_error("Color selection is too fragmented (one million boundary edges); adjust fuzziness");
        }
    }
    if (cancelled) return false;
    (void)mask->boundaryEdges();
    return !cancelled;
}
ColorSelectionResult buildColorSelection(const ColorSelectionField& field, int fuzziness,
    SelectionState original, SelectionOperation operation, const std::atomic_bool& cancelled)
{
    fuzziness = std::clamp(fuzziness,0,255);
    const auto count = std::uint64_t(field.extent.width) * field.extent.height;
    if (!count || count > ColorSelectionReference::maximumPixels || count != field.distances.size())
        throw std::invalid_argument("Invalid color selection comparison field");
    std::vector<std::uint8_t> coverage;
    coverage.reserve(std::size_t(count));
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 4096 == 0 && cancelled) return {};
        coverage.push_back(colorSelectionPixelCoverage(field, i, fuzziness));
    }
    if (cancelled) return {};
    auto incoming = SelectionMask::fromR8(field.extent, coverage, field.extent.width);
    if (cancelled) return {};
    auto combined = combineSelection(original, incoming, operation);
    if (!prepareSelectionBoundary(incoming, cancelled)) return {};
    if (combined != incoming && !prepareSelectionBoundary(combined, cancelled)) return {};
    return {std::move(incoming), std::move(combined)};
}
}
