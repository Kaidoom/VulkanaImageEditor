#include "imageeditor/ui/BackgroundRemovalMask.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/RegionalRasterSurface.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; \
    std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

template<class Function> void rejects(Function&& function, std::string_view reason)
{
    try {
        function();
        CHECK(false);
    } catch (const std::exception& error) {
        CHECK(std::string_view(error.what()).find(reason) != std::string_view::npos);
    }
}

c::Layer raster(c::Extent2u extent = {7, 3}, std::uint8_t alpha = 128)
{
    return c::Layer::raster("Source", std::make_shared<c::ContiguousRasterSurface>(
        extent, c::Rgba8{230, 40, 70, alpha}));
}

QImage gray(int width, int height, std::uint8_t value)
{
    QImage image(width, height, QImage::Format_Grayscale8);
    image.fill(value);
    return image;
}

c::LayerMaskState mask(c::Extent2u extent, std::uint8_t value, bool enabled = true)
{
    return std::make_shared<c::LayerMask>(
        c::LayerMask{c::SelectionMask::filled(extent, value), {}, 255, enabled});
}

void nativeSourceAndCoverage()
{
    auto layer = raster();
    layer.rasterOrigin = {-5.25, 14.5};
    layer.localToDocument = {1.2, -.4, -200, .3, .8, 350};
    layer.opacity = .1F;
    layer.fillOpacity = .2F;
    layer.blendMode = c::BlendMode::Multiply;
    layer.mask = mask({7, 3}, 0); // Source preparation must ignore the old mask.
    const auto initialTransform = layer.localToDocument;
    const auto source = u::backgroundRemovalSource(layer);
    CHECK(source.rgba.size() == QSize(7, 3));
    CHECK(source.rgba.format() == QImage::Format_RGBA8888);
    CHECK(source.originalMask == layer.mask);
    CHECK(source.rasterOrigin == layer.rasterOrigin);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 7; ++x) {
            const auto* pixel = source.rgba.constScanLine(y) + x * 4;
            CHECK(pixel[0] == 230 && pixel[1] == 40 && pixel[2] == 70 && pixel[3] == 128);
        }

    layer.mask.reset();
    const auto unmasked = u::backgroundRemovalSource(layer);
    const auto generated = gray(7, 3, 128);
    const auto result = u::backgroundRemovalMask(unmasked, generated);
    CHECK(result->enabled && result->outside == 0);
    CHECK(result->coverage->extent() == c::Extent2u{7, 3});
    CHECK(result->localToMask == c::AffineTransform{1, 0, 5.25, 0, 1, -14.5});
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 7; ++x) {
            // Source alpha stays 128. Multiplying it into the new mask would
            // produce 64 here and apply source transparency a second time.
            CHECK(result->coverage->coverageAtDocumentPixel(x, y) == 128);
            CHECK(std::abs(c::layerMaskCoverage(result,
                {layer.rasterOrigin.x + x + .5, layer.rasterOrigin.y + y + .5})
                - 128.F / 255) < 1.e-6F);
        }
    CHECK(c::layerMaskCoverage(result, {-100, -100}) == 0);
    CHECK(c::layerMaskCoverage(result, {100, 100}) == 0);
    CHECK(layer.localToDocument == initialTransform && !layer.mask);
    CHECK(u::backgroundRemovalSource(layer).rgba == unmasked.rgba);
}

void rowCopyAndOwnedSnapshot()
{
    auto layer = raster({3, 7});
    u::BackgroundRemovalSourceBuilder builder(layer);
    CHECK(builder.totalRows() == 7 && builder.rowsCopied() == 0);
    rejects([&] { (void)builder.source(); }, "incomplete");
    rejects([&] { (void)builder.step(0); }, "at least one row");
    CHECK(!builder.step(2) && builder.rowsCopied() == 2);
    CHECK(!builder.step(2) && builder.rowsCopied() == 4);
    CHECK(builder.step(3) && builder.rowsCopied() == 7);
    const auto snapshot = builder.source();
    const auto original = snapshot.rgba.copy();
    auto surface = std::get<c::RasterLayer>(layer.payload).surface;
    const std::array<std::byte, 4> replacement{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255}};
    (void)surface->replaceRgba8({0, 0, 1, 1}, replacement, 4);
    CHECK(snapshot.rgba == original);
    rejects([&] { (void)builder.step(); }, "source layer changed");

    u::BackgroundRemovalSourceBuilder interrupted(layer);
    CHECK(!interrupted.step(1));
    (void)surface->replaceRgba8({1, 0, 1, 1}, replacement, 4);
    rejects([&] { (void)interrupted.step(1); }, "source layer changed");
    rejects([&] { (void)interrupted.source(); }, "incomplete");
}

void existingMaskMappings()
{
    auto layer = raster({9, 5}, 255);
    layer.rasterOrigin = {-2.5, 3.25};
    auto old = std::make_shared<c::LayerMask>();
    std::array<std::uint8_t, 12> values{0, 30, 120, 255, 90, 210, 255, 100, 230, 170, 10, 128};
    old->coverage = c::SelectionMask::fromR8({4, 3}, values, 4);
    old->outside = 67;
    layer.mask = old;
    QImage generated = gray(9, 5, 255);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 9; ++x)
            generated.scanLine(y)[x] = std::uint8_t(20 + 13 * x + 9 * y);

    const std::array<c::AffineTransform, 5> maskMappings{
        c::AffineTransform{}, {1, 0, 2.5, 0, 1, -3.25},
        {.7, 0, 1.4, 0, 1.3, -4.1}, {0, -1, 7, 1, 0, 2},
        {1.1, .2, 1, -.15, .9, -3, .005, -.003, 1}};
    const std::array<c::AffineTransform, 4> transforms{
        c::AffineTransform{}, {1, 0, -200, 0, 1, 300},
        {0, -2, 40, 1.5, 0, -17}, {1, .2, 20, -.1, 1, 4, .001, -.0002, 1}};
    for (const auto& mapping : maskMappings) {
        // Never mutate a mask after publishing it as immutable state.
        old = std::make_shared<c::LayerMask>(*old);
        old->localToMask = mapping;
        layer.mask = old;
        c::LayerMaskState baseline;
        for (const auto& transform : transforms) {
            layer.localToDocument = transform;
            const auto source = u::backgroundRemovalSource(layer);
            const auto result = u::backgroundRemovalMask(source, generated);
            if (baseline)
                CHECK(c::equivalentLayerMasks(baseline, result));
            baseline = result;
            CHECK(layer.mask == old && source.originalMask == old);
            CHECK(old->outside == 67 && old->localToMask == mapping);
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 9; ++x) {
                    const c::Vec2d local{layer.rasterOrigin.x + x + .5, layer.rasterOrigin.y + y + .5};
                    const auto expected = std::lround(
                        generated.constScanLine(y)[x] * c::layerMaskCoverage(old, local));
                    CHECK(result->coverage->coverageAtDocumentPixel(x, y) == expected);
                    CHECK(std::abs(c::layerMaskCoverage(result, local) - double(expected) / 255.) < 1.e-6);
                }
        }
    }
    auto identity = raster({7, 3});
    identity.mask = mask({7, 3}, 128);
    const auto product = u::backgroundRemovalMask(u::backgroundRemovalSource(identity), gray(7, 3, 128));
    CHECK(product->coverage->coverageAtDocumentPixel(3, 1) == 64);
}

void expandedRasterStorage()
{
    const auto base = std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{3, 2}, c::Rgba8{27, 91, 220, 211});
    auto regional = std::make_shared<c::RegionalRasterSurface>(base, c::Vec2d{4, 3});
    auto storage = regional->state();
    storage.bounds = {-2, -1, 10, 7};
    regional->restore(storage);
    auto layer = c::Layer::raster("Expanded storage", regional);
    layer.rasterOrigin = regional->origin();
    layer.localToDocument = {0, -1, -170, 1, 0, 90};
    const auto source = u::backgroundRemovalSource(layer);
    CHECK(source.rgba.size() == QSize(10, 7));
    CHECK(source.rasterOrigin == c::Vec2d{-2, -1});
    for (int y = 0; y < 7; ++y)
        for (int x = 0; x < 10; ++x) {
            const auto* pixel = source.rgba.constScanLine(y) + x * 4;
            const bool inBase = x >= 6 && x < 9 && y >= 4 && y < 6;
            CHECK(pixel[3] == (inBase ? 211 : 0));
            if (inBase) CHECK(pixel[0] == 27 && pixel[1] == 91 && pixel[2] == 220);
        }
    const auto result = u::backgroundRemovalMask(source, gray(10, 7, 255));
    CHECK(result->localToMask == c::AffineTransform{1, 0, 2, 0, 1, 1});
    CHECK(c::layerMaskCoverage(result, {4.5, 3.5}) == 1);
    CHECK(c::layerMaskCoverage(result, {-2.5, -1.5}) == 0);
    CHECK(regional->state().bounds == storage.bounds);
}

void historyAndNoMutation()
{
    c::EditorSession session;
    auto document = std::make_unique<c::Document>(c::CanvasSpec{{8, 6}, 96});
    auto layer = raster({7, 3}, 173);
    layer.localToDocument = {1.1, .2, -4, -.1, .7, 12};
    layer.rasterOrigin = {-3, 5};
    layer.mask = mask({3, 2}, 191);
    const auto originalMask = layer.mask;
    const auto id = layer.id;
    CHECK(document->insertLayer(0, layer));
    auto unrelated = c::SelectionMask::rectangle({8, 6}, {2, 3, 2, 1}, 91);
    CHECK(document->setSelection(unrelated));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(id);
    auto& doc = *session.document();
    doc.markSaved();
    const auto before = doc.contentState();
    const auto revision = doc.revision();
    const auto source = u::backgroundRemovalSource(*doc.layer(id));
    const auto pixels = source.rgba.copy();
    const auto result = u::backgroundRemovalMask(source, gray(7, 3, 151));
    // Preview preparation and abandoning a result have no canonical effects.
    CHECK(doc.layer(id)->mask == originalMask && doc.selection() == unrelated);
    CHECK(doc.contentState() == before && doc.revision() == revision && !doc.isModified());
    CHECK(session.history().undoDepth() == 0);
    const auto selected = session.selectedLayers();
    CHECK(session.execute(std::make_unique<c::LayerMaskCommand>(
        id, originalMask, result, "Remove background")));
    CHECK(session.history().undoDepth() == 1 && session.history().redoDepth() == 0);
    CHECK(doc.layer(id)->mask == result && doc.isModified());
    CHECK(doc.selection() == unrelated && session.selectedLayers() == selected);
    CHECK(doc.layer(id)->localToDocument == layer.localToDocument);
    CHECK(doc.layer(id)->rasterOrigin == layer.rasterOrigin);
    CHECK(u::backgroundRemovalSource(*doc.layer(id)).rgba == pixels);
    CHECK(session.undo());
    CHECK(doc.layer(id)->mask == originalMask && !doc.isModified());
    CHECK(doc.selection() == unrelated && session.selectedLayers() == selected);
    CHECK(session.redo());
    CHECK(doc.layer(id)->mask == result && doc.selection() == unrelated);
    CHECK(u::backgroundRemovalSource(*doc.layer(id)).rgba == pixels);
}

void invalidAndEmptyResults()
{
    auto layer = raster();
    rejects([&] { (void)u::backgroundRemovalSource(c::Layer::text("Text", {})); }, "raster layer");
    layer.mask = mask({7, 3}, 128, false);
    rejects([&] { (void)u::backgroundRemovalSource(layer); }, "Enable the existing layer mask");
    CHECK(!layer.mask->enabled);
    layer.mask.reset();
    const auto source = u::backgroundRemovalSource(layer);
    rejects([&] { (void)u::backgroundRemovalMask(source, gray(3, 7, 255)); }, "dimensions");
    rejects([&] { (void)u::backgroundRemovalMask(source, QImage(7, 3, QImage::Format_RGB32)); }, "grayscale");
    rejects([&] { (void)u::backgroundRemovalMask(source, gray(7, 3, 0)); }, "hide every visible");
    rejects([&] { (void)u::backgroundRemovalMask(source, gray(7, 3, 255), [] { return true; }); }, "cancelled");
    int checks = 0;
    rejects([&] { (void)u::backgroundRemovalMask(source, gray(7, 3, 255), [&] { return ++checks == 3; }); }, "cancelled");
    CHECK(!layer.mask);

    auto transparent = raster({7, 3}, 0);
    rejects([&] { (void)u::backgroundRemovalSource(transparent); }, "no visible pixels");
    auto sparse = raster({7, 3}, 0);
    const std::array<std::byte, 4> visible{std::byte{12}, std::byte{17}, std::byte{21}, std::byte{128}};
    (void)std::get<c::RasterLayer>(sparse.payload).surface->replaceRgba8({1, 1, 1, 1}, visible, 4);
    auto hole = gray(7, 3, 255);
    hole.scanLine(1)[1] = 0;
    rejects([&] { (void)u::backgroundRemovalMask(u::backgroundRemovalSource(sparse), hole); }, "hide every visible");

    layer.crop = c::LayerCrop{2, 1, 3, 1};
    auto cropped = u::backgroundRemovalSource(layer);
    auto onlyOutsideCrop = gray(7, 3, 255);
    for (int x = 2; x < 5; ++x) onlyOutsideCrop.scanLine(1)[x] = 0;
    rejects([&] { (void)u::backgroundRemovalMask(cropped, onlyOutsideCrop); }, "hide every visible");
    onlyOutsideCrop.scanLine(1)[3] = 1;
    CHECK(u::backgroundRemovalMask(cropped, onlyOutsideCrop));
    CHECK(layer.crop == c::LayerCrop{2, 1, 3, 1});
    layer.crop = c::LayerCrop{40, 30, 2, 2};
    rejects([&] { (void)u::backgroundRemovalSource(layer); }, "no visible pixels");

    layer.crop.reset();
    layer.mask = mask({7, 3}, 0);
    rejects([&] { (void)u::backgroundRemovalMask(u::backgroundRemovalSource(layer), gray(7, 3, 255)); }, "hide every visible");
}
} // namespace

int main()
{
    nativeSourceAndCoverage();
    rowCopyAndOwnedSnapshot();
    existingMaskMappings();
    expandedRasterStorage();
    historyAndNoMutation();
    invalidAndEmptyResults();
    if (failures)
        std::cerr << failures << " background-removal mask checks failed.\n";
    else
        std::cout << "Background-removal mask/source checks passed.\n";
    return failures ? 1 : 0;
}
