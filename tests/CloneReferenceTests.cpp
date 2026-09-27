#include "imageeditor/core/CloneReference.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void near(PremultipliedColor actual, PremultipliedColor expected, float tolerance = 1e-6F)
{
    for (std::size_t i = 0; i < 4; ++i) CHECK(std::abs(actual[i] - expected[i]) <= tolerance);
}

std::shared_ptr<ContiguousRasterSurface> surface(Extent2u extent, Rgba8 color = {})
{
    return std::make_shared<ContiguousRasterSurface>(extent, color);
}

LayerId add(Document& document, std::shared_ptr<RasterSurface> pixels)
{
    auto layer = Layer::raster("Clone reference fixture", std::move(pixels));
    const auto id = layer.id;
    CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
    return id;
}

void put(RasterSurface& pixels, int x, int y, Rgba8 color)
{
    const std::array bytes {std::byte(color.red), std::byte(color.green),
        std::byte(color.blue), std::byte(color.alpha)};
    pixels.replaceRgba8({x, y, 1, 1}, bytes, 4);
}

CloneReference capture(const Document& document, LayerId target, LayerId source,
    CloneSampleSource mode = CloneSampleSource::SourceLayer, bool context = true)
{
    std::string diagnostic;
    auto result = CloneReference::capture(document, target, source, mode, diagnostic,
        CloneReference::defaultSnapshotLimit, context);
    if (!result) throw std::runtime_error(diagnostic);
    CHECK(diagnostic.empty());
    return *result;
}

void immutableOverlapAndRefresh()
{
    Document document({{8, 3}});
    auto pixels = surface({8, 3});
    std::array<Rgba8, 24> expected;
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 8; ++x) {
        const Rgba8 color {std::uint8_t(x * 31), std::uint8_t(255 - x * 29),
            std::uint8_t(y * 73), std::uint8_t(y == 0 ? 0 : y == 1 ? 128 : 255)};
        put(*pixels, x, y, color);
        expected[std::size_t(y * 8 + x)] = color.alpha ? color : Rgba8 {};
    }
    const auto id = add(document, pixels);
    const auto reference = capture(document, id, id);
    CHECK(reference.snapshotBytes() == 8 * 3 * 4);
    CHECK(reference.sourceCount() == 1);
    // Write first, and only then visit the same reference location. A lazy
    // cache of mutable pixels would recover the wrong pre-stroke content.
    put(*pixels, 7, 2, {255, 10, 20, 255});
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 8; ++x) {
        const Vec2d point {x + .5, y + .5};
        const auto wanted = expected[std::size_t(y * 8 + x)];
        CHECK(encodeColor(reference.sample(point)) == wanted);
        CHECK(encodeColor(reference.sampleDestination(point)) == wanted);
    }
    const auto refreshed = capture(document, id, id);
    CHECK(encodeColor(refreshed.sample({7.5, 2.5})) == Rgba8(255, 10, 20, 255));

    // Geometry and ownership are frozen too, and live document revisions do
    // not reject the stroke's own destination changes.
    CHECK(document.setLayerTransform(id, {.m02 = 20}));
    CHECK(document.setLayerCrop(id, LayerCrop {0, 0, 1, 1}));
    CHECK(document.setLayerVisibility(id, false));
    CHECK(document.takeLayer(id).has_value());
    CHECK(encodeColor(reference.sample({7.5, 2.5})) == expected[23]);
    CHECK(encodeColor(refreshed.sample({7.5, 2.5})) == Rgba8(255, 10, 20, 255));
}

void stableSourceAndSelectionIndependence()
{
    Document document({{8, 8}});
    const auto source = add(document, surface({8, 8}, {210, 40, 80, 255}));
    const auto firstTarget = add(document, surface({8, 8}));
    const auto secondTarget = add(document, surface({8, 8}, {5, 20, 240, 255}));
    CHECK(document.setSelection(SelectionMask::rectangle({8, 8}, {5, 5, 2, 2})));
    const auto first = capture(document, firstTarget, source, CloneSampleSource::SourceLayer, false);
    const auto second = capture(document, secondTarget, source, CloneSampleSource::SourceLayer, false);
    for (const auto point : {Vec2d {1.5, 1.5}, Vec2d {5.5, 5.5}}) {
        CHECK(encodeColor(first.sample(point)) == Rgba8(210, 40, 80, 255));
        CHECK(encodeColor(second.sample(point)) == Rgba8(210, 40, 80, 255));
    }
    CHECK(first.snapshotBytes() == 8 * 8 * 4);
    CHECK(second.snapshotBytes() == first.snapshotBytes());
}

void alphaAwareContinuousSamplingAndSourceBounds()
{
    Document document({{10, 10}});
    auto pixels = surface({2, 1});
    put(*pixels, 0, 0, {255, 0, 0, 255});
    put(*pixels, 1, 0, {0, 0, 255, 0});
    const auto id = add(document, pixels);
    const auto reference = capture(document, id, id);
    near(reference.sample({1, .5}), {.5F, 0, 0, .5F});
    near(reference.sample({.75, .5}), {.75F, 0, 0, .75F});
    CHECK(encodeColor(reference.sample({1, .5})) == Rgba8(255, 0, 0, 128));
    near(reference.sample({1.5, .5}), {});
    CHECK(reference.validSample({1.5, .5})); // Transparent inside bounds is an admissible anchor.
    CHECK(!reference.validSample({2, .5}));
    CHECK(!reference.validSample({-.01, .5}));
    for (const auto point : {Vec2d {-.01, .5}, Vec2d {2, .5}, Vec2d {.5, -.01},
             Vec2d {.5, 1}, Vec2d {std::numeric_limits<double>::quiet_NaN(), .5},
             Vec2d {.5, std::numeric_limits<double>::infinity()}})
        near(reference.sample(point), {});
}

void transformedSourcesAndIndependentDestinations()
{
    Document document({{100, 100}});
    auto pixels = surface({6, 5});
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 6; ++x)
        put(*pixels, x, y, {std::uint8_t(20 + x * 35), std::uint8_t(20 + y * 40), 60, 255});
    const auto source = add(document, pixels);
    const auto target = add(document, surface({10, 10}));
    CHECK(document.setLayerTransform(target, {-1.4, .4, 55, .3, .9, 33}));
    const std::array transforms {
        AffineTransform {1, 0, 15.25, 0, 1, 16.75},
        AffineTransform {0, -1, 25, 1, 0, 20},
        AffineTransform {-1, 0, 30, 0, 1, 12},
        AffineTransform {1.4, .35, 20, -.2, .75, 10},
        AffineTransform {-.6, .8, 28, .8, .6, 22}};
    for (const auto& transform : transforms) {
        CHECK(document.setLayerTransform(source, transform));
        const auto reference = capture(document, target, source, CloneSampleSource::SourceLayer, false);
        for (int y = 0; y < 5; ++y) for (int x = 0; x < 6; ++x)
            CHECK(encodeColor(reference.sample(transform.map({x + .5, y + .5})))
                == Rgba8(std::uint8_t(20 + x * 35), std::uint8_t(20 + y * 40), 60, 255));
        const auto a = decodeColor({55, 60, 60, 255}), b = decodeColor({90, 60, 60, 255});
        PremultipliedColor expected;
        for (std::size_t channel = 0; channel < 4; ++channel)
            expected[channel] = a[channel] * .75F + b[channel] * .25F;
        near(reference.sample(transform.map({1.75, 1.5})), expected);
        near(reference.sample(transform.map({-1, 2.5})), {});
        near(reference.sample(transform.map({7, 2.5})), {});
    }
}

void intrinsicEffectsAndRenderedCutoff()
{
    Document document({{8, 8}});
    const auto bottom = add(document, surface({8, 8}, {20, 70, 220, 255}));
    auto targetPixels = surface({8, 8}, {100, 130, 180, 128});
    const auto target = add(document, targetPixels);
    auto adjustments = std::make_shared<AdjustmentStack>();
    auto& invert = adjustments->items[std::size_t(AdjustmentType::Invert)];
    invert.enabled = true;
    CHECK(document.setLayerAdjustments(target, adjustments));
    CHECK(document.setLayerOpacity(target, .4F));
    CHECK(document.setLayerBlendMode(target, BlendMode::Multiply));
    const auto top = add(document, surface({8, 8}, {0, 255, 0, 255}));

    const auto raw = capture(document, target, target);
    CHECK(encodeColor(raw.sample({3.5, 3.5})) == Rgba8(100, 130, 180, 128));
    near(raw.sampleDestination({3.5, 3.5}), raw.sample({3.5, 3.5}));
    const auto below = capture(document, target, bottom, CloneSampleSource::CurrentAndBelow);
    const auto all = capture(document, target, bottom, CloneSampleSource::AllVisible);
    const auto bottomColor = decodeColor({20, 70, 220, 255});
    const auto adjusted = decodeColor({155, 125, 75, 128});
    const auto expectedBelow = compositeLayer(bottomColor, adjusted, .4F, BlendMode::Multiply);
    near(below.sample({3.5, 3.5}), expectedBelow);
    near(below.sampleDestination({3.5, 3.5}), expectedBelow);
    near(all.sampleDestination({3.5, 3.5}), expectedBelow);
    CHECK(encodeColor(all.sample({3.5, 3.5})) == Rgba8(0, 255, 0, 255));
    CHECK(below.sourceCount() == 2);
    CHECK(all.sourceCount() == 3);
    CHECK(below.snapshotBytes() == 2 * 8 * 8 * 4);
    CHECK(all.snapshotBytes() == 3 * 8 * 8 * 4);

    // Appearance sampling uses exactly the shared continuous renderer contract.
    const PinnedDocumentSampler authoritative(document, {}, ColorSampleSource::MergedVisible);
    near(all.sample({2.35, 3.75}), authoritative.sampleLinear({2.35, 3.75}));
    CHECK(document.setLayerOpacity(target, 0));
    const auto zeroOpacityRaw = capture(document, target, target);
    CHECK(encodeColor(zeroOpacityRaw.sample({3.5, 3.5})) == Rgba8(100, 130, 180, 128));
    CHECK(document.setLayerVisibility(top, false));
    near(all.sample({-1, 2}), {});
    near(all.sample({8, 2}), {});
}

void emptyRetouchLayerUsesUnderlyingContext()
{
    Document document({{6, 6}});
    auto background = surface({6, 6}, {160, 120, 90, 180});
    const auto source = add(document, background);
    auto retouch = surface({6, 6});
    const auto target = add(document, retouch);
    const auto reference = capture(document, target, source, CloneSampleSource::CurrentAndBelow);
    CHECK(encodeColor(reference.sample({2.5, 2.5})) == Rgba8(160, 120, 90, 180));
    CHECK(encodeColor(reference.sampleDestination({2.5, 2.5})) == Rgba8(160, 120, 90, 180));
    put(*retouch, 2, 2, {10, 230, 80, 255});
    put(*background, 3, 2, {30, 20, 250, 255});
    CHECK(encodeColor(reference.sampleDestination({2.5, 2.5})) == Rgba8(160, 120, 90, 180));
    CHECK(encodeColor(reference.sample({3.5, 2.5})) == Rgba8(160, 120, 90, 180));
    const auto refreshed = capture(document, target, source, CloneSampleSource::CurrentAndBelow);
    CHECK(encodeColor(refreshed.sample({2.5, 2.5})) == Rgba8(10, 230, 80, 255));
}

void cropChamferAndHierarchyVisibility()
{
    Document document({{8, 8}});
    const auto background = add(document, surface({8, 8}, {20, 40, 220, 255}));
    const auto source = add(document, surface({8, 8}, {240, 60, 20, 255}));
    const auto target = add(document, surface({8, 8}));
    LayerCrop crop {1, 1, 6, 6};
    crop.corners = {3, 2, 1, 0};
    CHECK(document.setLayerCrop(source, crop));
    const auto reference = capture(document, target, source, CloneSampleSource::SourceLayer, false);
    near(reference.sample({.5, .5}), {});
    near(reference.sample({1.5, 1.5}), {});
    CHECK(!reference.validSample({1.5, 1.5}));
    CHECK(reference.validSample({4.5, 4.5}));
    CHECK(encodeColor(reference.sample({4.5, 4.5})) == Rgba8(240, 60, 20, 255));
    const PreparedLayerSampler authoritative(*document.layer(source), false);
    for (const auto point : {Vec2d {3.2, 1.5}, Vec2d {5.4, 1.8}, Vec2d {6.2, 6.2}})
        near(reference.sample(point), authoritative.sample(point));

    const auto folder = makeLayerId();
    LayerTree tree {{background, folder, target}, {
        {folder, "Hidden source folder", ContainerKind::Folder, ColorLabel::None, {source}, false}}};
    CHECK(document.replaceStructure(document.tree(), std::move(tree)));
    std::string diagnostic;
    CHECK(!CloneReference::capture(document, target, source, CloneSampleSource::SourceLayer, diagnostic));
    CHECK(diagnostic.find("hidden") != std::string::npos);
    const auto rendered = capture(document, target, source, CloneSampleSource::AllVisible);
    CHECK(rendered.sourceCount() == 2);
    CHECK(encodeColor(rendered.sample({4.5, 4.5})) == Rgba8(20, 40, 220, 255));
    // Previously admitted strokes retain the original hierarchy visibility.
    CHECK(encodeColor(reference.sample({4.5, 4.5})) == Rgba8(240, 60, 20, 255));
}

void typedCachesAreFrozenWithoutFlattening()
{
    Document document({{40, 40}});
    TextLayer text;
    text.utf8 = "Editable source text";
    auto layer = Layer::text("Text source", text);
    const auto source = layer.id;
    auto pixels = surface({4, 4}, {220, 30, 80, 200});
    auto cache = std::make_shared<LayerRenderCache>();
    cache->surface = pixels;
    cache->logicalExtent = {2, 2};
    cache->pixelsToLocal = {.m00 = .5, .m02 = -1, .m11 = .5, .m12 = -2};
    layer.renderCache = cache;
    layer.localToDocument = {-1, .2, 20, .3, 1.2, 15};
    const auto point = renderTransform(layer).map({1.5, 1.5});
    CHECK(document.insertLayer(0, std::move(layer)));
    const auto target = add(document, surface({40, 40}));
    const auto reference = capture(document, target, source, CloneSampleSource::SourceLayer, false);
    CHECK(reference.snapshotBytes() == 4 * 4 * 4);
    CHECK(encodeColor(reference.sample(point)) == Rgba8(220, 30, 80, 200));
    CHECK(std::get<TextLayer>(document.layer(source)->payload).utf8 == text.utf8);
    put(*pixels, 1, 1, {0, 255, 0, 255});
    cache->pixelsToLocal = {.m02 = 100};
    document.layer(source)->renderCache.reset();
    CHECK(encodeColor(reference.sample(point)) == Rgba8(220, 30, 80, 200));
    std::string diagnostic;
    CHECK(!CloneReference::capture(document, target, source, CloneSampleSource::SourceLayer, diagnostic));
    CHECK(diagnostic.find("prepared image") != std::string::npos);
    CHECK(std::holds_alternative<TextLayer>(document.layer(source)->payload));
}

void snapshotBudgetAndSurfaceDeduplication()
{
    Document document({{8, 8}});
    auto sharedPixels = surface({8, 8}, {90, 100, 110, 255});
    const auto source = add(document, sharedPixels);
    const auto target = add(document, sharedPixels);
    std::string diagnostic;
    const auto alias = CloneReference::capture(document, target, source,
        CloneSampleSource::AllVisible, diagnostic, 8 * 8 * 4);
    CHECK(alias.has_value());
    CHECK(alias && alias->snapshotBytes() == 8 * 8 * 4);
    CHECK(alias && alias->sourceCount() == 2);
    CHECK(!CloneReference::capture(document, target, source, CloneSampleSource::AllVisible,
        diagnostic, 8 * 8 * 4 - 1));
    CHECK(diagnostic.find("memory limit") != std::string::npos);
    CHECK(!CloneReference::capture(document, target, source, CloneSampleSource::SourceLayer,
        diagnostic, 0));
    CHECK(diagnostic.find("memory limit") != std::string::npos);
    CHECK(!CloneReference::capture(document, 0, source, CloneSampleSource::SourceLayer, diagnostic));
    CHECK(diagnostic.find("raster destination") != std::string::npos);
    CHECK(!CloneReference::capture(document, target, 0, CloneSampleSource::SourceLayer, diagnostic));
}

void unrelatedTypedLayersDoNotRequireRasterization()
{
    Document document({{8, 8}});
    auto unprepared = Layer::text("Hidden unprepared text", TextLayer {});
    unprepared.visible = false;
    CHECK(document.insertLayer(0, std::move(unprepared)));
    const auto source = add(document, surface({8, 8}, {30, 80, 120, 255}));
    const auto target = add(document, surface({8, 8}));
    const auto upper = Layer::text("Visible text above destination", TextLayer {});
    CHECK(document.insertLayer(document.layers().size(), upper));
    const auto raw = capture(document, target, source, CloneSampleSource::SourceLayer, false);
    const auto below = capture(document, target, source, CloneSampleSource::CurrentAndBelow);
    CHECK(encodeColor(raw.sample({3.5, 3.5})) == Rgba8(30, 80, 120, 255));
    CHECK(encodeColor(below.sample({3.5, 3.5})) == Rgba8(30, 80, 120, 255));
    std::string diagnostic;
    CHECK(!CloneReference::capture(document, target, source, CloneSampleSource::AllVisible, diagnostic));
    CHECK(diagnostic.find("prepared image") != std::string::npos);
    CHECK(std::holds_alternative<TextLayer>(document.layer(upper.id)->payload));
}

void representativeCaptureCost()
{
    for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}}) {
        Document document({extent});
        const auto id = add(document, surface(extent, {160, 120, 80, 255}));
        const auto start = std::chrono::steady_clock::now();
        const auto reference = capture(document, id, id);
        const auto milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        CHECK(reference.snapshotBytes() == std::size_t(extent.width) * extent.height * 4);
        CHECK(encodeColor(reference.sample({extent.width - .5, extent.height - .5})) == Rgba8(160, 120, 80, 255));
        std::cout << "Clone reference " << extent.width << 'x' << extent.height << ": "
                  << milliseconds << " ms; " << reference.snapshotBytes() << " snapshot bytes\n";
    }
}

void cancellationAndChangingSourceDuringCapture()
{
    Document document({{128, 256}});
    auto pixels = surface({128, 256}, {120, 140, 160, 255});
    const auto id = add(document, pixels);
    const auto revision = pixels->revision();
    int checkpoints = 0;
    std::string diagnostic;
    CHECK(!CloneReference::capture(document, id, id, CloneSampleSource::SourceLayer,
        diagnostic, CloneReference::defaultSnapshotLimit, true,
        [&] { return ++checkpoints == 3; }));
    CHECK(checkpoints == 3);
    CHECK(diagnostic.find("cancelled") != std::string::npos);
    CHECK(pixels->revision() == revision);

    checkpoints = 0;
    CHECK(!CloneReference::capture(document, id, id, CloneSampleSource::SourceLayer,
        diagnostic, CloneReference::defaultSnapshotLimit, true,
        [&] {
            // A source edit while cooperative capture is in progress must be
            // detected even if the changed location was copied in an earlier row.
            if (++checkpoints == 2) put(*pixels, 0, 0, {255, 0, 0, 255});
            return false;
        }));
    CHECK(diagnostic.find("changed during capture") != std::string::npos);
    const auto next = capture(document, id, id);
    CHECK(encodeColor(next.sample({.5, .5})) == Rgba8(255, 0, 0, 255));
}

void suppliedDocumentCacheFreezesItsOwnGeometryAndPreservesViewportCache()
{
    Document document({{40, 40}});
    auto layer = Layer::text("Editable text with a viewport cache", TextLayer {});
    const auto source = layer.id;
    auto viewportPixels = surface({2, 2}, {0, 255, 0, 255});
    auto viewportCache = std::make_shared<LayerRenderCache>();
    viewportCache->surface = viewportPixels;
    viewportCache->pixelsToLocal = {.m00 = 2, .m02 = -2, .m11 = 2, .m12 = -2};
    viewportCache->logicalExtent = {2, 2};
    layer.renderCache = viewportCache;
    layer.localToDocument = {-1, .2, 20, .3, 1.2, 15};
    layer.opacity = .5F;
    const auto localToDocument = layer.localToDocument;
    CHECK(document.insertLayer(0, std::move(layer)));
    const auto target = add(document, surface({40, 40}));

    auto preparedPixels = surface({4, 4}, {220, 30, 80, 180});
    put(*preparedPixels, 2, 1, {20, 90, 160, 180});
    auto preparedCache = std::make_shared<LayerRenderCache>();
    preparedCache->surface = preparedPixels;
    preparedCache->pixelsToLocal = {.m02 = -1, .m12 = -2};
    preparedCache->logicalExtent = {2, 2};
    const std::array overrides {SampleCacheOverride {source, preparedCache}};
    const auto point = localToDocument.map(preparedCache->pixelsToLocal.map({2.5, 1.5}));
    const auto revision = document.revision();
    const auto viewportRevision = viewportPixels->revision();
    std::string diagnostic;
    const auto raw = CloneReference::capture(document, target, source, CloneSampleSource::SourceLayer,
        diagnostic, CloneReference::defaultSnapshotLimit, false, {}, overrides);
    CHECK(raw.has_value());
    if (!raw) return;
    CHECK(raw->snapshotBytes() == 4 * 4 * 4);
    CHECK(encodeColor(raw->sample(point)) == Rgba8(20, 90, 160, 180));
    CHECK(raw->validSample(point));
    // This center lies outside the low-density viewport cache quad, proving
    // the reference uses the prepared cache's bearings and intrinsic extent.
    const auto extraPoint = localToDocument.map({2.5, 1.5});
    CHECK(raw->validSample(extraPoint));
    CHECK(encodeColor(raw->sample(extraPoint)) == Rgba8(220, 30, 80, 180));
    for (const auto mode : {CloneSampleSource::CurrentAndBelow, CloneSampleSource::AllVisible}) {
        const auto rendered = CloneReference::capture(document, target, source, mode,
            diagnostic, CloneReference::defaultSnapshotLimit, true, {}, overrides);
        CHECK(rendered.has_value());
        if (rendered) {
            CHECK(encodeColor(rendered->sample(point)) == Rgba8(20, 90, 160, 90));
            CHECK(encodeColor(rendered->sampleDestination(point)) == Rgba8(20, 90, 160, 90));
        }
    }
    CHECK(document.revision() == revision);
    CHECK(document.layer(source)->renderCache == viewportCache);
    CHECK(viewportCache->surface == viewportPixels);
    CHECK(viewportPixels->revision() == viewportRevision);
    CHECK(viewportCache->pixelsToLocal == AffineTransform(2, 0, -2, 0, 2, -2));
    CHECK(std::holds_alternative<TextLayer>(document.layer(source)->payload));

    put(*preparedPixels, 2, 1, {255, 255, 0, 255});
    preparedCache->pixelsToLocal.m02 = 100;
    CHECK(encodeColor(raw->sample(point)) == Rgba8(20, 90, 160, 180));
    CHECK(document.layer(source)->renderCache == viewportCache);
}

void crossSurfaceAndMetadataChangesDuringCaptureAreRejected()
{
    for (const bool metadataChange : {false, true}) {
        Document document({{128, 128}});
        auto firstPixels = surface({128, 128}, {120, 140, 160, 255});
        const auto source = add(document, firstPixels);
        const auto target = add(document, surface({128, 128}));
        int checkpoints = 0;
        std::string diagnostic;
        const auto result = CloneReference::capture(document, target, source, CloneSampleSource::AllVisible,
            diagnostic, CloneReference::defaultSnapshotLimit, true,
            [&] {
                // Two 64-row chunks finish the first surface. Mutate it only
                // after the next surface begins, past its local revision check.
                if (++checkpoints == 3) {
                    if (metadataChange) CHECK(document.setLayerOpacity(source, .5F));
                    else put(*firstPixels, 0, 0, {255, 0, 0, 255});
                }
                return false;
            });
        CHECK(checkpoints >= 3);
        CHECK(!result);
        CHECK(diagnostic.find("changed during capture") != std::string::npos);
    }
}

void preparedCacheChangedAfterItsCopyIsRejected()
{
    Document document({{128, 128}});
    auto layer = Layer::text("Typed source", TextLayer {});
    const auto source = layer.id;
    auto viewport = std::make_shared<LayerRenderCache>();
    viewport->surface = surface({2, 2}, {0, 255, 0, 255});
    layer.renderCache = viewport;
    CHECK(document.insertLayer(0, std::move(layer)));
    const auto target = add(document, surface({128, 128}));
    auto preparedPixels = surface({128, 128}, {120, 140, 160, 255});
    auto prepared = std::make_shared<LayerRenderCache>();
    prepared->surface = preparedPixels;
    const std::array overrides {SampleCacheOverride {source, prepared}};
    int checkpoints = 0;
    std::string diagnostic;
    const auto result = CloneReference::capture(document, target, source, CloneSampleSource::CurrentAndBelow,
        diagnostic, CloneReference::defaultSnapshotLimit, true,
        [&] {
            if (++checkpoints == 3) put(*preparedPixels, 0, 0, {255, 0, 0, 255});
            return false;
        }, overrides);
    CHECK(checkpoints >= 3);
    CHECK(!result);
    CHECK(diagnostic.find("changed during capture") != std::string::npos);
    CHECK(document.layer(source)->renderCache == viewport);
}

} // namespace

int main()
{
    try {
        immutableOverlapAndRefresh();
        stableSourceAndSelectionIndependence();
        alphaAwareContinuousSamplingAndSourceBounds();
        transformedSourcesAndIndependentDestinations();
        intrinsicEffectsAndRenderedCutoff();
        emptyRetouchLayerUsesUnderlyingContext();
        cropChamferAndHierarchyVisibility();
        typedCachesAreFrozenWithoutFlattening();
        snapshotBudgetAndSurfaceDeduplication();
        unrelatedTypedLayersDoNotRequireRasterization();
        cancellationAndChangingSourceDuringCapture();
        suppliedDocumentCacheFreezesItsOwnGeometryAndPreservesViewportCache();
        crossSurfaceAndMetadataChangesDuringCaptureAreRejected();
        preparedCacheChangedAfterItsCopyIsRejected();
        representativeCaptureCost();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    if (!failures) std::cout << "Clone reference tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
