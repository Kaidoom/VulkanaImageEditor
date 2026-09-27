#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) { std::cerr << "FAIL " << line << ": " << expression << '\n'; ++failures; }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// Decorates the storage abstraction, checking fill never depends on concrete
// contiguous pixels or reads a complete large RGBA frame before doing work.
class ObservedSurface final : public RasterSurface {
public:
    explicit ObservedSurface(Extent2u extent, Rgba8 color = {}) : pixels(extent, color) {}
    ContiguousRasterSurface pixels;
    mutable std::size_t reads {0}, maximumReadBytes {0};
    mutable bool failNextRead {false};
    mutable int readsBeforeFailure {-1};
    std::size_t writes {0};
    SurfaceId id() const noexcept override { return pixels.id(); }
    Extent2u extent() const noexcept override { return pixels.extent(); }
    Revision revision() const noexcept override { return pixels.revision(); }
    DirtySet dirtySince(Revision revision) const override { return pixels.dirtySince(revision); }
    void copyRgba8(RectI region, std::span<std::byte> bytes, std::size_t stride) const override
    {
        if (failNextRead) { failNextRead = false; throw std::runtime_error("injected read failure"); }
        if (readsBeforeFailure == 0) { readsBeforeFailure = -1; throw std::runtime_error("injected later read failure"); }
        if (readsBeforeFailure > 0) --readsBeforeFailure;
        ++reads;
        maximumReadBytes = std::max(maximumReadBytes, std::size_t(region.width) * std::size_t(region.height) * 4);
        pixels.copyRgba8(region, bytes, stride);
    }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch> patches) override
    { ++writes; return pixels.replaceRgba8Batch(patches); }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch> patches) override
    { return pixels.swapRgba8Batch(patches); }
};

struct Fixture {
    Document document;
    std::shared_ptr<ObservedSurface> surface;
    LayerId layer {0};
    History history;
    explicit Fixture(Extent2u extent, Rgba8 color = {})
        : document({extent}), surface(std::make_shared<ObservedSurface>(extent, color))
    {
        auto item = Layer::raster("Fill target", surface); layer = item.id;
        CHECK(document.insertLayer(0, std::move(item)));
    }
};

std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto size = surface.extent();
    std::vector<std::byte> result(std::size_t(size.width) * size.height * 4);
    surface.copyRgba8({0, 0, int(size.width), int(size.height)}, result, std::size_t(size.width) * 4);
    return result;
}
Rgba8 pixel(const RasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> result {};
    surface.copyRgba8({x, y, 1, 1}, result, 4);
    return {std::to_integer<std::uint8_t>(result[0]), std::to_integer<std::uint8_t>(result[1]),
        std::to_integer<std::uint8_t>(result[2]), std::to_integer<std::uint8_t>(result[3])};
}
void setPixel(RasterSurface& surface, int x, int y, Rgba8 color)
{
    const std::array data {std::byte(color.red), std::byte(color.green), std::byte(color.blue), std::byte(color.alpha)};
    (void)surface.replaceRgba8({x, y, 1, 1}, data, 4);
}
FillState finish(FillOperation& fill, std::size_t budget = 4096)
{
    for (std::size_t n = 0; n < 200000; ++n) {
        const auto state = fill.step(budget);
        if (state != FillState::Applying && state != FillState::Discovering) return state;
    }
    CHECK(false && "fill did not terminate"); return fill.state();
}
void execute(Fixture& f, FillOptions options, RasterEditCommitResult expected = RasterEditCommitResult::Committed)
{
    FillOperation operation(f.document, f.layer, options);
    CHECK(finish(operation) == FillState::Ready);
    CHECK(operation.commit(f.history) == expected);
    CHECK(operation.state() == FillState::Finished);
}

// Independent scalar reference, explicitly including the RGBA8 candidate
// boundary before the transaction's once-only mask interpolation.
double decode(std::uint8_t value)
{
    const auto x = double(value) / 255;
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}
std::uint8_t encode(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    const auto x = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return std::uint8_t(std::clamp(std::lround(x * 255), 0L, 255L));
}
std::uint8_t alphaByte(double value) { return std::uint8_t(std::clamp(std::lround(value * 255), 0L, 255L)); }
Rgba8 reference(Rgba8 before, Rgba8 color, double opacity, std::uint8_t mask = 255)
{
    if (!mask || !color.alpha || opacity <= 0) return before;
    const double sourceAlpha = double(color.alpha) / 255 * std::clamp(opacity, 0.0, 1.0);
    const double oldAlpha = double(before.alpha) / 255;
    const double newAlpha = sourceAlpha + oldAlpha * (1 - sourceAlpha);
    const std::array old {before.red, before.green, before.blue};
    const std::array source {color.red, color.green, color.blue};
    std::array<std::uint8_t, 3> candidate {};
    for (std::size_t c = 0; c < 3; ++c)
        candidate[c] = encode((decode(source[c]) * sourceAlpha + decode(old[c]) * oldAlpha * (1 - sourceAlpha)) / newAlpha);
    auto result = Rgba8 {candidate[0], candidate[1], candidate[2], alphaByte(newAlpha)};
    if (mask == 255) return result;
    const double weight = double(mask) / 255;
    const double storedAlpha = double(result.alpha) / 255;
    const double blendedAlpha = oldAlpha * (1 - weight) + storedAlpha * weight;
    for (std::size_t c = 0; c < 3; ++c)
        candidate[c] = blendedAlpha > 0
            ? encode((decode(old[c]) * oldAlpha * (1 - weight) + decode(candidate[c]) * storedAlpha * weight) / blendedAlpha)
            : old[c];
    return {candidate[0], candidate[1], candidate[2], alphaByte(blendedAlpha)};
}

void solidSelectionAndCompositing()
{
    Fixture f({139, 79});
    for (int y = 0; y < 79; ++y) for (int x = 0; x < 139; ++x)
        setPixel(*f.surface, x, y, {std::uint8_t(x * 37), std::uint8_t(y * 43), std::uint8_t(x + y), std::uint8_t((x + y) % 256)});
    std::vector<std::uint8_t> coverage(139 * 79);
    for (int y = 0; y < 79; ++y) for (int x = 0; x < 139; ++x) {
        if ((x < 42 && y > 4 && y < 65) || (x > 85 && y > 8 && y < 72))
            coverage[std::size_t(y * 139 + x)] = (x > 10 && x < 28 && y > 20 && y < 40)
                ? 0 : std::uint8_t(1 + (x * 17 + y * 31) % 255);
    }
    CHECK(f.document.setSelection(SelectionMask::fromR8({139, 79}, coverage, 139)));
    const auto selection = f.document.selection();
    const auto original = bytes(*f.surface);
    const Rgba8 color {220, 35, 130, 127};
    const auto revision = f.surface->revision();
    CHECK(f.document.setLayerOpacity(f.layer, 0.27F));
    execute(f, {.color = color, .opacity = 0.53});
    CHECK(f.document.selection() == selection);
    CHECK(f.document.layer(f.layer)->opacity == 0.27F);
    CHECK(f.history.undoDepth() == 1);
    const auto dirty = f.surface->dirtySince(revision);
    CHECK(!dirty.fullRefresh && !dirty.empty());
    std::vector<bool> invalidated(coverage.size());
    for (const auto region : dirty.regions)
        for (int y = region.y; y < region.bottom(); ++y) for (int x = region.x; x < region.right(); ++x)
            invalidated[std::size_t(y * 139 + x)] = true;
    for (int y = 0; y < 79; ++y) for (int x = 0; x < 139; ++x) {
        const auto i = std::size_t(y * 139 + x), p = i * 4;
        const Rgba8 before {std::to_integer<std::uint8_t>(original[p]), std::to_integer<std::uint8_t>(original[p + 1]),
            std::to_integer<std::uint8_t>(original[p + 2]), std::to_integer<std::uint8_t>(original[p + 3])};
        const auto expected = reference(before, color, 0.53, coverage[i]);
        CHECK(pixel(*f.surface, x, y) == expected);
        CHECK(invalidated[i] == (expected != before));
    }
    const auto filled = bytes(*f.surface);
    CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == original);
    CHECK(f.history.redo(f.document)); CHECK(bytes(*f.surface) == filled);
}

void transparencyAndNoops()
{
    for (const auto before : {Rgba8 {211, 14, 170, 0}, Rgba8 {10, 80, 190, 128}, Rgba8 {30, 40, 50, 255}})
        for (const auto mask : {std::uint8_t(1), std::uint8_t(64), std::uint8_t(128), std::uint8_t(255)}) {
            Fixture f({1, 1}, before);
            CHECK(f.document.setSelection(SelectionMask::filled({1, 1}, mask)));
            const Rgba8 color {128, 210, 15, 128};
            const auto expected = reference(before, color, 0.5, mask);
            execute(f, {.color = color, .opacity = 0.5}, expected == before
                ? RasterEditCommitResult::NoChanges : RasterEditCommitResult::Committed);
            CHECK(pixel(*f.surface, 0, 0) == expected);
        }
    Fixture f({130, 90}, {10, 20, 30, 255});
    execute(f, {.color = {230, 80, 20, 255}});
    CHECK(f.history.undo(f.document));
    const auto revision = f.surface->revision();
    for (const auto options : {FillOptions {.color = {10, 20, 30, 255}},
             FillOptions {.color = {200, 200, 200, 0}}, FillOptions {.color = {200, 200, 200, 255}, .opacity = 0.0},
             FillOptions {.color = {200, 200, 200, 255}, .opacity = -2.0}}) {
        FillOperation operation(f.document, f.layer, options);
        CHECK(finish(operation) == FillState::Ready);
        CHECK(operation.commit(f.history) == RasterEditCommitResult::NoChanges);
        CHECK(operation.stats().journalBytes == 0);
        CHECK(operation.stats().writeBatches == 0);
        CHECK(f.surface->revision() == revision);
        CHECK(f.history.undoDepth() == 0 && f.history.redoDepth() == 1);
    }
    CHECK(f.document.setSelection(SelectionMask::filled({130, 90}, 0)));
    FillOperation empty(f.document, f.layer, {.color = {255, 255, 255, 255}});
    CHECK(empty.state() == FillState::Ready);
    CHECK(empty.commit(f.history) == RasterEditCommitResult::NoChanges);
    CHECK(empty.stats().evaluatedPixels == 0 && empty.stats().journalBytes == 0);
    CHECK(f.history.redoDepth() == 1 && f.surface->revision() == revision);

    Fixture invisible({9, 7}, {210, 80, 33, 0});
    const auto invisibleRevision = invisible.surface->revision();
    execute(invisible, {.color = {8, 120, 250, 1}, .opacity = 0.01}, RasterEditCommitResult::NoChanges);
    CHECK(pixel(*invisible.surface, 4, 3) == Rgba8({210, 80, 33, 0}));
    CHECK(invisible.surface->revision() == invisibleRevision);
    CHECK(!invisible.history.canUndo() && !invisible.history.canRedo());
}

bool scalarMatch(Rgba8 a, Rgba8 b, int tolerance)
{
    const std::array source {a.red, a.green, a.blue};
    const std::array target {b.red, b.green, b.blue};
    if (std::abs(int(a.alpha) - int(b.alpha)) > tolerance) return false;
    for (std::size_t c = 0; c < 3; ++c)
        if (std::abs((int(source[c]) * a.alpha + 127) / 255 - (int(target[c]) * b.alpha + 127) / 255) > tolerance) return false;
    return true;
}
std::vector<bool> referenceRegion(Extent2u extent, int seedX, int seedY, int tolerance,
    const std::vector<std::optional<Rgba8>>& source)
{
    std::vector<bool> visited(source.size()), result(source.size());
    if (seedX < 0 || seedY < 0 || seedX >= int(extent.width) || seedY >= int(extent.height)) return result;
    const auto seed = source[std::size_t(seedY) * extent.width + std::size_t(seedX)];
    if (!seed) return result;
    std::deque<std::array<int, 2>> pending {{{seedX, seedY}}};
    while (!pending.empty()) {
        const auto p = pending.front(); pending.pop_front();
        const auto index = std::size_t(p[1]) * extent.width + std::size_t(p[0]);
        if (visited[index]) continue;
        visited[index] = true;
        if (!source[index] || !scalarMatch(*seed, *source[index], tolerance)) continue;
        result[index] = true;
        for (const auto delta : {std::array {-1, 0}, std::array {1, 0}, std::array {0, -1}, std::array {0, 1}}) {
            const auto x = p[0] + delta[0], y = p[1] + delta[1];
            if (x >= 0 && y >= 0 && x < int(extent.width) && y < int(extent.height)) pending.push_back({x, y});
        }
    }
    return result;
}
void compareRegion(Extent2u extent, int seedX, int seedY, std::uint8_t tolerance,
    const std::vector<std::optional<Rgba8>>& source, std::size_t budget)
{
    const auto expected = referenceRegion(extent, seedX, seedY, tolerance, source);
    std::size_t reads = 0;
    RegionFinder region(extent, seedX, seedY, tolerance, [&](int x, int y) {
        ++reads; return source[std::size_t(y) * extent.width + std::size_t(x)];
    });
    std::size_t steps = 0;
    while (!region.step(budget) && ++steps < source.size() * 10 + 100) {}
    CHECK(steps < source.size() * 10 + 100);
    CHECK(region.pixelCount() == std::size_t(std::count(expected.begin(), expected.end(), true)));
    CHECK(reads <= source.size() + 1);
    RectI expectedBounds;
    for (int y = 0; y < int(extent.height); ++y) for (int x = 0; x < int(extent.width); ++x)
        if (expected[std::size_t(y) * extent.width + std::size_t(x)]) expectedBounds = expectedBounds.united({x, y, 1, 1});
    CHECK(region.bounds() == expectedBounds);
    for (int y = 0; y < int(extent.height); ++y) for (int x = 0; x < int(extent.width); ++x)
        CHECK(region.contains(x, y) == expected[std::size_t(y) * extent.width + std::size_t(x)]);
    CHECK(!region.contains(-1, 0) && !region.contains(int(extent.width), 0));
}
void regionDiscovery()
{
    CHECK(RegionFinder::matches({100, 50, 90, 255}, {110, 40, 90, 255}, 10));
    CHECK(!RegionFinder::matches({100, 50, 90, 255}, {111, 40, 90, 255}, 10));
    CHECK(!RegionFinder::matches({100, 50, 90, 255}, {110, 39, 90, 255}, 10));
    CHECK(RegionFinder::matches({255, 120, 30, 0}, {0, 255, 180, 0}, 0));
    CHECK(!RegionFinder::matches({255, 120, 30, 0}, {0, 255, 180, 1}, 0));
    std::vector<std::optional<Rgba8>> gradient;
    for (int x = 0; x < 64; ++x) gradient.push_back(Rgba8 {std::uint8_t(x * 4), 0, 0, 255});
    compareRegion({64, 1}, 0, 0, 16, gradient, 1);
    RegionFinder fixed({64, 1}, 0, 0, 16, [&](int x, int) { return gradient[std::size_t(x)]; });
    while (!fixed.step(1)) {}
    CHECK(fixed.pixelCount() == 5);
    std::vector<std::optional<Rgba8>> diagonal(100, Rgba8 {200, 200, 200, 255});
    for (int i = 0; i < 10; ++i) diagonal[std::size_t(i * 11)] = Rgba8 {0, 0, 0, 255};
    compareRegion({10, 10}, 0, 0, 0, diagonal, 2);
    std::mt19937 random(0xFB172);
    for (int run = 0; run < 35; ++run) {
        const Extent2u size {std::uint32_t(9 + run % 25), std::uint32_t(11 + run % 19)};
        std::vector<std::optional<Rgba8>> source(std::size_t(size.width) * size.height);
        for (auto& item : source) {
            const auto value = random() % 15;
            if (value < 2) continue;
            item = value < 11 ? Rgba8 {45, 67, 81, 255} : Rgba8 {200, 180, 220, 255};
        }
        for (const auto budget : {std::size_t(1), std::size_t(19), std::size_t(65536)})
            compareRegion(size, run % int(size.width), (run * 7) % int(size.height), 0, source, budget);
    }
    compareRegion({64, 1}, -1, 0, 0, gradient, 1);
    compareRegion({0, 0}, 0, 0, 0, {}, 1);
    bool threw = false;
    try { RegionFinder excessive({8001, 8000}, 0, 0, 0, [](int, int) { return std::optional(Rgba8 {}); }); }
    catch (const std::length_error&) { threw = true; }
    CHECK(threw);
}

void contiguousSelectionAndTransparentSeed()
{
    Fixture f({140, 70}, {1, 2, 3, 0});
    for (int y = 0; y < 70; ++y) {
        setPixel(*f.surface, 66, y, {30, 40, 50, 255});
        for (int x = 0; x < 66; ++x)
            setPixel(*f.surface, x, y, {std::uint8_t(x * 17), std::uint8_t(y * 23), 255, 0});
    }
    auto selection = SelectionMask::rectangle({140, 70}, {3, 5, 125, 55}, 128);
    selection = selection->combined(*SelectionMask::rectangle({140, 70}, {20, 10, 8, 15}), SelectionOperation::Subtract);
    CHECK(f.document.setSelection(selection));
    const auto original = bytes(*f.surface);
    FillOperation fill(f.document, f.layer, {.mode = FillMode::Contiguous, .color = {200, 100, 50, 255}, .tolerance = 0, .seed = {5.5, 6.5}});
    const auto initialRevision = f.surface->revision();
    while (fill.state() == FillState::Discovering) {
        (void)fill.step(17);
        CHECK(f.surface->revision() == initialRevision);
        CHECK(fill.stats().journalBytes == 0);
    }
    CHECK(finish(fill) == FillState::Ready);
    CHECK(fill.commit(f.history) == RasterEditCommitResult::Committed);
    for (int y = 0; y < 70; ++y) for (int x = 0; x < 140; ++x) {
        const auto shouldFill = x < 66 && selection->coverageAtDocumentPixel(x, y) > 0;
        CHECK(pixel(*f.surface, x, y).alpha == (shouldFill ? 128 : x == 66 ? 255 : 0));
    }
    CHECK(f.document.selection() == selection);
    CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == original);
    CHECK(f.history.redo(f.document)); CHECK(f.history.undoDepth() == 1);

    // A zero-coverage cut is an impassable discovery boundary, not merely a
    // post-discovery paint mask that leaks into a disconnected selected island.
    Fixture split({15, 5});
    auto mask = SelectionMask::filled({15, 5}, 255)->combined(
        *SelectionMask::rectangle({15, 5}, {7, 0, 1, 5}), SelectionOperation::Subtract);
    CHECK(split.document.setSelection(mask));
    execute(split, {.mode = FillMode::Contiguous, .color = {255, 0, 0, 255}, .tolerance = 0, .seed = {1, 1}});
    CHECK(pixel(*split.surface, 6, 2).alpha == 255);
    CHECK(pixel(*split.surface, 7, 2).alpha == 0 && pixel(*split.surface, 8, 2).alpha == 0);
}

void transformedTargetsAndCanvasClipping()
{
    const std::array maps {
        AffineTransform {.m02 = -5, .m12 = 3},
        AffineTransform {.m00 = 0, .m01 = -1, .m02 = 29, .m10 = 1, .m11 = 0, .m12 = -4},
        AffineTransform {.m00 = -1.25, .m01 = 0.15, .m02 = 31, .m10 = 0.35, .m11 = 0.8, .m12 = 1},
        AffineTransform {.m00 = 0.4, .m01 = -0.7, .m02 = 23, .m10 = 0.6, .m11 = 0.3, .m12 = 4}
    };
    for (const auto transform : maps) for (const bool useMask : {false, true}) {
        Fixture f({37, 27}, {20, 70, 100, 127});
        CHECK(f.document.setCanvas({{29, 23}}));
        CHECK(f.document.setLayerTransform(f.layer, transform));
        SelectionState mask;
        if (useMask) {
            mask = SelectionMask::rectangle({29, 23}, {1, 2, 25, 19}, 96)->combined(
                *SelectionMask::rectangle({29, 23}, {8, 7, 6, 5}), SelectionOperation::Subtract);
            CHECK(f.document.setSelection(mask));
        }
        const auto id = f.surface->id();
        const auto original = bytes(*f.surface);
        const Rgba8 color {210, 15, 70, 177};
        execute(f, {.color = color, .opacity = 0.75});
        for (int y = 0; y < 27; ++y) for (int x = 0; x < 37; ++x) {
            const auto p = transform.map({x + 0.5, y + 0.5});
            const bool inside = p.x >= 0 && p.y >= 0 && p.x < 29 && p.y < 23;
            const auto coverage = inside ? mask ? mask->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y))) : std::uint8_t(255) : std::uint8_t(0);
            CHECK(pixel(*f.surface, x, y) == reference({20, 70, 100, 127}, color, 0.75, coverage));
        }
        CHECK(f.surface->id() == id && f.surface->extent() == Extent2u({37, 27}));
        CHECK(f.document.layer(f.layer)->localToDocument == transform);
        CHECK(f.document.selection() == mask);
        const auto output = bytes(*f.surface);
        CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == original);
        CHECK(f.history.redo(f.document)); CHECK(bytes(*f.surface) == output);
    }
    Fixture flipped({8, 5});
    CHECK(flipped.document.setLayerTransform(flipped.layer, {.m00 = -1, .m02 = 8}));
    for (int y = 0; y < 5; ++y) setPixel(*flipped.surface, 3, y, {255, 255, 255, 255});
    execute(flipped, {.mode = FillMode::Contiguous, .color = {120, 80, 40, 255}, .tolerance = 0, .seed = {1.5, 2.5}});
    CHECK(pixel(*flipped.surface, 6, 2).alpha == 255);
    CHECK(pixel(*flipped.surface, 2, 2).alpha == 0);
}

void chunkCancellationFailureAndDirtyTracking()
{
    Fixture f({300, 190}, {10, 20, 30, 127});
    const auto original = bytes(*f.surface);
    execute(f, {.color = {50, 60, 70, 255}});
    CHECK(f.history.undo(f.document));
    {
        FillOperation fill(f.document, f.layer, {.color = {230, 0, 0, 255}});
        CHECK(fill.step(4096) == FillState::Applying);
        CHECK(fill.step(4096) == FillState::Applying);
        CHECK(fill.stats().journalBytes == 2 * 64 * 64 * 4);
        CHECK(fill.stats().writeBatches == 2);
        CHECK(bytes(*f.surface) != original);
        CHECK(fill.commit(f.history) == RasterEditCommitResult::TargetUnavailable);
        fill.cancel(); CHECK(fill.state() == FillState::Cancelled);
    }
    CHECK(bytes(*f.surface) == original);
    CHECK(f.history.undoDepth() == 0 && f.history.redoDepth() == 1);
    {
        FillOperation fill(f.document, f.layer, {.color = {50, 230, 0, 255}});
        CHECK(fill.step(4096) == FillState::Applying);
        f.surface->failNextRead = true;
        CHECK(fill.step(4096) == FillState::Failed);
        CHECK(!fill.error().empty());
        CHECK(bytes(*f.surface) == original);
        CHECK(f.history.undoDepth() == 0 && f.history.redoDepth() == 1);
    }
    {
        FillOperation fill(f.document, f.layer, {.color = {10, 100, 210, 255}});
        (void)fill.step(4096);
        CHECK(f.document.setSelection(SelectionMask::rectangle({300, 190}, {0, 0, 3, 3})));
        CHECK(fill.step(4096) == FillState::Failed);
        CHECK(bytes(*f.surface) == original);
        CHECK(f.document.setSelection({}));
    }
    {
        FillOperation fill(f.document, f.layer, {.color = {40, 110, 240, 255}});
        CHECK(finish(fill) == FillState::Ready);
        // One tile has already been finalized when a later read fails. The
        // transaction must still own every original snapshot for rollback.
        f.surface->readsBeforeFailure = 1;
        CHECK(fill.commit(f.history) == RasterEditCommitResult::HistoryRejected);
        CHECK(fill.state() == FillState::Failed);
        CHECK(bytes(*f.surface) == original);
        CHECK(f.history.undoDepth() == 0 && f.history.redoDepth() == 1);
    }
    // Cancellation while discovering cannot touch any raster revision.
    {
        const auto revision = f.surface->revision();
        FillOperation fill(f.document, f.layer, {.mode = FillMode::Contiguous, .color = {100, 200, 50, 255}, .seed = {0, 0}});
        (void)fill.step(1); fill.cancel();
        CHECK(f.surface->revision() == revision && fill.stats().journalBytes == 0);
    }
    // Simulate the renderer catching up after each cooperative slice. Long
    // jobs must remain incremental even beyond the dirty-journal length.
    Fixture large({1100, 1100});
    large.surface->maximumReadBytes = 0;
    FillOperation fill(large.document, large.layer, {.color = {90, 170, 30, 255}});
    auto uploaded = large.surface->revision();
    std::size_t steps = 0;
    while (fill.state() == FillState::Applying) {
        (void)fill.step(4096); ++steps;
        const auto dirty = large.surface->dirtySince(uploaded);
        CHECK(!dirty.fullRefresh);
        CHECK(dirty.revision >= uploaded);
        uploaded = dirty.revision;
    }
    CHECK(steps > 256);
    CHECK(large.surface->maximumReadBytes <= 64 * 64 * 4);
    CHECK(fill.commit(large.history) == RasterEditCommitResult::Committed);
    CHECK(large.history.undoDepth() == 1);
    CHECK(large.history.undo(large.document));
    CHECK(!large.surface->dirtySince(uploaded).fullRefresh);
}

void targetInvalidationAndBadInputs()
{
    Fixture f({130, 130}, {9, 19, 29, 255});
    const auto original = bytes(*f.surface);
    for (int scenario = 0; scenario < 4; ++scenario) {
        FillOperation fill(f.document, f.layer, {.color = {200, 0, 50, 255}});
        CHECK(fill.step(4096) == FillState::Applying);
        if (scenario == 0) {
            auto removed = f.document.takeLayer(f.layer);
            CHECK(removed.has_value());
            CHECK(fill.step() == FillState::Failed);
            CHECK(f.document.insertLayer(0, std::move(removed->layer)));
        } else if (scenario == 1) {
            auto& raster = std::get<RasterLayer>(f.document.layer(f.layer)->payload);
            raster.surface = std::make_shared<ContiguousRasterSurface>(Extent2u {130, 130});
            CHECK(fill.step() == FillState::Failed);
            raster.surface = f.surface;
        } else if (scenario == 2) {
            CHECK(f.document.setLayerTransform(f.layer, {.m02 = 2}));
            CHECK(fill.step() == FillState::Failed);
            CHECK(f.document.setLayerTransform(f.layer, {}));
        } else {
            CHECK(f.document.setCanvas({{120, 130}}));
            CHECK(fill.step() == FillState::Failed);
            CHECK(f.document.setCanvas({{130, 130}}));
        }
        CHECK(bytes(*f.surface) == original && !f.history.canUndo());
    }
    FillOperation missing(f.document, std::numeric_limits<LayerId>::max(), {.color = {1, 2, 3, 255}});
    CHECK(missing.state() == FillState::Failed && !missing.error().empty());
    FillOperation nan(f.document, f.layer, {.color = {1, 2, 3, 255}, .opacity = std::numeric_limits<double>::quiet_NaN()});
    CHECK(nan.state() == FillState::Failed);
    // Normal model setters reject singular transforms. Corrupt this fixture
    // deliberately to exercise the fill boundary's defensive validation.
    f.document.layer(f.layer)->localToDocument = {.m00 = 0, .m11 = 0};
    FillOperation singular(f.document, f.layer, {.color = {1, 2, 3, 255}});
    CHECK(singular.state() == FillState::Failed);
    f.document.layer(f.layer)->localToDocument = {};
    for (const auto seed : {Vec2d {-1, 5}, Vec2d {130, 5}, Vec2d {2, -1}, Vec2d {2, 130},
             Vec2d {std::numeric_limits<double>::infinity(), 5}})
        execute(f, {.mode = FillMode::Contiguous, .color = {1, 2, 3, 255}, .seed = seed}, RasterEditCommitResult::NoChanges);
    CHECK(bytes(*f.surface) == original && !f.history.canUndo());
    execute(f, {.color = {170, 110, 20, 255}});
    auto removed = f.document.takeLayer(f.layer);
    CHECK(removed.has_value());
    CHECK(!f.history.undo(f.document));
    CHECK(f.history.undoDepth() == 1 && f.history.redoDepth() == 0);
    CHECK(f.document.insertLayer(0, std::move(removed->layer)));
    CHECK(f.history.undo(f.document));
    CHECK(bytes(*f.surface) == original);
}

void smallAreasDoNotScanTheLayer()
{
    Fixture f({5120, 2880}, {20, 40, 60, 255});
    const auto small = SelectionMask::rectangle({5120, 2880}, {1901, 1203, 11, 7});
    CHECK(f.document.setSelection(small));
    FillOperation selected(f.document, f.layer, {.color = {220, 80, 10, 255}});
    CHECK(finish(selected) == FillState::Ready);
    CHECK(selected.stats().candidatePixels == 11 * 7);
    CHECK(selected.stats().evaluatedPixels <= 4 * 64 * 64);
    CHECK(selected.stats().journalBytes <= 4 * 64 * 64 * 4);
    CHECK(selected.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(f.history.undo(f.document));
    CHECK(f.document.setSelection({}));

    // The isolated region lies far from the source origin. Discovery reports
    // exact bounds; application should visit only intersecting journal tiles.
    for (int y = 2223; y < 2231; ++y) for (int x = 4201; x < 4210; ++x)
        setPixel(*f.surface, x, y, {0, 0, 0, 0});
    FillOperation region(f.document, f.layer, {.mode = FillMode::Contiguous,
        .color = {80, 130, 210, 255}, .tolerance = 0, .seed = {4205.5, 2226.5}});
    CHECK(finish(region, 1) == FillState::Ready);
    CHECK(region.stats().candidatePixels == 9 * 8);
    CHECK(region.stats().evaluatedPixels <= 4 * 64 * 64);
    CHECK(region.stats().journalBytes <= 4 * 64 * 64 * 4);
    CHECK(region.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface, 4205, 2226) == Rgba8({80, 130, 210, 255}));
    CHECK(pixel(*f.surface, 4200, 2226) == Rgba8({20, 40, 60, 255}));
    CHECK(f.history.undo(f.document));

    // Inverse-mapped selection bounds must stay local and conservative for a
    // rotated/flipped layer, while not expanding to a whole large source.
    const AffineTransform rotatedFlip {.m00 = 0, .m01 = -1, .m02 = 5120,
        .m10 = -1, .m11 = 0, .m12 = 2880};
    CHECK(f.document.setLayerTransform(f.layer, rotatedFlip));
    CHECK(f.document.setSelection(SelectionMask::rectangle({5120, 2880}, {3007, 802, 13, 17})));
    FillOperation transformed(f.document, f.layer, {.color = {190, 70, 20, 255}});
    CHECK(finish(transformed) == FillState::Ready);
    CHECK(transformed.stats().candidatePixels == 13 * 17);
    CHECK(transformed.stats().evaluatedPixels <= 4 * 64 * 64);
    CHECK(transformed.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(f.history.undo(f.document));
    CHECK(f.document.setSelection({}));
    CHECK(f.document.setLayerTransform(f.layer, {}));
    CHECK(f.document.setCanvas({{17, 11}}));
    FillOperation canvas(f.document, f.layer, {.color = {90, 170, 70, 255}});
    CHECK(finish(canvas) == FillState::Ready);
    CHECK(canvas.stats().candidatePixels == 17 * 11);
    CHECK(canvas.stats().evaluatedPixels <= 64 * 64);
    CHECK(canvas.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface, 16, 10) == Rgba8({90, 170, 70, 255}));
    CHECK(pixel(*f.surface, 17, 10) == Rgba8({20, 40, 60, 255}));
}
}

void eraseSelectionCoverageAndHistory()
{
    Fixture f({32, 24}, {80, 120, 160, 200});
    const auto original = bytes(*f.surface);
    // No mask clears the entire source, including cropped/off-canvas pixels.
    f.document.setLayerTransform(f.layer, {1,0,200,0,1,200});
    f.document.setLayerCrop(f.layer, LayerCrop{4,4,5,5});
    execute(f, {.eraseSelection = true}, RasterEditCommitResult::Committed);
    for (int y=0;y<24;++y) for(int x=0;x<32;++x) CHECK(pixel(*f.surface,x,y).alpha == 0);
    const auto clearRevision=f.surface->revision();
    execute(f, {.eraseSelection = true}, RasterEditCommitResult::NoChanges);
    CHECK(f.surface->revision()==clearRevision);
    CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface)==original);
    f.document.setLayerCrop(f.layer, {});
    f.document.setLayerTransform(f.layer, {});
    CHECK(f.document.setSelection(SelectionMask::rectangle({32,24}, {})));
    execute(f, {.eraseSelection = true}, RasterEditCommitResult::NoChanges);
    auto mask = SelectionMask::rectangle({32,24}, {4,4,20,16}, 128)->combined(
        *SelectionMask::rectangle({32,24}, {10,8,4,4}), SelectionOperation::Subtract);
    CHECK(f.document.setSelection(mask));
    const AffineTransform transform {.m00=-1.2, .m01=.3, .m02=29, .m10=.2, .m11=1.1, .m12=-2};
    CHECK(f.document.setLayerTransform(f.layer, transform));
    execute(f, {.color = {}, .opacity = 0, .eraseSelection = true}, RasterEditCommitResult::Committed);
    CHECK(f.history.undoDepth() == 1);
    for (int y=0; y<24; ++y) for (int x=0; x<32; ++x) {
        const auto p = transform.map({x+.5,y+.5});
        const auto coverage = p.x>=0 && p.y>=0 && p.x<32 && p.y<24
            ? mask->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y))) : 0;
        const auto after = pixel(*f.surface, x, y);
        CHECK(after.alpha == std::uint8_t(std::lround(200.0*(1-double(coverage)/255))));
        CHECK(after.red == 80 && after.green == 120 && after.blue == 160);
    }
    CHECK(f.document.selection() == mask);
    CHECK(f.document.layer(f.layer)->localToDocument == transform);
    const auto erased = bytes(*f.surface);
    CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == original);
    CHECK(f.history.redo(f.document)); CHECK(bytes(*f.surface) == erased);
    CHECK(f.history.undo(f.document));
    FillOperation cancelled(f.document, f.layer, {.eraseSelection = true});
    CHECK(cancelled.step(4096) == FillState::Ready);
    cancelled.cancel();
    CHECK(bytes(*f.surface) == original && f.history.redoDepth() == 1);
    CHECK(f.document.setSelection({}));
    FillOperation clearCancelled(f.document, f.layer, {.eraseSelection = true});
    clearCancelled.step(4096); clearCancelled.cancel();
    CHECK(bytes(*f.surface) == original && f.history.redoDepth() == 1);
}

int main()
{
    eraseSelectionCoverageAndHistory();
    solidSelectionAndCompositing();
    transparencyAndNoops();
    regionDiscovery();
    contiguousSelectionAndTransparentSeed();
    transformedTargetsAndCanvasClipping();
    chunkCancellationFailureAndDirtyTracking();
    targetInvalidationAndBadInputs();
    smallAreasDoNotScanTheLayer();
    std::cout << "Fill/region tests: " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
