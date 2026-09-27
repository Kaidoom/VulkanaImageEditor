#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"

#include <QBuffer>
#include <QFontDatabase>
#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)
c::Rgba8 pixel(const QImage& image, int x, int y)
{
    const auto* bytes = image.constScanLine(y) + size_t(x) * 4;
    return { bytes[0], bytes[1], bytes[2], bytes[3] };
}
bool near(c::Rgba8 a, c::Rgba8 b, int tolerance = 1)
{
    return std::abs(int(a.red) - int(b.red)) <= tolerance
        && std::abs(int(a.green) - int(b.green)) <= tolerance
        && std::abs(int(a.blue) - int(b.blue)) <= tolerance
        && std::abs(int(a.alpha) - int(b.alpha)) <= tolerance;
}
bool sameAppearance(const QImage& first, const QImage& second)
{
    if (first.size() != second.size() || first.format() != QImage::Format_RGBA8888
        || second.format() != QImage::Format_RGBA8888)
        return false;
    for (int y = 0; y < first.height(); ++y)
        for (int x = 0; x < first.width(); ++x) {
            const auto a = pixel(first, x, y), b = pixel(second, x, y);
            // Fractional samples can quantize alpha to zero while preserving
            // hidden RGB. A baked transparent pixel has no visible contribution.
            if (a != b && (a.alpha != 0 || b.alpha != 0))
                return false;
        }
    return true;
}
c::Rgba8 encode(const c::PremultipliedColor& color)
{
    return c::encodeColor(color);
}
std::vector<std::byte> pixels(const c::RasterSurface& surface)
{
    const auto size = surface.extent();
    std::vector<std::byte> result(size_t(size.width) * size.height * 4);
    surface.copyRgba8({ 0, 0, int32_t(size.width), int32_t(size.height) }, result, size_t(size.width) * 4);
    return result;
}
struct SourceState {
    c::LayerTree tree;
    std::vector<c::Layer> layers;
    std::vector<std::vector<std::byte>> rasterPixels;
    std::vector<c::Revision> rasterRevisions;
    c::SelectionState selection;
    c::Revision revision, selectionRevision;
    uint64_t contentState;
    explicit SourceState(const c::Document& document)
        : tree(document.tree()), layers(document.layers()), selection(document.selection()),
          revision(document.revision()), selectionRevision(document.selectionRevision()), contentState(document.contentState())
    {
        for (const auto& layer : layers) {
            const auto* raster = std::get_if<c::RasterLayer>(&layer.payload);
            rasterPixels.push_back(raster ? pixels(*raster->surface) : std::vector<std::byte> {});
            rasterRevisions.push_back(raster ? raster->surface->revision() : 0);
        }
    }
    void unchanged(const c::Document& document, bool revisions = true, bool caches = true) const
    {
        CHECK(document.tree() == tree && document.layers().size() == layers.size());
        if(revisions)CHECK(document.revision() == revision && document.contentState() == contentState);
        CHECK(document.selection() == selection);
        if(revisions)CHECK(document.selectionRevision() == selectionRevision);
        for (size_t i = 0; i < layers.size(); ++i) {
            const auto& before = layers[i];
            const auto* after = document.layer(before.id);
            CHECK(after && after->name == before.name && after->localToDocument == before.localToDocument
                && after->visible == before.visible && after->opacity == before.opacity
                && after->colorLabel == before.colorLabel && (!caches || after->renderCache == before.renderCache)
                && (!caches || after->filterCache == before.filterCache) && after->filters == before.filters
                && after->adjustments == before.adjustments && after->crop == before.crop
                && after->blendMode == before.blendMode);
            if (!after)
                continue;
            CHECK(after->payload.index() == before.payload.index());
            if (const auto* raster = std::get_if<c::RasterLayer>(&after->payload)) {
                CHECK(raster->surface == std::get<c::RasterLayer>(before.payload).surface);
                CHECK(raster->surface->revision() == rasterRevisions[i] && pixels(*raster->surface) == rasterPixels[i]);
            } else if (const auto* text = std::get_if<c::TextLayer>(&after->payload))
                CHECK(*text == std::get<c::TextLayer>(before.payload));
            else
                CHECK(std::get<c::ShapeLayer>(after->payload) == std::get<c::ShapeLayer>(before.payload));
        }
    }
};
c::Layer raster(const std::string& name, c::Extent2u size, c::Rgba8 color)
{
    return c::Layer::raster(name, std::make_shared<c::ContiguousRasterSurface>(size, color));
}
c::LayerId add(c::Document& document, c::Layer layer)
{
    const auto id = layer.id;
    CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
    return id;
}
c::Layer fromImage(const QImage& image, c::Vec2d origin = {})
{
    CHECK(!image.isNull() && image.format() == QImage::Format_RGBA8888);
    const c::Extent2u extent { uint32_t(image.width()), uint32_t(image.height()) };
    std::vector<std::byte> bytes(size_t(extent.width) * extent.height * 4);
    for (uint32_t y = 0; y < extent.height; ++y)
        std::memcpy(bytes.data() + size_t(y) * extent.width * 4, image.constScanLine(int(y)), size_t(extent.width) * 4);
    auto layer = c::Layer::raster("Merged", std::make_shared<c::ContiguousRasterSurface>(extent, std::move(bytes)));
    layer.localToDocument.m02 = origin.x;
    layer.localToDocument.m12 = origin.y;
    return layer;
}
c::Layer fromMerged(const u::FlattenedDocumentResult& merged)
{
    CHECK(merged);
    return fromImage(merged.image, merged.origin);
}

void identityRasterCopies()
{
    // A huge canvas must not increase a tiny merge's allocation or work. These
    // colors exercise every alpha value, including nonzero hidden RGB at a=0.
    c::Document document({ { 30000, 30000 }, 96 });
    const c::Extent2u extent { 19, 17 };
    std::vector<std::byte> bytes(size_t(extent.width) * extent.height * 4);
    for (size_t i = 0; i < bytes.size() / 4; ++i) {
        bytes[i * 4] = std::byte((i * 31 + 7) % 256);
        bytes[i * 4 + 1] = std::byte((i * 13 + 19) % 256);
        bytes[i * 4 + 2] = std::byte((i * 97 + 23) % 256);
        bytes[i * 4 + 3] = std::byte(i % 256);
    }
    auto source = c::Layer::raster("Exact bytes", std::make_shared<c::ContiguousRasterSurface>(extent, bytes));
    source.localToDocument = { 1, 0, -37, 0, 1, 11 };
    const auto id = add(document, source);
    const SourceState before(document);
    uint64_t totalWork = 0;
    const auto merged = u::flattenLayerItems(document, std::array { id },
        [&](auto, auto total) { totalWork = total; return true; }, { 19 * 17, 0, 65536 });
    CHECK(merged && merged.origin == c::Vec2d { -37, 11 } && merged.image.size() == QSize(19, 17));
    if (merged) {
        CHECK(pixels(*std::get<c::RasterLayer>(fromMerged(merged).payload).surface) == bytes);
        CHECK(merged.image.sizeInBytes() == qsizetype(bytes.size()));
    }
    CHECK(totalWork <= uint64_t(extent.width) * extent.height + 1);
    before.unchanged(document);
}

void transformedRasterUsesOriginal()
{
    const c::Extent2u extent { 31, 23 };
    std::vector<std::byte> bytes(size_t(extent.width) * extent.height * 4);
    for (uint32_t y = 0; y < extent.height; ++y)
        for (uint32_t x = 0; x < extent.width; ++x) {
            const auto i = (size_t(y) * extent.width + x) * 4;
            bytes[i] = std::byte((x + y) % 2 ? 255 : 0);
            bytes[i + 1] = std::byte((x * 61 + y * 17) % 256);
            bytes[i + 2] = std::byte((x * 7 + y * 97) % 256);
            bytes[i + 3] = std::byte((x * 43 + y * 31) % 256);
        }
    for (const auto transform : { c::AffineTransform { .5, 0, 3.25, 0, .5, 4.75 },
             c::AffineTransform { .8, -.6, 12.3, .6, .8, -2.7 },
             c::AffineTransform { -1.25, .3, 25.75, .2, .75, 6.125 } }) {
        c::Document document({ { 64, 64 }, 96 });
        auto source = c::Layer::raster("One source sampling pass",
            std::make_shared<c::ContiguousRasterSurface>(extent, bytes));
        source.localToDocument = transform;
        const auto id = add(document, source);
        const auto merged = u::flattenLayerItems(document, std::array { id });
        CHECK(merged);
        if (!merged)
            continue;
        const c::PreparedLayerSampler original(source), baked(fromMerged(merged));
        // Continuous sampling of original source pixels is the live rendering
        // contract. Reflattening the source would not catch two sampling passes.
        bool equal = true;
        for (int y = 0; y < merged.image.height(); ++y)
            for (int x = 0; x < merged.image.width(); ++x) {
                const c::Vec2d point { merged.origin.x + x + .5, merged.origin.y + y + .5 };
                const auto expected = encode(original.sample(point));
                const auto actual = pixel(merged.image, x, y);
                const auto displayed = encode(baked.sample(point));
                const bool matching = expected.alpha == 0 && actual.alpha == 0
                    ? displayed.alpha == 0 : expected == actual && expected == displayed;
                if (!matching && equal) {
                    const auto print = [](c::Rgba8 color) {
                        std::cerr << '(' << int(color.red) << ',' << int(color.green) << ','
                                  << int(color.blue) << ',' << int(color.alpha) << ')';
                    };
                    std::cerr << "Transformed raster mismatch transform=" << transform.m00 << ',' << transform.m01
                              << ',' << transform.m02 << ',' << transform.m10 << ',' << transform.m11 << ',' << transform.m12
                              << " document=" << point.x << ',' << point.y << " original=";
                    print(expected); std::cerr << " merged="; print(actual); std::cerr << " displayed="; print(displayed);
                    std::cerr << '\n';
                }
                equal = equal && matching;
            }
        CHECK(equal);
    }
}

void passThroughAndOpacity()
{
    c::Document document({ { 20, 16 }, 96 });
    const auto backdrop = add(document, raster("Unselected green", { 20, 16 }, { 0, 255, 0, 255 }));
    auto lowerLayer = raster("Red", { 12, 10 }, { 255, 0, 0, 128 });
    lowerLayer.opacity = .5F;
    const auto lower = add(document, lowerLayer);
    auto upperLayer = raster("Blue", { 12, 10 }, { 0, 0, 255, 128 });
    upperLayer.opacity = .75F;
    const auto upper = add(document, upperLayer);
    const std::array leafs { lower, upper };
    const auto ungrouped = u::flattenLayerItems(document, leafs);
    const auto appearance = u::flattenDocument(document);
    CHECK(ungrouped && appearance);
    if (!ungrouped || !appearance)
        return;
    const auto group = c::makeLayerId(), folder = c::makeLayerId(), inner = c::makeLayerId();
    c::LayerTree tree;
    tree.roots = { backdrop, group };
    tree.containers = {
        { group, "Group", c::ContainerKind::Group, c::ColorLabel::Blue, { folder, upper } },
        { folder, "Pass-through folder", c::ContainerKind::Folder, c::ColorLabel::None, { inner } },
        { inner, "Nested group", c::ContainerKind::Group, c::ColorLabel::None, { lower } },
    };
    CHECK(document.replaceStructure(document.tree(), tree));
    const SourceState before(document);
    const std::array targets { group, inner, lower, group }; // Normalize ancestors and duplicates once.
    const auto grouped = u::flattenLayerItems(document, targets);
    CHECK(grouped && grouped.image == ungrouped.image && grouped.origin == ungrouped.origin);
    const auto afterAppearance = u::flattenDocument(document);
    CHECK(afterAppearance && afterAppearance.image == appearance.image);
    if (grouped) {
        const double a = 128.0 / 255 * .5, b = 128.0 / 255 * .75;
        const double alpha = b + a * (1 - b);
        CHECK(near(pixel(grouped.image, 5, 5), { c::linearToSrgb(a * (1 - b) / alpha), 0,
            c::linearToSrgb(b / alpha), c::alphaToByte(alpha) }));
        CHECK(grouped.origin == c::Vec2d {} && grouped.image.size() == QSize(12, 10));
    }
    before.unchanged(document);
}

void offCanvasOrientationAndAlpha()
{
    c::Document document({ { 2, 2 }, 96 });
    const std::array<c::Rgba8, 6> colors { c::Rgba8 { 255, 0, 0, 128 }, { 0, 255, 0, 128 },
        { 0, 0, 255, 128 }, { 255, 255, 0, 128 }, { 255, 0, 255, 128 }, { 255, 255, 255, 0 } };
    std::vector<std::byte> bytes;
    for (const auto color : colors)
        for (auto channel : { color.red, color.green, color.blue, color.alpha })
            bytes.push_back(std::byte(channel));
    auto layer = c::Layer::raster("Rotated entirely off canvas", std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 2, 3 }, bytes));
    layer.opacity = .5F;
    layer.localToDocument = { 0, -1, -2, 1, 0, -3 };
    const auto id = add(document, layer);
    const SourceState before(document);
    const auto merged = u::flattenLayerItems(document, std::array { id });
    CHECK(merged);
    if (!merged)
        return;
    CHECK(merged.origin == c::Vec2d { -5, -3 } && merged.image.size() == QSize(3, 2));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 2; ++x) {
            auto expected = colors[size_t(y) * 2 + size_t(x)];
            if (expected.alpha)
                expected.alpha = 64;
            else
                expected = {};
            CHECK(near(pixel(merged.image, 2 - y, x), expected));
        }
    before.unchanged(document);

    // A reflected, sheared, fractionally positioned source preserves its full
    // bounds. Replacing it by the document-pixel result agrees on the canvas.
    auto transform = document.layer(id)->localToDocument;
    transform = { -1.75, .5, 1.25, .25, 2, -.75 };
    CHECK(document.setLayerTransform(id, transform));
    const auto expected = u::flattenDocument(document);
    const auto transformed = u::flattenLayerItems(document, std::array { id });
    CHECK(expected && transformed && transformed.origin.x < 0 && transformed.origin.y < 0);
    if (expected && transformed) {
        c::Document replacement(document.canvas());
        add(replacement, fromMerged(transformed));
        const auto result = u::flattenDocument(replacement);
        CHECK(result && result.image == expected.image);
    }
}

std::shared_ptr<c::LayerRenderCache> misleadingCache()
{
    auto cache = std::make_shared<c::LayerRenderCache>();
    cache->surface = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 1, 1 }, c::Rgba8 { 255, 0, 255, 255 });
    cache->pixelsToLocal = { 10000, 0, -5000, 0, 10000, -5000 };
    cache->density = .0001;
    return cache;
}

QTransform painterTransform(const c::AffineTransform& transform)
{
    return { transform.m00, transform.m10, transform.m01, transform.m11,
        transform.m02, transform.m12 };
}

QImage shapeReference(const c::Layer& layer, QSize size)
{
    const auto& shape = std::get<c::ShapeLayer>(layer.payload);
    QPainterPath path;
    const QRectF rectangle(0, 0, shape.size.width, shape.size.height);
    if (shape.kind == c::ShapeKind::Ellipse)
        path.addEllipse(rectangle);
    else {
        CHECK(shape.kind == c::ShapeKind::RoundedRectangle);
        path.addRoundedRect(rectangle, shape.cornerRadius, shape.cornerRadius, Qt::AbsoluteSize);
    }
    QPainterPathStroker stroker;
    stroker.setWidth(shape.strokeWidth);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCurveThreshold(.03125);
    const auto coverage = [&](const QPainterPath& geometry, bool enabled) {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        if (enabled) {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setWorldTransform(painterTransform(layer.localToDocument));
            painter.fillPath(geometry, Qt::white);
        }
        return image;
    };
    const auto fill = coverage(path, shape.fillEnabled);
    const auto stroke = coverage(stroker.createStroke(path), shape.strokeEnabled && shape.strokeWidth > 0);
    QImage result(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            const double a = (double(qAlpha(fill.pixel(x, y))) / 255) * (double(shape.fillColor.alpha) / 255);
            const double b = (double(qAlpha(stroke.pixel(x, y))) / 255) * (double(shape.strokeColor.alpha) / 255);
            const double alpha = b + a * (1 - b);
            const auto channel = [&](uint8_t lower, uint8_t upper) {
                return alpha <= 0 ? uint8_t(0) : c::linearToSrgb(
                    (c::srgbToLinear(upper) * b + c::srgbToLinear(lower) * a * (1 - b)) / alpha);
            };
            auto* output = result.scanLine(y) + x * 4;
            output[0] = channel(shape.fillColor.red, shape.strokeColor.red);
            output[1] = channel(shape.fillColor.green, shape.strokeColor.green);
            output[2] = channel(shape.fillColor.blue, shape.strokeColor.blue);
            output[3] = c::alphaToByte(alpha);
        }
    return result;
}

QImage textReference(const c::Layer& layer, QSize size)
{
    // Use the canonical layout directly on a document-pixel paint device. No
    // render service/cache or destructive output implementation is consulted.
    u::QtTextLayout layout(std::get<c::TextLayer>(layer.payload));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setWorldTransform(painterTransform(layer.localToDocument));
        layout.document().documentLayout()->draw(&painter, QAbstractTextDocumentLayout::PaintContext {});
    }
    return image.convertToFormat(QImage::Format_RGBA8888);
}

void typedAndMixedDocumentPixelAppearance(const std::string& font)
{
    const c::Extent2u extent { 112, 96 };
    const QSize imageSize(int(extent.width), int(extent.height));
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::Ellipse;
    shape.size = { 29, 17 };
    shape.fillColor = { 50, 200, 70, 139 };
    shape.strokeEnabled = true;
    shape.strokeWidth = 2.3;
    shape.strokeColor = { 225, 30, 90, 187 };
    auto shapeLayer = c::Layer::shape("Transformed fill and stroke", shape);
    shapeLayer.localToDocument = { 1.25, -.4, 29.375, .35, .8, 19.625 };
    shapeLayer.opacity = .73F;
    c::TextLayer text;
    text.utf8 = "OHi\nFj";
    text.defaultStyle.font = { font, "Regular", 400, false };
    text.defaultStyle.sizePixels = 19;
    text.defaultStyle.color = { 20, 115, 235, 211 };
    auto accent = text.defaultStyle;
    accent.sizePixels = 25;
    accent.color = { 230, 180, 20, 149 };
    text.runs.push_back({ 1, 1, accent });
    auto textLayer = c::Layer::text("Transformed rich text", text);
    textLayer.localToDocument = { .9, .2, 39.25, -.2, 1.05, 32.375 };
    textLayer.opacity = .87F;
    auto rasterLayer = raster("Transformed raster", { 30, 21 }, { 190, 70, 120, 143 });
    rasterLayer.localToDocument = { .8, -.3, 35.125, .3, .8, 24.25 };
    rasterLayer.opacity = .61F;
    auto shapePixels = fromImage(shapeReference(shapeLayer, imageSize));
    shapePixels.opacity = shapeLayer.opacity;
    auto textPixels = fromImage(textReference(textLayer, imageSize));
    textPixels.opacity = textLayer.opacity;
    for (int fixture = 0; fixture < 3; ++fixture) {
        c::Document document({ extent, 96 }), reference({ extent, 96 });
        const auto backdrop = raster("Backdrop outside merge", extent, { 57, 91, 123, 255 });
        add(document, backdrop);
        add(reference, backdrop);
        std::vector<c::LayerId> selected;
        if (fixture == 2) {
            selected.push_back(add(document, rasterLayer));
            add(reference, rasterLayer);
        }
        if (fixture != 1) {
            selected.push_back(add(document, shapeLayer));
            add(reference, shapePixels);
        }
        if (fixture != 0) {
            selected.push_back(add(document, textLayer));
            add(reference, textPixels);
        }
        const auto unpoisoned = u::flattenLayerItems(document, selected);
        for (const auto id : selected)
            document.layer(id)->renderCache = misleadingCache();
        const SourceState before(document);
        const auto merged = u::flattenLayerItems(document, selected);
        CHECK(merged && unpoisoned && merged.origin == unpoisoned.origin && merged.image == unpoisoned.image);
        if (!merged)
            continue;
        CHECK(merged.image.width() < int(extent.width) && merged.image.height() < int(extent.height));
        c::Document replacement({ extent, 96 });
        add(replacement, backdrop);
        add(replacement, fromMerged(merged));
        const c::PinnedDocumentSampler expected(reference, {}, c::ColorSampleSource::MergedVisible);
        const c::PinnedDocumentSampler actual(replacement, {}, c::ColorSampleSource::MergedVisible);
        int maximumDifference = 0;
        for (uint32_t y = 0; y < extent.height; ++y)
            for (uint32_t x = 0; x < extent.width; ++x) {
                const c::Vec2d point { x + .5, y + .5 };
                const auto a = expected.sample(point), b = actual.sample(point);
                maximumDifference = std::max({ maximumDifference, std::abs(int(a.red) - int(b.red)),
                    std::abs(int(a.green) - int(b.green)), std::abs(int(a.blue) - int(b.blue)),
                    std::abs(int(a.alpha) - int(b.alpha)) });
            }
        if (maximumDifference > 1)
            std::cerr << "Typed merge fixture " << fixture << " maximum RGBA difference " << maximumDifference << '\n';
        // One final straight-RGBA8 storage boundary may move an opaque composited
        // channel by one level. Larger errors expose AA phase or double sampling.
        CHECK(maximumDifference <= 1);
        before.unchanged(document);
    }
}
void authoritativeTypedContent(const std::string& font)
{
    c::Document document({ { 80, 60 }, 144 });
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::RoundedRectangle;
    shape.size = { 30, 20 };
    shape.cornerRadius = 7;
    shape.fillEnabled = false;
    shape.strokeEnabled = true;
    shape.strokeWidth = 4;
    shape.strokeColor = { 210, 40, 60, 160 };
    auto shapeLayer = c::Layer::shape("Hollow rounded rectangle", shape);
    shapeLayer.localToDocument = { 0, -1, 10, -1, 0, 14 };
    shapeLayer.opacity = .75F;
    const auto shapeId = add(document, shapeLayer);
    c::TextLayer text;
    text.utf8 = "OO\nHi";
    text.defaultStyle.font = { font, "Regular", 400, false };
    text.defaultStyle.sizePixels = 17;
    text.defaultStyle.color = { 20, 210, 90, 180 };
    auto highlight = text.defaultStyle;
    highlight.sizePixels = 25;
    highlight.color = { 70, 100, 230, 130 };
    text.runs.push_back({ 1, 1, highlight });
    auto textLayer = c::Layer::text("Rich editable text", text);
    textLayer.localToDocument = { -1.25, .2, 30, .25, 1.5, 10 };
    const auto textId = add(document, textLayer);
    const std::array ids { shapeId, textId };
    const auto expected = u::flattenDocument(document);
    const auto original = u::flattenLayerItems(document, ids);
    CHECK(original && expected);
    document.layer(shapeId)->renderCache = misleadingCache();
    document.layer(textId)->renderCache = misleadingCache();
    const SourceState before(document);
    const auto merged = u::flattenLayerItems(document, ids);
    CHECK(merged && original && merged.image == original.image && merged.origin == original.origin);
    CHECK(merged && merged.origin.x < 0 && merged.origin.y < 0 && merged.image.width() < 200);
    if (merged) {
        bool red = false, green = false, blue = false, transparent = false, partial = false;
        for (int y = 0; y < merged.image.height(); ++y)
            for (int x = 0; x < merged.image.width(); ++x) {
                const auto color = pixel(merged.image, x, y);
                red = red || (color.red > 150 && color.green < 100 && color.alpha > 20);
                green = green || (color.green > 150 && color.alpha > 20);
                blue = blue || (color.blue > 150 && color.alpha > 20);
                transparent = transparent || color.alpha == 0;
                partial = partial || (color.alpha > 0 && color.alpha < 100);
            }
        CHECK(red && green && blue && transparent && partial);
        c::Document replacement(document.canvas());
        const auto mergedId = add(replacement, fromMerged(merged));
        const auto output = u::flattenDocument(replacement);
        CHECK(output && expected && sameAppearance(output.image, expected.image));
        CHECK(replacement.layer(mergedId)->opacity == 1 && std::holds_alternative<c::RasterLayer>(replacement.layer(mergedId)->payload));
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto path = directory.filePath("merged.vulkana");
        CHECK(u::saveProject(path, replacement));
        const auto reopened = u::loadProject(path);
        CHECK(reopened && reopened.document->layers().size() == 1
            && std::holds_alternative<c::RasterLayer>(reopened.document->layers()[0].payload)
            && reopened.document->tree().containers.empty());
        if (reopened) {
            const auto restored = u::flattenDocument(*reopened.document);
            CHECK(restored && restored.image == output.image);
        }
    }
    before.unchanged(document);
}

void hiddenAndRasterSelection()
{
    c::Document document({ { 20, 16 }, 96 });
    const auto visible = add(document, raster("Visible", { 8, 6 }, { 90, 120, 180, 128 }));
    c::ShapeLayer huge;
    huge.size = { 1e6, 1e6 };
    auto hidden = c::Layer::shape("Hidden giant content must not render", huge);
    hidden.visible = false;
    const auto hiddenId = add(document, hidden);
    auto zero = hidden;
    zero.id = c::makeLayerId();
    zero.name = "Zero-opacity content";
    zero.visible = true;
    zero.opacity = 0;
    const auto zeroId = add(document, zero);
    const auto group = c::makeLayerId();
    c::LayerTree tree;
    tree.roots = { group };
    tree.containers = { { group, "Hidden members", c::ContainerKind::Group, c::ColorLabel::None, { visible, hiddenId, zeroId } } };
    CHECK(document.replaceStructure(document.tree(), tree));
    const auto expected = u::flattenLayerItems(document, std::array { visible });
    CHECK(expected);
    for (const auto& selection : { c::SelectionMask::filled(document.canvas().extent, 0),
             c::SelectionMask::rectangle(document.canvas().extent, { 2, 2, 2, 2 }, 50) }) {
        CHECK(document.setSelection(selection));
        const SourceState before(document);
        const auto merged = u::flattenLayerItems(document, std::array { group });
        CHECK(merged && expected && merged.image == expected.image && merged.origin == expected.origin);
        before.unchanged(document);
    }
    CHECK(!u::flattenLayerItems(document, std::array { hiddenId, zeroId }));
    CHECK(!u::flattenLayerItems(document, std::span<const c::LayerId> {}));
    CHECK(!u::flattenLayerItems(document, std::array { c::makeLayerId() }));
}

void exactOutputDensityAndPreflight(const std::string& font)
{
    c::ShapeLayer ellipse;
    ellipse.kind = c::ShapeKind::Ellipse;
    ellipse.size = { 10, 6 };
    ellipse.fillColor = { 40, 180, 210, 175 };
    u::QtShapeRenderService service;
    const auto viewport = service.render({ ellipse, 16, {} });
    const auto exact = service.render({ ellipse, 16, size_t(65536) });
    CHECK(viewport && exact && viewport->density == 8 && exact->density == 16);
    CHECK(viewport->surface->extent() == c::Extent2u { 84, 52 });
    CHECK(exact->surface->extent() == c::Extent2u { 164, 100 });
    bool rejected = false;
    try { (void)service.render({ ellipse, 16, size_t(1) }); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected); // Never fall back to a lower-resolution destructive result.
    rejected = false;
    try { (void)service.render({ ellipse, 4000, size_t(64 * 1024 * 1024) }); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected); // >32768 output-cache edge rejected before allocating it.

    c::Document document({ { 20, 20 }, 96 });
    auto layer = c::Layer::shape("16x ellipse", ellipse);
    layer.localToDocument = { 16, 0, -3, 0, 16, 4 };
    layer.renderCache = viewport; // Existing viewport-cache ceiling is irrelevant.
    const auto id = add(document, layer);
    const SourceState before(document);
    const auto merged = u::flattenLayerItems(document, std::array { id });
    CHECK(merged && merged.image.size() == QSize(164, 100) && merged.origin == c::Vec2d { -5, 2 });
    if (merged) {
        const auto expectedPixels = pixels(*exact->surface);
        for (int y = 0; y < merged.image.height(); ++y)
            CHECK(std::memcmp(merged.image.constScanLine(y),
                expectedPixels.data() + size_t(y) * 164 * 4, 164 * 4) == 0);
    }
    const auto denied = u::flattenLayerItems(document, std::array { id }, {}, { 65536, 1, 65536 });
    CHECK(!denied && denied.image.isNull() && denied.error.contains("budget"));
    before.unchanged(document);

    c::TextLayer text;
    text.utf8 = "I";
    text.defaultStyle.font = { font, "Regular", 400, false };
    text.defaultStyle.sizePixels = 7;
    const u::QtTextLayout layout(text);
    const auto textViewport = layout.rasterize(16);
    const auto textExact = layout.rasterize(16, size_t(65536));
    CHECK(textViewport && textExact && textViewport->density == 8 && textExact->density == 16);
    CHECK(textExact->surface->extent().width >= textViewport->surface->extent().width * 2 - 1);
    CHECK(textExact->surface->extent().height >= textViewport->surface->extent().height * 2 - 1);
    rejected = false;
    try { (void)layout.rasterize(16, size_t(1)); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
}

void filteredRasterExtentAndCacheIndependence()
{
    c::Document document({ { 4, 4 }, 96 });
    auto layer = raster("Off-canvas filtered source", { 7, 5 }, { 180, 50, 100, 169 });
    layer.localToDocument = { 1, 0, 14, 0, 1, -3 };
    auto adjustments = std::make_shared<c::AdjustmentStack>();
    adjustments->items[0].enabled = true;
    adjustments->items[0].parameters = c::ExposureParameters { .7 };
    layer.adjustments = adjustments;
    auto filters = std::make_shared<c::SpatialFilterStack>();
    filters->items[0].enabled = true;
    filters->items[0].parameters = c::GaussianBlurParameters { 2, 3 };
    layer.filters = filters;
    auto reference = layer;
    reference.filterCache = c::prepareLayerSpatialFilters(reference);
    CHECK(reference.filterCache);
    if (!reference.filterCache)
        return;
    const c::PreparedLayerSampler sampler(reference);
    // A stale display result must never replace canonical source rendering.
    // Valid raster filter results are source-resolution and may be reused.
    auto poison = std::make_shared<c::LayerSpatialFilterCache>(*reference.filterCache);
    poison->surface = std::make_shared<c::ContiguousRasterSurface>(poison->surface->extent(), c::Rgba8 { 255, 0, 255, 255 });
    ++poison->sourceRevision;
    layer.filterCache = poison;
    layer.renderCache = misleadingCache();
    CHECK(!c::layerSpatialFilterCacheValid(layer));
    const auto id = add(document, layer);
    const SourceState before(document);
    const auto merged = u::flattenLayerItems(document, std::array { id });
    CHECK(merged && merged.origin == c::Vec2d { 12, -6 } && merged.image.size() == QSize(11, 11));
    if (merged) {
        bool equal = true, fringe = false;
        for (int y = 0; y < merged.image.height(); ++y)
            for (int x = 0; x < merged.image.width(); ++x) {
                const auto actual = pixel(merged.image, x, y);
                const auto expected = encode(sampler.sample({ merged.origin.x + x + .5, merged.origin.y + y + .5 }));
                equal = equal && actual == expected;
                fringe = fringe || ((x < 2 || y < 3) && actual.alpha > 0);
            }
        CHECK(equal && fringe);
    }
    before.unchanged(document);
}

void croppedTypedBoundsUseOnlyUnion()
{
    c::Document document({ { 4, 4 }, 96 });
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::Rectangle;
    shape.size = { 1e6, 1e6 };
    shape.fillColor = { 90, 150, 70, 255 };
    auto layer = c::Layer::shape("Tiny crop of huge source geometry", shape);
    layer.localToDocument = { 1, 0, 2, 0, 1, -3 };
    layer.crop = c::LayerCrop { 100, 200, 5, 4 };
    layer.renderCache = misleadingCache();
    const auto id = add(document, layer);
    const SourceState before(document);
    // The unneeded million-pixel-wide shape surface must never be allocated.
    const auto merged = u::flattenLayerItems(document, std::array { id }, {}, { 20, 512, 65536 });
    CHECK(merged && merged.origin == c::Vec2d { 102, 197 } && merged.image.size() == QSize(5, 4));
    if (merged)
        for (int y = 0; y < merged.image.height(); ++y)
            for (int x = 0; x < merged.image.width(); ++x)
                CHECK(pixel(merged.image, x, y) == shape.fillColor);
    before.unchanged(document);
}

void rotatedCropBoundsIntersectBeforeTransform()
{
    const double diagonal = std::sqrt(.5);
    for (const bool filtered : { false, true }) {
        c::Document document({ { 4, 4 }, 96 });
        auto layer = raster("Rotated cropped source", { 10, 10 }, { 80, 140, 210, 191 });
        layer.localToDocument = { diagonal, -diagonal, 20, diagonal, diagonal, 2 };
        if (filtered) {
            auto filters = std::make_shared<c::SpatialFilterStack>();
            filters->items[0].enabled = true;
            filters->items[0].parameters = c::GaussianBlurParameters { 2, 2 };
            layer.filters = filters;
        }
        // These local rectangles are disjoint even though their transformed
        // document AABBs overlap. An empty crop must not create a blank layer.
        layer.crop = c::LayerCrop { filtered ? 13.0 : 11.0, -5, 1, 20 };
        const auto id = add(document, layer);
        const auto empty = u::flattenLayerItems(document, std::array { id });
        CHECK(!empty && empty.image.isNull() && empty.error.contains("bounds"));

        // Intersect locally, allowing the crop's half-document-pixel coverage
        // footprint on each local axis. Two transformed AABBs alone inflate
        // this strip; an unexpanded local intersection can clip partial AA.
        document.layer(id)->crop = c::LayerCrop { filtered ? 11.0 : 9.0, -5, 1, 20 };
        const SourceState before(document);
        const auto merged = u::flattenLayerItems(document, std::array { id });
        CHECK(merged && merged.origin == c::Vec2d { 18, 7 }
            && merged.image.size() == (filtered ? QSize(12, 12) : QSize(10, 10)));
        if (merged) {
            const c::PreparedLayerSampler reference(*document.layer(id));
            const c::PreparedLayerSampler replacement(fromMerged(merged));
            bool equal = true;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 48; ++x) {
                    const c::Vec2d point { x + .5, y + .5 };
                    const auto expected = encode(reference.sample(point));
                    const auto actual = encode(replacement.sample(point));
                    equal = equal && ((expected.alpha == 0 && actual.alpha == 0) || expected == actual);
                }
            CHECK(equal);
        }
        before.unchanged(document);
    }
}

void rowCompositorMatchesLivePointSampling()
{
    const c::Extent2u extent { 9, 7 };
    std::vector<std::byte> bytes(size_t(extent.width) * extent.height * 4);
    uint32_t state = 0x73ac2e91;
    for (auto& byte : bytes) {
        state = state * 1664525 + 1013904223;
        byte = std::byte(state >> 24);
    }
    const auto surface = std::make_shared<c::ContiguousRasterSurface>(extent, bytes);
    const std::array transforms { c::AffineTransform { 1, 0, 3, 0, 1, 2 },
        c::AffineTransform { .5, 0, 3.25, 0, .5, 4.75 },
        c::AffineTransform { .8, -.6, 12.3, .6, .8, 2.7 },
        c::AffineTransform { -1.25, .3, 25.75, .2, .75, 6.125 } };
    for (const auto transform : transforms)
        for (const auto mode : c::allBlendModes) {
            auto backdrop = raster("Opaque backdrop", { 32, 24 }, { 79, 153, 204, 255 });
            auto layer = c::Layer::raster("Row source", surface);
            layer.localToDocument = transform;
            layer.opacity = .713F;
            layer.blendMode = mode;
            layer.crop = c::LayerCrop { .25, .75, 8.25, 5.5 };
            layer.crop->corners = { 1, 0, .5, 1.5 };
            auto adjustments = std::make_shared<c::AdjustmentStack>();
            adjustments->items[0].enabled = true;
            adjustments->items[0].parameters = c::ExposureParameters { .37 };
            layer.adjustments = adjustments;
            const std::array<const c::Layer*, 2> layers { &backdrop, &layer };
            const c::PinnedDocumentSampler sampler(layers, { 32, 24 });
            std::array<c::PremultipliedColor, 32> row;
            bool equal = true;
            for (int y = 0; y < 24; ++y) {
                sampler.sampleRow(0, y, row);
                for (size_t x = 0; x < row.size(); ++x) {
                    const auto expected = sampler.sampleLinear({ double(x) + .5, y + .5 });
                    for (size_t channel = 0; channel < 4; ++channel)
                        equal = equal && std::abs(row[x][channel] - expected[channel]) <= 2e-6F;
                }
            }
            CHECK(equal);
            const c::PinnedDocumentSampler region(layers, { 8, 6 }, {}, { 5, 4 });
            std::array<c::PremultipliedColor, 8> regionRow;
            bool regionEqual = true;
            for (int y = 0; y < 6; ++y) {
                region.sampleRow(0, y, regionRow);
                for (size_t x = 0; x < regionRow.size(); ++x) {
                    const auto expected = sampler.sampleLinear({ double(x) + 5.5, y + 4.5 });
                    const auto point = region.sampleLinear({ double(x) + .5, y + .5 });
                    for (size_t channel = 0; channel < 4; ++channel)
                        regionEqual = regionEqual && std::abs(regionRow[x][channel] - expected[channel]) <= 2e-6F
                            && point[channel] == expected[channel];
                }
            }
            CHECK(regionEqual);
        }
}

void cancellationLimitsAndRevisionChecks()
{
    c::Document document({ { 20, 16 }, 96 });
    const auto id = add(document, raster("Large off-canvas source", { 256, 256 }, { 70, 120, 180, 200 }));
    const std::array ids { id };
    const SourceState before(document);
    for (const auto threshold : { uint64_t(0), uint64_t(1), uint64_t(4096), uint64_t(65537) }) {
        const auto result = u::flattenLayerItems(document, ids,
            [=](auto done, auto) { return done < threshold; });
        CHECK(!result && result.cancelled && result.image.isNull());
        before.unchanged(document);
    }
    const auto boundsLimit = u::flattenLayerItems(document, ids, {}, { 100, 65536, 65536 });
    CHECK(!boundsLimit && boundsLimit.error.contains("bounds"));
    const auto metadataLimit = u::flattenLayerItems(document, ids, {}, { 65536, 65536, 0 });
    CHECK(!metadataLimit && metadataLimit.error.contains("metadata"));
    before.unchanged(document);

    c::Document shapeDocument({ { 10, 10 }, 96 });
    const auto shapeId = add(shapeDocument, c::Layer::shape("Cache budget", {}));
    const SourceState shapeBefore(shapeDocument);
    const auto cacheLimit = u::flattenLayerItems(shapeDocument, std::array { shapeId }, {}, { 65536, 1, 65536 });
    CHECK(!cacheLimit && cacheLimit.error.contains("cache"));
    shapeBefore.unchanged(shapeDocument);

    // External changes deliberately remain; output detects them and never
    // restores stale content over the caller's edit or publishes mixed pixels.
    bool changed = false;
    const auto geometryChange = u::flattenLayerItems(document, ids, [&](auto done, auto) {
        if (done && !changed) {
            changed = true;
            CHECK(document.setLayerOpacity(id, .25F));
        }
        return true;
    });
    CHECK(changed && !geometryChange && geometryChange.image.isNull() && geometryChange.error.contains("changed"));
    CHECK(document.layer(id)->opacity == .25F && document.tree() == before.tree);
    auto surface = std::get<c::RasterLayer>(document.layer(id)->payload).surface;
    const auto originalRevision = surface->revision();
    changed = false;
    const auto pixelsChange = u::flattenLayerItems(document, ids, [&](auto done, auto) {
        if (done && !changed) {
            changed = true;
            const std::array bytes { std::byte(1), std::byte(2), std::byte(3), std::byte(4) };
            (void)surface->replaceRgba8({ 0, 0, 1, 1 }, bytes, 4);
        }
        return true;
    });
    CHECK(changed && !pixelsChange && pixelsChange.image.isNull() && pixelsChange.error.contains("changed"));
    CHECK(surface->revision() == originalRevision + 1 && pixels(*surface)[0] == std::byte(1));
    CHECK(document.tree() == before.tree && document.layers().size() == 1);
}

QImage decodedPng(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes); CHECK(buffer.open(QIODevice::WriteOnly));
    CHECK(image.save(&buffer,"PNG"));
    auto decoded=QImage::fromData(bytes,"PNG").convertToFormat(QImage::Format_RGBA8888);
    CHECK(!decoded.isNull()); return decoded;
}

// Independent reference: spell out the consolidated layer order, evaluate
// original models per document pixel, then encode/decode a native-size PNG.
// Neither the merge flattener nor the structural consolidation helper is used.
QImage referencePng(const c::Document& doc, const std::vector<c::LayerId>& order)
{
    std::vector<c::Layer> layers;
    std::vector<c::PreparedLayerSampler> samples;
    layers.reserve(order.size()); samples.reserve(order.size());
    for(auto id:order) {
        layers.push_back(*doc.layer(id));auto& l=layers.back();
        if(auto* s=std::get_if<c::ShapeLayer>(&l.payload))l.renderCache=u::QtShapeRenderService{}.renderDocument(*s,l.localToDocument,65536);
        if(auto* t=std::get_if<c::TextLayer>(&l.payload))l.renderCache=u::QtTextLayout(*t).rasterizeDocument(l.localToDocument,65536);
        samples.emplace_back(l);
    }
    const auto e=doc.canvas().extent;
    QImage image(int(e.width),int(e.height),QImage::Format_RGBA8888);
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
        c::PremultipliedColor value{};
        for(size_t i=0;i<layers.size();++i)
            value=c::compositeLayer(value,samples[i].sample({x+.5,y+.5}),layers[i].opacity,layers[i].blendMode);
        auto rgba=encode(value);if(!rgba.alpha)rgba={};
        auto* p=image.scanLine(y)+x*4;p[0]=rgba.red;p[1]=rgba.green;p[2]=rgba.blue;p[3]=rgba.alpha;
    }
    return decodedPng(image);
}

void consolidationOrderHistoryAndPng(const std::string& family)
{
    for(int hierarchy=0;hierarchy<3;++hierarchy)for(bool typed:{false,true})for(bool overlap:{false,true}) {
        QImage firstResult;
        for(bool reversed:{false,true}) {
            auto document=std::make_unique<c::Document>(c::CanvasSpec{{64,48},96});
            auto& doc=*document;
            auto lowerLayer=raster("Selected lower",{20,18},{230,40,20,255});
            lowerLayer.localToDocument.m02=4;lowerLayer.localToDocument.m12=3;
            const auto lower=add(doc,lowerLayer);
            c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={13.5,11.25};
            shape.fillColor={200,170,40,170};shape.strokeEnabled=true;shape.strokeWidth=1.5;
            auto extraLayer=c::Layer::shape("Selected child",shape);extraLayer.localToDocument={.9,-.2,12.5,.2,.9,8.25};
            const auto extra=hierarchy==2?add(doc,extraLayer):c::LayerId{};
            auto interveningLayer=raster("Unselected intervening",{20,18},{10,220,60,255});
            interveningLayer.localToDocument.m02=overlap?4:38;interveningLayer.localToDocument.m12=overlap?3:28;
            const auto intervening=add(doc,interveningLayer);
            auto upperLayer=raster("Selected upper",{20,18},{20,50,240,110});
            if(typed) {
                c::TextLayer text;text.utf8="Aa ffi";text.defaultStyle.font.family=family;
                text.defaultStyle.sizePixels=17;text.defaultStyle.color={30,80,230,190};
                upperLayer=c::Layer::text("Selected upper text",c::normalizedText(text));
            }
            upperLayer.localToDocument={1,typed?.1:0,6.0,0,1,5};
            const auto upper=add(doc,upperLayer);
            auto capLayer=raster("Unselected above",{3,4},{220,100,250,255});
            capLayer.localToDocument.m02=5;capLayer.localToDocument.m12=5;
            const auto cap=add(doc,capLayer);
            const auto lowFolder=c::makeLayerId(),highFolder=c::makeLayerId(),nested=c::makeLayerId();
            if(hierarchy) {
                auto tree=doc.tree();tree.roots={lowFolder,intervening,highFolder};
                tree.containers={{lowFolder,"Lower folder",c::ContainerKind::Folder,c::ColorLabel::Red,{lower}},
                    {highFolder,"Upper folder",c::ContainerKind::Folder,c::ColorLabel::Blue,{upper,cap}}};
                if(extra) {
                    tree.containers.front().children.push_back(nested);
                    tree.containers.push_back({nested,"Nested group",c::ContainerKind::Group,c::ColorLabel::Green,{extra}});
                }
                CHECK(doc.replaceStructure(doc.tree(),std::move(tree),{},{}));
            }
            c::EditorSession session;session.replaceDocument(std::move(document));
            std::vector<c::LayerId> ids=hierarchy==2?std::vector{lowFolder,lower,extra,upper}:std::vector{lower,upper};
            if(reversed)std::ranges::reverse(ids);
            session.setLayerSelection(ids,ids.back(),ids.front());
            CHECK(session.execute(std::make_unique<c::SetItemMetadataCommand>(lower,"Temporary name",c::ColorLabel::None)));
            CHECK(session.undo());
            const auto selection=session.layerSelectionState();const SourceState before(doc);
            const auto originalPng=decodedPng(u::flattenDocument(doc).image);
            std::vector<c::LayerId> planned={intervening,lower};if(extra)planned.push_back(extra);
            planned.push_back(upper);planned.push_back(cap);
            const auto expected=referencePng(doc,planned);
            const auto redo=session.history().redoDepth();
            const auto cancelled=u::flattenLayerItems(doc,ids,[](auto done,auto){return done<3;});
            CHECK(cancelled.cancelled && !cancelled);before.unchanged(doc);
            CHECK(session.history().redoDepth()==redo && session.layerSelectionState()==selection);
            const auto baked=u::flattenLayerItems(doc,ids);CHECK(baked);if(!baked)continue;
            auto merged=fromMerged(baked);const auto mergedId=merged.id;
            CHECK(session.execute(c::consolidateLayerItems(doc,selection,std::move(merged))));
            CHECK(session.history().undoDepth()==1 && session.selectedLayers()==std::vector{mergedId});
            CHECK(doc.tree().placement(mergedId)==c::ItemPlacement(hierarchy?highFolder:0,hierarchy?0:1));
            CHECK(doc.layer(intervening)->localToDocument==interveningLayer.localToDocument);
            CHECK(std::get<c::RasterLayer>(doc.layer(intervening)->payload).surface==std::get<c::RasterLayer>(interveningLayer.payload).surface);
            CHECK(doc.layer(cap) && !doc.layer(lower) && !doc.layer(upper));
            if(hierarchy==1)CHECK(doc.tree().container(lowFolder) && doc.tree().container(lowFolder)->children.empty());
            if(hierarchy==2)CHECK(!doc.containsItem(lowFolder) && !doc.containsItem(nested) && !doc.layer(extra));
            std::vector<c::LayerId> actualOrder;for(const auto& l:doc.layers())actualOrder.push_back(l.id);
            CHECK(actualOrder==std::vector<c::LayerId>({intervening,mergedId,cap}));
            const auto actual=decodedPng(u::flattenDocument(doc).image);
            bool equal=true;for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x)
                equal=equal&&near(pixel(actual,x,y),pixel(expected,x,y),typed||extra?2:0);
            CHECK(equal);
            if(overlap)CHECK(actual!=originalPng);
            else {
                bool unchanged=true;for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x)
                    unchanged=unchanged&&near(pixel(actual,x,y),pixel(originalPng,x,y),typed||extra?2:0);
                CHECK(unchanged);
            }
            if(firstResult.isNull())firstResult=actual;else CHECK(actual==firstResult);
            const auto mergedTree=doc.tree();
            QTemporaryDir directory;const auto path=directory.filePath("consolidated.vulkana");
            CHECK(u::saveProject(path,doc));auto reopened=u::loadProject(path);
            CHECK(reopened && reopened.document->tree()==mergedTree);
            if(reopened)CHECK(decodedPng(u::flattenDocument(*reopened.document).image)==actual);
            CHECK(session.undo());CHECK(session.layerSelectionState()==selection);before.unchanged(doc,false,false);
            CHECK(u::saveProject(path,doc));reopened=u::loadProject(path);
            CHECK(reopened && reopened.document->tree()==before.tree);
            if(reopened)for(const auto& original:before.layers) {
                const auto* restored=reopened.document->layer(original.id);
                CHECK(restored && restored->payload.index()==original.payload.index() && restored->localToDocument==original.localToDocument);
                if(restored && std::holds_alternative<c::TextLayer>(original.payload))CHECK(std::get<c::TextLayer>(restored->payload)==std::get<c::TextLayer>(original.payload));
                if(restored && std::holds_alternative<c::ShapeLayer>(original.payload))CHECK(std::get<c::ShapeLayer>(restored->payload)==std::get<c::ShapeLayer>(original.payload));
            }
            CHECK(session.redo());CHECK(doc.tree()==mergedTree && session.activeLayer()==mergedId);
            CHECK(decodedPng(u::flattenDocument(doc).image)==actual);
        }
    }
}

void consolidationTopmostContainer()
{
    // A selected topmost root is a container slot, not its last leaf. This is
    // also true for an empty selected folder above other selected content.
    for(bool empty:{false,true}) {
        auto document=std::make_unique<c::Document>(c::CanvasSpec{{16,16},96});
        auto& doc=*document;
        const auto lower=add(doc,raster("Lower",{16,16},{220,50,20,255}));
        const auto middle=add(doc,raster("Unselected intervener",{16,16},{20,200,80,255}));
        const auto upper=empty?c::LayerId{}:add(doc,raster("Upper child",{8,8},{50,40,210,150}));
        const auto cap=add(doc,raster("Unselected above",{3,3},{220,190,40,255}));
        const auto parent=c::makeLayerId(),top=c::makeLayerId();
        auto tree=doc.tree();tree.roots={lower,middle,parent};
        tree.containers={{parent,"Retained parent",c::ContainerKind::Folder,c::ColorLabel::Blue,{top,cap}},
            {top,"Selected container",empty?c::ContainerKind::Folder:c::ContainerKind::Group,c::ColorLabel::Red,{}}};
        if(upper)tree.containers.back().children.push_back(upper);
        CHECK(doc.replaceStructure(doc.tree(),std::move(tree),{},{}));
        c::EditorSession session;session.replaceDocument(std::move(document));
        std::vector<c::LayerId> selected{top,lower};if(upper)selected.push_back(upper);
        session.setLayerSelection(selected,lower,top);
        const auto state=session.layerSelectionState();const SourceState before(doc);
        std::vector<c::LayerId> order{middle,lower};if(upper)order.push_back(upper);order.push_back(cap);
        const auto expected=referencePng(doc,order);
        const auto baked=u::flattenLayerItems(doc,selected);CHECK(baked);if(!baked)continue;
        auto merged=fromMerged(baked);const auto id=merged.id;
        CHECK(session.execute(c::consolidateLayerItems(doc,state,std::move(merged))));
        CHECK(doc.tree().placement(id)==c::ItemPlacement(parent,0));
        CHECK(doc.tree().container(parent)->children==std::vector<c::LayerId>({id,cap}));
        CHECK(!doc.containsItem(top) && doc.layer(middle) && doc.layer(cap));
        CHECK(decodedPng(u::flattenDocument(doc).image)==expected);
        CHECK(session.undo());before.unchanged(doc,false,false);CHECK(session.layerSelectionState()==state);
        CHECK(session.redo());CHECK(session.selectedLayers()==std::vector{id});
        CHECK(decodedPng(u::flattenDocument(doc).image)==expected);
    }
}

void isolatedBlendAdmission()
{
    c::Document doc({{16,16},96});
    const auto lower=add(doc,raster("Selected shield",{16,16},{230,90,40,255}));
    const auto middle=add(doc,raster("Intervening external",{16,16},{20,200,90,180}));
    auto top=raster("Selected blend",{16,16},{90,60,200,160});const auto upper=add(doc,top);
    const std::array ids{upper,lower};
    for(auto mode:c::allBlendModes) {
        CHECK(doc.setLayerBlendMode(upper,mode)||mode==c::BlendMode::Normal);
        const SourceState before(doc);
        CHECK(u::flattenLayerItems(doc,ids)); // Every named mode is supported over selected-only content.
        before.unchanged(doc);
    }
    CHECK(doc.setLayerBlendMode(upper,c::BlendMode::Multiply));
    CHECK(doc.setLayerOpacity(lower,.5f));
    const SourceState before(doc);const auto isolated=u::flattenLayerItems(doc,ids);
    CHECK(isolated);before.unchanged(doc);
    for(const auto mode:{c::BlendMode::Multiply,c::BlendMode::Screen,c::BlendMode::Darken,c::BlendMode::Lighten,
        c::BlendMode::LinearDodge,c::BlendMode::LinearBurn,c::BlendMode::HardLight}) {
        const bool white=mode==c::BlendMode::Screen||mode==c::BlendMode::Lighten||mode==c::BlendMode::LinearDodge;
        const c::Rgba8 color=mode==c::BlendMode::HardLight?c::Rgba8{255,0,0,130}:white?c::Rgba8{255,255,255,130}:c::Rgba8{0,0,0,130};
        doc.layer(upper)->payload=c::RasterLayer{std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{16,16},color)};
        CHECK(doc.setLayerBlendMode(upper,mode)||mode==c::BlendMode::Multiply);
        const auto baked=u::flattenLayerItems(doc,ids);CHECK(baked);
        if(baked) {
            const auto expected=referencePng(doc,{middle,lower,upper});
            c::Document result(doc.canvas());add(result,*doc.layer(middle));add(result,fromMerged(baked));
            const auto actual=decodedPng(u::flattenDocument(result).image);
            bool equal=true;for(int y=0;y<16;++y)for(int x=0;x<16;++x)equal=equal&&near(pixel(actual,x,y),pixel(expected,x,y),1);
            CHECK(equal);
        }
    }
    doc.layer(upper)->payload=top.payload;CHECK(doc.setLayerBlendMode(upper,c::BlendMode::Multiply));
    CHECK(doc.setLayerVisibility(middle,false));CHECK(u::flattenLayerItems(doc,ids));
    CHECK(doc.setLayerVisibility(middle,true));
    auto moved=doc.layer(middle)->localToDocument;moved.m02=100;
    CHECK(doc.setLayerTransform(middle,moved));CHECK(u::flattenLayerItems(doc,ids));
    moved.m02=0;CHECK(doc.setLayerTransform(middle,moved));
    // No appearance-preservation veto remains, with or without selected cover.
    const auto cover=add(doc,raster("Opaque selected cover",{16,16},{190,100,60,255}));
    CHECK(u::flattenLayerItems(doc,std::array{lower,upper,cover}));
    // A non-Normal bottommost selected layer evaluates over transparency.
    CHECK(doc.setLayerVisibility(cover,false));
    CHECK(doc.setLayerBlendMode(lower,c::BlendMode::Screen));
    CHECK(doc.setLayerBlendMode(upper,c::BlendMode::Normal));
    CHECK(u::flattenLayerItems(doc,ids)); // Isolated output no longer needs an external backdrop proof.
}

void optionalBenchmark()
{
    using Clock = std::chrono::steady_clock;
    for (const auto extent : { c::Extent2u { 3840, 2160 }, c::Extent2u { 5120, 2880 } }) {
        c::Document document({ extent, 96 });
        const auto lowerSurface = std::make_shared<c::ContiguousRasterSurface>(extent, c::Rgba8 { 220, 90, 45, 160 });
        const auto upperSurface = std::make_shared<c::ContiguousRasterSurface>(extent, c::Rgba8 { 20, 100, 210, 120 });
        const auto lower = add(document, c::Layer::raster("Benchmark lower", lowerSurface));
        const auto upper = add(document, c::Layer::raster("Benchmark upper", upperSurface));
        const std::array targets { lower, upper };
        const auto lowerId = lowerSurface->id(), upperId = upperSurface->id();
        const auto lowerRevision = lowerSurface->revision(), upperRevision = upperSurface->revision();
        const auto documentRevision = document.revision(), contentState = document.contentState();
        const auto started = Clock::now();
        auto previousCallback = started;
        double maximumCallbackGapMs = 0;
        uint64_t callbacks = 0;
        u::MergeProfile profile;
        const auto merged = u::flattenLayerItems(document, targets, [&](auto, auto) {
            const auto now = Clock::now();
            maximumCallbackGapMs = std::max(maximumCallbackGapMs,
                std::chrono::duration<double, std::milli>(now - previousCallback).count());
            previousCallback = now;
            ++callbacks;
            return true;
        },{},&profile);
        const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        CHECK(merged && merged.image.size() == QSize(int(extent.width), int(extent.height))
            && merged.image.format() == QImage::Format_RGBA8888 && merged.origin == c::Vec2d {});
        if (!merged)
            std::cerr << "Benchmark failed: " << merged.error.toStdString() << '\n';
        CHECK(document.revision() == documentRevision && document.contentState() == contentState);
        CHECK(document.layers().size() == 2 && document.layers()[0].id == lower && document.layers()[1].id == upper);
        CHECK(std::get<c::RasterLayer>(document.layer(lower)->payload).surface == lowerSurface
            && std::get<c::RasterLayer>(document.layer(upper)->payload).surface == upperSurface);
        CHECK(lowerSurface->id() == lowerId && upperSurface->id() == upperId
            && lowerSurface->revision() == lowerRevision && upperSurface->revision() == upperRevision);
        const auto expectedBytes = uint64_t(extent.width) * extent.height * 4;
        CHECK(merged && uint64_t(merged.image.sizeInBytes()) == expectedBytes);
        std::cout << "MERGE_BENCHMARK " << extent.width << 'x' << extent.height
                  << " layers=2 elapsed_ms=" << std::fixed << std::setprecision(3) << elapsedMs
                  << " max_callback_gap_ms=" << maximumCallbackGapMs
                  << " callbacks=" << callbacks
                  << " output_bytes=" << merged.image.sizeInBytes()
                  << " setup_ms=" << profile.setupMs << " typed_ms=" << profile.typedRasterizationMs
                  << " effects_ms=" << profile.spatialEffectsMs << " bounds_ms=" << profile.boundsMs
                  << " allocation_ms=" << profile.allocationMs << " sample_blend_ms=" << profile.samplingBlendMs
                  << " encode_ms=" << profile.encodingMs << '\n';
        const auto copyStart=Clock::now();auto replacement=fromMerged(merged);
        const auto copyEnd=Clock::now();
        auto command=c::consolidateLayerItems(document,c::LayerSelectionState{{lower,upper},upper,lower},std::move(replacement));
        const auto historyPrepared=Clock::now();CHECK(command->apply(document));const auto historyApplied=Clock::now();
        std::cout << "MERGE_PUBLICATION " << extent.width << 'x' << extent.height
            << " raster_transfer_ms=" << std::chrono::duration<double,std::milli>(copyEnd-copyStart).count()
            << " history_prepare_ms=" << std::chrono::duration<double,std::milli>(historyPrepared-copyEnd).count()
            << " history_apply_ms=" << std::chrono::duration<double,std::milli>(historyApplied-historyPrepared).count() << '\n';
        CHECK(command->undo(document));
    }
    CHECK(QFontDatabase::addApplicationFont(QStringLiteral(IMAGEEDITOR_MERGE_FONT_DIR "/NotoSans-Regular.ttf"))>=0);
    for(bool filtered:{false,true}) {
        c::Document document({{1024,768},96});
        std::vector<c::LayerId> ids;
        ids.push_back(add(document,raster("Mixed base",{1024,768},{30,80,140,255})));
        c::ShapeLayer shape;shape.kind=c::ShapeKind::RoundedRectangle;shape.size={800,500};
        shape.fillColor={240,80,60,190};shape.strokeEnabled=true;shape.strokeWidth=3.5;
        auto layer=c::Layer::shape("Filtered shape",shape);layer.localToDocument={.95,-.15,100,.15,.95,50};
        if(filtered) {
            auto filters=std::make_shared<c::SpatialFilterStack>();filters->items[0].enabled=true;
            filters->items[0].parameters=c::GaussianBlurParameters{6,6};layer.filters=filters;
        }
        ids.push_back(add(document,layer));
        c::TextLayer text;text.utf8="Native document pixels\nEditable until merged";
        text.defaultStyle.font.family="Noto Sans";text.defaultStyle.sizePixels=42;
        text.defaultStyle.color={220,220,230,210};
        auto label=c::Layer::text("Text",c::normalizedText(text));label.localToDocument.m02=100;label.localToDocument.m12=180;
        ids.push_back(add(document,label));u::MergeProfile profile;
        const auto start=Clock::now();const auto merge=u::flattenLayerItems(document,ids,{}, {}, &profile);CHECK(merge);
        std::cout << "MERGE_MIXED_PROFILE filtered=" << filtered
            << " total_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count()
            << " typed_ms=" << profile.typedRasterizationMs << " effects_ms=" << profile.spatialEffectsMs
            << " bounds_ms=" << profile.boundsMs << " allocation_ms=" << profile.allocationMs
            << " sample_blend_ms=" << profile.samplingBlendMs << " encode_ms=" << profile.encodingMs
            << " derived_bytes=" << profile.derivedBytes << '\n';
    }
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        if (std::any_of(argv + 1, argv + argc,
                [](const char* argument) { return std::string_view(argument) == "--benchmark"; })) {
            optionalBenchmark();
            return failures ? 1 : 0;
        }
        const auto fixture = QFontDatabase::addApplicationFont(QStringLiteral(IMAGEEDITOR_MERGE_FONT_DIR "/NotoSans-Regular.ttf"));
        CHECK(fixture >= 0);
        const auto families = QFontDatabase::applicationFontFamilies(fixture);
        CHECK(!families.empty());
        if (families.empty())
            return 1;
        passThroughAndOpacity();
        identityRasterCopies();
        transformedRasterUsesOriginal();
        offCanvasOrientationAndAlpha();
        typedAndMixedDocumentPixelAppearance(families.front().toStdString());
        authoritativeTypedContent(families.front().toStdString());
        hiddenAndRasterSelection();
        exactOutputDensityAndPreflight(families.front().toStdString());
        filteredRasterExtentAndCacheIndependence();
        croppedTypedBoundsUseOnlyUnion();
        rotatedCropBoundsIntersectBeforeTransform();
        rowCompositorMatchesLivePointSampling();
        consolidationOrderHistoryAndPng(families.front().toStdString());
        consolidationTopmostContainer();
        isolatedBlendAdmission();
        cancellationLimitsAndRevisionChecks();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << error.what() << '\n';
    }
    if (!failures)
        std::cout << "Layer merge authoritative rendering, grouping, opacity, off-canvas bounds, typed content, cancellation and limits passed\n";
    return failures ? 1 : 0;
}
