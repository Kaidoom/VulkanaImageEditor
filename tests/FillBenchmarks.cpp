#include "imageeditor/core/FillOperation.hpp"

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
    explicit CountedSurface(Extent2u extent) : pixels(extent, {29, 71, 111, 128}) {}
    ContiguousRasterSurface pixels;
    mutable std::size_t maximumRead {0}, bytesRead {0};
    SurfaceId id() const noexcept override { return pixels.id(); }
    Extent2u extent() const noexcept override { return pixels.extent(); }
    Revision revision() const noexcept override { return pixels.revision(); }
    DirtySet dirtySince(Revision revision) const override { return pixels.dirtySince(revision); }
    void copyRgba8(RectI area, std::span<std::byte> output, std::size_t stride) const override
    {
        const auto amount = std::size_t(area.width) * std::size_t(area.height) * 4;
        maximumRead = std::max(maximumRead, amount); bytesRead += amount;
        pixels.copyRgba8(area, output, stride);
    }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch> patches) override { return pixels.replaceRgba8Batch(patches); }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch> patches) override { return pixels.swapRgba8Batch(patches); }
};

void run(Extent2u extent, FillMode mode, bool selected)
{
    Document document({extent});
    auto surface = std::make_shared<CountedSurface>(extent);
    auto layer = Layer::raster("Benchmark", surface); const auto id = layer.id;
    require(document.insertLayer(0, std::move(layer)), "Could not create fixture");
    if (selected) {
        // Partial selection, disconnected pieces and a genuine hole. Exclude
        // mask construction from measured fill time.
        auto mask = SelectionMask::rectangle(extent, {32, 32, int(extent.width) - 64, int(extent.height) - 64}, 128);
        mask = mask->combined(*SelectionMask::rectangle(extent, {int(extent.width / 2), 0, 64, int(extent.height)}), SelectionOperation::Subtract);
        mask = mask->combined(*SelectionMask::rectangle(extent, {160, 160, 140, 140}), SelectionOperation::Subtract);
        require(document.setSelection(mask), "Could not create selection");
    }
    History history;
    const auto start = Clock::now();
    FillOperation fill(document, id, {.mode = mode, .color = {230, 75, 40, 177}, .opacity = 0.75,
        .tolerance = 16, .seed = {80.5, 80.5}});
    const auto constructed = Clock::now();
    require(fill.stats().journalBytes == 0, "Fill copied journal pixels on construction");
    std::size_t slices = 0, uploadedBytes = 0;
    double longestSlice = 0, discoveryMs = 0, applyingMs = 0;
    auto uploaded = surface->revision();
    while (fill.state() == FillState::Applying || fill.state() == FillState::Discovering) {
        const auto phase = fill.state();
        const auto before = Clock::now();
        (void)fill.step(65536);
        const auto elapsed = ms(Clock::now() - before);
        longestSlice = std::max(longestSlice, elapsed);
        if (phase == FillState::Discovering) discoveryMs += elapsed; else applyingMs += elapsed;
        ++slices;
        const auto dirty = surface->dirtySince(uploaded);
        require(!dirty.fullRefresh, "Fill overflowed incremental upload history");
        for (const auto region : dirty.regions) uploadedBytes += std::size_t(region.width) * std::size_t(region.height) * 4;
        uploaded = dirty.revision;
    }
    require(fill.state() == FillState::Ready, "Fill did not reach ready state");
    const auto ready = Clock::now();
    require(fill.commit(history) == RasterEditCommitResult::Committed, "Fill did not commit");
    const auto committed = Clock::now();
    require(history.undo(document), "Fill undo failed");
    const auto undone = Clock::now();
    require(!surface->dirtySince(uploaded).fullRefresh, "Undo lost incremental invalidation");
    require(history.redo(document), "Fill redo failed");
    const auto redone = Clock::now();
    require(history.undoDepth() == 1, "Fill created multiple undo commands");
    require(surface->maximumRead <= std::max(std::size_t(extent.width) * 4, std::size_t(64 * 64 * 4)), "Fill read a full frame");
    constexpr auto MiB = 1024.0 * 1024.0;
    std::cout << extent.width << 'x' << extent.height << ' '
              << (mode == FillMode::Contiguous ? "region" : "solid")
              << (selected ? " masked" : " unrestricted")
              << " construct_ms=" << ms(constructed - start)
              << " discovery_ms=" << discoveryMs << " apply_ms=" << applyingMs
              << " max_slice_ms=" << longestSlice << " slices=" << slices
              << " commit_ms=" << ms(committed - ready)
              << " undo_ms=" << ms(undone - committed) << " redo_ms=" << ms(redone - undone)
              << " discovery_MiB=" << double(fill.stats().discoveryBytes) / MiB
              << " journal_MiB=" << double(fill.stats().journalBytes) / MiB
              << " history_MiB=" << double(history.memoryUsed()) / MiB
              << " upload_MiB=" << double(uploadedBytes) / MiB
              << " max_read_KiB=" << double(surface->maximumRead) / 1024.0 << '\n';
}
}

int main()
{
    try {
        std::cout << std::fixed << std::setprecision(2);
        for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}})
            for (const auto mode : {FillMode::SelectionOrLayer, FillMode::Contiguous})
                for (const bool masked : {false, true}) run(extent, mode, masked);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
