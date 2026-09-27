#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SelectionCommands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        if (failures < 60) std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
void near(double actual, double expected, double tolerance, std::string_view label)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        if (failures < 60) std::cerr << label << ": " << actual << " vs " << expected << '\n';
        ++failures;
    }
}

double cross(Vec2d a, Vec2d b) { return a.x * b.y - a.y * b.x; }

// Independent double-precision oracle. Unlike the production routine (which
// clips a pixel quad against an axis-aligned crop), this clips the crop against
// the four oriented pixel-edge half planes. The final area uses a triangle fan
// rather than a cyclic origin-based sum. No shipping coverage/math helper is
// used in the reference.
double referenceCoverage(RectD crop, Vec2d center, Vec2d dx, Vec2d dy)
{
    if (crop.width <= 0 || crop.height <= 0) return 0;
    const double determinant = cross(dx, dy);
    if (determinant == 0) return 0;
    const double orientation = determinant > 0 ? 1 : -1;
    const std::array footprint {
        (dx + dy) * -.5, (dx - dy) * .5,
        (dx + dy) * .5, (dy - dx) * .5};
    std::vector<Vec2d> polygon {
        {crop.x - center.x, crop.y - center.y},
        {crop.right() - center.x, crop.y - center.y},
        {crop.right() - center.x, crop.bottom() - center.y},
        {crop.x - center.x, crop.bottom() - center.y}};
    for (std::size_t side = 0; side < footprint.size(); ++side) {
        if (polygon.empty()) return 0;
        const auto origin = footprint[side];
        const auto edge = footprint[(side + 1) % footprint.size()] - origin;
        auto distance = [&](Vec2d p) { return orientation * cross(edge, p - origin); };
        std::vector<Vec2d> clipped;
        auto previous = polygon.back();
        double previousDistance = distance(previous);
        for (const auto current : polygon) {
            const double currentDistance = distance(current);
            if ((previousDistance >= 0) != (currentDistance >= 0)) {
                const double t = previousDistance / (previousDistance - currentDistance);
                clipped.push_back(previous + (current - previous) * t);
            }
            if (currentDistance >= 0) clipped.push_back(current);
            previous = current;
            previousDistance = currentDistance;
        }
        polygon = std::move(clipped);
    }
    if (polygon.size() < 3) return 0;
    double twiceArea = 0;
    for (std::size_t i = 2; i < polygon.size(); ++i)
        twiceArea += cross(polygon[i - 1] - polygon[0], polygon[i] - polygon[0]);
    return std::clamp(std::abs(twiceArea) / (2 * std::abs(determinant)), 0.0, 1.0);
}

AffineTransform rotation(double degrees)
{
    const double a = degrees * std::numbers::pi / 180;
    return {.m00 = std::cos(a), .m01 = -std::sin(a),
        .m10 = std::sin(a), .m11 = std::cos(a)};
}
std::array<AffineTransform, 5> sourceMappings()
{
    return {AffineTransform {.m02 = 12, .m12 = 10},
        AffineTransform {.m00 = 2, .m02 = 7, .m11 = .75, .m12 = 13},
        composeAffine(AffineTransform {.m02 = 22, .m12 = 12}, rotation(37)),
        AffineTransform {.m00 = -1.1, .m01 = .45, .m02 = 34,
            .m10 = .2, .m11 = 1.3, .m12 = 9},
        composeAffine(AffineTransform {.m02 = 32, .m12 = 28},
            composeAffine(rotation(-71), AffineTransform {.m00 = .65, .m01 = -.4, .m11 = -1.5}))};
}

std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto e = surface.extent();
    std::vector<std::byte> result(std::size_t(e.width) * e.height * 4);
    surface.copyRgba8({0, 0, int(e.width), int(e.height)}, result, std::size_t(e.width) * 4);
    return result;
}
Rgba8 pixel(const RasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> data {};
    surface.copyRgba8({x, y, 1, 1}, data, 4);
    return {std::to_integer<std::uint8_t>(data[0]), std::to_integer<std::uint8_t>(data[1]),
        std::to_integer<std::uint8_t>(data[2]), std::to_integer<std::uint8_t>(data[3])};
}
DirtySet writeSolid(RasterEditTransaction& edit, RectI region, Rgba8 color)
{
    std::vector<std::byte> data(std::size_t(region.width) * std::size_t(region.height) * 4);
    for (std::size_t i = 0; i < data.size(); i += 4) {
        data[i] = std::byte(color.red); data[i + 1] = std::byte(color.green);
        data[i + 2] = std::byte(color.blue); data[i + 3] = std::byte(color.alpha);
    }
    return edit.writeRgba8(region, data, std::size_t(region.width) * 4);
}
struct Fixture {
    Document document {CanvasSpec {.extent = {64, 64}}};
    History history;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId id;
    explicit Fixture(Extent2u extent = {16, 12}, Rgba8 color = {80, 130, 220, 200})
        : surface(std::make_shared<ContiguousRasterSurface>(extent, color))
    {
        auto layer = Layer::raster("Crop source", surface);
        id = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
};

void exactCoverageFixtures()
{
    const Vec2d dx {1, 0}, dy {0, 1}, p {.5, .5};
    CHECK(layerCropCoverage({0, 0, 1, 1}, p, dx, dy) == 1);
    CHECK(layerCropCoverage({1, 0, 1, 1}, p, dx, dy) == 0);
    CHECK(layerCropCoverage({0, 1, 1, 1}, p, dx, dy) == 0);
    CHECK(layerCropCoverage({.5, 0, 1, 1}, p, dx, dy) == .5F);
    CHECK(layerCropCoverage({.5, .5, 1, 1}, p, dx, dy) == .25F);
    CHECK(layerCropCoverage({.25, .25, .5, .5}, p, dx, dy) == .25F);
    CHECK(layerCropCoverage({0, 0, 0, 9}, p, dx, dy) == 0);
    CHECK(layerCropCoverage({0, 0, 9, 0}, p, dx, dy) == 0);
    CHECK(layerCropCoverage({0, 0, -1, 1}, p, dx, dy) == 0);
    CHECK(layerCropCoverage({0, 0, 1, 1}, {std::numeric_limits<double>::infinity(), 0}, dx, dy) == 0);
    CHECK(layerCropCoverage({0, 0, 1, 1}, p, {std::numeric_limits<double>::quiet_NaN(), 0}, dy) == 0);
    // Translation must not destroy subpixel coverage far from the origin.
    for (const double offset : {-100'000'000.0, 100'000'000.0})
        near(layerCropCoverage({offset + .25, offset + .25, .5, .5},
                 {offset + .5, offset + .5}, dx, dy), .25, 1e-7, "Far-origin crop");
    // A 45-degree pixel centered on a straight crop edge has half its area
    // covered; mirroring the footprint must not reverse the inside test.
    const double q = std::sqrt(.5);
    for (const double flip : {-1.0, 1.0})
        near(layerCropCoverage({0, -10, 10, 20}, {}, {q * flip, q * flip}, {-q, q}),
            .5, 1e-7, "Rotated/flipped half-plane");
    // Alpha is applied to all premultiplied channels, not alpha alone or the
    // underlying straight RGB. Uncropped output remains bit-identical.
    const PremultipliedColor source {.031F, .11F, .299F, .37F};
    CHECK(applyLayerCrop(source, {}, {}, {.5, .5}) == source);
    const auto half = applyLayerCrop(source, RectD {.5, 0, 2, 2}, {}, {.5, .5});
    for (std::size_t c = 0; c < 4; ++c) CHECK(half[c] == source[c] * .5F);
    CHECK(applyLayerCrop(source, RectD {0, 0, 0, 1}, {}, p) == PremultipliedColor {});
    CHECK(cropAllowsTexel(RectD {.9, .9, .2, .2}, 0, 0));
    CHECK(cropAllowsTexel(RectD {.9, .9, .2, .2}, 1, 1));
    CHECK(!cropAllowsTexel(RectD {1, 1, 2, 2}, 0, 1));
    CHECK(!cropAllowsTexel(RectD {1, 1, 0, 2}, 1, 1));
    CHECK(cropAllowsTexel({}, -100, -100)); // Surface bounds are a separate gate.
}

void independentCoverageReference()
{
    std::mt19937 random(0xC20F2026U);
    std::uniform_real_distribution<double> unit(0, 1), signedUnit(-1, 1);
    for (int i = 0; i < 12'000; ++i) {
        const auto rotate = rotation(signedUnit(random) * 180);
        const double sx = .08 + unit(random) * 8, sy = .08 + unit(random) * 8;
        const auto mapping = composeAffine(rotate,
            AffineTransform {.m00 = i % 2 ? sx : -sx,
                .m01 = signedUnit(random) * sy * .8, .m11 = sy});
        const Vec2d dx {mapping.m00, mapping.m10}, dy {mapping.m01, mapping.m11};
        const Vec2d center {signedUnit(random) * 200, signedUnit(random) * 200};
        const RectD crop {center.x + signedUnit(random) * 5, center.y + signedUnit(random) * 5,
            unit(random) * 9, unit(random) * 9};
        const double expected = referenceCoverage(crop, center, dx, dy);
        const float actual = layerCropCoverage(crop, center, dx, dy);
        CHECK(actual >= 0 && actual <= 1);
        near(actual, expected, 3e-5, "Independent oriented-polygon clipping");
        near(layerCropCoverage(crop, center, dx * -1, dy), expected, 3e-5,
            "Signed X scale preserves footprint coverage");
        near(layerCropCoverage(crop, center, dy, dx), expected, 3e-5,
            "Swapping footprint axes preserves coverage");
        const PremultipliedColor source {.1F, .2F, .3F, .4F};
        const Vec2d outputPoint {7.5, 5.5};
        auto inverse = mapping;
        inverse.m02 = center.x - inverse.m00 * outputPoint.x - inverse.m01 * outputPoint.y;
        inverse.m12 = center.y - inverse.m10 * outputPoint.x - inverse.m11 * outputPoint.y;
        const auto result = applyLayerCrop(source, crop, inverse, outputPoint);
        for (std::size_t c = 0; c < 4; ++c)
            near(result[c], double(source[c]) * expected, 2e-5, "Output-to-local footprint mapping");
    }
}

void metadataOnlyAndTypedCacheStability()
{
    Fixture f({127, 93});
    const auto original = bytes(*f.surface);
    const auto sourceRevision = f.surface->revision(), sourceId = f.surface->id();
    CHECK(f.document.setLayerTransform(f.id, sourceMappings()[3]));
    auto adjustments = std::make_shared<AdjustmentStack>();
    adjustments->items[0].enabled = true;
    std::get<ExposureParameters>(adjustments->items[0].parameters).stops = 1;
    CHECK(f.document.setLayerAdjustments(f.id, adjustments));
    const auto adjustmentRevision = f.document.layer(f.id)->adjustmentRevision;
    const auto transform = f.document.layer(f.id)->localToDocument;
    const auto selection = SelectionMask::rectangle({64, 64}, {4, 5, 12, 13});
    CHECK(f.document.setSelection(selection));
    const auto selectionRevision = f.document.selectionRevision();
    const auto before = f.document.snapshot();
    for (int i = 0; i < 60; ++i) {
        const RectD crop {2.125 + i * .25, 4.5, 19.25, 11.75};
        CHECK(f.document.setLayerCrop(f.id, crop));
        const auto revision = f.document.revision();
        CHECK(!f.document.setLayerCrop(f.id, crop));
        CHECK(f.document.revision() == revision);
        CHECK(f.document.snapshot().layersBottomToTop.front().crop == crop);
        CHECK(!before.layersBottomToTop.front().crop);
        CHECK(f.document.layer(f.id)->localToDocument == transform);
        CHECK(f.document.layer(f.id)->adjustments == adjustments);
        CHECK(f.document.layer(f.id)->adjustmentRevision == adjustmentRevision);
        CHECK(f.surface->revision() == sourceRevision && f.surface->id() == sourceId);
        CHECK(f.surface->dirtySince(sourceRevision).empty());
        CHECK(f.document.setLayerCrop(f.id, {}));
    }
    CHECK(bytes(*f.surface) == original);
    CHECK(f.document.selection() == selection && f.document.selectionRevision() == selectionRevision);
    const auto revision = f.document.revision();
    CHECK(!f.document.setLayerCrop(f.id, RectD {0, 0, -1, 2}));
    CHECK(!f.document.setLayerCrop(f.id, RectD {0, 0, 2, std::numeric_limits<double>::infinity()}));
    CHECK(!f.document.setLayerCrop(f.id, RectD {std::numeric_limits<double>::quiet_NaN(), 0, 2, 2}));
    CHECK(!f.document.setLayerCrop(0, RectD {0, 0, 2, 2}));
    CHECK(f.document.revision() == revision);

    for (const bool text : {false, true}) {
        TextLayer textData;
        textData.utf8 = "Editable O";
        auto layer = text ? Layer::text("Text crop", textData) : Layer::shape("Shape crop", ShapeLayer {});
        const auto id = layer.id;
        auto cache = std::make_shared<LayerRenderCache>();
        cache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u {48, 36}, Rgba8 {200, 90, 70, 255});
        cache->pixelsToLocal = {.m00 = .5, .m02 = -2, .m11 = .5, .m12 = -3};
        cache->logicalExtent = {20, 12}; cache->density = 2;
        layer.renderCache = cache;
        const auto payload = layer.payload;
        const auto textRevision = layer.textRevision, shapeRevision = layer.shapeRevision;
        CHECK(f.document.insertLayer(f.document.layers().size(), std::move(layer)));
        CHECK(layerSourceBounds(*f.document.layer(id)) == RectD {-2, -3, 24, 18});
        for (int i = 0; i < 12; ++i) {
            CHECK(f.document.setLayerCrop(id, RectD {-1.25, -.5, 8.5, 6.25}));
            CHECK(f.document.layer(id)->renderCache == cache);
            CHECK(f.document.layer(id)->textRevision == textRevision);
            CHECK(f.document.layer(id)->shapeRevision == shapeRevision);
            CHECK(layerVisibleBounds(*f.document.layer(id)) == RectD {-1.25, -.5, 8.5, 6.25});
            CHECK(f.document.setLayerCrop(id, {}));
        }
        if (text) CHECK(std::get<TextLayer>(f.document.layer(id)->payload) == std::get<TextLayer>(payload));
        else CHECK(std::get<ShapeLayer>(f.document.layer(id)->payload) == std::get<ShapeLayer>(payload));
        CHECK(cache->surface->revision() == 1);
    }
}

void appearanceAndRawSourceSampling()
{
    const Rgba8 color {80, 130, 220, 200};
    Fixture f({16, 12}, color);
    const RectD crop {2.25, 1.75, 8.5, 6.5};
    CHECK(f.document.setLayerCrop(f.id, crop));
    auto settings = std::make_shared<AdjustmentStack>();
    settings->items[0].enabled = true;
    std::get<ExposureParameters>(settings->items[0].parameters).stops = 1;
    CHECK(f.document.setLayerAdjustments(f.id, settings));
    CHECK(f.document.setLayerOpacity(f.id, .5F));
    const auto original = bytes(*f.surface);
    auto decode = [](std::uint8_t c) {
        const double v = double(c) / 255;
        return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
    };
    for (const auto transform : sourceMappings()) {
        CHECK(f.document.setLayerTransform(f.id, transform));
        const auto inverse = *transform.inverted();
        const PreparedRasterSampler raw(*f.document.layer(f.id));
        const PreparedLayerSampler appearance(*f.document.layer(f.id));
        const PinnedDocumentSampler pinned(f.document, f.id, ColorSampleSource::MergedVisible);
        const std::array<const Layer*, 1> layers {f.document.layer(f.id)};
        const PinnedDocumentSampler prepared(layers, {64, 64});
        CHECK(raw.sample(transform.map({.5, .5})) == color); // Explicit source access is uncropped.
        CHECK(appearance.sample(transform.map({.5, .5})) == PremultipliedColor {});
        CHECK(!hitTestRasterLayer(f.document, transform.map({.5, .5})));
        CHECK(hitTestRasterLayer(f.document, transform.map({5, 4})) == f.id);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 48; ++x) {
            const Vec2d point {x + .5, y + .5};
            const auto local = inverse.map(point);
            const bool inSource = local.x >= 0 && local.y >= 0 && local.x < 16 && local.y < 12;
            const double coverage = inSource ? referenceCoverage(crop, local,
                {inverse.m00, inverse.m10}, {inverse.m01, inverse.m11}) : 0;
            const auto value = appearance.sample(point);
            const double alpha = double(color.alpha) / 255;
            near(value[3], alpha * coverage, 2e-6, "Transformed crop appearance alpha");
            const std::array channels {color.red, color.green, color.blue};
            for (std::size_t c = 0; c < 3; ++c)
                near(value[c], std::min(1.0, decode(channels[c]) * 2) * alpha * coverage,
                    2e-6, "Adjustments precede crop without straight-RGB contamination");
            const auto merged = sampleDocumentColor(f.document, {}, point, ColorSampleSource::MergedVisible);
            CHECK(merged.available());
            CHECK(merged.color == pinned.sample(point));
            CHECK(merged.color == prepared.sample(point));
            near(merged.color.alpha, std::round(255 * alpha * coverage * .5), 1,
                "Layer opacity applied exactly once after crop");
        }
        CHECK(f.document.setLayerCrop(f.id, RectD {2, 2, 0, 1}));
        CHECK(!pinned.matches(f.document));
        CHECK(sampleDocumentColor(f.document, {}, transform.map({5, 4}),
            ColorSampleSource::MergedVisible).color == Rgba8 {});
        CHECK(f.document.setLayerCrop(f.id, crop));
    }
    CHECK(bytes(*f.surface) == original && f.surface->revision() == 1);

    // Typed caches include a negative local origin and independent pixel
    // density. Crop follows local geometry, never cache pixel coordinates.
    auto text = Layer::text("Typed cache", TextLayer {});
    auto cache = std::make_shared<LayerRenderCache>();
    cache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u {48, 36}, color);
    cache->pixelsToLocal = {.m00 = .5, .m02 = -2, .m11 = .5, .m12 = -3};
    cache->logicalExtent = {20, 12}; text.renderCache = cache;
    text.localToDocument = sourceMappings()[3]; text.crop = crop;
    const auto inverse = *text.localToDocument.inverted();
    const PreparedLayerSampler typed(text);
    for (const Vec2d local : {Vec2d {2.5, 4}, Vec2d {5, 4}, Vec2d {11, 4}, Vec2d {5, 1.5}}) {
        const auto point = text.localToDocument.map(local);
        const auto expected = referenceCoverage(crop, local,
            {inverse.m00, inverse.m10}, {inverse.m01, inverse.m11});
        near(typed.sample(point)[3], double(color.alpha) / 255 * expected, 2e-6,
            "Typed cache offset/density does not rebase crop");
    }
}

void rasterEditingUsesBinaryCropEligibility()
{
    const Rgba8 initial {20, 60, 180, 0}, candidate {210, 70, 30, 255};
    const RectD crop {2.75, 1.25, 3.5, 3.25};
    for (const auto transform : sourceMappings()) for (const bool partialSelection : {false, true}) {
        Fixture f({10, 8}, initial);
        CHECK(f.document.setLayerTransform(f.id, transform));
        CHECK(f.document.setLayerCrop(f.id, crop));
        if (partialSelection) {
            std::vector<std::uint8_t> plane(64 * 64);
            for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
                plane[std::size_t(y * 64 + x)] = x % 3 == 0 ? 0 : x % 3 == 1 ? 64 : 255;
            CHECK(f.document.setSelection(SelectionMask::fromR8({64, 64}, plane, 64)));
        }
        const auto before = bytes(*f.surface);
        RasterEditTransaction transaction(f.document, f.id, "Cropped edit");
        CHECK(transaction.active());
        const auto dirty = writeSolid(transaction, {0, 0, 10, 8}, candidate);
        CHECK(!dirty.empty() && !dirty.fullRefresh);
        const auto afterFirst = bytes(*f.surface);
        const auto firstRevision = f.surface->revision();
        CHECK(writeSolid(transaction, {0, 0, 10, 8}, candidate).empty());
        CHECK(bytes(*f.surface) == afterFirst && f.surface->revision() == firstRevision);
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 10; ++x) {
            const bool eligible = x + 1.0 > crop.x && y + 1.0 > crop.y
                && x < crop.right() && y < crop.bottom();
            const auto p = transform.map({x + .5, y + .5});
            const auto coverage = !eligible ? 0 : partialSelection
                ? f.document.selection()->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y))) : 255;
            const auto expected = coverage ? Rgba8 {candidate.red, candidate.green, candidate.blue,
                std::uint8_t(coverage)} : initial;
            CHECK(pixel(*f.surface, x, y) == expected);
        }
        for (const auto r : dirty.regions) for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x) CHECK(cropAllowsTexel(crop, x, y));
        CHECK(transaction.commit(f.history) == RasterEditCommitResult::Committed);
        CHECK(f.history.undoDepth() == 1 && f.history.memoryUsed() > 0);
        CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == before);
        CHECK(f.history.redo(f.document)); CHECK(bytes(*f.surface) == afterFirst);
        CHECK(f.document.layer(f.id)->crop == crop);
        CHECK(f.document.layer(f.id)->localToDocument == transform);
        RasterEditTransaction cancelled(f.document, f.id, "Cancelled crop edit");
        CHECK(!writeSolid(cancelled, {0, 0, 10, 8}, {10, 200, 30, 255}).empty());
        cancelled.cancel();
        CHECK(bytes(*f.surface) == afterFirst && f.history.undoDepth() == 1);
    }
}

void noOpAndPinnedCropSafety()
{
    Fixture f({12, 9});
    CHECK(f.history.execute(f.document, std::make_unique<SetLayerOpacityCommand>(f.id, .25F)));
    CHECK(f.history.undo(f.document));
    const auto before = bytes(*f.surface);
    const auto revision = f.surface->revision();
    CHECK(f.document.setLayerCrop(f.id, RectD {4.25, 4.25, 0, 2}));
    RasterEditTransaction empty(f.document, f.id, "Empty crop");
    CHECK(writeSolid(empty, {0, 0, 12, 9}, {255, 0, 0, 255}).empty());
    CHECK(empty.commit(f.history) == RasterEditCommitResult::NoChanges);
    CHECK(bytes(*f.surface) == before && f.surface->revision() == revision);
    CHECK(f.history.canRedo() && !f.history.canUndo());
    CHECK(f.document.setLayerCrop(f.id, RectD {4, 3, 3, 2}));
    RasterEditTransaction hidden(f.document, f.id, "Fully hidden patch");
    CHECK(writeSolid(hidden, {0, 0, 2, 2}, {255, 0, 0, 255}).empty());
    CHECK(hidden.commit(f.history) == RasterEditCommitResult::NoChanges);
    CHECK(f.surface->revision() == revision && f.history.canRedo());
    RasterEditTransaction changed(f.document, f.id, "Pinned crop");
    CHECK(!writeSolid(changed, {0, 0, 12, 9}, {255, 0, 0, 255}).empty());
    CHECK(f.document.setLayerCrop(f.id, RectD {1, 1, 2, 2}));
    CHECK(!changed.targetAvailable());
    CHECK(changed.commit(f.history) == RasterEditCommitResult::TargetUnavailable);
    CHECK(bytes(*f.surface) == before && f.history.canRedo());
    CHECK(f.document.layer(f.id)->crop == RectD {1, 1, 2, 2});
}

FillState finish(FillOperation& operation)
{
    for (int n = 0; n < 10'000; ++n) {
        const auto result = operation.step(23);
        if (result != FillState::Applying && result != FillState::Discovering) return result;
    }
    CHECK(false && "Bounded crop fill failed to terminate");
    return operation.state();
}
void fillBrushAndEraseKeepHiddenSourcePixels()
{
    const RectD crop {6.75, 4.25, 8.5, 9.5};
    for (const auto mode : {FillMode::SelectionOrLayer, FillMode::Contiguous}) {
        Fixture f({24, 20}, {});
        CHECK(f.document.setLayerCrop(f.id, crop));
        const auto before = bytes(*f.surface);
        FillOperation fill(f.document, f.id, {.mode = mode, .color = {220, 50, 30, 255},
            .opacity = 1, .tolerance = 0, .seed = {10.5, 10.5}});
        CHECK(finish(fill) == FillState::Ready);
        CHECK(fill.commit(f.history) == RasterEditCommitResult::Committed);
        for (int y = 0; y < 20; ++y) for (int x = 0; x < 24; ++x)
            CHECK(pixel(*f.surface, x, y).alpha == (cropAllowsTexel(crop, x, y) ? 255 : 0));
        CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == before);
        FillOperation outside(f.document, f.id, {.mode = FillMode::Contiguous,
            .color = {255, 0, 0, 255}, .seed = {1.5, 1.5}});
        const auto state = finish(outside);
        CHECK(state == FillState::Ready || state == FillState::Failed);
        if (state == FillState::Ready) CHECK(outside.commit(f.history) == RasterEditCommitResult::NoChanges);
        CHECK(bytes(*f.surface) == before && f.history.canRedo());
    }
    for (const auto mode : {BrushCompositeMode::Paint, BrushCompositeMode::Erase}) {
        const Rgba8 initial = mode == BrushCompositeMode::Paint ? Rgba8 {} : Rgba8 {70, 110, 230, 255};
        Fixture f({24, 20}, initial);
        CHECK(f.document.setLayerCrop(f.id, crop));
        auto brush = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
        brush.sizePixels = 80; brush.hardness = 1; brush.opacity = 1; brush.flow = 1;
        brush.foreground = {210, 50, 30, 255}; brush.pressureToSize = false; brush.pressureToFlow = false;
        brush.smoothing = BrushSmoothingMode::None;
        const NormalizedPointerSample sample {.documentPosition = {12, 10}, .pressure = 1,
            .pointerType = PointerType::Mouse, .buttons = PointerButtonPrimary};
        const auto before = bytes(*f.surface);
        BasicPixelBrushStroke stroke(f.document, f.id, brush, mode);
        CHECK(stroke.begin(sample));
        CHECK(stroke.end(sample, f.history) == RasterEditCommitResult::Committed);
        for (int y = 0; y < 20; ++y) for (int x = 0; x < 24; ++x) {
            const auto result = pixel(*f.surface, x, y);
            if (!cropAllowsTexel(crop, x, y)) CHECK(result == initial);
            else CHECK(result.alpha == (mode == BrushCompositeMode::Paint ? 255 : 0));
        }
        const auto after = bytes(*f.surface);
        CHECK(f.history.undo(f.document)); CHECK(bytes(*f.surface) == before);
        CHECK(f.history.redo(f.document)); CHECK(bytes(*f.surface) == after);
    }
}

void duplicateAndSelectedExtraction()
{
    const Rgba8 sourceColor {90, 140, 230, 200};
    const RectD crop {2.25, 1.75, 8.5, 6.5};
    {
        Fixture f({16, 12}, sourceColor);
        CHECK(f.document.setLayerCrop(f.id, crop));
        CHECK(f.document.setLayerTransform(f.id, sourceMappings()[3]));
        const auto before = bytes(*f.surface);
        CHECK(f.history.execute(f.document, std::make_unique<LayerViaCopyCommand>(f.id, f.id)));
        const auto created = *f.history.activeLayerHint();
        CHECK(created != f.id && f.document.layer(created));
        CHECK(f.document.layer(created)->crop == crop);
        CHECK(f.document.layer(created)->localToDocument == f.document.layer(f.id)->localToDocument);
        CHECK(bytes(*renderedSurface(*f.document.layer(created))) == before);
        CHECK(renderedSurface(*f.document.layer(created))->id() != f.surface->id());
        CHECK(f.history.undo(f.document)); CHECK(!f.document.layer(created));
        CHECK(f.history.activeLayerHint() == f.id);
        CHECK(f.history.redo(f.document)); CHECK(f.document.layer(created)->crop == crop);
        CHECK(bytes(*f.surface) == before);
    }
    for (const auto transform : sourceMappings()) {
        Fixture f({16, 12}, sourceColor);
        CHECK(f.document.setLayerCrop(f.id, crop));
        CHECK(f.document.setLayerTransform(f.id, transform));
        CHECK(f.document.setLayerOpacity(f.id, .4F));
        auto adjustments = std::make_shared<AdjustmentStack>();
        adjustments->items[0].enabled = true;
        std::get<ExposureParameters>(adjustments->items[0].parameters).stops = .5;
        CHECK(f.document.setLayerAdjustments(f.id, adjustments));
        std::vector<std::uint8_t> mask(64 * 64, 255);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            if (x % 3 == 0) mask[std::size_t(y * 64 + x)] = 128;
        const auto selection = SelectionMask::fromR8({64, 64}, mask, 64);
        CHECK(f.document.setSelection(selection));
        const auto original = bytes(*f.surface);
        CHECK(f.history.execute(f.document, std::make_unique<LayerViaCopyCommand>(f.id, f.id)));
        const auto created = *f.history.activeLayerHint();
        const auto* result = f.document.layer(created);
        CHECK(result && created != f.id);
        if (!result || created == f.id) continue;
        // The existing extraction path is document-aligned raster output.
        // Rotated local crops cannot be retained as axis-aligned crop metadata
        // in that new coordinate system; their coverage must be baked once.
        CHECK(!result->crop);
        CHECK(result->opacity == .4F && result->blendMode == f.document.layer(f.id)->blendMode);
        CHECK(result->adjustments && equivalentAdjustments(result->adjustments, adjustments));
        CHECK(f.document.selection() == selection);
        const auto output = renderedSurface(*result);
        const auto inverse = *transform.inverted();
        for (int y = 0; y < int(output->extent().height); ++y)
            for (int x = 0; x < int(output->extent().width); ++x) {
                const auto p = result->localToDocument.map({x + .5, y + .5});
                const auto local = inverse.map(p);
                const bool validSource = local.x >= 0 && local.y >= 0 && local.x < 16 && local.y < 12;
                const double coverage = validSource ? referenceCoverage(crop, local,
                    {inverse.m00, inverse.m10}, {inverse.m01, inverse.m11}) : 0;
                const double selected = selection->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y))) / 255.0;
                const auto actual = pixel(*output, x, y);
                near(actual.alpha, std::round(sourceColor.alpha * coverage * selected), 1,
                    "Ctrl+J applies crop and selection once, never layer opacity");
                if (actual.alpha) CHECK(actual.red == sourceColor.red && actual.green == sourceColor.green && actual.blue == sourceColor.blue);
            }
        const auto extracted = bytes(*output);
        CHECK(f.history.undo(f.document)); CHECK(!f.document.layer(created));
        CHECK(bytes(*f.surface) == original);
        CHECK(f.history.redo(f.document));
        CHECK(bytes(*renderedSurface(*f.document.layer(created))) == extracted);
        CHECK(f.document.layer(f.id)->crop == crop && f.document.layer(f.id)->localToDocument == transform);
    }
    Fixture empty;
    CHECK(empty.document.setLayerCrop(empty.id, RectD {2, 2, 0, 4}));
    CHECK(empty.document.setSelection(SelectionMask::filled({64, 64}, 255)));
    CHECK(empty.history.execute(empty.document, std::make_unique<SetLayerOpacityCommand>(empty.id, .5F)));
    CHECK(empty.history.undo(empty.document));
    CHECK(!empty.history.execute(empty.document, std::make_unique<LayerViaCopyCommand>(empty.id, empty.id)));
    CHECK(empty.document.layers().size() == 1 && empty.history.canRedo());
}
void tinyCropBoundsHighResolutionBrushWork()
{
    Fixture f({4096,4096},{});
    CHECK(f.document.setLayerTransform(f.id,{.1,0,0,0,.1,0}));
    CHECK(f.document.setLayerCrop(f.id,RectD{25,25,10,10}));
    auto brush=proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    brush.sizePixels=1000;brush.hardness=1;brush.opacity=1;brush.flow=1;
    brush.pressureToSize=false;brush.pressureToFlow=false;brush.foreground={220,80,20,255};
    const NormalizedPointerSample sample{.documentPosition={3,3},.pressure=1,
        .pointerType=PointerType::Mouse,.buttons=PointerButtonPrimary};
    BasicPixelBrushStroke stroke(f.document,f.id,brush);
    CHECK(stroke.begin(sample));
    CHECK(stroke.end(sample,f.history)==RasterEditCommitResult::Committed);
    CHECK(stroke.stats().maximumCandidatePixels==100);
    CHECK(stroke.stats().rejectedCandidatePixels==0);
    CHECK(stroke.stats().retainedStrokeTiles==1);
    CHECK(pixel(*f.surface,24,25).alpha==0&&pixel(*f.surface,25,25).alpha==255);
    CHECK(f.history.undo(f.document));CHECK(pixel(*f.surface,25,25).alpha==0);
}
} // namespace

int main()
{
    exactCoverageFixtures();
    independentCoverageReference();
    metadataOnlyAndTypedCacheStability();
    appearanceAndRawSourceSampling();
    rasterEditingUsesBinaryCropEligibility();
    noOpAndPinnedCropSafety();
    fillBrushAndEraseKeepHiddenSourcePixels();
    duplicateAndSelectedExtraction();
    tinyCropBoundsHighResolutionBrushWork();
    if (failures) std::cerr << failures << " layer-crop checks failed\n";
    else std::cout << "Layer crop core checks passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
