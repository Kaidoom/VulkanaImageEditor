#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EditorColors.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

// A synthetic CPU-authoritative surface makes accidental full-surface reads
// observable without allocating a 4K/5K image for a point-sampling test.
class CountingSurface final : public RasterSurface {
public:
    using PixelFunction = std::function<Rgba8(std::int32_t, std::int32_t)>;

    CountingSurface(Extent2u extent, PixelFunction pixel)
        : extent_(extent)
        , pixel_(std::move(pixel))
        , id_(nextId_++)
    {
    }

    [[nodiscard]] SurfaceId id() const noexcept override { return id_; }
    [[nodiscard]] Extent2u extent() const noexcept override { return extent_; }
    [[nodiscard]] Revision revision() const noexcept override { return 1; }
    [[nodiscard]] DirtySet dirtySince(Revision revision) const override
    {
        return {.revision = 1, .fullRefresh = revision != 1, .regions = {}};
    }

    void copyRgba8(RectI region, std::span<std::byte> destination,
        std::size_t destinationStride) const override
    {
        ++readCalls;
        const bool validRegion = region.width >= 1 && region.width <= 2
            && region.height >= 1 && region.height <= 2
            && region.x >= 0 && region.y >= 0
            && region.right() <= static_cast<std::int32_t>(extent_.width)
            && region.bottom() <= static_cast<std::int32_t>(extent_.height);
        CHECK(validRegion);
        if (!validRegion) {
            return;
        }
        const auto texels = static_cast<std::size_t>(region.width)
            * static_cast<std::size_t>(region.height);
        texelsRead += texels;
        maximumReadTexels = std::max(maximumReadTexels, texels);
        CHECK(region.width >= 1 && region.width <= 2);
        CHECK(region.height >= 1 && region.height <= 2);
        CHECK(region.x >= 0 && region.y >= 0);
        CHECK(region.right() <= static_cast<std::int32_t>(extent_.width));
        CHECK(region.bottom() <= static_cast<std::int32_t>(extent_.height));
        const auto rowBytes = static_cast<std::size_t>(region.width) * 4U;
        CHECK(destinationStride >= rowBytes);
        CHECK(destination.size() >= destinationStride
                * static_cast<std::size_t>(region.height - 1) + rowBytes);
        if (region.empty() || region.width > 2 || region.height > 2
            || destinationStride < rowBytes
            || destination.size() < destinationStride
                    * static_cast<std::size_t>(region.height - 1) + rowBytes) {
            return;
        }
        for (std::int32_t y = 0; y < region.height; ++y) {
            for (std::int32_t x = 0; x < region.width; ++x) {
                const auto color = pixel_(region.x + x, region.y + y);
                const auto offset = static_cast<std::size_t>(y) * destinationStride
                    + static_cast<std::size_t>(x) * 4U;
                destination[offset] = static_cast<std::byte>(color.red);
                destination[offset + 1U] = static_cast<std::byte>(color.green);
                destination[offset + 2U] = static_cast<std::byte>(color.blue);
                destination[offset + 3U] = static_cast<std::byte>(color.alpha);
            }
        }
    }

    [[nodiscard]] DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override
    {
        ++writeCalls;
        CHECK(false && "Color sampling must never write a raster surface");
        return {.revision = 1, .fullRefresh = false, .regions = {}};
    }

    [[nodiscard]] DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override
    {
        ++writeCalls;
        CHECK(false && "Color sampling must never swap a raster surface");
        return {.revision = 1, .fullRefresh = false, .regions = {}};
    }

    mutable std::size_t readCalls {0};
    mutable std::size_t texelsRead {0};
    mutable std::size_t maximumReadTexels {0};
    std::size_t writeCalls {0};

private:
    inline static SurfaceId nextId_ {1000000};
    Extent2u extent_;
    PixelFunction pixel_;
    SurfaceId id_;
};

std::shared_ptr<CountingSurface> solidSurface(Extent2u extent, Rgba8 color)
{
    return std::make_shared<CountingSurface>(extent,
        [color](std::int32_t, std::int32_t) { return color; });
}

LayerId addRaster(Document& document, const std::shared_ptr<RasterSurface>& surface,
    const std::string& name = "Sample source")
{
    auto layer = Layer::raster(name, surface);
    const auto id = layer.id;
    CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
    return id;
}

ColorSample activeSample(const Document& document, LayerId layerId, Vec2d point)
{
    return sampleDocumentColor(document, layerId, point, ColorSampleSource::ActiveLayer);
}

ColorSample mergedSample(const Document& document, Vec2d point)
{
    return sampleDocumentColor(document, {}, point, ColorSampleSource::MergedVisible);
}

void activeIdentityPreservesVisibleBytesAndCanonicalizesTransparentRgb()
{
    const auto pixel = [](std::int32_t x, std::int32_t y) {
        const auto alpha = y == 0 ? 0 : y == 1 ? x : y == 2 ? 128 : 255;
        return Rgba8 {static_cast<std::uint8_t>(x),
            static_cast<std::uint8_t>(255 - x),
            static_cast<std::uint8_t>((x * 73) % 256),
            static_cast<std::uint8_t>(alpha)};
    };
    Document document(CanvasSpec {.extent = {256, 4}});
    auto surface = std::make_shared<CountingSurface>(Extent2u {256, 4}, pixel);
    const auto layerId = addRaster(document, surface);
    CHECK(document.setLayerVisibility(layerId, false));
    CHECK(document.setLayerOpacity(layerId, 0.0F));
    const auto documentRevision = document.revision();
    for (std::int32_t y = 0; y < 4; ++y) {
        for (std::int32_t x = 0; x < 256; ++x) {
            const auto sampled = activeSample(document, layerId,
                {static_cast<double>(x) + 0.01, static_cast<double>(y) + 0.99});
            CHECK(sampled.available());
            const auto stored = pixel(x, y);
            CHECK(sampled.color == (stored.alpha == 0 ? Rgba8 {} : stored));
            CHECK(sampled.layersVisited == 1);
            CHECK(sampled.texelsRead == 1);
        }
    }
    CHECK(surface->maximumReadTexels == 1);
    CHECK(surface->readCalls == 1024);
    CHECK(surface->texelsRead == 1024);
    CHECK(surface->writeCalls == 0);
    CHECK(surface->revision() == 1);
    CHECK(surface->dirtySince(1).empty());
    CHECK(document.revision() == documentRevision);
}

void mergedSourceOverIsLinearAndRespectsStackState()
{
    Document document(CanvasSpec {.extent = {3, 3}});
    const auto blue = addRaster(document,
        solidSurface({3, 3}, {0, 0, 255, 255}), "Blue bottom");
    const auto red = addRaster(document,
        solidSurface({3, 3}, {255, 0, 0, 128}), "Red top");
    CHECK(document.setLayerOpacity(red, 0.5F));
    auto sampled = mergedSample(document, {1.2, 1.7});
    CHECK(sampled.available());
    // Red contributes 128/255 * 0.5 in linear light over opaque blue.
    CHECK(sampled.color == Rgba8({137, 0, 224, 255}));
    CHECK(sampled.texelsRead == 2);
    CHECK(sampled.layersVisited == 2);

    CHECK(document.moveLayer(blue, 1));
    CHECK(mergedSample(document, {1.2, 1.7}).color == Rgba8({0, 0, 255, 255}));
    CHECK(document.moveLayer(blue, 0));
    CHECK(document.setLayerVisibility(red, false));
    CHECK(mergedSample(document, {1.2, 1.7}).color == Rgba8({0, 0, 255, 255}));
    CHECK(document.setLayerVisibility(red, true));
    CHECK(document.setLayerOpacity(red, 0.0F));
    CHECK(mergedSample(document, {1.2, 1.7}).color == Rgba8({0, 0, 255, 255}));
    CHECK(activeSample(document, red, {1.2, 1.7}).color == Rgba8({255, 0, 0, 128}));

    CHECK(document.setLayerVisibility(blue, false));
    CHECK(document.setLayerOpacity(red, 0.5F));
    sampled = mergedSample(document, {1.2, 1.7});
    // The document is composited over transparency, not a checker or black.
    CHECK(sampled.color == Rgba8({255, 0, 0, 64}));
    const auto hiddenRgb = addRaster(document,
        solidSurface({3, 3}, {19, 231, 174, 0}), "Transparent colored pixels");
    CHECK(mergedSample(document, {1.2, 1.7}).color == sampled.color);
    CHECK(activeSample(document, hiddenRgb, {1.2, 1.7}).color
        == Rgba8({0, 0, 0, 0}));
    CHECK(document.setLayerVisibility(red, false));
    CHECK(mergedSample(document, {1.2, 1.7}).color == Rgba8({0, 0, 0, 0}));

    Document translucent(CanvasSpec {.extent = {1, 1}});
    const auto translucentRed = addRaster(translucent,
        solidSurface({1, 1}, {255, 0, 0, 128}));
    addRaster(translucent, solidSurface({1, 1}, {0, 0, 255, 128}));
    CHECK(mergedSample(translucent, {0.1, 0.1}).color == Rgba8({156, 0, 213, 192}));
    CHECK(translucent.moveLayer(translucentRed, 1));
    CHECK(mergedSample(translucent, {0.1, 0.1}).color == Rgba8({213, 0, 156, 192}));
}

std::shared_ptr<CountingSurface> cornerSurface()
{
    return std::make_shared<CountingSurface>(Extent2u {2, 2},
        [](std::int32_t x, std::int32_t y) {
            constexpr std::array corners {
                Rgba8 {255, 0, 0, 255}, Rgba8 {0, 255, 0, 128},
                Rgba8 {0, 0, 255, 64}, Rgba8 {255, 255, 255, 0}};
            return corners[static_cast<std::size_t>(y * 2 + x)];
        });
}

void transformedSamplingUsesTexelCentersAndClampedBilinearFiltering()
{
    Document document(CanvasSpec {.extent = {8, 8}});
    auto surface = cornerSurface();
    const auto layerId = addRaster(document, surface);
    auto* layer = document.layer(layerId);
    CHECK(layer != nullptr);
    if (!layer) {
        return;
    }
    layer->localToDocument = {.m02 = 2.0, .m12 = 1.0};
    CHECK(activeSample(document, layerId, {2.9, 1.2}).color == Rgba8({255, 0, 0, 255}));
    CHECK(activeSample(document, layerId, {3.2, 1.8}).color == Rgba8({0, 255, 0, 128}));
    CHECK(activeSample(document, layerId, {2.1, 2.1}).color == Rgba8({0, 0, 255, 64}));
    CHECK(activeSample(document, layerId, {3.1, 2.1}).color == Rgba8({0, 0, 0, 0}));
    CHECK(activeSample(document, layerId, {1.8, 1.2}).texelsRead == 0);
    CHECK(activeSample(document, layerId, {4.0, 1.2}).texelsRead == 0);

    layer->localToDocument = {.m02 = 0.5, .m12 = 0.5};
    auto sampled = activeSample(document, layerId, {1.2, 1.8});
    // Four equal weights in premultiplied linear light. Unassociation gives
    // RGB = (255,128,64)/447 before sRGB encoding; hidden white contributes zero.
    CHECK(sampled.color == Rgba8({199, 146, 106, 112}));
    CHECK(sampled.texelsRead == 4);
    CHECK(mergedSample(document, {1.2, 1.8}).color == sampled.color);
    CHECK(activeSample(document, layerId, {0.1, 0.1}).color == Rgba8({255, 0, 0, 255}));
    CHECK(activeSample(document, layerId, {2.1, 0.1}).color == Rgba8({0, 0, 0, 0}));
    CHECK(activeSample(document, layerId, {2.1, 0.1}).texelsRead == 0);

    layer->localToDocument = {.m00 = 2.0, .m11 = 2.0};
    // Top row weights 3/4 and 1/4, with source alphas 255 and 128.
    CHECK(activeSample(document, layerId, {1.1, 0.1}).color == Rgba8({238, 106, 0, 223}));
    layer->localToDocument = {.m00 = -1.0, .m02 = 2.0};
    CHECK(activeSample(document, layerId, {0.1, 0.1}).color == Rgba8({0, 255, 0, 128}));
    layer->localToDocument = {.m00 = 0.0, .m01 = -1.0,
        .m02 = 2.0, .m10 = 1.0, .m11 = 0.0};
    CHECK(activeSample(document, layerId, {0.1, 0.1}).color == Rgba8({0, 0, 255, 64}));
    layer->localToDocument = {.m01 = 0.5};
    CHECK(activeSample(document, layerId, {1.1, 1.1}).color == Rgba8({0, 0, 255, 48}));

    const auto readsBeforeInvalidTransforms = surface->readCalls;
    for (const auto transform : std::array {
             AffineTransform {.m00 = 0.0},
             AffineTransform {.m02 = std::numeric_limits<double>::quiet_NaN()},
             AffineTransform {.m11 = std::numeric_limits<double>::infinity()},
         }) {
        layer->localToDocument = transform;
        sampled = activeSample(document, layerId, {1.1, 1.1});
        CHECK(sampled.available());
        CHECK(sampled.color == Rgba8({0, 0, 0, 0}));
        CHECK(sampled.texelsRead == 0);
    }
    CHECK(surface->readCalls == readsBeforeInvalidTransforms);
    CHECK(surface->maximumReadTexels == 4);
    CHECK(surface->writeCalls == 0);
}

void documentClipAndOneTexelEdgesNeverReadOutsideStorage()
{
    Document document(CanvasSpec {.extent = {2, 2}});
    auto surface = solidSurface({4, 4}, {39, 67, 145, 203});
    const auto layerId = addRaster(document, surface);
    CHECK(activeSample(document, layerId, {1.999, 1.999}).color == Rgba8({39, 67, 145, 203}));
    const auto initialReads = surface->readCalls;
    for (const auto point : std::array {
             Vec2d {-0.001, 0.5}, Vec2d {0.5, -0.001},
             Vec2d {2.0, 0.5}, Vec2d {0.5, 2.0},
             Vec2d {std::numeric_limits<double>::quiet_NaN(), 0.5},
             Vec2d {0.5, std::numeric_limits<double>::infinity()},
         }) {
        CHECK(activeSample(document, layerId, point).status == ColorSampleStatus::OutsideCanvas);
        CHECK(mergedSample(document, point).status == ColorSampleStatus::OutsideCanvas);
    }
    CHECK(surface->readCalls == initialReads);

    Document tinyDocument(CanvasSpec {.extent = {3, 3}});
    auto tiny = solidSurface({1, 1}, {17, 91, 203, 144});
    const auto tinyId = addRaster(tinyDocument, tiny);
    tinyDocument.layer(tinyId)->localToDocument = {.m00 = 3.0, .m11 = 3.0};
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            const auto sampled = activeSample(tinyDocument, tinyId,
                {static_cast<double>(x), static_cast<double>(y)});
            CHECK(sampled.color == Rgba8({17, 91, 203, 144}));
            CHECK(sampled.texelsRead == 1);
        }
    }
    CHECK(tiny->maximumReadTexels == 1);
}

void unavailableAndUnsupportedSourcesAreExplicit()
{
    Document document(CanvasSpec {.extent = {2, 2}});
    CHECK(mergedSample(document, {0.1, 0.1}).available());
    CHECK(mergedSample(document, {0.1, 0.1}).color == Rgba8({0, 0, 0, 0}));
    CHECK(sampleDocumentColor(document, {}, {0.1, 0.1}, ColorSampleSource::ActiveLayer).status
        == ColorSampleStatus::NoActiveRaster);
    CHECK(activeSample(document, 999999999, {0.1, 0.1}).status == ColorSampleStatus::NoActiveRaster);

    auto text = Layer::text("Unsupported editable text",
        TextLayer {.utf8 = "Text", .defaultStyle = {}, .runs = {}});
    const auto textId = text.id;
    CHECK(document.insertLayer(0, std::move(text)));
    CHECK(activeSample(document, textId, {0.1, 0.1}).status == ColorSampleStatus::NoActiveRaster);
    CHECK(mergedSample(document, {0.1, 0.1}).status == ColorSampleStatus::UnsupportedLayer);
    CHECK(document.setLayerVisibility(textId, false));
    CHECK(mergedSample(document, {0.1, 0.1}).available());
    CHECK(document.setLayerVisibility(textId, true));
    CHECK(document.setLayerOpacity(textId, 0.0F));
    CHECK(mergedSample(document, {0.1, 0.1}).available());

    auto nullRaster = Layer::raster("Missing surface", solidSurface({2, 2}, {}));
    const auto nullId = nullRaster.id;
    std::get<RasterLayer>(nullRaster.payload).surface.reset();
    CHECK(document.insertLayer(document.layers().size(), std::move(nullRaster)));
    CHECK(activeSample(document, nullId, {0.1, 0.1}).status == ColorSampleStatus::NoActiveRaster);
    CHECK(mergedSample(document, {0.1, 0.1}).available());
}

void viewportZoomAndPanDoNotChangeTheDocumentSample()
{
    Document document(CanvasSpec {.extent = {256, 128}});
    auto surface = std::make_shared<CountingSurface>(document.canvas().extent,
        [](std::int32_t x, std::int32_t y) {
            return Rgba8 {static_cast<std::uint8_t>(x),
                static_cast<std::uint8_t>(y), 143, 255};
        });
    const auto layerId = addRaster(document, surface);
    const Vec2d documentPoint {91.23, 44.67};
    const auto expected = activeSample(document, layerId, documentPoint).color;
    for (const double zoom : {0.05, 0.25, 1.0, 2.0, 8.0, 32.0}) {
        ViewportState viewport;
        viewport.setZoom(zoom);
        viewport.setPan({143.25, -37.5});
        const auto logical = viewport.documentToViewport(documentPoint,
            {256.0, 128.0}, {1400.0, 900.0});
        const auto roundTrip = viewport.viewportToDocument(logical,
            {256.0, 128.0}, {1400.0, 900.0});
        CHECK(activeSample(document, layerId, roundTrip).color == expected);
        CHECK(mergedSample(document, roundTrip).color == expected);
    }
}

void colorsAndPickingDoNotTouchDocumentHistoryOrClearRedo()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {4, 4}});
    auto surface = solidSurface({4, 4}, {28, 123, 211, 192});
    const auto layerId = addRaster(*document, surface);
    session.replaceDocument(std::move(document));
    CHECK(session.execute(std::make_unique<SetLayerVisibilityCommand>(layerId, false)));
    CHECK(session.undo());
    CHECK(session.history().canRedo());
    const auto documentRevision = session.document()->revision();
    const auto historyBytes = session.history().memoryUsed();
    const auto undoDepth = session.history().undoDepth();
    const auto redoDepth = session.history().redoDepth();

    EditorColors colors;
    const auto initialPrimary = colors.primary;
    const Rgba8 alternate {202, 37, 83, 71};
    colors.setColor(ColorSlot::Secondary, alternate);
    CHECK(colors.foreground() == initialPrimary);
    colors.active = ColorSlot::Secondary;
    CHECK(colors.foreground() == alternate);
    session.setColors(colors);
    const auto sampled = activeSample(*session.document(), layerId, {1.2, 1.7});
    CHECK(sampled.available());
    session.setForegroundColor(sampled.color);
    CHECK(session.colors().primary == initialPrimary);
    CHECK(session.colors().secondary == sampled.color);
    CHECK(session.foregroundColor() == sampled.color);
    colors = session.colors();
    colors.switchActive();
    CHECK(colors.active == ColorSlot::Primary);
    CHECK(colors.primary == initialPrimary);
    CHECK(colors.secondary == sampled.color);
    CHECK(colors.foreground() == initialPrimary);
    session.setColors(colors);

    CHECK(session.document()->revision() == documentRevision);
    CHECK(surface->revision() == 1);
    CHECK(surface->dirtySince(1).empty());
    CHECK(surface->writeCalls == 0);
    CHECK(session.history().memoryUsed() == historyBytes);
    CHECK(session.history().undoDepth() == undoDepth);
    CHECK(session.history().redoDepth() == redoDepth);
    CHECK(session.redo());
    CHECK(!session.document()->layer(layerId)->visible);
    CHECK(session.colors() == colors);

    auto replacement = std::make_unique<Document>(CanvasSpec {.extent = {2, 2}});
    addRaster(*replacement, solidSurface({2, 2}, {}));
    session.replaceDocument(std::move(replacement));
    CHECK(session.colors() == colors);
}

void retainedOffCanvasPixelsCanBePickedWithoutExpandingColorSampling()
{
    Document document(CanvasSpec {.extent = {32, 24}});
    const auto lower = addRaster(document, solidSurface({8, 8}, {90, 40, 10, 255}));
    const auto upper = addRaster(document, std::make_shared<CountingSurface>(Extent2u {8, 8},
        [](int x, int) { return Rgba8 {20, 80, 170, static_cast<std::uint8_t>(x < 4 ? 0 : 255)}; }));
    for (const auto offset : {Vec2d {-12, 0}, Vec2d {0, -12}, Vec2d {40, 0}, Vec2d {0, 32}}) {
        const AffineTransform transform {.m02 = offset.x, .m12 = offset.y};
        CHECK(document.setLayerTransform(lower, transform));
        CHECK(document.setLayerTransform(upper, transform));
        const Vec2d opaque {offset.x + 6.5, offset.y + 4.5};
        const Vec2d transparent {offset.x + 1.5, offset.y + 4.5};
        CHECK(hitTestRasterLayer(document, opaque) == upper);
        CHECK(hitTestRasterLayer(document, transparent) == lower);
        CHECK(!hitTestRasterLayer(document, {offset.x - 1, offset.y - 1}));
        CHECK(!activeSample(document, upper, opaque).available());
        CHECK(!mergedSample(document, opaque).available());
        CHECK(document.setLayerVisibility(upper, false));
        CHECK(hitTestRasterLayer(document, opaque) == lower);
        CHECK(document.setLayerVisibility(upper, true));
        CHECK(document.setLayerOpacity(upper, 0));
        CHECK(hitTestRasterLayer(document, opaque) == lower);
        CHECK(document.setLayerOpacity(upper, 1));
    }
    // The same inverse mapping/crop/mask semantics apply on the pasteboard,
    // including projective and flipped layers, rather than rectangle picking.
    const AffineTransform distorted {.m00 = -1.2, .m01 = .2, .m02 = -20,
        .m10 = .1, .m11 = .9, .m12 = -18, .m20 = .015, .m21 = -.01};
    CHECK(document.setLayerTransform(upper, distorted));
    const auto point = distorted.map({6.5, 4.5});
    CHECK(hitTestRasterLayer(document, point) == upper);
    CHECK(document.setLayerCrop(upper, RectD {0, 0, 3, 8}));
    CHECK(!hitTestRasterLayer(document, point));
    CHECK(document.setLayerCrop(upper, {}));
    auto blackMask = std::make_shared<LayerMask>(LayerMask {SelectionMask::filled({8, 8}, 0), {}, 0});
    CHECK(document.setLayerMask(upper, blackMask));
    CHECK(!hitTestRasterLayer(document, point));
    auto disabledMask = std::make_shared<LayerMask>(*blackMask);
    disabledMask->enabled = false;
    CHECK(document.setLayerMask(upper, disabledMask));
    CHECK(hitTestRasterLayer(document, point) == upper);
    CHECK(!hitTestRasterLayer(document, {std::numeric_limits<double>::quiet_NaN(), 0}));
}

void boundedLargeDocumentDiagnostics()
{
    constexpr std::size_t layerCount = 8;
    constexpr std::size_t sampleCount = 2048;
    for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}}) {
        Document document(CanvasSpec {.extent = extent});
        std::vector<std::shared_ptr<CountingSurface>> surfaces;
        for (std::size_t index = 0; index < layerCount; ++index) {
            auto surface = solidSurface(extent,
                {static_cast<std::uint8_t>(20U + index * 25U), 80, 170, 160});
            const auto layerId = addRaster(document, surface);
            document.layer(layerId)->localToDocument = {.m02 = 0.25, .m12 = 0.25};
            CHECK(document.setLayerOpacity(layerId, 0.6F));
            surfaces.push_back(std::move(surface));
        }
        const auto documentRevision = document.revision();
        const auto started = std::chrono::steady_clock::now();
        std::size_t totalTexels = 0;
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const Vec2d point {
                2.0 + static_cast<double>((index * 127U) % (extent.width - 4U)),
                2.0 + static_cast<double>((index * 61U) % (extent.height - 4U))};
            const auto sampled = mergedSample(document, point);
            CHECK(sampled.available());
            CHECK(sampled.layersVisited == layerCount);
            CHECK(sampled.texelsRead == 4U * layerCount);
            totalTexels += sampled.texelsRead;
        }
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - started).count();
        CHECK(document.revision() == documentRevision);
        for (const auto& surface : surfaces) {
            CHECK(surface->readCalls == sampleCount);
            CHECK(surface->texelsRead == 4U * sampleCount);
            CHECK(surface->maximumReadTexels == 4);
            CHECK(surface->revision() == 1);
            CHECK(surface->writeCalls == 0);
        }
        // Hardware-sensitive time is diagnostic only; bounded work is gated.
        std::cout << "color-sample-benchmark," << extent.width << 'x' << extent.height
                  << ",layers=" << layerCount << ",samples=" << sampleCount
                  << ",texels=" << totalTexels << ",total_us=" << elapsed
                  << ",mean_us=" << elapsed / static_cast<double>(sampleCount) << '\n';
    }

    Document tinyFootprint(CanvasSpec {.extent = {5120, 2880}});
    auto largeSurface = solidSurface({5120, 2880}, {18, 33, 247, 255});
    const auto tinyId = addRaster(tinyFootprint, largeSurface);
    tinyFootprint.layer(tinyId)->localToDocument = {.m00 = 0.0001, .m11 = 0.0002};
    const auto sampled = activeSample(tinyFootprint, tinyId, {0.1, 0.1});
    CHECK(sampled.available());
    CHECK(sampled.color == Rgba8({18, 33, 247, 255}));
    CHECK(sampled.texelsRead <= 4);
    CHECK(largeSurface->maximumReadTexels <= 4);
    CHECK(largeSurface->readCalls == 1);
}

} // namespace

int main()
{
    activeIdentityPreservesVisibleBytesAndCanonicalizesTransparentRgb();
    mergedSourceOverIsLinearAndRespectsStackState();
    transformedSamplingUsesTexelCentersAndClampedBilinearFiltering();
    documentClipAndOneTexelEdgesNeverReadOutsideStorage();
    unavailableAndUnsupportedSourcesAreExplicit();
    viewportZoomAndPanDoNotChangeTheDocumentSample();
    colorsAndPickingDoNotTouchDocumentHistoryOrClearRedo();
    retainedOffCanvasPixelsCanBePickedWithoutExpandingColorSampling();
    boundedLargeDocumentDiagnostics();
    if (failures != 0) {
        std::cerr << failures << " color sampler assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All color sampler tests passed\n";
    return EXIT_SUCCESS;
}
