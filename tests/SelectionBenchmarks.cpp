#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace imageeditor::core;
using Clock = std::chrono::steady_clock;

double milliseconds(Clock::duration elapsed)
{
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

template <class Callback>
double measure(Callback&& callback)
{
    const auto start = Clock::now();
    callback();
    return milliseconds(Clock::now() - start);
}

void require(bool result, const char* message)
{
    if (!result) throw std::runtime_error(message);
}

std::string dimensions(Extent2u extent)
{
    return std::to_string(extent.width) + 'x' + std::to_string(extent.height);
}

struct Fixture {
    Document document;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId layer {0};

    explicit Fixture(Extent2u extent)
        : document(CanvasSpec {.extent = extent})
        , surface(std::make_shared<ContiguousRasterSurface>(extent, Rgba8 {34, 67, 113, 180}))
    {
        auto raster = Layer::raster("Benchmark", surface);
        layer = raster.id;
        require(document.insertLayer(0, std::move(raster)), "Unable to construct document");
    }
};

void benchmarkMasks(Extent2u extent)
{
    SelectionState first, second, combined;
    const int centerX = int(extent.width / 2), centerY = int(extent.height / 2);
    const double createMs = measure([&] {
        first = SelectionMask::rectangle(extent, {centerX - 700, centerY - 500, 1400, 1000});
        second = SelectionMask::rectangle(extent, {centerX - 250, centerY - 200, 500, 400});
    });
    const double combineMs = measure([&] {
        combined = first->combined(*second, SelectionOperation::Subtract);
    });
    std::size_t boundaryCount = 0;
    const double boundaryMs = measure([&] { boundaryCount = combined->boundaryEdges().size(); });
    // Repeated requests must hit the immutable geometry cache, not scan the
    // mask again. Retain an observable count to prevent dead-code elimination.
    std::size_t cachedBoundaryCount = 0;
    const double cachedMs = measure([&] {
        for (int index = 0; index < 100; ++index)
            cachedBoundaryCount += combined->boundaryEdges().size();
    });
    require(cachedBoundaryCount == 100 * boundaryCount, "Boundary cache changed");

    double previewSum = 0, previewMaximum = 0;
    std::size_t previewEdgeCount = 0;
    constexpr std::array operations {SelectionOperation::Replace, SelectionOperation::Add,
        SelectionOperation::Subtract, SelectionOperation::Intersect};
    for (std::size_t index = 0; index < 100; ++index) {
        // A moving edge crosses the old selection and its hole. Repeated
        // preview calls never construct or publish a replacement mask.
        const RectI rectangle {centerX - 800 + int(index * 11), centerY - 550 + int(index * 3),
            900 + int(index % 7) * 13, 650};
        const auto elapsed = measure([&] {
            previewEdgeCount += rectangleSelectionPreviewEdges(combined, rectangle,
                operations[index % operations.size()], extent).size();
        });
        previewSum += elapsed;
        previewMaximum = std::max(previewMaximum, elapsed);
    }
    require(previewEdgeCount > 0, "Preview has no boundary");
    std::cout << std::left << std::setw(12) << dimensions(extent)
              << std::right << std::setw(10) << createMs << std::setw(11) << combineMs
              << std::setw(12) << boundaryMs << std::setw(13) << cachedMs / 100.0
              << std::setw(13) << previewSum / 100.0 << std::setw(13) << previewMaximum
              << std::setw(11) << double(combined->memoryCost()) / 1024.0 << '\n';
}

void benchmarkCopy(Extent2u extent, bool bounded, bool rotated)
{
    Fixture fixture(extent); // Fixture initialization is outside timed work.
    if (rotated) {
        constexpr double angle = 0.37;
        const double cosine = std::cos(angle), sine = std::sin(angle);
        const double x = double(extent.width) * 0.5, y = double(extent.height) * 0.5;
        require(fixture.document.setLayerTransform(fixture.layer,
                    {cosine, -sine, x - cosine * x + sine * y,
                        sine, cosine, y - sine * x - cosine * y}),
            "Unable to rotate copy source");
    }
    const auto selected = bounded
        ? SelectionMask::rectangle(extent,
              {int(extent.width / 2) - 256, int(extent.height / 2) - 256, 512, 512})
        : SelectionMask::filled(extent, 255);
    require(fixture.document.setSelection(selected), "Unable to set copy selection");
    require(fixture.document.setLayerOpacity(fixture.layer, 0.55F), "Unable to set copy opacity");
    History history;
    const auto originalRevision = fixture.surface->revision();
    std::unique_ptr<LayerViaCopyCommand> command = std::make_unique<LayerViaCopyCommand>(fixture.layer, fixture.layer);
    const auto* commandPointer = command.get();
    const auto elapsed = measure([&] {
        require(history.execute(fixture.document, std::move(command)), "Layer via copy failed");
    });
    const auto created = commandPointer->createdLayerId();
    require(created.has_value(), "Copy did not create a layer");
    const auto* layer = fixture.document.layer(*created);
    const auto copiedExtent = std::get<RasterLayer>(layer->payload).surface->extent();
    require(layer->opacity == 0.55F, "Copy altered layer opacity");
    require(fixture.surface->revision() == originalRevision, "Copy modified source pixels");
    const auto undoMs = measure([&] { require(history.undo(fixture.document), "Copy undo failed"); });
    require(fixture.document.selection() == selected, "Copy undo altered selection");
    std::cout << std::left << std::setw(12) << dimensions(extent)
              << std::setw(18) << (rotated ? "rotated / 512" : bounded ? "identity / 512" : "identity / full")
              << std::right << std::setw(12) << elapsed << std::setw(12) << undoMs
              << std::setw(12) << double(std::uint64_t(copiedExtent.width) * copiedExtent.height) / 1000000.0
              << std::setw(14) << double(history.memoryUsed()) / (1024.0 * 1024.0) << '\n';
}

enum class MaskScenario { Inactive, Selected, Empty, Partial };

std::string_view scenarioName(MaskScenario scenario)
{
    switch (scenario) {
    case MaskScenario::Inactive: return "inactive";
    case MaskScenario::Selected: return "selected";
    case MaskScenario::Empty: return "active-empty";
    case MaskScenario::Partial: return "partial-128";
    }
    return {};
}

NormalizedPointerSample sample(Vec2d point, std::uint64_t timestamp)
{
    return {.documentPosition = point,
        .timestampMicroseconds = timestamp,
        .pressure = 1.0,
        .pointerType = PointerType::Mouse,
        .buttons = PointerButtonPrimary};
}

void benchmarkBrush(Extent2u extent, double size, MaskScenario scenario)
{
    Fixture fixture(extent);
    const double centerX = double(extent.width) * 0.5;
    const double centerY = double(extent.height) * 0.5;
    if (scenario != MaskScenario::Inactive) {
        const auto selection = scenario == MaskScenario::Selected
            ? SelectionMask::filled(extent, 255)
            : scenario == MaskScenario::Empty ? SelectionMask::filled(extent, 0)
            : SelectionMask::rectangle(extent, {int(centerX), int(centerY) - 700, 700, 1400}, 128);
        require(fixture.document.setSelection(selection), "Unable to set brush selection");
    }
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = size;
    settings.spacingPercent = 10;
    settings.hardness = 0.8;
    settings.opacity = 0.8;
    settings.flow = 0.6;
    settings.foreground = {220, 85, 35, 255};
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    settings.smoothing = BrushSmoothingMode::None;
    settings.tip.rotationMode = BrushTipRotationMode::Fixed;
    History history;
    const auto originalRevision = fixture.surface->revision();
    const auto start = Clock::now();
    BasicPixelBrushStroke stroke(fixture.document, fixture.layer, settings);
    double maximumInputMs = measure([&] {
        require(stroke.begin(sample({centerX - 128, centerY}, 0)), "Brush begin failed");
    });
    constexpr std::uint64_t eventCount = 33;
    for (std::uint64_t index = 1; index < eventCount - 1; ++index) {
        const double progress = double(index) / double(eventCount - 1);
        const auto elapsed = measure([&] {
            require(stroke.append(sample({centerX - 128 + 256 * progress, centerY}, index * 5000)),
                "Brush append failed");
        });
        maximumInputMs = std::max(maximumInputMs, elapsed);
    }
    RasterEditCommitResult result {};
    const auto endMs = measure([&] {
        result = stroke.end(sample({centerX + 128, centerY}, (eventCount - 1) * 5000), history);
    });
    const auto totalMs = milliseconds(Clock::now() - start);
    const bool empty = scenario == MaskScenario::Empty;
    require(result == (empty ? RasterEditCommitResult::NoChanges : RasterEditCommitResult::Committed),
        "Unexpected selected brush result");
    const auto dirty = fixture.surface->dirtySince(originalRevision);
    require(!dirty.fullRefresh, "Brush edit required a full surface upload");
    std::uint64_t dirtyBytes = 0;
    for (const auto region : dirty.regions)
        dirtyBytes += std::uint64_t(region.width) * std::uint64_t(region.height) * 4;
    const auto& statistics = stroke.stats();
    if (empty) {
        require(statistics.retainedStrokeTiles == 0, "Active-empty stroke retained raster tiles");
        require(dirty.empty() && !history.canUndo(), "Active-empty stroke mutated content/history");
    }
    const auto committedRevision = fixture.surface->revision();
    double undoMs = 0;
    if (!empty) {
        undoMs = measure([&] { require(history.undo(fixture.document), "Selected brush undo failed"); });
        require(!fixture.surface->dirtySince(committedRevision).fullRefresh,
            "Selected brush undo required a full upload");
    }
    std::cout << std::left << std::setw(12) << dimensions(extent)
              << std::right << std::setw(5) << int(size) << ' '
              << std::left << std::setw(14) << scenarioName(scenario)
              << std::right << std::setw(11) << totalMs << std::setw(11) << endMs
              << std::setw(11) << undoMs << std::setw(12) << maximumInputMs
              << std::setw(8) << statistics.retainedStrokeTiles
              << std::setw(12) << double(dirtyBytes) / 1024.0
              << std::setw(12) << double(history.memoryUsed()) / 1024.0 << '\n';
}

std::uint64_t maskFingerprint(const SelectionMask& mask)
{
    // Untimed, exhaustive immutability check; no baseline R8 copy is retained.
    std::uint64_t hash = 14695981039346656037ULL;
    const auto extent = mask.extent();
    for (std::uint32_t y = 0; y < extent.height; ++y)
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            hash ^= mask.coverageAtDocumentPixel(int(x), int(y));
            hash *= 1099511628211ULL;
        }
    return hash;
}

void requireMaskDimensions(const SelectionState& mask, Extent2u extent)
{
    require(mask && mask->extent() == extent, "Manipulation changed document dimensions");
    const auto bounds = mask->bounds();
    require(bounds.empty() || (bounds.x >= 0 && bounds.y >= 0
                && bounds.right() <= int(extent.width) && bounds.bottom() <= int(extent.height)),
        "Manipulation produced bounds outside the document");
}

void benchmarkManipulations(Extent2u extent, bool mostlyFull)
{
    const int centerX = int(extent.width / 2), centerY = int(extent.height / 2);
    const auto outer = mostlyFull ? SelectionMask::filled(extent, 255)
        : SelectionMask::rectangle(extent, {centerX - 700, centerY - 500, 1400, 1000});
    const auto hole = SelectionMask::rectangle(extent, {centerX - 250, centerY - 200, 500, 400});
    const auto source = outer->combined(*hole, SelectionOperation::Subtract);
    const auto revision = source->revision();
    const auto bounds = source->bounds();
    const Vec2d pivot {bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5};
    const auto fingerprint = maskFingerprint(*source);
    const auto baselineEdges = source->boundaryEdges().size(); // Warm immutable source boundary cache.
    const auto memory = source->memoryCost();
    const auto scenario = mostlyFull ? "mostly-full/hole" : "sparse/hole";
    std::cout << '\n' << dimensions(extent) << ' ' << scenario
              << "; source " << double(memory) / 1024.0 << " KiB, " << baselineEdges << " edges\n";

    double previewTotal = 0, previewMaximum = 0;
    std::size_t previewEdges = 0;
    for (int index = 0; index < 100; ++index) {
        // Deliberately clip both sides and return, always against the same mask.
        const int dx = (index % 25 - 12) * int(extent.width) / 20;
        const int dy = (index / 25 - 2) * int(extent.height) / 4;
        const double elapsed = measure([&] {
            previewEdges += translatedSelectionPreviewEdges(source, dx, dy).size();
        });
        previewTotal += elapsed;
        previewMaximum = std::max(previewMaximum, elapsed);
    }
    require(previewEdges > 0, "Translated previews have no visible boundaries");
    std::cout << "Translated preview / 100 deltas: avg " << previewTotal / 100.0
              << " ms, max " << previewMaximum << " ms; source "
              << double(source->memoryCost()) / 1024.0 << " KiB (no replacement mask)\n"
              << "Operation                 Mask ms Boundary ms   Mask KiB    Edges\n";
    const auto report = [&](std::string_view operation, auto&& callback) {
        SelectionState result;
        const double operationMs = measure([&] { result = callback(); });
        requireMaskDimensions(result, extent);
        std::size_t edges = 0;
        const double boundaryMs = measure([&] { edges = result->boundaryEdges().size(); });
        std::cout << std::left << std::setw(24) << operation << std::right
                  << std::setw(9) << operationMs << std::setw(12) << boundaryMs
                  << std::setw(11) << double(result->memoryCost()) / 1024.0
                  << std::setw(9) << edges << '\n';
    };
    report("Translate +137,-93", [&] { return source->translated(137, -93); });
    report("Grow +10,+10", [&] { return source->adjusted(10, 10); });
    report("Shrink -10,-10", [&] { return source->adjusted(-10, -10); });
    report("Mixed +10,-10", [&] { return source->adjusted(10, -10); });
    report("Mixed -10,+10", [&] { return source->adjusted(-10, 10); });
    for (const int degrees : {1, 17, 90})
        report("Rotate " + std::to_string(degrees), [&] { return source->rotated(degrees, pivot); });

    double stepTotal = 0, stepMaximum = 0;
    std::size_t stepEdges = 0, maximumStepMemory = 0;
    for (int degrees = 1; degrees <= 5; ++degrees) {
        SelectionState result;
        const double elapsed = measure([&] {
            // Accumulated angle from the fixed baseline, never the previous preview.
            result = source->rotated(degrees, pivot);
            stepEdges += result->boundaryEdges().size();
        });
        requireMaskDimensions(result, extent);
        stepTotal += elapsed;
        stepMaximum = std::max(stepMaximum, elapsed);
        maximumStepMemory = std::max(maximumStepMemory, result->memoryCost());
    }
    require(stepEdges > 0, "Rotation step previews have no boundary");
    std::cout << "Rotate baseline / 1..5 deg (including boundary): avg " << stepTotal / 5.0
              << " ms, max " << stepMaximum << " ms; peak result "
              << double(maximumStepMemory) / 1024.0 << " KiB\n";
    require(source->revision() == revision && source->bounds() == bounds
            && source->boundaryEdges().size() == baselineEdges && source->memoryCost() == memory
            && maskFingerprint(*source) == fingerprint,
        "Manipulation mutated its immutable baseline");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        bool manipulations = false, manipulationOnly = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--manipulation") manipulations = true;
            else if (argument == "--manipulation-only") manipulations = manipulationOnly = true;
            else throw std::runtime_error("Usage: imageeditor_selection_benchmarks [--manipulation|--manipulation-only]");
        }
        constexpr std::array extents {Extent2u {3840, 2160}, Extent2u {5120, 2880}};
        std::cout << std::fixed << std::setprecision(3)
                  << "Selection benchmark; milliseconds, fixture initialization excluded.\n";
        if (manipulations) {
            std::cout << "Manipulation: immutable sparse and mostly-full masks with holes; "
                         "first boundary extraction is timed separately.\n";
            for (const auto extent : extents)
                for (const bool mostlyFull : {false, true}) benchmarkManipulations(extent, mostlyFull);
        }
        if (manipulationOnly) return 0;
        std::cout
                  << "Masks: two rectangles + subtraction hole; previews are 100 deterministic moving rectangles.\n"
                  << "Document       Create ms Combine ms Boundary ms Cached avg ms Preview avg ms Preview max ms  Mask KiB\n";
        for (const auto extent : extents) benchmarkMasks(extent);
        std::cout << "\nLayer via Copy (opacity preserved separately; no source writes):\n"
                  << "Document    Source/selection       Copy ms     Undo ms    Output MP   History MiB\n";
        for (const auto extent : extents) {
            benchmarkCopy(extent, false, false);
            benchmarkCopy(extent, true, false);
            benchmarkCopy(extent, true, true);
        }
        std::cout << "\nBrush: 256px horizontal path / 33 events; selected=full255, partial=half-plane128.\n"
                  << "Dirty KiB sums incremental CPU dirty requests (overlaps possible), not GPU readback.\n"
                  << "Document     Size Selection        Total ms     End ms    Undo ms Max input ms   Tiles   Dirty KiB History KiB\n";
        for (const auto extent : extents)
            for (const auto size : {64.0, 1000.0})
                for (const auto scenario : {MaskScenario::Inactive, MaskScenario::Selected,
                         MaskScenario::Empty, MaskScenario::Partial})
                    benchmarkBrush(extent, size, scenario);
    } catch (const std::exception& error) {
        std::cerr << "Selection benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
