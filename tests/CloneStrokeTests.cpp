#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        if (failures < 40) std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4);
    surface.copyRgba8({0, 0, int(extent.width), int(extent.height)}, result, std::size_t(extent.width) * 4);
    return result;
}
Rgba8 pixel(const RasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> values;
    surface.copyRgba8({x, y, 1, 1}, values, 4);
    return {std::to_integer<std::uint8_t>(values[0]), std::to_integer<std::uint8_t>(values[1]),
        std::to_integer<std::uint8_t>(values[2]), std::to_integer<std::uint8_t>(values[3])};
}
Rgba8 pattern(int x, int y)
{
    return {std::uint8_t((x * 37 + y * 13) % 240 + 8), std::uint8_t((x * 11 + y * 29) % 240 + 8),
        std::uint8_t((x * 19 + y * 7) % 240 + 8), 255};
}
std::shared_ptr<ContiguousRasterSurface> patterned(Extent2u extent)
{
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4);
    for (std::uint32_t y = 0; y < extent.height; ++y) for (std::uint32_t x = 0; x < extent.width; ++x) {
        const auto p = pattern(int(x), int(y));
        const auto i = (std::size_t(y) * extent.width + x) * 4;
        result[i] = std::byte(p.red); result[i + 1] = std::byte(p.green);
        result[i + 2] = std::byte(p.blue); result[i + 3] = std::byte(p.alpha);
    }
    return std::make_shared<ContiguousRasterSurface>(extent, std::move(result));
}
LayerId add(Document& document, std::shared_ptr<RasterSurface> pixels)
{
    auto layer = Layer::raster("Clone test", std::move(pixels));
    const auto id = layer.id;
    CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
    return id;
}
CloneReference reference(const Document& document, LayerId target, LayerId source,
    CloneSettings settings = {})
{
    std::string diagnostic;
    auto value = CloneReference::capture(document, target, source, settings.source, diagnostic,
        CloneReference::defaultSnapshotLimit, settings.mode == CloneMode::Heal);
    if (!value) throw std::runtime_error(diagnostic);
    return *value;
}
BrushSettings brush(double size = 12)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = size; settings.hardness = 1; settings.flow = 1; settings.opacity = 1;
    settings.pressureToFlow = false; settings.pressureToSize = false; settings.spacingPercent = 20;
    return settings;
}
NormalizedPointerSample sample(Vec2d point, std::uint64_t time = 0, double pressure = 1)
{
    return {.documentPosition = point, .timestampMicroseconds = time, .pressure = pressure,
        .pointerType = PointerType::Pen, .buttons = PointerButtonPrimary};
}
void nearColor(Rgba8 a, Rgba8 b, int tolerance = 1)
{
    CHECK(std::abs(int(a.red) - b.red) <= tolerance);
    CHECK(std::abs(int(a.green) - b.green) <= tolerance);
    CHECK(std::abs(int(a.blue) - b.blue) <= tolerance);
    CHECK(std::abs(int(a.alpha) - b.alpha) <= tolerance);
}

void integerCloneEveryPixelOverlapAndHistory()
{
    Document document({{80, 48}});
    auto pixels = patterned({80, 48});
    const auto id = add(document, pixels);
    document.markSaved();
    const auto before = bytes(*pixels);
    History history;
    CloneStroke stroke(document, id, brush(), {}, reference(document, id, id), {-12, 0});
    CHECK(stroke.begin(sample({20.5, 24.5})));
    CHECK(stroke.append(sample({39.5, 24.5}, 19000)));
    CHECK(stroke.end(sample({60.5, 24.5}, 40000), history) == RasterEditCommitResult::Committed);
    // Every output location follows the original document offset, including
    // source pixels already crossed and modified by earlier destination dabs.
    for (int x = 20; x <= 60; ++x) for (int y = 23; y <= 25; ++y)
        CHECK(pixel(*pixels, x, y) == pattern(x - 12, y));
    CHECK(history.undoDepth() == 1);
    CHECK(document.isModified());
    CHECK(stroke.stats().surfaceWriteBatches > 0);
    CHECK(stroke.stats().uploadedRegionBytes < before.size());
    const auto after = bytes(*pixels);
    CHECK(history.undo(document));
    CHECK(bytes(*pixels) == before);
    CHECK(!document.isModified());
    CHECK(history.redo(document));
    CHECK(bytes(*pixels) == after);
}

void sameOpaqueSourceNoOpAndTransparentSourceDoesNotErase()
{
    Document document({{64, 48}});
    auto pixels = patterned({64, 48});
    const auto id = add(document, pixels);
    History history;
    const auto before = bytes(*pixels);
    const auto revision = pixels->revision();
    CloneStroke same(document, id, brush(), {}, reference(document, id, id), {});
    CHECK(same.begin(sample({12.5, 20.5})));
    CHECK(same.end(sample({45.5, 20.5}, 30000), history) == RasterEditCommitResult::NoChanges);
    CHECK(history.undoDepth() == 0);
    CHECK(bytes(*pixels) == before);
    CHECK(pixels->revision() == revision);
    CHECK(same.stats().uploadedRegionBytes == 0);
    CHECK(same.stats().surfaceWriteBatches == 0);

    const auto source = add(document, std::make_shared<ContiguousRasterSurface>(Extent2u {64, 48}, Rgba8 {255, 0, 255, 0}));
    CloneStroke transparent(document, id, brush(), {}, reference(document, id, source), {});
    CHECK(transparent.begin(sample({20.5, 20.5})));
    CHECK(transparent.end(sample({20.5, 20.5}, 1000), history) == RasterEditCommitResult::NoChanges);
    CHECK(bytes(*pixels) == before);
    CHECK(transparent.stats().uploadedRegionBytes == 0);
}

void sourceAlphaPressureOpacityFlowAndSelectionOnce()
{
    Document document({{32, 32}});
    const auto source = add(document, std::make_shared<ContiguousRasterSurface>(Extent2u {32, 32}, Rgba8 {255, 0, 0, 128}));
    auto targetPixels = std::make_shared<ContiguousRasterSurface>(Extent2u {32, 32}, Rgba8 {0, 0, 255, 128});
    const auto target = add(document, targetPixels);
    std::vector<std::uint8_t> mask(32 * 32, 128);
    mask[16 * 32 + 15] = 0;
    CHECK(document.setSelection(SelectionMask::fromR8({32, 32}, mask, 32)));
    auto settings = brush(); settings.opacity = .6; settings.flow = .5; settings.pressureToFlow = true;
    History history;
    CloneStroke stroke(document, target, settings, {}, reference(document, target, source), {});
    CHECK(stroke.begin(sample({16.5, 16.5}, 0, .5)));
    CHECK(stroke.end(sample({16.5, 16.5}, 1000, .5), history) == RasterEditCommitResult::Committed);
    CHECK(stroke.stats().emittedDabs == 1);
    const auto expected = encodeColor(compositeLayer(decodeColor({0, 0, 255, 128}),
        decodeColor({255, 0, 0, 128}), float(.6 * .5 * .5 * 128 / 255), BlendMode::Normal));
    nearColor(pixel(*targetPixels, 16, 16), expected);
    CHECK(pixel(*targetPixels, 15, 16) == Rgba8(0, 0, 255, 128));
}

void transformedLayersAndTipRotationOnlyAffectFootprint()
{
    const std::array sourceTransforms {AffineTransform {}, AffineTransform {0, -1, 80, 1, 0, 0},
        AffineTransform {1, .2, 10, .15, 1, 0}};
    const std::array targetTransforms {AffineTransform {}, AffineTransform {-1, 0, 80, 0, 1, 0},
        AffineTransform {.8, -.2, 20, .3, 1, 5}};
    for (std::size_t i = 0; i < sourceTransforms.size(); ++i) {
        Document document({{128, 128}});
        const auto source = add(document, patterned({96, 96}));
        auto targetPixels = std::make_shared<ContiguousRasterSurface>(Extent2u {64, 64});
        const auto target = add(document, targetPixels);
        if (i) CHECK(document.setLayerTransform(source, sourceTransforms[i]));
        if (i) CHECK(document.setLayerTransform(target, targetTransforms[i]));
        auto settings = brush(24); settings.tip.assetId = BrushAssetIds::ProceduralEllipseTip;
        settings.tip.aspectRatio = .5; settings.tip.angleDegrees = 67;
        settings.tip.rotationMode = BrushTipRotationMode::Fixed;
        const auto ref = reference(document, target, source);
        const Vec2d offset {-4, 2};
        const auto center = targetTransforms[i].map({24.5, 24.5});
        History history;
        CloneStroke stroke(document, target, settings, {}, ref, offset);
        CHECK(stroke.begin(sample(center)));
        CHECK(stroke.end(sample(center, 1000), history) == RasterEditCommitResult::Committed);
        for (int y = 23; y <= 25; ++y) for (int x = 23; x <= 25; ++x) {
            const auto wanted = encodeColor(ref.sample(targetTransforms[i].map({x + .5, y + .5}) + offset));
            CHECK(pixel(*targetPixels, x, y) == wanted);
        }
    }
}

void cancellationAndTargetInvalidation()
{
    for (const auto mode : {CloneMode::Stamp, CloneMode::Heal}) {
        Document document({{80, 48}});
        auto pixels = patterned({80, 48});
        const auto id = add(document, pixels);
        const auto before = bytes(*pixels);
        CloneSettings settings; settings.mode = mode;
        History history;
        CloneStroke stroke(document, id, brush(), settings, reference(document, id, id, settings), {-12, 0});
        CHECK(stroke.begin(sample({25.5, 24.5})));
        CHECK(stroke.append(sample({45.5, 24.5}, 20000)));
        stroke.cancel();
        CHECK(bytes(*pixels) == before);
        CHECK(history.undoDepth() == 0);

        CloneStroke cancelledFinal(document, id, brush(), settings, reference(document, id, id, settings), {-12, 0});
        CHECK(cancelledFinal.begin(sample({25.5, 24.5})));
        int callbacks = 0;
        CHECK(cancelledFinal.end(sample({45.5, 24.5}, 20000), history,
            [&] { ++callbacks; return true; }) == RasterEditCommitResult::TargetUnavailable);
        CHECK(callbacks > 0);
        CHECK(bytes(*pixels) == before);
        CHECK(history.undoDepth() == 0);

        CloneStroke changed(document, id, brush(), settings, reference(document, id, id, settings), {-12, 0});
        CHECK(changed.begin(sample({25.5, 24.5})));
        CHECK(document.setLayerTransform(id, {.m02 = 1}));
        CHECK(!changed.append(sample({45.5, 24.5}, 20000)));
        CHECK(bytes(*pixels) == before);
    }
}

void finalHealReapplyCancellationAndTargetReplacementNeverPublish()
{
    // Cover cancellation, a document-level target change, and an in-place
    // payload replacement that changes only SurfaceId. Interruption occurs
    // after reconstruction, between final reapply tiles, before publication.
    for (int interruption = 0; interruption < 3; ++interruption) {
        Document document({{256, 128}});
        auto pixels = patterned({256, 128});
        const auto target = add(document, pixels);
        auto replacement = std::make_shared<ContiguousRasterSurface>(Extent2u {256, 128}, Rgba8 {7, 19, 33, 77});
        const auto before = bytes(*pixels), replacementBefore = bytes(*replacement);
        const auto revision = pixels->revision(), replacementRevision = replacement->revision();
        History history;
        CloneSettings clone; clone.mode = CloneMode::Heal; clone.adaptation = 0;
        CloneStroke stroke(document, target, brush(100), clone, reference(document, target, target, clone), {-48, 0});
        CHECK(stroke.begin(sample({128.5, 64.5})));
        CHECK(stroke.stats().retainedStrokeTiles >= 2);
        int callbacksAfterSolve = 0;
        const auto result = stroke.end(sample({128.5, 64.5}, 1000), history,
            [&] {
                if (stroke.healingResult().pixels.empty()) return false;
                // First callback checks the finished solve; the following
                // callbacks guard successive final reapply tiles.
                if (++callbacksAfterSolve != 3) return false;
                if (interruption == 1) CHECK(document.setLayerOpacity(target, .75F));
                if (interruption == 2) std::get<RasterLayer>(document.layer(target)->payload).surface = replacement;
                return interruption == 0;
            });
        CHECK(callbacksAfterSolve == 3);
        CHECK(result == RasterEditCommitResult::TargetUnavailable);
        CHECK(!stroke.healingResult().pixels.empty());
        CHECK(history.undoDepth() == 0);
        CHECK(bytes(*pixels) == before);
        CHECK(pixels->revision() == revision);
        CHECK(bytes(*replacement) == replacementBefore);
        CHECK(replacement->revision() == replacementRevision);
        CHECK(stroke.stats().surfaceWriteBatches == 0);
        CHECK(stroke.stats().uploadedRegionBytes == 0);
    }
}

std::vector<std::byte> eventRateResult(int divisions, CloneMode mode)
{
    Document document({{96, 64}});
    const auto source = add(document, patterned({96, 64}));
    auto targetPixels = std::make_shared<ContiguousRasterSurface>(Extent2u {96, 64});
    const auto target = add(document, targetPixels);
    auto settings = brush(14); settings.flow = .35; settings.opacity = .72; settings.hardness = .4;
    CloneSettings clone; clone.mode = mode; clone.adaptation = 0;
    History history;
    CloneStroke stroke(document, target, settings, clone, reference(document, target, source, clone), {-10.25, .5});
    CHECK(stroke.begin(sample({25.5, 32.5})));
    for (int i = 1; i < divisions; ++i)
        CHECK(stroke.append(sample({25.5 + 45.0 * i / divisions, 32.5}, std::uint64_t(45000 * i / divisions))));
    CHECK(stroke.end(sample({70.5, 32.5}, 45000), history) == RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);
    return bytes(*targetPixels);
}

void sparseDenseAndHealZeroAdaptationAreIdentical()
{
    const auto sparse = eventRateResult(1, CloneMode::Stamp);
    const auto dense = eventRateResult(90, CloneMode::Stamp);
    CHECK(sparse == dense);
    CHECK(sparse == eventRateResult(90, CloneMode::Heal));
}

void healRetouchCropSelectionAndExactHistory()
{
    Document document({{80, 64}});
    const auto source = add(document, patterned({80, 64}));
    auto pixels = std::make_shared<ContiguousRasterSurface>(Extent2u {80, 64});
    const auto target = add(document, pixels);
    CHECK(document.setLayerCrop(target, LayerCrop {25, 20, 20, 25}));
    std::vector<std::uint8_t> mask(80 * 64, 255);
    for (int y = 27; y < 34; ++y) for (int x = 33; x < 36; ++x) mask[std::size_t(y * 80 + x)] = 0;
    CHECK(document.setSelection(SelectionMask::fromR8({80, 64}, mask, 80)));
    document.markSaved();
    CloneSettings clone; clone.mode = CloneMode::Heal; clone.source = CloneSampleSource::CurrentAndBelow;
    auto settings = brush(22); settings.opacity = .5; settings.hardness = .35;
    History history;
    const auto before = bytes(*pixels); const auto revision = pixels->revision();
    CloneStroke stroke(document, target, settings, clone, reference(document, target, source, clone), {-16, 0});
    CHECK(stroke.begin(sample({30.5, 30.5})));
    CHECK(stroke.append(sample({42.5, 30.5}, 12000)));
    CHECK(pixels->revision() == revision); // Final adaptive repair is deferred.
    CHECK(bytes(*pixels) == before);
    CHECK(stroke.end(sample({42.5, 30.5}, 13000), history) == RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);
    CHECK(stroke.healingResult().pixels.size() > 0);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 80; ++x) {
        const auto p = pixel(*pixels, x, y);
        if (x < 25 || x >= 45 || y < 20 || y >= 45 || mask[std::size_t(y * 80 + x)] == 0)
            CHECK(p == Rgba8 {});
        CHECK(p.alpha <= 128);
    }
    CHECK(pixel(*pixels, 29, 30).alpha > 0);
    const auto after = bytes(*pixels);
    CHECK(document.isModified());
    CHECK(history.undo(document)); CHECK(bytes(*pixels) == before); CHECK(!document.isModified());
    CHECK(history.redo(document)); CHECK(bytes(*pixels) == after);
}

void alignedAnchorOnlyPublishesCompletedOffsets()
{
    CloneAnchor anchor {42, {12.25, 18.5}, {}, {}};
    const auto first = anchor.offsetFor({35.5, 25.5}, true);
    CHECK(first == Vec2d(-23.25, -7));
    // Cancelling never calls completed(), so the next stroke still resets to
    // the original anchor. Changing targets does not alter this document offset.
    CHECK(anchor.offsetFor({40.5, 25.5}, true) == Vec2d(-28.25, -7));
    anchor.completed(first, true);
    CHECK(anchor.offsetFor({40.5, 25.5}, true) == first);
    CHECK(anchor.offsetFor({40.5, 25.5}, false) == Vec2d(-28.25, -7));
    anchor.completed({-50, -50}, false);
    CHECK(anchor.offsetFor({40.5, 25.5}, true) == first);
    anchor = CloneAnchor {77, {7, 8}, {.m02 = 5}, {}};
    CHECK(anchor.offsetFor({40.5, 25.5}, true) == Vec2d(-33.5, -17.5));
}

void rawAdjustedDestinationAndRenderedRetouchWorkflow()
{
    Document document({{64, 48}});
    auto pixels = patterned({64, 48});
    const auto source = add(document, pixels);
    auto adjustments = std::make_shared<AdjustmentStack>();
    adjustments->items[std::size_t(AdjustmentType::Invert)].enabled = true;
    CHECK(document.setLayerAdjustments(source, adjustments));
    CHECK(document.setLayerOpacity(source, .5F));
    History history;
    CloneStroke direct(document, source, brush(), {}, reference(document, source, source), {-12, 0});
    CHECK(direct.begin(sample({32.5, 24.5})));
    CHECK(direct.end(sample({32.5, 24.5}, 1000), history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*pixels, 32, 24) == pattern(20, 24));

    auto retouch = std::make_shared<ContiguousRasterSurface>(Extent2u {64, 48});
    const auto target = add(document, retouch);
    CloneSettings clone; clone.source = CloneSampleSource::CurrentAndBelow;
    CloneStroke rendered(document, target, brush(), clone, reference(document, target, source, clone), {-12, 0});
    CHECK(rendered.begin(sample({32.5, 24.5})));
    CHECK(rendered.end(sample({32.5, 24.5}, 1000), history) == RasterEditCommitResult::Committed);
    const auto color = pattern(20, 24);
    CHECK(pixel(*retouch, 32, 24) == Rgba8(std::uint8_t(255 - color.red),
        std::uint8_t(255 - color.green), std::uint8_t(255 - color.blue), 128));
    CHECK(!document.layer(target)->adjustments);
    CHECK(document.layer(target)->opacity == 1);
}

std::size_t peakRssKiB()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) if (line.starts_with("VmHWM:")) return std::stoull(line.substr(6));
    return 0;
}
void benchmark()
{
    using Clock = std::chrono::steady_clock;
    for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}})
        for (const auto mode : {CloneMode::Stamp, CloneMode::Heal}) {
            Document document({extent});
            const auto id = add(document, patterned(extent));
            CloneSettings clone; clone.mode = mode;
            auto settings = brush(72); settings.hardness = .65;
            const auto start = Clock::now();
            auto frozen = reference(document, id, id, clone);
            const auto captured = Clock::now();
            History history;
            CloneStroke stroke(document, id, settings, clone, std::move(frozen), {-180, 0});
            CHECK(stroke.begin(sample({extent.width * .5, extent.height * .5})));
            for (int i = 1; i <= 16; ++i)
                CHECK(stroke.append(sample({extent.width * .5 + i * 10, extent.height * .5}, std::uint64_t(i * 1000))));
            const auto input = Clock::now();
            CHECK(stroke.end(sample({extent.width * .5 + 160, extent.height * .5}, 17000), history)
                == RasterEditCommitResult::Committed);
            const auto end = Clock::now();
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            std::cout << extent.width << 'x' << extent.height << ' ' << (mode == CloneMode::Stamp ? "Stamp" : "Heal")
                << ": capture=" << ms(start, captured) << "ms input=" << ms(captured, input)
                << "ms final=" << ms(input, end) << "ms snapshot=" << stroke.snapshotBytes()
                << "B upload-regions=" << stroke.stats().uploadedRegionBytes << "B peak-RSS=" << peakRssKiB()
                << "KiB solver-scratch=" << stroke.healingResult().diagnostics.estimatedScratchBytes
                << "B solver-iterations=" << stroke.healingResult().diagnostics.iterations
                << " fallback-components=" << stroke.healingResult().diagnostics.fallbackComponents << '\n';
        }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc > 1 && std::string(argv[1]) == "--bench") benchmark();
        else {
            integerCloneEveryPixelOverlapAndHistory();
            sameOpaqueSourceNoOpAndTransparentSourceDoesNotErase();
            sourceAlphaPressureOpacityFlowAndSelectionOnce();
            transformedLayersAndTipRotationOnlyAffectFootprint();
            cancellationAndTargetInvalidation();
            finalHealReapplyCancellationAndTargetReplacementNeverPublish();
            sparseDenseAndHealZeroAdaptationAreIdentical();
            healRetouchCropSelectionAndExactHistory();
            alignedAnchorOnlyPublishesCompletedOffsets();
            rawAdjustedDestinationAndRenderedRetouchWorkflow();
        }
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n'; ++failures;
    }
    if (!failures) std::cout << "Clone stroke tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
