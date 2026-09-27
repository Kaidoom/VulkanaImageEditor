#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/CreativeBrushes.hpp"
#include "imageeditor/core/History.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace c = imageeditor::core;
using Clock = std::chrono::steady_clock;

namespace {
c::NormalizedPointerSample sample(int index)
{
    // Fast back-and-forth mouse motion; events are 25.6 document pixels apart.
    const auto leg = index / 30;
    const auto t = double(index % 30) / 30;
    return {.documentPosition = {128 + 768 * (leg % 2 ? 1 - t : t), 128 + 5.5 * index},
        .timestampMicroseconds = std::uint64_t(index) * 4000,
        .pressure = 1, .pointerType = c::PointerType::Mouse,
        .buttons = c::PointerButtonPrimary};
}

void benchmark(std::string_view name, double size, int iteration, unsigned workers)
{
    auto settings = c::proceduralBrushPreset(c::ProceduralBrushPreset::HardRound);
    if (name != "round") {
        const auto presets = c::creativeBrushPresets();
        const auto found = std::find_if(presets.begin(), presets.end(), [name](const auto& p) {
            return p.id == std::string("builtin.preset.") + std::string(name) + ".v1";
        });
        if (found == presets.end()) throw std::runtime_error("Unknown brush");
        settings = found->settings;
    }
    settings.sizePixels = size;
    settings.foreground = {187, 74, 213, 231};
    settings.smoothing = c::BrushSmoothingMode::None;
    c::Document document(c::CanvasSpec{.extent = {1024, 1024}});
    auto surface = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{1024, 1024}, c::Rgba8{31, 56, 78, 255});
    auto layer = c::Layer::raster("Benchmark", surface);
    const auto id = layer.id;
    if (!document.insertLayer(0, std::move(layer))) throw std::runtime_error("Layer insertion failed");
    c::History history;
    c::BasicPixelBrushStroke stroke(document, id, settings, std::make_unique<c::BasicPixelBrushEngine>(),
        {}, {}, nullptr, {}, {.workers = workers});
    std::vector<double> inputs;
    const auto start = Clock::now();
    if (!stroke.begin(sample(0))) throw std::runtime_error("Begin failed");
    for (int i = 1; i < 120; ++i) {
        const auto begin = Clock::now();
        if (!stroke.append(sample(i))) throw std::runtime_error("Append failed");
        inputs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    }
    const auto finish = Clock::now();
    if (stroke.end(sample(120), history) != c::RasterEditCommitResult::Committed)
        throw std::runtime_error("Commit failed");
    const auto end = Clock::now();
    std::sort(inputs.begin(), inputs.end());
    std::vector<std::byte> pixels(1024 * 1024 * 4);
    surface->copyRgba8({0, 0, 1024, 1024}, pixels, 1024 * 4);
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto pixel : pixels) {
        hash ^= std::to_integer<std::uint8_t>(pixel);
        hash *= 1099511628211ULL;
    }
    const auto& stats = stroke.stats();
    std::cout << name << ',' << size << ',' << iteration << ','
        << std::chrono::duration<double, std::milli>(end - start).count() << ','
        << inputs[inputs.size() / 2] << ',' << inputs[inputs.size() * 95 / 100] << ',' << inputs.back() << ','
        << std::chrono::duration<double, std::milli>(end - finish).count() << ','
        << stats.emittedDabs << ',' << stats.evaluatedPixels << ',' << stats.uploadedRegionBytes << ','
        << history.memoryUsed() << ',' << std::hex << hash << std::dec << ','
        << stats.parallelDabs << ',' << stats.coverageScratchBytes << '\n';
}
}

int main(int argc, char** argv)
{
    // Keep one-time asset generation out of steady-state painting measurements.
    (void)c::builtinBrushAssetResolver().cacheStats();
    std::cout << "brush,size,iteration,total_ms,input_median_ms,input_p95_ms,input_max_ms,finish_ms,"
                 "dabs,evaluated_pixels,upload_bytes,history_bytes,rgba_hash,parallel_dabs,coverage_scratch_bytes\n" << std::fixed << std::setprecision(3);
    for (const auto name : {"round", "chalk", "charcoal", "acrylic"}) {
        if (argc > 1 && std::string_view(argv[1]) != name) continue;
        for (const auto size : {200., 500.})
            for (int i = 0; i < 3; ++i) benchmark(name, size, i, argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 0U);
    }
}
