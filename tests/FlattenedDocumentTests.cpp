#include "imageeditor/ui/FlattenedDocument.hpp"

#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <QFontDatabase>
#include <QGuiApplication>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

c::Rgba8 pixel(const QImage& image, int x, int y)
{
    const auto* rgba = image.constScanLine(y) + std::size_t(x) * 4;
    return { rgba[0], rgba[1], rgba[2], rgba[3] };
}
bool near(c::Rgba8 a, c::Rgba8 b, int tolerance = 1)
{
    return std::abs(int(a.red) - int(b.red)) <= tolerance
        && std::abs(int(a.green) - int(b.green)) <= tolerance
        && std::abs(int(a.blue) - int(b.blue)) <= tolerance
        && std::abs(int(a.alpha) - int(b.alpha)) <= tolerance;
}
std::shared_ptr<c::LayerRenderCache> misleadingCache()
{
    auto cache = std::make_shared<c::LayerRenderCache>();
    cache->surface
        = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 1, 1 }, c::Rgba8 { 255, 0, 255, 255 });
    cache->pixelsToLocal = { 1000, 0, -100, 0, 1000, -100 };
    cache->density = .0001;
    cache->requestedDensity = 8;
    return cache;
}

void mixtureAndViewportIndependence(const std::string& fontFamily)
{
    c::Document document({ { 100, 100 }, 300 });
    const auto raster
        = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 100, 100 }, c::Rgba8 { 0, 0, 255, 255 });
    CHECK(document.insertLayer(0, c::Layer::raster("Blue", raster)));
    c::ShapeLayer rectangle;
    rectangle.size = { 40, 30 };
    rectangle.fillColor = { 255, 0, 0, 128 };
    auto shape = c::Layer::shape("Flipped rotated rectangle", rectangle);
    shape.localToDocument = { 0, -1, 80, -1, 0, 90 };
    shape.opacity = .5F;
    const auto shapeId = shape.id;
    CHECK(document.insertLayer(1, std::move(shape)));
    c::TextLayer text;
    text.utf8 = "OO";
    text.defaultStyle.font = { fontFamily, "Regular", 400, false };
    text.defaultStyle.sizePixels = 18;
    text.defaultStyle.color = { 0, 255, 0, 230 };
    auto letters = c::Layer::text("Text", text);
    letters.localToDocument = { 1.5, .2, 2, .1, 1.25, 3 };
    const auto textId = letters.id;
    CHECK(document.insertLayer(2, std::move(letters)));

    c::ShapeLayer invisibleShape;
    invisibleShape.size = { 1e6, 1e6 };
    invisibleShape.fillColor = { 255, 255, 255, 255 };
    auto invisible = c::Layer::shape("Hidden never allocates a giant cache", invisibleShape);
    invisible.visible = false;
    CHECK(document.insertLayer(3, std::move(invisible)));
    document.markSaved();
    const auto revision = document.revision();
    const auto state = document.contentState();
    const auto rasterRevision = raster->revision();
    const auto output = u::flattenDocument(document);
    CHECK(output);
    if (!output) {
        std::cerr << output.error.toStdString() << '\n';
        return;
    }
    CHECK(output.image.format() == QImage::Format_RGBA8888);
    CHECK(output.image.size() == QSize(100, 100));
    CHECK(output.image.dotsPerMeterX() == 11811 && output.image.dotsPerMeterY() == 11811);
    CHECK(pixel(output.image, 99, 99) == c::Rgba8 { 0, 0, 255, 255 });
    const auto alpha = (128.0 / 255) * .5;
    CHECK(near(pixel(output.image, 65, 70), { c::linearToSrgb(alpha), 0, c::linearToSrgb(1 - alpha), 255 }));
    bool hasText = false;
    for (int y = 0; y < 45; ++y)
        for (int x = 0; x < 48; ++x)
            hasText = hasText || pixel(output.image, x, y).green > 100;
    CHECK(hasText);
    CHECK(document.revision() == revision && document.contentState() == state && !document.isModified());
    CHECK(raster->revision() == rasterRevision);
    CHECK(!document.layer(shapeId)->renderCache && !document.layer(textId)->renderCache);

    const auto oldShapeCache = misleadingCache(), oldTextCache = misleadingCache();
    document.layer(shapeId)->renderCache = oldShapeCache;
    document.layer(textId)->renderCache = oldTextCache;
    const auto second = u::flattenDocument(document);
    CHECK(second && second.image == output.image);
    CHECK(document.layer(shapeId)->renderCache == oldShapeCache);
    CHECK(document.layer(textId)->renderCache == oldTextCache);
    CHECK(std::get<c::ShapeLayer>(document.layer(shapeId)->payload) == rectangle);
    CHECK(std::get<c::TextLayer>(document.layer(textId)->payload) == c::normalizedText(text));
    const auto emptySelection = c::SelectionMask::filled(document.canvas().extent, 0);
    CHECK(document.setSelection(emptySelection));
    const auto selectionRevision = document.selectionRevision();
    const auto selected = u::flattenDocument(document);
    CHECK(selected && selected.image == output.image); // Selection is not a content clipping mask.
    CHECK(document.selection() == emptySelection && document.selectionRevision() == selectionRevision);
}

void alphaClippingAndStackOrder()
{
    c::Document document({ { 20, 16 }, 96 });
    c::ShapeLayer shape;
    shape.size = { 16, 12 };
    shape.fillColor = { 20, 180, 90, 128 };
    auto layer = c::Layer::shape("Off canvas", shape);
    layer.localToDocument.m02 = -8;
    layer.localToDocument.m12 = -4;
    layer.opacity = .5F;
    const auto id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    auto output = u::flattenDocument(document);
    CHECK(output);
    if (!output)
        return;
    CHECK(near(pixel(output.image, 3, 3), { 20, 180, 90, 64 }));
    CHECK(pixel(output.image, 19, 15).alpha == 0);
    CHECK(document.layer(id)->localToDocument.m02 == -8);

    shape.fillColor = { 255, 0, 0, 255 };
    shape.size = { 20, 16 };
    auto top = c::Layer::shape("Topmost", shape);
    const auto topId = top.id;
    CHECK(document.insertLayer(1, std::move(top)));
    output = u::flattenDocument(document);
    CHECK(output && pixel(output.image, 3, 3) == c::Rgba8 { 255, 0, 0, 255 });
    CHECK(document.moveLayer(topId, 0));
    output = u::flattenDocument(document);
    CHECK(output && pixel(output.image, 3, 3).green > 0);
    CHECK(document.setLayerOpacity(id, 0));
    output = u::flattenDocument(document);
    CHECK(output && pixel(output.image, 3, 3) == c::Rgba8 { 255, 0, 0, 255 });
}

class FailingSurface final : public c::RasterSurface {
public:
    c::SurfaceId id() const noexcept override { return 123456; }
    c::Extent2u extent() const noexcept override { return { 2, 2 }; }
    c::Revision revision() const noexcept override { return 1; }
    c::DirtySet dirtySince(c::Revision) const override { return { }; }
    void copyRgba8(c::RectI, std::span<std::byte>, std::size_t) const override { throw std::bad_alloc(); }
    c::DirtySet replaceRgba8Batch(std::span<const c::RasterPatch>) override { return { }; }
    c::DirtySet swapRgba8Batch(std::span<c::MutableRasterPatch>) override { return { }; }
};

void failureCancellationAndLimits()
{
    c::Document document({ { 128, 128 }, 96 });
    auto surface = std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u { 128, 128 }, c::Rgba8 { 30, 60, 120, 255 });
    auto layer = c::Layer::raster("Stable input", surface);
    const auto id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    const auto early = u::flattenDocument(document, [](auto, auto) { return false; });
    CHECK(early.cancelled && !early && early.image.isNull());
    const auto mid = u::flattenDocument(document, [](auto done, auto) { return done < 4096; });
    CHECK(mid.cancelled && !mid && mid.image.isNull());
    const auto last = u::flattenDocument(document, [](auto done, auto total) { return done < total; });
    CHECK(last.cancelled && !last && last.image.isNull());
    const auto exceeded = u::flattenDocument(document, { }, { 100, 1024, 1024 });
    CHECK(!exceeded && !exceeded.error.isEmpty() && exceeded.image.isNull());
    const auto metadata = u::flattenDocument(document, { }, { 65536, 1024, 0 });
    CHECK(!metadata && metadata.error.contains("metadata"));

    bool changed = false;
    const auto changedGeometry = u::flattenDocument(document, [&](auto done, auto) {
        if (done > 0 && !changed) {
            changed = true;
            document.setLayerOpacity(id, .25F);
        }
        return true;
    });
    CHECK(!changedGeometry && changedGeometry.error.contains("changed"));
    changed = false;
    const auto changedPixels = u::flattenDocument(document, [&](auto done, auto) {
        if (done > 0 && !changed) {
            changed = true;
            const std::array<std::byte, 4> bytes { std::byte(255), std::byte(0), std::byte(0),
                std::byte(255) };
            (void)surface->replaceRgba8({ 0, 0, 1, 1 }, bytes, 4);
        }
        return true;
    });
    CHECK(!changedPixels && changedPixels.error.contains("changed"));

    c::Document tooBig({ { 8193, 8193 }, 96 });
    CHECK(!u::flattenDocument(tooBig));
    c::Document cacheBudget({ { 50, 50 }, 96 });
    CHECK(cacheBudget.insertLayer(0, c::Layer::shape("Cache too large", { })));
    const auto budget = u::flattenDocument(cacheBudget, { }, { 65536, 100, 65536 });
    CHECK(!budget && budget.error.contains("cache"));
    c::Document failure({ { 2, 2 }, 96 });
    CHECK(
        failure.insertLayer(0, c::Layer::raster("Allocation injection", std::make_shared<FailingSurface>())));
    const auto allocation = u::flattenDocument(failure);
    CHECK(!allocation && allocation.image.isNull() && allocation.error.contains("memory"));
}

void boundedPerformance()
{
    c::Document document({ { 1024, 1024 }, 96 });
    CHECK(document.insertLayer(0,
        c::Layer::raster("Backdrop",
            std::make_shared<c::ContiguousRasterSurface>(
                c::Extent2u { 1024, 1024 }, c::Rgba8 { 10, 20, 30, 255 }))));
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::Ellipse;
    shape.size = { 800, 800 };
    shape.fillColor = { 240, 20, 90, 128 };
    CHECK(document.insertLayer(1, c::Layer::shape("Ellipse", shape)));
    std::uint64_t previous = 0, calls = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto result = u::flattenDocument(document, [&](auto done, auto total) {
        CHECK(done >= previous && done <= total);
        CHECK(done - previous <= 4096);
        previous = done;
        ++calls;
        return true;
    });
    CHECK(result);
    CHECK(calls >= 256 && calls < 300);
    const auto elapsed
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "1K flattened raster + editable ellipse: " << elapsed << " ms, " << calls
              << " bounded callbacks\n";
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        const auto fontId = QFontDatabase::addApplicationFont(
            QStringLiteral(IMAGEEDITOR_FLATTEN_FONT_DIR "/NotoSans-Regular.ttf"));
        CHECK(fontId >= 0);
        if (fontId < 0)
            return 1;
        const auto families = QFontDatabase::applicationFontFamilies(fontId);
        CHECK(!families.empty());
        if (families.empty())
            return 1;
        mixtureAndViewportIndependence(families.front().toStdString());
        alphaClippingAndStackOrder();
        failureCancellationAndLimits();
        boundedPerformance();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << error.what() << '\n';
    }
    if (!failures)
        std::cout << "Flattened raster/text/shape output, clipping, independence, "
                     "limits and cancellation passed\n";
    return failures ? 1 : 0;
}
