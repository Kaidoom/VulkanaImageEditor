#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/render/DirtyRegionCoalescer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace {

using namespace imageeditor::core;
using Clock = std::chrono::steady_clock;

struct DocumentFixture {
    Document document;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId layerId {0};

    explicit DocumentFixture(Extent2u extent, Rgba8 fill = {})
        : document(CanvasSpec {.extent = extent})
        , surface(std::make_shared<ContiguousRasterSurface>(extent, fill))
    {
        auto layer = Layer::raster("Benchmark", surface);
        layerId = layer.id;
        if (!document.insertLayer(0, std::move(layer))) {
            throw std::runtime_error("Unable to construct benchmark document");
        }
    }
};

double microseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::micro>(duration).count();
}

void benchmarkJournal(Extent2u extent, std::uint32_t tileSize)
{
    DocumentFixture fixture(extent);
    History history;
    RasterEditTransaction transaction(fixture.document, fixture.layerId,
        "Benchmark edit", {.journalTileSize = tileSize});
    const auto diagonal = std::min(extent.width, extent.height);
    std::vector<std::array<std::byte, 4>> pixels;
    std::vector<RasterPatch> patches;
    constexpr std::uint32_t step = 31;
    const auto count = (diagonal + step - 1U) / step;
    pixels.resize(count, {std::byte {0x53}, std::byte {0x7F},
        std::byte {0xF2}, std::byte {0xFF}});
    patches.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto coordinate = std::min(index * step, diagonal - 1U);
        patches.push_back({{static_cast<std::int32_t>(coordinate),
                               static_cast<std::int32_t>(coordinate), 1, 1},
            pixels[index], 4});
    }

    const auto start = Clock::now();
    const auto dirty = transaction.writeRgba8Batch(patches);
    const auto capturedTiles = transaction.capturedTileCount();
    const auto capturedBytes = transaction.capturedPixelBytes();
    const auto commit = transaction.commit(history);
    const auto elapsed = Clock::now() - start;
    if (commit != RasterEditCommitResult::Committed || dirty.fullRefresh) {
        throw std::runtime_error("Journal benchmark did not remain incremental");
    }
    const auto committedRevision = fixture.surface->revision();
    const auto undoStart = Clock::now();
    if (!history.undo(fixture.document)) {
        throw std::runtime_error("Journal benchmark undo failed");
    }
    const auto undoElapsed = Clock::now() - undoStart;
    const auto undoDirty = fixture.surface->dirtySince(committedRevision);
    if (undoDirty.fullRefresh) {
        throw std::runtime_error("Journal benchmark undo became a full refresh");
    }
    const auto coalesceStart = Clock::now();
    const auto gpuRegions = imageeditor::render::coalesceDirtyRegionsForUpload(
        undoDirty.regions, extent);
    const auto coalesceElapsed = Clock::now() - coalesceStart;
    std::cout << "journal," << extent.width << 'x' << extent.height << ','
              << tileSize << ',' << patches.size() << ",0," << capturedTiles
              << ",0," << capturedBytes << ",0," << history.memoryUsed() << ','
              << std::fixed << std::setprecision(1) << microseconds(elapsed)
              << ",0,0," << undoDirty.regions.size() << ','
              << microseconds(undoElapsed) << ',' << gpuRegions.size() << ','
              << microseconds(coalesceElapsed) << '\n';
}

NormalizedPointerSample pointerSample(double x, double y,
    std::uint64_t timestampMicroseconds)
{
    return {
        .documentPosition = {x, y},
        .timestampMicroseconds = timestampMicroseconds,
        .pressure = 1.0,
        .tiltX = 0.25,
        .tiltY = -0.15,
        .rotationDegrees = 20.0,
        .barrelRotationDegrees = 20.0,
        .pointerType = PointerType::Pen,
        .buttons = PointerButtonPrimary,
        .modifiers = PointerModifierNone,
    };
}

void benchmarkBrush(Extent2u extent, ProceduralBrushPreset preset,
    BrushTipRotationMode rotationMode, BrushCompositeMode compositeMode,
    std::string_view benchmarkKind)
{
    // Erase needs an alpha-bearing target; a transparent fixture would only
    // benchmark the intentional no-op path.
    DocumentFixture fixture(extent, compositeMode == BrushCompositeMode::Erase
            ? Rgba8 {74, 112, 198, 224} : Rgba8 {});
    History history;
    auto settings = proceduralBrushPreset(preset);
    settings.sizePixels = 64.0;
    settings.spacingPercent = 8.0;
    settings.smoothing = BrushSmoothingMode::None;
    settings.tip.rotationMode = rotationMode;
    constexpr std::uint32_t eventCount = 181;
    BasicPixelBrushStroke stroke(
        fixture.document, fixture.layerId, settings, compositeMode);
    const auto uploadedRevision = fixture.surface->revision();

    auto maximumInput = Clock::duration::zero();
    auto totalInput = Clock::duration::zero();
    const auto start = Clock::now();
    if (!stroke.begin(pointerSample(32.0, 32.0, 0))) {
        throw std::runtime_error("Unable to begin benchmark stroke");
    }
    for (std::uint32_t index = 1; index + 1 < eventCount; ++index) {
        const auto amount = static_cast<double>(index)
            / static_cast<double>(eventCount - 1U);
        const auto inputStart = Clock::now();
        if (!stroke.append(pointerSample(
                32.0 + (static_cast<double>(extent.width) - 64.0) * amount,
                32.0 + (static_cast<double>(extent.height) - 64.0) * amount,
                static_cast<std::uint64_t>(amount * 1000000.0)))) {
            throw std::runtime_error("Benchmark stroke append failed");
        }
        const auto duration = Clock::now() - inputStart;
        totalInput += duration;
        maximumInput = std::max(maximumInput, duration);
    }
    const auto endResult = stroke.end(pointerSample(
        static_cast<double>(extent.width) - 32.0,
        static_cast<double>(extent.height) - 32.0, 1000000), history);
    const auto total = Clock::now() - start;
    const auto dirty = fixture.surface->dirtySince(uploadedRevision);
    if (endResult != RasterEditCommitResult::Committed || dirty.fullRefresh) {
        throw std::runtime_error("Brush benchmark fell back to a full upload");
    }
    const auto& stats = stroke.stats();
    const auto committedRevision = fixture.surface->revision();
    const auto undoStart = Clock::now();
    if (!history.undo(fixture.document)) {
        throw std::runtime_error("Brush benchmark undo failed");
    }
    const auto undoElapsed = Clock::now() - undoStart;
    const auto undoDirty = fixture.surface->dirtySince(committedRevision);
    if (undoDirty.fullRefresh) {
        throw std::runtime_error("Brush benchmark undo became a full refresh");
    }
    const auto coalesceStart = Clock::now();
    const auto gpuRegions = imageeditor::render::coalesceDirtyRegionsForUpload(
        undoDirty.regions, extent);
    const auto coalesceElapsed = Clock::now() - coalesceStart;
    std::cout << benchmarkKind << ',' << extent.width << 'x' << extent.height
              << ",64,"
              << eventCount << ',' << stats.emittedDabs << ",0,"
              << stats.retainedStrokeTiles << ",0," << stats.uploadedRegionBytes
              << ',' << history.memoryUsed() << ',' << std::fixed << std::setprecision(1)
              << microseconds(total) << ','
              << microseconds(totalInput) / static_cast<double>(eventCount - 2U) << ','
              << microseconds(maximumInput) << ',' << undoDirty.regions.size() << ','
              << microseconds(undoElapsed) << ',' << gpuRegions.size() << ','
              << microseconds(coalesceElapsed) << '\n';
}

} // namespace

int main()
{
    std::cout << "kind,document,journal_tile,input_items,emitted_dabs,"
                 "captured_tiles,retained_stroke_tiles,snapshot_bytes,"
                 "incremental_upload_bytes,history_bytes,total_us,"
                 "avg_input_us,max_input_us,undo_regions,undo_us,"
                 "gpu_upload_regions,coalesce_us\n";
    benchmarkJournal({3840, 2160}, 64);
    benchmarkJournal({3840, 2160}, 128);
    benchmarkJournal({5120, 2880}, 64);
    benchmarkJournal({5120, 2880}, 128);
    struct BrushBenchmarkCase {
        ProceduralBrushPreset preset;
        BrushTipRotationMode rotationMode;
        BrushCompositeMode compositeMode;
        std::string_view kind;
    };
    for (const auto& test : std::array {
             BrushBenchmarkCase {ProceduralBrushPreset::PressureRound,
                 BrushTipRotationMode::Fixed, BrushCompositeMode::Paint,
                 "brush-round-fixed"},
             BrushBenchmarkCase {ProceduralBrushPreset::PressureRound,
                 BrushTipRotationMode::FollowStrokeDirection,
                 BrushCompositeMode::Paint,
                 "brush-round-follow"},
             BrushBenchmarkCase {ProceduralBrushPreset::InkPen,
                 BrushTipRotationMode::FollowStrokeDirection,
                 BrushCompositeMode::Paint,
                 "brush-ink-follow"},
             BrushBenchmarkCase {ProceduralBrushPreset::DryInk,
                 BrushTipRotationMode::Fixed, BrushCompositeMode::Paint,
                 "brush-dry-ink-fixed"},
             BrushBenchmarkCase {ProceduralBrushPreset::DryInk,
                 BrushTipRotationMode::FollowStrokeDirection,
                 BrushCompositeMode::Paint, "brush-dry-ink-follow"},
             BrushBenchmarkCase {ProceduralBrushPreset::PressureRound,
                 BrushTipRotationMode::Fixed, BrushCompositeMode::Erase,
                 "eraser-round-fixed"},
             BrushBenchmarkCase {ProceduralBrushPreset::DryInk,
                 BrushTipRotationMode::FollowStrokeDirection,
                 BrushCompositeMode::Erase, "eraser-dry-ink-follow"},
         }) {
        benchmarkBrush({3840, 2160}, test.preset,
            test.rotationMode, test.compositeMode, test.kind);
        benchmarkBrush({5120, 2880}, test.preset,
            test.rotationMode, test.compositeMode, test.kind);
    }
    return 0;
}
