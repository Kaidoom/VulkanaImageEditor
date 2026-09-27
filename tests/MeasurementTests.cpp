#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/core/RulerTicks.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

using namespace imageeditor::core;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)
bool near(double actual, double expected, double tolerance = 1e-9)
{
    return std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

// Large-document bounds must use geometry only: any pixel/dirty-cache access
// is an observable failure, without allocating a 5K raster just to measure it.
class GeometryOnlySurface final : public RasterSurface {
public:
    explicit GeometryOnlySurface(Extent2u size) : size_(size) {}
    SurfaceId id() const noexcept override { return 123; }
    Extent2u extent() const noexcept override { return size_; }
    Revision revision() const noexcept override { return 1; }
    DirtySet dirtySince(Revision) const override { ++pixelOperations; return {}; }
    void copyRgba8(RectI, std::span<std::byte>, std::size_t) const override { ++pixelOperations; }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override { ++pixelOperations; return {}; }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override { ++pixelOperations; return {}; }
    mutable int pixelOperations {0};
private:
    Extent2u size_;
};

void distancesAndAngles()
{
    const auto value = measurementValues({{1.25, -4.75}, {4.25, -.75}});
    CHECK(value);
    if (!value) return;
    CHECK((value->delta == Vec2d {3, 4}));
    CHECK(near(value->distance, 5));
    CHECK(value->angleDegrees && near(*value->angleDegrees, 53.13010235415598));
    const std::array<Vec2d, 8> compass {{{1, 0}, {1, 1}, {0, 1}, {-1, 1},
        {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};
    const std::array<double, 8> angles {0, 45, 90, 135, 180, -135, -90, -45};
    for (std::size_t i = 0; i < compass.size(); ++i) {
        const auto sample = measurementValues({{}, compass[i]});
        CHECK(sample && sample->angleDegrees && near(*sample->angleDegrees, angles[i]));
    }
    const auto zero = measurementValues({{.001, -.003}, {.001, -.003}});
    CHECK(zero && zero->distance == 0 && !zero->angleDegrees);
    const auto signedZero = measurementValues({{0, 0}, {-1, -0.0}});
    CHECK(signedZero && signedZero->angleDegrees == 180.0 && !std::signbit(signedZero->delta.y));
    const auto tiny = measurementValues({{}, {1e-15, -1e-15}});
    CHECK(tiny && tiny->distance > 0 && tiny->angleDegrees == -45.0);
    const auto huge = measurementValues({{-100000, 200000}, {200000, 600000}});
    CHECK(huge && near(huge->distance, 500000));
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    CHECK(!measurementValues({{nan, 0}, {1, 1}}));
    CHECK(!measurementValues({{0, 0}, {infinity, 1}}));
    const auto maximum = std::numeric_limits<double>::max();
    CHECK(!measurementValues({{-maximum, 0}, {maximum, 0}}));
}

void constrainedLines()
{
    const Vec2d anchor {10.25, -20.75};
    constexpr double radius = 31.625;
    for (int direction = -4; direction <= 4; ++direction) {
        const auto expected = direction * std::numbers::pi / 4;
        for (const auto deviation : {-0.25, 0.0, 0.25}) {
            const auto pointer = anchor + Vec2d {std::cos(expected + deviation) * radius,
                std::sin(expected + deviation) * radius};
            const auto snapped = constrainLineEndpoint(anchor, pointer);
            CHECK(near(snapped.x, anchor.x + std::cos(expected) * radius));
            CHECK(near(snapped.y, anchor.y + std::sin(expected) * radius));
            const auto values = measurementValues({anchor, snapped});
            CHECK(values && near(values->distance, radius));
        }
    }
    CHECK(constrainLineEndpoint(anchor, anchor) == anchor);
}

void transformedAndContainerBounds()
{
    Document document({.extent = {5120, 2880}});
    auto surface = std::make_shared<GeometryOnlySurface>(Extent2u {32, 24});
    auto raster = Layer::raster("Raster", surface);
    raster.localToDocument = {1, 0, 10, 0, 1, -20};
    raster.visible = false;
    const auto rasterId = raster.id;
    CHECK(document.insertLayer(0, std::move(raster)));
    ShapeLayer shapeData;
    shapeData.size = {31.5, 19.25};
    shapeData.strokeEnabled = true;
    shapeData.strokeWidth = 100; // Logical bounds, deliberately not stroke/cache extents.
    auto shape = Layer::shape("Shape", shapeData);
    shape.localToDocument = {-1, .5, 100, .25, 2, -40};
    const auto shapeId = shape.id;
    CHECK(document.insertLayer(1, std::move(shape)));
    auto text = Layer::text("Text", {});
    auto cache = std::make_shared<LayerRenderCache>();
    cache->logicalExtent = {24, 18};
    cache->surface = surface;
    cache->pixelsToLocal = {.m00 = .25, .m02 = -100, .m11 = .25, .m12 = -100};
    text.renderCache = cache;
    text.localToDocument = {0, -2, -20, 1, .5, 50};
    const auto textId = text.id;
    CHECK(document.insertLayer(2, std::move(text)));
    const auto group = makeLayerId(), nested = makeLayerId(), folder = makeLayerId();
    LayerTree tree {{folder}, {
        {folder, "Folder", ContainerKind::Folder, ColorLabel::None, {group}, false},
        {group, "Group", ContainerKind::Group, ColorLabel::None, {rasterId, nested}},
        {nested, "Nested", ContainerKind::Group, ColorLabel::None, {shapeId, textId}},
    }};
    CHECK(document.replaceStructure(document.tree(), tree));
    document.markSaved();
    const auto revision = document.revision(), contentState = document.contentState();
    const auto one = selectedLayerBounds(document, std::array {shapeId});
    CHECK(one);
    if (one) {
        CHECK((one->minimum == Vec2d {68.5, -40}));
        CHECK((one->maximum == Vec2d {109.625, 6.375}));
        CHECK(near(one->extent().width, 41.125));
        CHECK(near(one->extent().height, 46.375));
    }
    const auto all = selectedLayerBounds(document, std::array {rasterId, shapeId, textId});
    CHECK(all);
    if (all) {
        CHECK((all->minimum == Vec2d {-56, -40}));
        CHECK((all->maximum == Vec2d {109.625, 83}));
        CHECK(near(all->extent().width, 165.625));
        CHECK(near(all->extent().height, 123));
    }
    CHECK(selectedLayerBounds(document, std::array {group}) == all);
    CHECK(selectedLayerBounds(document, std::array {folder}) == all);
    CHECK(selectedLayerBounds(document, std::array {folder, group, nested, shapeId, shapeId}) == all);
    for (int i = 0; i < 1000; ++i)
        CHECK(selectedLayerBounds(document, std::array {group}) == all);
    CHECK(surface->pixelOperations == 0);
    CHECK(document.layer(textId)->renderCache == cache);
    CHECK(document.revision() == revision && document.contentState() == contentState && !document.isModified());
    CHECK(!selectedLayerBounds(document, std::span<const LayerId> {}));
    CHECK(!selectedLayerBounds(document, std::array {makeLayerId()}));
    CHECK(!selectedLayerBounds(document, std::array {rasterId, makeLayerId()}));
    document.layer(textId)->renderCache.reset();
    CHECK(!selectedLayerBounds(document, std::array {textId}));
    CHECK(!selectedLayerBounds(document, std::array {group}));
    document.layer(rasterId)->localToDocument.m02 = std::numeric_limits<double>::infinity();
    CHECK(!selectedLayerBounds(document, std::array {rasterId}));
}

void highResolutionAndDegenerateGeometry()
{
    Document document({.extent = {5120, 2880}});
    for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}}) {
        auto surface = std::make_shared<GeometryOnlySurface>(extent);
        auto layer = Layer::raster("Geometry only", surface);
        const auto id = layer.id;
        CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
        const auto bounds = selectedLayerBounds(document, std::array {id});
        CHECK(bounds && bounds->extent().width == extent.width && bounds->extent().height == extent.height);
        CHECK(surface->pixelOperations == 0);
    }
    ShapeLayer line;
    line.kind = ShapeKind::Line;
    line.size = {100.25, 0};
    line.points = {{0, 0}, {100.25, 0}};
    auto layer = Layer::shape("Flat line", line);
    const auto id = layer.id;
    CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
    const auto bounds = selectedLayerBounds(document, std::array {id});
    CHECK(bounds && bounds->extent().width == 100.25 && bounds->extent().height == 0);
}

void rulerMappingAndClipping()
{
    const auto ticks = makeRulerTicks(1, 0, 0, 500, 1000);
    CHECK(ticks.majorStep == 100 && ticks.minorStep == 20);
    CHECK(ticks.ticks.size() == 26);
    for (std::size_t i = 0; i < ticks.ticks.size(); ++i) {
        CHECK(ticks.ticks[i].document == double(i * 20));
        CHECK(ticks.ticks[i].logical == double(i * 20));
        CHECK(ticks.ticks[i].major == (i % 5 == 0));
    }
    const auto panned = makeRulerTicks(1, -150, 0, 1000, 10000);
    CHECK(!panned.ticks.empty());
    if (!panned.ticks.empty()) {
        CHECK(panned.ticks.front().document == 160 && panned.ticks.front().logical == 10);
        CHECK(!panned.ticks.front().major);
    }
    const auto margin = makeRulerTicks(1, 100, 40, 500, 300);
    CHECK(!margin.ticks.empty());
    if (!margin.ticks.empty()) {
        CHECK(margin.ticks.front().document == 0 && margin.ticks.front().logical == 100);
        CHECK(margin.ticks.back().document == 300 && margin.ticks.back().logical == 400);
    }
    CHECK(makeRulerTicks(1, -150, 0, 1000, 100).ticks.empty());
    CHECK(makeRulerTicks(1, 2000, 0, 1000, 100).ticks.empty());
    // Two strips at different inner dock boundaries retain the same document
    // zero and tick positions; only their clipped visible ranges differ.
    const auto docked = makeRulerTicks(1.25, 27.5, 340, 700, 5000);
    const auto floating = makeRulerTicks(1.25, 27.5, 0, 1200, 5000);
    CHECK(docked.majorStep == floating.majorStep);
    for (const auto& tick : docked.ticks) {
        CHECK(std::ranges::any_of(floating.ticks, [&tick](auto other) {
            return tick.document == other.document && tick.logical == other.logical && tick.major == other.major;
        }));
    }
    // DPR never enters this document/logical mapping; fractional logical zoom
    // and offsets are retained until the ruler's device-space drawing stage.
    const auto fractional = makeRulerTicks(.375, 51.125, 10.25, 903.5, 5120);
    for (const auto& tick : fractional.ticks)
        CHECK(near(tick.logical, 51.125 + tick.document * .375));
}

void adaptiveAndBoundedTicks()
{
    for (const auto zoom : {.0001, .05, .125, .5, 1.0, 1.25, 2.0, 16.0, 1000.0, 1e-100, 1e100}) {
        const auto ticks = makeRulerTicks(zoom, 0, 0, 2048, 1000000);
        CHECK(ticks.ticks.size() <= 4096);
        CHECK(ticks.majorStep * zoom >= 80 * (1 - 1e-12));
        CHECK(ticks.majorStep * zoom <= 200 * (1 + 1e-12));
        CHECK(ticks.minorStep * zoom >= 8);
        for (std::size_t i = 0; i < ticks.ticks.size(); ++i) {
            const auto& tick = ticks.ticks[i];
            CHECK(std::isfinite(tick.document) && std::isfinite(tick.logical));
            CHECK(tick.document >= 0 && tick.document <= 1000000 && tick.logical >= 0 && tick.logical <= 2048);
            CHECK(near(tick.logical, tick.document * zoom));
            if (i) CHECK(tick.logical > ticks.ticks[i - 1].logical && tick.document > ticks.ticks[i - 1].document);
        }
    }
    const auto large = makeRulerTicks(1, 0, 0, 1e12, 1e12);
    CHECK(!large.ticks.empty() && large.ticks.size() <= 4096);
    const auto extreme = makeRulerTicks(1, -1e300, 0, 1000, 1e301);
    CHECK(extreme.ticks.size() <= 4096);
    for (const auto& tick : extreme.ticks) CHECK(std::isfinite(tick.logical));
    for (const auto invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()}) {
        CHECK(makeRulerTicks(invalid, 0, 0, 100, 100).ticks.empty());
        CHECK(makeRulerTicks(1, 0, 0, invalid, 100).ticks.empty());
        CHECK(makeRulerTicks(1, 0, 0, 100, invalid).ticks.empty());
    }
    CHECK(makeRulerTicks(1, std::numeric_limits<double>::quiet_NaN(), 0, 100, 100).ticks.empty());
    CHECK(makeRulerTicks(1, 0, std::numeric_limits<double>::infinity(), 100, 100).ticks.empty());
    CHECK(makeRulerTicks(std::numeric_limits<double>::denorm_min(), 0, 0, 100, 100).ticks.empty());
}
}

int main()
{
    distancesAndAngles();
    constrainedLines();
    transformedAndContainerBounds();
    highResolutionAndDegenerateGeometry();
    rulerMappingAndClipping();
    adaptiveAndBoundedTicks();
    if (failures) return EXIT_FAILURE;
    std::cout << "Measurement and ruler geometry tests passed\n";
    return EXIT_SUCCESS;
}
