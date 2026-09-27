#include "imageeditor/ui/QtShapeRenderService.hpp"

#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <QGuiApplication>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QTransform>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
void near(double actual, double expected, double tolerance, std::string_view message)
{
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
std::vector<std::byte> pixels(const c::RasterSurface& surface)
{
    const auto extent = surface.extent();
    const auto stride = static_cast<std::size_t>(extent.width) * 4;
    std::vector<std::byte> result(stride * extent.height);
    surface.copyRgba8({ 0, 0, static_cast<std::int32_t>(extent.width),
        static_cast<std::int32_t>(extent.height) }, result, stride);
    return result;
}
c::Rgba8 pixel(const c::LayerRenderCache& cache, int x, int y)
{
    const auto extent = cache.surface->extent();
    if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= extent.width
        || static_cast<std::uint32_t>(y) >= extent.height)
        return {};
    std::array<std::byte, 4> data {};
    cache.surface->copyRgba8({ x, y, 1, 1 }, data, 4);
    return { std::to_integer<std::uint8_t>(data[0]), std::to_integer<std::uint8_t>(data[1]),
        std::to_integer<std::uint8_t>(data[2]), std::to_integer<std::uint8_t>(data[3]) };
}
c::Rgba8 at(const c::LayerRenderCache& cache, c::Vec2d local)
{
    const auto p = cache.pixelsToLocal.inverted()->map(local);
    return pixel(cache, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)));
}
bool empty(const c::LayerRenderCache& cache)
{
    const auto bytes = pixels(*cache.surface);
    for (std::size_t i = 3; i < bytes.size(); i += 4)
        if (bytes[i] != std::byte(0))
            return false;
    return true;
}
bool partial(const c::LayerRenderCache& cache)
{
    const auto bytes = pixels(*cache.surface);
    for (std::size_t i = 3; i < bytes.size(); i += 4) {
        const auto alpha = std::to_integer<int>(bytes[i]);
        if (alpha > 0 && alpha < 255)
            return true;
    }
    return false;
}
c::ShapeLayer rectangle()
{
    c::ShapeLayer shape;
    shape.size = { 20, 16 };
    shape.fillColor = { 170, 21, 93, 128 };
    shape.strokeColor = { 20, 220, 43, 255 };
    return shape;
}

void fillStrokeAndAlpha()
{
    u::QtShapeRenderService service;
    auto shape = rectangle();
    auto cache = service.render({ shape, 1 });
    check(at(*cache, { 10.5, 8.5 }) == shape.fillColor, "fill color remains exact straight RGBA8");
    check(at(*cache, { -1, 8 }).alpha == 0, "fill does not escape geometry");
    check(cache->logicalExtent == c::Extent2u { 20, 16 }, "stroke/AA excluded from transform frame");

    shape.fillEnabled = false;
    shape.strokeEnabled = true;
    shape.strokeWidth = 4;
    cache = service.render({ shape, 1 });
    check(at(*cache, { 10.5, 8.5 }).alpha == 0, "hollow shape has transparent interior");
    check(at(*cache, { -.5, 8.5 }) == shape.strokeColor, "centered stroke extends outside frame");
    check(at(*cache, { 2.5, 8.5 }).alpha == 0, "centered stroke not inside-only");
    check(cache->pixelsToLocal.m02 <= -4 && cache->pixelsToLocal.m12 <= -4,
        "render bounds contain half stroke plus AA extent");
    shape.strokeWidth = 0;
    check(empty(*service.render({ shape, 3 })), "zero width is no stroke, never cosmetic hairline");
    shape.strokeWidth = 20;
    shape.strokeEnabled = false;
    check(empty(*service.render({ shape, 3 })), "disabled fill and stroke render nothing");
    shape.strokeEnabled = true;
    shape.strokeColor.alpha = 0;
    check(empty(*service.render({ shape, 3 })), "transparent stroke produces transparent cache");

    shape = rectangle();
    shape.fillColor = { 255, 0, 0, 128 };
    shape.strokeColor = { 0, 0, 255, 128 };
    shape.strokeEnabled = true;
    shape.strokeWidth = 8;
    cache = service.render({ shape, 1 });
    const auto overlap = at(*cache, { .5, 8.5 });
    const double topAlpha = 128.0 / 255;
    const double remainingAlpha = topAlpha * (1 - topAlpha);
    const double alpha = topAlpha + remainingAlpha;
    near(overlap.red, c::linearToSrgb(remainingAlpha / alpha), 1,
        "overlapping fill and stroke composite red in linear light");
    near(overlap.blue, c::linearToSrgb(topAlpha / alpha), 1,
        "overlapping fill and stroke composite blue in linear light");
    near(overlap.alpha, c::alphaToByte(alpha), 1, "fill and stroke alpha each applied exactly once");
}

void kindsAndDegeneracy()
{
    u::QtShapeRenderService service;
    for (const auto kind : { c::ShapeKind::Rectangle, c::ShapeKind::RoundedRectangle,
             c::ShapeKind::Ellipse, c::ShapeKind::Triangle }) {
        auto shape = rectangle();
        shape.kind = kind;
        shape.fillColor.alpha = 255;
        shape.strokeEnabled = true;
        shape.strokeWidth = .3;
        const auto original = shape;
        const auto cache = service.render({ shape, 1.5 });
        check(!empty(*cache), "all bounded primitive kinds render");
        check(partial(*cache), "fractional density and thin strokes retain AA coverage");
        check(shape == original, "rasterizing never changes canonical geometry/style");
        shape.size.width = 0;
        check(empty(*service.render({ shape, 1 })), "zero-width closed shapes are safely degenerate");
    }
    auto shape = rectangle();
    shape.kind = c::ShapeKind::Line;
    shape.size = { 20, 0 };
    shape.points = { { 0, 0 }, { 20, 0 } };
    check(empty(*service.render({ shape, 1 })), "line ignores fill, requires enabled positive stroke");
    shape.strokeEnabled = true;
    shape.strokeWidth = 6;
    auto cache = service.render({ shape, 1 });
    check(cache->logicalExtent == c::Extent2u { 20, 1 }, "horizontal line preserves usable transform frame");
    check(at(*cache, { -1.5, .5 }).alpha > 240, "round caps extend beyond line endpoint");
    check(at(*cache, { -2.5, 2.5 }).alpha < 40, "round cap is not rectangular square cap");
    shape.points[1] = shape.points[0];
    check(empty(*service.render({ shape, 1 })), "zero-length line is safely transparent");

    shape = rectangle();
    shape.kind = c::ShapeKind::Polygon;
    shape.size = { 100, 100 };
    shape.points = { { 0, 100 }, { 50, 0 }, { 51, 100 } };
    shape.strokeEnabled = true;
    shape.strokeWidth = 20;
    cache = service.render({ shape, 1 });
    check(cache->pixelsToLocal.m12 > -13, "acute polygon round joins never grow long miter spikes");
    check(!empty(*cache), "acute polygon is rasterized");
    shape.points = { { 0, 0 }, { 100, 100 }, { 0, 100 }, { 100, 0 } };
    cache = service.render({ shape, 1 });
    check(at(*cache, { 50.5, 15.5 }).alpha > 0 && at(*cache, { 50.5, 85.5 }).alpha > 0,
        "self-crossing polygon renders both even-odd lobes despite cancelling signed area");
    auto reversed = shape;
    std::reverse(reversed.points.begin(), reversed.points.end());
    check(pixels(*cache->surface) == pixels(*service.render({ reversed, 1 })->surface),
        "polygon reverse winding has identical even-odd coverage");
    shape.points = { { 0, 0 }, { 50, 50 }, { 100, 100 } };
    check(empty(*service.render({ shape, 1 })), "collinear polygon is safely transparent");

    shape = rectangle();
    shape.kind = c::ShapeKind::RoundedRectangle;
    shape.cornerRadius = 1000;
    const auto largeRadius = service.render({ shape, 2 });
    shape.cornerRadius = 8;
    check(pixels(*largeRadius->surface) == pixels(*service.render({ shape, 2 })->surface),
        "rounded radius clamps only for rendering to half smaller axis");
}

void ellipsesAndResolution()
{
    u::QtShapeRenderService service;
    for (const c::Extent2d extent : { c::Extent2d { .5, .5 }, { 1.5, 1.5 }, { 3, 3 },
             { 15, 15 }, { .5, 16 }, { 32, .75 } }) {
        auto shape = rectangle();
        shape.kind = c::ShapeKind::Ellipse;
        shape.size = extent;
        shape.fillColor.alpha = 255;
        const auto cache = service.render({ shape, 2 });
        check(!empty(*cache), "small and eccentric ellipse retains coverage");
        check(partial(*cache), "ellipse has subpixel AA edges");
        if (extent.width == extent.height && extent.width >= 3) {
            const auto e = cache->surface->extent();
            for (std::uint32_t y = 0; y < e.height; ++y)
                for (std::uint32_t x = 0; x < e.width; ++x)
                    near(pixel(*cache, static_cast<int>(x), static_cast<int>(y)).alpha,
                        pixel(*cache, static_cast<int>(e.width - x - 1), static_cast<int>(y)).alpha,
                        5, "circle AA stays horizontally symmetric");
        }
    }

    auto shape = rectangle();
    shape.kind = c::ShapeKind::Ellipse;
    shape.strokeEnabled = true;
    shape.strokeWidth = 3;
    const auto original = shape;
    const auto first = service.render({ shape, 1 });
    for (const double density : { .125, .5, 1., 2., 8. }) {
        const auto cache = service.render({ shape, density });
        check(cache->logicalExtent == first->logicalExtent && shape == original,
            "zoom and monitor density do not change canonical transform frame");
        near(cache->pixelsToLocal.m00 * cache->density, 1, 1e-12,
            "cache pixel transform compensates resolution without geometry mutation");
        near(cache->requestedDensity, density, 0, "requested density retained for cache reuse decisions");
        const auto e = cache->surface->extent();
        for (std::uint32_t x = 0; x < e.width; ++x) {
            check(pixel(*cache, static_cast<int>(x), 0).alpha == 0,
                "top AA padding never clips visible stroke");
            check(pixel(*cache, static_cast<int>(x), static_cast<int>(e.height - 1)).alpha == 0,
                "bottom AA padding never clips visible stroke");
        }
    }
    check(u::QtShapeRenderService::densityForScale(1.01) == 2
            && u::QtShapeRenderService::densityForScale(1.99) == 2
            && u::QtShapeRenderService::densityForScale(.7) == 1,
        "stable density tiers avoid rebuild on each fractional zoom");
    for (int degrees = -720; degrees <= 720; ++degrees) {
        const auto radians = double(degrees) * std::numbers::pi / 180;
        for (const double tier : { .125, .5, 1., 2., 4., 8. }) {
            const auto rotatedScale = std::hypot(tier * std::cos(radians), tier * std::sin(radians));
            check(u::QtShapeRenderService::densityForScale(rotatedScale) == tier,
                "pure rotation floating-point noise never changes the raster density tier");
        }
    }
    check(pixels(*first->surface) == pixels(*service.render({ shape, 1 })->surface),
        "regenerating geometry at same resolution is byte-deterministic");
    shape.size = { 1000000, 1000000 };
    const auto bounded = service.render({ shape, 8 });
    const auto e = bounded->surface->extent();
    check(e.width <= 8192 && e.height <= 8192
            && static_cast<std::uint64_t>(e.width) * e.height <= 16 * 1024 * 1024,
        "giant shape respects exact 16M pixel and 8192 edge budget");
    check(shape.size.width == 1000000 && bounded->requestedDensity == 8,
        "cache limiting does not alter document geometry or requested resolution");

    shape = rectangle();
    for (const double density : { 1e-300, 1e300, std::numeric_limits<double>::quiet_NaN() }) {
        const auto finiteCache = service.render({ shape, density });
        check(finiteCache->pixelsToLocal.inverted().has_value(),
            "extreme/invalid requested density never produces nonfinite cache coordinates");
    }
    shape.strokeWidth = std::numeric_limits<double>::quiet_NaN();
    bool rejected = false;
    try {
        (void)service.render({ shape, 1 });
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "invalid authoritative style rejected before Qt path/raster allocation");
}

void geometricHits()
{
    u::QtShapeRenderService service;
    auto shape = rectangle();
    shape.kind = c::ShapeKind::Ellipse;
    shape.size = { 100, 100 };
    shape.fillEnabled = false;
    shape.strokeEnabled = true;
    shape.strokeWidth = 2;
    check(service.hit(shape, {}, { 50, 50 }, 0), "hollow ellipse interior remains draggable");
    check(!service.hit(shape, {}, { 2, 2 }, 0), "ellipse targeting rejects bounding-box corners");
    shape.kind = c::ShapeKind::Polygon;
    shape.points = { { 0, 0 }, { 100, 0 }, { 100, 30 }, { 30, 30 }, { 30, 100 }, { 0, 100 } };
    check(service.hit(shape, {}, { 15, 85 }, 0), "concave hollow polygon interior targets");
    check(!service.hit(shape, {}, { 80, 80 }, 0), "concave polygon missing corner does not target");

    shape.kind = c::ShapeKind::Line;
    shape.size = { 100, 0 };
    shape.points = { { 0, 0 }, { 100, 0 } };
    shape.strokeWidth = 2;
    for (const double zoom : { .1, 1., 8. }) {
        const c::AffineTransform t { zoom, 0, 200, 0, zoom, 300 };
        const double visibleHalfStroke = zoom;
        check(service.hit(shape, t, { 200 + 50 * zoom, 300 + visibleHalfStroke + 4 }, 5),
            "line hit tolerance stays five screen pixels outside visible stroke at all zooms");
        check(!service.hit(shape, t, { 200 + 50 * zoom, 300 + visibleHalfStroke + 7 }, 5),
            "line tolerance remains bounded at all zooms");
    }
    const c::AffineTransform rotatedFlip { 0, -3, 400, -2, 0, 500 };
    check(service.hit(shape, rotatedFlip, { 406, 400 }, 5),
        "rotated anisotropic flipped line has geometric stroke plus screen tolerance");
    check(!service.hit(shape, rotatedFlip, { 414, 400 }, 5),
        "transformed line tolerance is not inflated by layer scale");
    c::AffineTransform invalid;
    invalid.m00 = std::numeric_limits<double>::quiet_NaN();
    check(!service.hit(shape, invalid, { 50, 50 }), "nonfinite transform safely rejects hits");
}

QPainterPath referencePath(const c::ShapeLayer& shape)
{
    QPainterPath path;
    const QRectF rectangle(0, 0, shape.size.width, shape.size.height);
    switch (shape.kind) {
    case c::ShapeKind::Rectangle: path.addRect(rectangle); break;
    case c::ShapeKind::RoundedRectangle: {
        const double radius = std::min({ shape.cornerRadius, shape.size.width / 2, shape.size.height / 2 });
        path.addRoundedRect(rectangle, radius, radius, Qt::AbsoluteSize);
        break;
    }
    case c::ShapeKind::Ellipse: path.addEllipse(rectangle); break;
    case c::ShapeKind::Triangle:
        path.moveTo(shape.size.width / 2, 0);
        path.lineTo(shape.size.width, shape.size.height);
        path.lineTo(0, shape.size.height);
        path.closeSubpath();
        break;
    case c::ShapeKind::Line:
    case c::ShapeKind::Polygon:
        path.moveTo(shape.points.front().x, shape.points.front().y);
        for (std::size_t i = 1; i < shape.points.size(); ++i)
            path.lineTo(shape.points[i].x, shape.points[i].y);
        if (shape.kind == c::ShapeKind::Polygon)
            path.closeSubpath();
        break;
    }
    return path;
}

bool legacyHit(const c::ShapeLayer& shape, const c::AffineTransform& t, c::Vec2d p, double tolerance)
{
    const auto stroke = [](const QPainterPath& path, double width) {
        if (width <= 0)
            return QPainterPath {};
        QPainterPathStroker stroker;
        stroker.setWidth(width);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        stroker.setCurveThreshold(.03125);
        return stroker.createStroke(path);
    };
    const auto path = referencePath(shape);
    const QTransform transform(t.m00, t.m10, t.m01, t.m11, t.m02, t.m12);
    const QPointF point(p.x, p.y);
    const auto screenPath = transform.map(path);
    if (shape.kind != c::ShapeKind::Line && screenPath.contains(point))
        return true;
    if (shape.strokeEnabled && shape.strokeWidth > 0) {
        const auto outline = transform.map(stroke(path, shape.strokeWidth));
        if (outline.contains(point) || stroke(outline, tolerance * 2).contains(point))
            return true;
    }
    return stroke(screenPath, tolerance * 2).contains(point);
}

void preparedHits()
{
    u::QtShapeRenderService service;
    for (const auto kind : { c::ShapeKind::Rectangle, c::ShapeKind::RoundedRectangle,
             c::ShapeKind::Ellipse, c::ShapeKind::Triangle, c::ShapeKind::Line, c::ShapeKind::Polygon }) {
        auto shape = rectangle();
        shape.size = { 100, 100 };
        shape.kind = kind;
        if (kind == c::ShapeKind::Line)
            shape.points = { { 0, 100 }, { 100, 0 } };
        if (kind == c::ShapeKind::Polygon)
            shape.points = { { 0, 0 }, { 100, 0 }, { 100, 30 }, { 30, 30 }, { 30, 100 }, { 0, 100 } };
        shape.fillEnabled = false;
        shape.strokeEnabled = true;
        shape.strokeWidth = 3.5;
        const auto prepared = service.prepareHit(shape);
        for (const c::AffineTransform transform : { c::AffineTransform {},
                 { 0, -2, 201, 3, 0, -39 }, { -2, .3, 4, .1, .5, 5 },
                 { .1, 0, 75, 0, .1, 101 }, { 1, 0, 123, 0, 1, -45 } }) {
            for (const double tolerance : { 0., 1., 5., 11. }) {
                for (int i = -2; i < 16; ++i) {
                    const auto point = transform.map({ double(i) * 7.317 + .17,
                        double((i + 2) % 7) * 16.313 + .39 });
                    check(prepared->hit(transform, point, tolerance)
                            == legacyHit(shape, transform, point, tolerance),
                        "prepared paths preserve former transformed Qt path/stroker hit behavior");
                }
            }
        }
        check(prepared->hit({}, { 7.31, 7.19 }, 5) == service.hit(shape, {}, { 7.31, 7.19 }, 5),
            "compatibility wrapper agrees with prepared hit geometry");
    }

    auto shape = rectangle();
    const auto retained = service.prepareHit(shape);
    shape.size = { 1, 1 };
    check(retained->hit({}, { 10, 8 }, 0), "prepared geometry owns immutable source paths");
    check(!service.prepareHit(shape)->hit({}, { 10, 8 }, 0), "new revision receives new hit geometry");

    shape.size = { 1000, 1000 };
    shape.kind = c::ShapeKind::Polygon;
    shape.strokeEnabled = true;
    shape.strokeWidth = 1.5;
    for (int side = 0; side < 4; ++side) {
        for (int i = 0; i < 25000; ++i) {
            const auto position = double(i) / 25;
            shape.points.push_back(side == 0 ? c::Vec2d { position, 0 }
                : side == 1 ? c::Vec2d { 1000, position }
                : side == 2 ? c::Vec2d { 1000 - position, 1000 }
                            : c::Vec2d { 0, 1000 - position });
        }
    }
    const auto large = service.prepareHit(shape);
    check(large->hit({}, { -4, 500 }, 5), "large polygon prepares screen-space edge tolerance once");
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        check(!large->hit({}, { -100, double(i) }, 5), "large polygon rejects distant points by cached bounds");
        check(large->hit({}, { -4, double(i) + 400 }, 5), "large polygon reuses tolerance paths during hover");
        check(large->hit({}, { 100 + double(i), 500 }, 5), "large polygon uses inverse-local interior hit");
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "100k-vertex prepared hit: 300 mixed hover tests in " << elapsed << " ms\n";
    const c::AffineTransform moved { 0, -2, 100, -1, 0, 20 };
    check(large->hit(moved, moved.map({ -2, 500 }), 5),
        "prepared polygon updates its one-entry tolerance cache after rotation/flip/movement");
}

void transformedSampling()
{
    u::QtShapeRenderService service;
    c::Document document({ { 64, 64 }, 96 });
    auto shape = rectangle();
    shape.fillColor = { 204, 55, 89, 128 };
    auto layer = c::Layer::shape("Editable shape", shape);
    layer.renderCache = service.render({ shape, 2 });
    const auto cache = layer.renderCache;
    // Rotation, anisotropic scaling, horizontal reflection and translation.
    layer.localToDocument = { 0, -2, 40, -1.5, 0, 40 };
    const auto id = layer.id;
    const auto transform = layer.localToDocument;
    check(document.insertLayer(0, std::move(layer)), "shape layer inserted for merged sampling");
    const auto local = c::Vec2d { 10, 8 };
    const auto point = transform.map(local);
    const auto sample = c::sampleDocumentColor(document, id, point, c::ColorSampleSource::MergedVisible);
    check(sample.available() && sample.color == shape.fillColor,
        "merged sampling sees transformed editable shape cache with straight color and alpha");
    check(document.setLayerOpacity(id, .5F), "shape layer opacity changed");
    const auto faded = c::sampleDocumentColor(document, id, point, c::ColorSampleSource::MergedVisible);
    near(faded.color.alpha, 64, 1, "layer opacity remains separate from shape fill alpha");
    document.setLayerVisibility(id, false);
    check(c::sampleDocumentColor(document, id, point, c::ColorSampleSource::MergedVisible).color.alpha == 0,
        "hidden shape excluded from merged sampling");
    document.setLayerVisibility(id, true);
    check(c::sampleDocumentColor(document, id, { -1, 20 }, c::ColorSampleSource::MergedVisible).status
            == c::ColorSampleStatus::OutsideCanvas,
        "sampling remains document-clipped independent of render-cache outsets");
    check(document.layer(id)->renderCache == cache
            && std::get<c::ShapeLayer>(document.layer(id)->payload) == shape,
        "transform, opacity and visibility reuse resources without flattening canonical shape");

    shape.fillEnabled = false;
    shape.strokeEnabled = true;
    shape.strokeWidth = 4;
    document.setLayerShape(id, shape);
    document.layer(id)->renderCache = service.render({ shape, 1 });
    document.setLayerTransform(id, { 1, 0, 10, 0, 1, 10 });
    document.setLayerOpacity(id, 1);
    const auto strokeSample = c::sampleDocumentColor(document, id, { 9.5, 18.5 },
        c::ColorSampleSource::MergedVisible);
    check(strokeSample.color == shape.strokeColor,
        "merged sampling includes centered stroke outside logical transform frame");
}

void geometryResizeStrokeContract()
{
    u::QtShapeRenderService service;
    auto shape = rectangle();
    shape.fillEnabled = false;
    shape.strokeEnabled = true;
    shape.strokeWidth = 4;
    shape.cornerRadius = 12;
    // Integrating AA coverage across a straight edge measures the configured
    // width without depending on which side of a physical pixel it falls on.
    const auto thickness = [](const c::LayerRenderCache& cache, bool vertical, double along) {
        const auto extent = cache.surface->extent();
        const auto inverse = cache.pixelsToLocal.inverted().value();
        const auto crossing = inverse.map(vertical ? c::Vec2d { 0, along } : c::Vec2d { along, 0 });
        double covered = 0;
        const auto count = vertical ? extent.width : extent.height;
        for (std::uint32_t i = 0; i < count; ++i) {
            const int x = vertical ? static_cast<int>(i) : static_cast<int>(std::floor(crossing.x));
            const int y = vertical ? static_cast<int>(std::floor(crossing.y)) : static_cast<int>(i);
            const auto local = cache.pixelsToLocal.map({ x + .5, y + .5 });
            if (std::abs(vertical ? local.x : local.y) < 8)
                covered += pixel(cache, x, y).alpha / 255.;
        }
        return covered / cache.density;
    };

    for (const auto kind : { c::ShapeKind::Rectangle, c::ShapeKind::RoundedRectangle }) {
        shape.kind = kind;
        for (const c::Extent2d size : { c::Extent2d { 100, 100 }, { 100, 300 }, { 300, 100 },
                 { 67.25, 210.75 }, { 100, 100 } }) {
            shape.size = size;
            for (const double density : { .5, 1., 2., 4. }) {
                const auto cache = service.render({ shape, density });
                near(thickness(*cache, true, size.height / 2), 4, .025,
                    "geometry-resized vertical edge keeps four document-pixel stroke at every zoom");
                near(thickness(*cache, false, size.width / 2), 4, .025,
                    "geometry-resized horizontal edge keeps four document-pixel stroke at every zoom");
                check(at(*cache, { size.width / 2, size.height / 2 }).alpha == 0,
                    "tall/wide geometry resize preserves hollow interior");
                check(cache->logicalExtent == c::shapeGeometryExtent(shape),
                    "resized authoritative geometry drives render-cache logical bounds");
                check(shape.strokeWidth == 4 && shape.cornerRadius == 12,
                    "repeated intrinsic resizing does not scale stroke or requested corner radius");
            }
        }
    }

    // Rebuilding the path preserves the original round corner rather than
    // stretching an already stroked square (which would make an ellipse here).
    shape.kind = c::ShapeKind::RoundedRectangle;
    shape.size = { 100, 100 };
    const auto square = service.render({ shape, 2 });
    shape.size = { 100, 300 };
    const auto tall = service.render({ shape, 2 });
    for (double y = -3.75; y < 25; y += .5)
        for (double x = -3.75; x < 25; x += .5)
            near(at(*tall, { x, y }).alpha, at(*square, { x, y }).alpha, 1,
                "top-left rounded corner is unchanged after tall intrinsic resize");

    shape.kind = c::ShapeKind::Ellipse;
    for (const c::Extent2d size : { c::Extent2d { 18, 18 }, { 18, 95 }, { 95, 18 }, { 3.25, 2.75 } }) {
        shape.size = size;
        for (const double density : { .5, 1., 2., 4. }) {
            const auto cache = service.render({ shape, density });
            check(!empty(*cache) && partial(*cache),
                "geometry-resized small/tall/wide ellipse has bounded antialiased stroke coverage");
            const auto hit = service.prepareHit(shape);
            check(hit->hit({ }, { size.width / 2, size.height / 2 }, 0),
                "new hollow ellipse geometry remains hittable from enclosed interior");
            check(!hit->hit({ }, { size.width + 8, size.height / 2 }, 0),
                "ellipse resize rebuilds hit bounds rather than retaining stale shape extents");
        }
    }

    for (const auto kind : { c::ShapeKind::Line, c::ShapeKind::Polygon }) {
        shape.kind = kind;
        for (const c::Extent2d size : { c::Extent2d { 120, 60 }, { 120, 300 }, { 300, 60 } }) {
            shape.size = size;
            shape.points = kind == c::ShapeKind::Line
                ? std::vector<c::Vec2d> { { 0, size.height / 2 }, { size.width, size.height / 2 } }
                : std::vector<c::Vec2d> { { 0, 0 }, { size.width, 0 }, { size.width, size.height },
                      { size.width / 2, size.height / 3 }, { 0, size.height } };
            const auto cache = service.render({ shape, 2 });
            check(!empty(*cache) && shape.strokeWidth == 4,
                "resized line endpoints and polygon vertices rebuild a constant-width path");
            if (kind == c::ShapeKind::Line) {
                check(at(*cache, { -.75, size.height / 2 }).alpha > 240,
                    "line endpoint retains round cap after geometry resize");
                check(at(*cache, { size.width / 2, size.height / 2 + 2.75 }).alpha == 0,
                    "line geometry height is not applied as a stroke-scale multiplier");
            } else {
                check(at(*cache, { size.width / 2, -.75 }).alpha > 240,
                    "polygon rebuilt top edge retains configured centered stroke");
                check(cache->pixelsToLocal.m12 >= -3.1,
                    "resized acute polygon retains bounded round join rather than a miter spike");
            }
        }
    }

    // A previous whole-layer Ctrl+T transform remains an independent baseline.
    // Intrinsic geometry changes must rebuild local stroke coverage, not bake
    // the old cache or normalize away rotation, reflection, shear or scaling.
    shape.kind = c::ShapeKind::Rectangle;
    shape.points.clear();
    shape.size = { 100, 300 };
    for (const c::AffineTransform baseline : { c::AffineTransform { },
             { 0, -1, 0, 1, 0, 0 }, { -1, 0, 0, 0, 1, 0 }, { -1.5, .3, 0, .4, .6, 0 } }) {
        auto transform = baseline;
        transform.m02 = 500;
        transform.m12 = 500;
        c::Document document({ { 1000, 1000 }, 96 });
        auto layer = c::Layer::shape("Resized geometry with existing affine", shape);
        layer.localToDocument = transform;
        layer.renderCache = service.render({ shape, 4 });
        const auto id = layer.id;
        check(document.insertLayer(0, std::move(layer)), "resized shape inserted for merged sampling");
        for (const auto local : { c::Vec2d { .5, 150 }, { 50, .5 }, { 99.5, 150 }, { 50, 299.5 } }) {
            const auto sample = c::sampleDocumentColor(document, id, transform.map(local),
                c::ColorSampleSource::MergedVisible);
            check(sample.available() && sample.color == shape.strokeColor,
                "merged sampling agrees with regenerated stroke under existing rotation/flip/scale/shear");
        }
        check(document.layer(id)->localToDocument == transform
                && std::get<c::ShapeLayer>(document.layer(id)->payload) == shape,
            "rendering and sampling retain authoritative resized geometry and independent affine");
    }
}

void renderTimings()
{
    // Informational rather than a machine-dependent pass/fail threshold.
    u::QtShapeRenderService service;
    auto shape = rectangle();
    shape.size = { 3840, 2160 };
    shape.fillColor.alpha = 255;
    for (int scenario = 0; scenario < 3; ++scenario) {
        shape.strokeEnabled = scenario != 0;
        shape.strokeWidth = 64;
        const double density = scenario == 2 ? .34 : 1.0;
        std::array<double, 3> timings {};
        std::uint64_t pixelCount = 0;
        for (auto& timing : timings) {
            const auto start = std::chrono::steady_clock::now();
            const auto cache = service.render({ shape, density });
            timing = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            pixelCount = static_cast<std::uint64_t>(cache->surface->extent().width)
                * cache->surface->extent().height;
            check(!cache->surface->extent().empty(), "4K render benchmark produced a cache");
        }
        std::sort(timings.begin(), timings.end());
        std::cout << "4K shape cache " << (scenario == 0 ? "fill" : scenario == 1 ? "fill+stroke" : "preview")
                  << ": median " << timings[1] << " ms, " << pixelCount << " pixels\n";
    }
}

void documentGridRasterization()
{
    u::QtShapeRenderService service;
    // Document-grid caches may share pixels only for EXACT integer translation.
    // Even a one-ULP fractional move must reject reuse, not silently snap geometry.
    auto hollow = rectangle();
    hollow.fillEnabled = false;
    hollow.strokeEnabled = true;
    hollow.strokeWidth = 4;
    const auto original = service.renderDocument(hollow, {}, 65536);
    for (const double offset : {20., -20., 20.25, std::nextafter(20., 0.)}) {
        c::AffineTransform moved;
        moved.m02 = moved.m12 = offset;
        const auto reused = c::translatedDocumentRenderCache(original, moved);
        if (offset == std::floor(offset)) {
            check(reused && reused->surface == original->surface,
                "integer hollow-shape translation shares the exact surface");
            check(reused && reused->rasterizedDocumentTransform == moved
                    && reused->documentOrigin == original->documentOrigin + c::Vec2d{offset, offset},
                "integer cache reuse updates origin and authoritative affine key");
        } else {
            check(!reused, "fractional translation rejects cache reuse without epsilon snapping");
            const auto rebuilt = service.renderDocument(hollow, moved, 65536);
            check(rebuilt->surface != original->surface && rebuilt->rasterizedDocumentTransform == moved,
                "fractional hollow-shape translation rebuilds on its exact document grid");
        }
    }
    auto shape = rectangle();
    shape.kind = c::ShapeKind::Ellipse;
    shape.size = { 23.25, 17.75 };
    shape.strokeEnabled = true;
    shape.strokeWidth = 2.5;
    shape.strokeColor.alpha = 137;
    for (const c::AffineTransform transform : { c::AffineTransform {},
             { 0, -1.3, 37.25, .7, 0, -9.5 }, { -1.1, .4, 60.125, .3, 1.7, -20.25 } }) {
        const auto full = service.renderDocument(shape, transform, 65536);
        const auto extent = full->surface->extent();
        const auto origin = full->documentOrigin;
        check(full->rasterizedDocumentTransform == transform,
            "document-grid shape cache retains its affine key");
        check(origin.x == std::floor(origin.x) && origin.y == std::floor(origin.y),
            "fractional/rotated/reflected shape is anchored to integer document pixels");
        const auto mapped = transform.map(full->pixelsToLocal.map({ 7.5, 9.5 }));
        near(mapped.x, origin.x + 7.5, 1e-10, "document shape x mapping contains no density rescale");
        near(mapped.y, origin.y + 9.5, 1e-10, "document shape y mapping contains no density rescale");
        check(full->logicalExtent == c::shapeGeometryExtent(shape),
            "document shape raster bounds do not alter logical shape geometry");
        const c::RectI clip { int(origin.x) + 3, int(origin.y) + 4,
            int(extent.width) - 7, int(extent.height) - 8 };
        const auto clipped = service.renderDocument(shape, transform,
            std::size_t(clip.width) * std::size_t(clip.height), clip);
        check(clipped->surface->extent() == c::Extent2u { std::uint32_t(clip.width), std::uint32_t(clip.height) },
            "document clipping restricts shape allocation before budget admission");
        bool same = true;
        for (int y = 0; y < clip.height; ++y)
            for (int x = 0; x < clip.width; ++x)
                same &= pixel(*clipped, x, y) == pixel(*full, x + 3, y + 4);
        check(same, "shape clip preserves exact AA/fill/stroke pixels on the same document grid");
        bool rejected = false;
        try {
            (void)service.renderDocument(shape, transform, std::size_t(extent.width) * extent.height - 1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "exact shape output rejects an insufficient budget without downsampling");
    }
    shape = rectangle();
    shape.size = { 100000, 100000 };
    const auto clipped = service.renderDocument(shape, {}, 64, c::RectI { 100, 200, 8, 8 });
    check(clipped->surface->extent() == c::Extent2u { 8, 8 }
            && pixel(*clipped, 4, 4) == shape.fillColor,
        "small merge region of a huge authoritative shape never allocates its full bounds");
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        fillStrokeAndAlpha();
        kindsAndDegeneracy();
        ellipsesAndResolution();
        geometricHits();
        preparedHits();
        transformedSampling();
        geometryResizeStrokeContract();
        documentGridRasterization();
        renderTimings();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "EXCEPTION: " << exception.what() << '\n';
    }
    if (!failures)
        std::cout << "Shape path, AA, style, bounded cache, geometry targeting and transformed sampling passed\n";
    return failures ? 1 : 0;
}
