#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/CreativeBrushes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace imageeditor::core;
namespace {
void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
constexpr Extent2u extent {384, 384};
// A surface need not be thread-safe: even parallel strokes must read/write it
// only on the originating thread, including journal capture and undo/redo.
class OwnerSurface final : public RasterSurface {
public:
    OwnerSurface() : storage(::extent, Rgba8{37, 65, 113, 173}) {}
    void owner() const { check(std::this_thread::get_id() == thread, "surface accessed by brush worker"); }
    SurfaceId id() const noexcept override { return storage.id(); }
    Extent2u extent() const noexcept override { return storage.extent(); }
    Revision revision() const noexcept override { return storage.revision(); }
    DirtySet dirtySince(Revision r) const override { owner(); return storage.dirtySince(r); }
    void copyRgba8(RectI r, std::span<std::byte> bytes, std::size_t stride) const override
    { owner(); storage.copyRgba8(r, bytes, stride); }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch> p) override
    { owner(); return storage.replaceRgba8Batch(p); }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch> p) override
    { owner(); return storage.swapRgba8Batch(p); }
    ContiguousRasterSurface storage;
    const std::thread::id thread = std::this_thread::get_id();
};
std::vector<std::byte> bytes(const RasterSurface& surface)
{
    std::vector<std::byte> result(extent.width * extent.height * 4);
    surface.copyRgba8({0, 0, int(extent.width), int(extent.height)}, result, extent.width * 4);
    return result;
}
struct Fixture {
    Document document {CanvasSpec{.extent = extent}};
    std::shared_ptr<OwnerSurface> surface = std::make_shared<OwnerSurface>();
    LayerId id;
    History history;
    explicit Fixture(bool transformed = false) {
        auto layer = Layer::raster("Parallel brush", surface);
        id = layer.id;
        check(document.insertLayer(0, std::move(layer)), "insert layer");
        if (transformed) {
            check(document.setLayerTransform(id, {.m00 = -.85, .m01 = .2, .m02 = 310,
                .m10 = .12, .m11 = .82, .m12 = 15, .m20 = .0002, .m21 = .0001}), "projective flipped target");
            LayerCrop crop {17.25, 23.5, 310.5, 300.75};
            crop.corners = {31, 12, 42, 19};
            check(document.setLayerCrop(id, crop), "chamfered crop");
            std::vector<std::uint8_t> mask(extent.width * extent.height);
            for (unsigned y = 0; y < extent.height; ++y) for (unsigned x = 0; x < extent.width; ++x)
                mask[y * extent.width + x] = std::uint8_t((x * 13 + y * 7) % 256);
            check(document.setSelection(SelectionMask::fromR8(extent, mask, extent.width)), "soft selection");
        }
    }
};
BrushSettings preset(std::string_view name)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    if (name != "round") {
        bool found = false;
        for (const auto& p : creativeBrushPresets())
            if (p.displayName == name) { settings = p.settings; found = true; }
        check(found, "unknown preset in test");
    }
    settings.sizePixels = 220;
    settings.foreground = {213, 62, 191, 197};
    settings.opacity = .73;
    settings.smoothing = BrushSmoothingMode::None;
    return settings;
}
NormalizedPointerSample sample(int i)
{
    return {.documentPosition = {90.25 + 44 * (i % 4), 110.75 + 16 * i},
        .timestampMicroseconds = std::uint64_t(i) * 4000, .pressure = .8 + (i % 3) * .1,
        .rotationDegrees = 11. * i, .pointerType = PointerType::Pen, .buttons = PointerButtonPrimary};
}
struct Result {
    std::vector<std::vector<std::byte>> previews;
    BrushStrokeStats stats;
    std::size_t historyBytes;
};
Result render(std::string_view name, unsigned workers, bool transformed, BrushCompositeMode mode)
{
    Fixture f(transformed);
    const auto original = bytes(*f.surface);
    const auto settings = preset(name);
    BasicPixelBrushStroke stroke(f.document, f.id, settings, mode,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, nullptr, {}, {}, {}, false, {}, {.workers = workers});
    check(stroke.begin(sample(0)), "begin");
    Result result;
    result.previews.push_back(bytes(*f.surface));
    for (int i = 1; i < 6; ++i) {
        check(stroke.append(sample(i)), "append");
        result.previews.push_back(bytes(*f.surface));
    }
    check(stroke.end(sample(6), f.history) == RasterEditCommitResult::Committed, "commit");
    result.previews.push_back(bytes(*f.surface));
    result.stats = stroke.stats();
    result.historyBytes = f.history.memoryUsed();
    check(f.history.undoDepth() == 1 && f.history.undo(f.document), "single atomic undo");
    check(bytes(*f.surface) == original, "undo exactly restores source including alpha");
    check(f.history.redo(f.document) && bytes(*f.surface) == result.previews.back(), "exact redo");
    check(f.history.undo(f.document), "prepare redo branch");
    {
        BasicPixelBrushStroke cancelled(f.document, f.id, settings);
        check(cancelled.begin(sample(0)) && cancelled.append(sample(1)), "cancelled preview");
        cancelled.cancel();
        check(bytes(*f.surface) == original && f.history.canRedo(), "cancel preserves bytes and redo");
    }
    auto noopSettings = settings;
    noopSettings.opacity = 0;
    BasicPixelBrushStroke noop(f.document, f.id, noopSettings);
    check(noop.begin(sample(0)) && noop.end(sample(1), f.history) == RasterEditCommitResult::NoChanges,
        "zero opacity remains no-op");
    check(bytes(*f.surface) == original && f.history.canRedo(), "no-op preserves redo and source");
    return result;
}
void equivalence()
{
    for (const auto name : {"Chalk", "Charcoal", "Acrylic", "Bristles", "round"})
        for (const bool transformed : {false, true}) for (const auto mode : {BrushCompositeMode::Paint, BrushCompositeMode::Erase}) {
            const auto serial = render(name, 1, transformed, mode);
            for (const unsigned workers : {2U, 4U, 8U}) {
                const auto parallel = render(name, workers, transformed, mode);
                check(serial.previews == parallel.previews, "all preview/commit bytes must match serial exactly");
                check(serial.historyBytes == parallel.historyBytes, "history accounting unchanged");
                check(serial.stats.emittedDabs == parallel.stats.emittedDabs
                    && serial.stats.evaluatedPixels == parallel.stats.evaluatedPixels
                    && serial.stats.changedPixels == parallel.stats.changedPixels
                    && serial.stats.uploadedRegionBytes == parallel.stats.uploadedRegionBytes
                    && serial.stats.retainedStrokeTiles == parallel.stats.retainedStrokeTiles,
                    "same dab order, pixel work, tile retention and upload footprint");
                if (boundedParallelWorkerCount() > 1 && name == std::string_view("Chalk"))
                    check(parallel.stats.parallelDabs > 0, "large Chalk fixture actually exercises MT");
                check(parallel.stats.coverageScratchBytes <= kMaximumBrushDabCandidatePixels * sizeof(double), "bounded scratch");
            }
        }
}
class OwnerGrain final : public IBrushGrain {
public:
    void prepareDab(const BrushDab&, double) noexcept override {}
    double modulation(Vec2d) const noexcept override {
        if (std::this_thread::get_id() != owner) wrongThread = true;
        return .5;
    }
    const std::thread::id owner = std::this_thread::get_id();
    mutable std::atomic_bool wrongThread {false};
};
class OwnerSelection final : public SelectionMaskInput {
public:
    Revision revision() const noexcept override { return 1; }
    std::uint8_t coverageAtDocumentPixel(int, int) const noexcept override {
        if (std::this_thread::get_id() != owner) wrongThread = true;
        return 127;
    }
    const std::thread::id owner = std::this_thread::get_id();
    mutable std::atomic_bool wrongThread {false};
};
void fallbacks()
{
    Fixture f;
    auto custom = std::make_unique<OwnerGrain>();
    const auto* grain = custom.get();
    BasicPixelBrushStroke stroke(f.document, f.id, preset("Chalk"), std::make_unique<BasicPixelBrushEngine>(), {}, std::move(custom));
    check(stroke.begin(sample(0)) && stroke.append(sample(1)), "custom grain stroke");
    check(!grain->wrongThread && stroke.stats().parallelDabs == 0, "custom grain stays serial unless explicitly opted in");
    stroke.cancel();
    OwnerSelection selection;
    BasicPixelBrushStroke masked(f.document, f.id, preset("Chalk"), std::make_unique<BasicPixelBrushEngine>(), {}, {}, nullptr,
        {.selectionMask = &selection});
    check(masked.begin(sample(0)), "borrowed selection stroke");
    check(!selection.wrongThread && masked.stats().parallelDabs == 0, "borrowed selection stays on owner");
    masked.cancel();
    auto tiny = preset("Chalk"); tiny.sizePixels = 12;
    BasicPixelBrushStroke small(f.document, f.id, tiny);
    check(small.begin(sample(0)) && small.stats().parallelDabs == 0 && small.stats().coverageScratchBytes == 0, "small dabs avoid MT overhead");
    small.cancel();
    check(f.document.setSelection(SelectionMask::filled(extent, 0)), "empty selection");
    BasicPixelBrushStroke empty(f.document, f.id, preset("Chalk"));
    check(empty.begin(sample(0)) && empty.stats().retainedStrokeTiles == 0 && empty.stats().coverageScratchBytes == 0,
        "empty selection allocates no brush scratch/tiles");
    empty.cancel();

    if (boundedParallelWorkerCount() == 1) return;
    std::latch acquired(1), release(1);
    auto owner = std::async(std::launch::async, [&] {
        return boundedParallel(2, [&](unsigned rank, unsigned) {
            if (!rank) acquired.count_down();
            release.wait();
        });
    });
    acquired.wait();
    // This separate caller creates/owns its fixture; its surface ownership is
    // still checked, and it must finish before the busy executor is released.
    auto interactive = std::async(std::launch::async, [] {
        return render("Chalk", 8, false, BrushCompositeMode::Paint);
    });
    const auto ready = interactive.wait_for(std::chrono::seconds(5));
    release.count_down();
    check(owner.get(), "background batch completes");
    const auto fallback = interactive.get();
    check(ready == std::future_status::ready && fallback.stats.parallelDabs == 0, "busy pool immediately falls back to serial");
    check(fallback.previews == render("Chalk", 1, false, BrushCompositeMode::Paint).previews, "busy fallback remains pixel-exact");
}
}
int main()
{
    try { equivalence(); fallbacks(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Parallel brush: exact previews, history, owner-thread surfaces and safe fallbacks passed\n";
}
