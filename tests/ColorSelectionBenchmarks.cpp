#include "imageeditor/core/ColorSelection.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace imageeditor::core;
using Clock = std::chrono::steady_clock;
double ms(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }

class CountedSurface final : public RasterSurface {
public:
    CountedSurface(Extent2u extent, bool overlay) : extent_(extent), overlay_(overlay) {}
    SurfaceId id() const noexcept override { return overlay_ ? 999002 : 999001; }
    Extent2u extent() const noexcept override { return extent_; }
    Revision revision() const noexcept override { return 1; }
    DirtySet dirtySince(Revision revision) const override
    { return {.revision = 1, .fullRefresh = revision != 1, .regions = {}}; }
    void copyRgba8(RectI area, std::span<std::byte> output, std::size_t stride) const override
    {
        const auto count = std::size_t(area.width) * std::size_t(area.height);
        maximumRead = std::max(maximumRead, count); texelsRead += count;
        for (int y = 0; y < area.height; ++y) for (int x = 0; x < area.width; ++x) {
            const auto offset = std::size_t(y) * stride + std::size_t(x) * 4;
            const auto value = std::uint8_t(std::uint32_t(area.x + x) * 255 / (extent_.width - 1));
            output[offset] = std::byte(overlay_ ? 190 : value);
            output[offset + 1] = std::byte(overlay_ ? 35 : value);
            output[offset + 2] = std::byte(overlay_ ? 80 : value);
            output[offset + 3] = std::byte(overlay_ ? 64 : 255);
        }
    }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override { throw std::runtime_error("Unexpected source write"); }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override { throw std::runtime_error("Unexpected source write"); }
    mutable std::size_t maximumRead {0}, texelsRead {0};
private:
    Extent2u extent_;
    bool overlay_;
};

void run(Extent2u extent, ColorSampleSource source)
{
    Document document({extent});
    auto surface = std::make_shared<CountedSurface>(extent, false);
    auto overlay = std::make_shared<CountedSurface>(extent, true);
    auto layer = Layer::raster("Gradient", surface); const auto id = layer.id;
    require(document.insertLayer(0, std::move(layer)), "Could not create source");
    if (source == ColorSampleSource::MergedVisible)
        require(document.insertLayer(1, Layer::raster("Translucent overlay", overlay)), "Could not create overlay");
    const auto begin = Clock::now();
    ColorSelectionReference reference(document, id, source, {double(extent.width / 2) + .5, .5});
    const auto constructed = Clock::now();
    std::size_t slices = 0;
    double longestSlice = 0;
    while (!reference.field()) {
        const auto start = Clock::now();
        const auto before = reference.sampledPixels();
        (void)reference.step(4096);
        longestSlice = std::max(longestSlice, ms(Clock::now() - start)); ++slices;
        require(reference.sampledPixels() - before <= 4096, "Sampling exceeded cooperative pixel budget");
    }
    const auto scanned = Clock::now();
    const auto field = reference.field();
    const auto sampledPixels = reference.sampledPixels();
    const auto readPixels = surface->texelsRead + overlay->texelsRead;
    const auto original = SelectionMask::rectangle(extent, {32, 32, int(extent.width) - 64, int(extent.height) - 64}, 180);
    const std::atomic_bool cancel {false};
    double refinementMs = 0, longestRefinement = 0;
    SelectionState last;
    for (const int fuzziness : {8, 32, 16}) {
        const auto start = Clock::now();
        const auto result = buildColorSelection(*field, fuzziness, original, SelectionOperation::Subtract, cancel);
        const auto elapsed = ms(Clock::now() - start);
        refinementMs += elapsed; longestRefinement = std::max(longestRefinement, elapsed);
        require(result.combined != nullptr && result.incoming != nullptr, "Missing refined selection");
        last = result.combined;
    }
    require(reference.sampledPixels() == sampledPixels, "Refinement changed reference sample count");
    require(surface->texelsRead + overlay->texelsRead == readPixels, "Refinement resampled source content");
    require(sampledPixels == std::size_t(extent.width) * extent.height, "Incomplete comparison field");
    require(surface->maximumRead <= 4 && overlay->maximumRead <= 4, "Sampling made a full-image readback");
    require(!document.selection() && !document.isModified(), "Sampling modified document state");
    std::cout << extent.width << 'x' << extent.height
        << (source == ColorSampleSource::ActiveLayer ? " active" : " merged-two-layers")
        << " construct_ms=" << ms(constructed - begin) << " scan_ms=" << ms(scanned - constructed)
        << " max_scan_slice_ms=" << longestSlice << " slices=" << slices
        << " average_refinement_ms=" << refinementMs / 3 << " max_refinement_ms=" << longestRefinement
        << " sampled_pixels=" << sampledPixels << " source_texels_read=" << readPixels
        << " refinement_source_reads=0 comparison_MiB=" << double(field->distances.size() * sizeof(std::uint16_t)) / (1024 * 1024)
        << " final_mask_MiB=" << double(last->memoryCost()) / (1024 * 1024) << '\n';
}
}
int main()
{
    try {
        std::cout << std::fixed << std::setprecision(2);
        for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}})
            for (const auto source : {ColorSampleSource::ActiveLayer, ColorSampleSource::MergedVisible}) run(extent, source);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
