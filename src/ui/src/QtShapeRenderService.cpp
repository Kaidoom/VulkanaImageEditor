#include "imageeditor/ui/QtShapeRenderService.hpp"

#include "imageeditor/core/ColorMath.hpp"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace imageeditor::ui {
namespace {
constexpr double maxEdge = 8192;
constexpr double maxPixels = 16 * 1024 * 1024;
constexpr double paddingPixels = 2;

QTransform qtTransform(const core::AffineTransform& t)
{
    return { t.m00, t.m10, t.m20, t.m01, t.m11, t.m21, t.m02, t.m12, t.m22 };
}

QPainterPath geometryPath(const core::ShapeLayer& shape)
{
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    if (!core::hasShapeGeometry(shape))
        return path;
    const QRectF rect(0, 0, shape.size.width, shape.size.height);
    switch (shape.kind) {
    case core::ShapeKind::Rectangle:
        if (!rect.isEmpty())
            path.addRect(rect);
        break;
    case core::ShapeKind::RoundedRectangle:
        if (!rect.isEmpty()) {
            const auto radius = std::min({ shape.cornerRadius, rect.width() * .5, rect.height() * .5 });
            path.addRoundedRect(rect, radius, radius, Qt::AbsoluteSize);
        }
        break;
    case core::ShapeKind::Ellipse:
        if (!rect.isEmpty())
            path.addEllipse(rect);
        break;
    case core::ShapeKind::Triangle:
        if (!rect.isEmpty()) {
            path.moveTo(rect.width() * .5, 0);
            path.lineTo(rect.width(), rect.height());
            path.lineTo(0, rect.height());
            path.closeSubpath();
        }
        break;
    case core::ShapeKind::Line:
    case core::ShapeKind::Polygon:
        if (shape.points.size() >= (shape.kind == core::ShapeKind::Line ? 2U : 3U)) {
            path.moveTo(shape.points.front().x, shape.points.front().y);
            const auto count = shape.kind == core::ShapeKind::Line ? std::size_t(2) : shape.points.size();
            for (std::size_t i = 1; i < count; ++i)
                path.lineTo(shape.points[i].x, shape.points[i].y);
            if (shape.kind == core::ShapeKind::Polygon)
                path.closeSubpath();
        }
        break;
    }
    return path;
}

QPainterPath strokePath(const QPainterPath& path, double width,
    core::ShapeJoin join=core::ShapeJoin::Round, core::ShapeCap cap=core::ShapeCap::Round, double miter=2)
{
    // Width zero is explicitly no stroke, never Qt's cosmetic hairline.
    if (path.isEmpty() || !std::isfinite(width) || width <= 0)
        return {};
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(cap==core::ShapeCap::Butt?Qt::FlatCap:cap==core::ShapeCap::Square?Qt::SquareCap:Qt::RoundCap);
    stroker.setJoinStyle(join==core::ShapeJoin::Miter?Qt::MiterJoin:join==core::ShapeJoin::Bevel?Qt::BevelJoin:Qt::RoundJoin);
    stroker.setMiterLimit(miter);
    // Use a finer flattening threshold than Qt's 0.25 default so curved
    // outlines stay smooth when the cache is regenerated at up to 8x density.
    stroker.setCurveThreshold(.03125);
    return stroker.createStroke(path);
}

core::Extent2u logicalExtent(const core::ShapeLayer& shape)
{
    const auto extent = core::shapeGeometryExtent(shape);
    return { std::max(1U, extent.width), std::max(1U, extent.height) };
}

std::shared_ptr<core::LayerRenderCache> emptyCache(const core::ShapeLayer& shape, double requested)
{
    auto cache = std::make_shared<core::LayerRenderCache>();
    cache->surface = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u { 1, 1 });
    cache->logicalExtent = logicalExtent(shape);
    cache->requestedDensity = requested;
    return cache;
}

void rasterizeCoverage(QImage& image, const QPainterPath& path, const QTransform& transform)
{
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setWorldTransform(transform);
    painter.fillPath(path, Qt::white);
}

// At most 256 x 256 coverage combinations occur for constant fill/stroke
// colors. Avoid pow() per pixel while retaining the exact shared color math.
std::array<std::uint8_t, 4> composite(core::Rgba8 fill, int fillCoverage,
    core::Rgba8 stroke, int strokeCoverage)
{
    const double fillAlpha = (double(fill.alpha) / 255) * (double(fillCoverage) / 255);
    const double strokeAlpha = (double(stroke.alpha) / 255) * (double(strokeCoverage) / 255);
    const double remainder = fillAlpha * (1 - strokeAlpha);
    const double alpha = strokeAlpha + remainder;
    if (alpha <= 0)
        return {};
    const auto channel = [&](std::uint8_t base, std::uint8_t top) {
        return core::linearToSrgb((core::srgbToLinear(top) * strokeAlpha
            + core::srgbToLinear(base) * remainder) / alpha);
    };
    return { channel(fill.red, stroke.red), channel(fill.green, stroke.green),
        channel(fill.blue, stroke.blue), core::alphaToByte(alpha) };
}

void colorizeCoverage(const QImage& coverage, core::Rgba8 color, std::vector<std::byte>& bytes)
{
    std::array<std::array<std::uint8_t, 4>, 256> values;
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = composite({}, 0, color, static_cast<int>(i));
    const int width = coverage.width();
    for (int y = 0; y < coverage.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(coverage.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)) * 4;
            const auto& value = values[static_cast<std::size_t>(qAlpha(row[x]))];
            for (std::size_t channel = 0; channel < 4; ++channel)
                bytes[offset + channel] = std::byte(value[channel]);
        }
    }
}

std::vector<std::byte> rasterizeShape(const core::ShapeLayer& shape, const QPainterPath& path,
    const QPainterPath& stroke, bool drawFill, int width, int height, const QTransform& transform)
{
    QImage coverage(width, height, QImage::Format_ARGB32_Premultiplied);
    if (coverage.isNull())
        throw std::bad_alloc();
    const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::byte> bytes(pixelCount * 4);
    if (!drawFill || stroke.isEmpty()) {
        // One coverage pass is enough when there is no fill/stroke overlap.
        rasterizeCoverage(coverage, drawFill ? path : stroke, transform);
        colorizeCoverage(coverage, drawFill ? shape.fillColor : shape.strokeColor, bytes);
    } else {
        rasterizeCoverage(coverage, path, transform);
        for (int y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(coverage.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(x)) * 4;
                // Preserve unrounded fill coverage until combined with stroke.
                bytes[offset + 3] = std::byte(qAlpha(row[x]));
            }
        }
        rasterizeCoverage(coverage, stroke, transform);

        std::array<std::array<std::uint8_t, 4>, 65536> combinations {};
        std::array<bool, 65536> resolved {};
        for (int y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(coverage.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(x)) * 4;
                const int fillCoverage = std::to_integer<int>(bytes[offset + 3]);
                const int strokeCoverage = qAlpha(row[x]);
                const auto key = static_cast<std::size_t>(fillCoverage * 256 + strokeCoverage);
                if (!resolved[key]) {
                    combinations[key] = composite(shape.fillColor, fillCoverage, shape.strokeColor, strokeCoverage);
                    resolved[key] = true;
                }
                for (std::size_t channel = 0; channel < 4; ++channel)
                    bytes[offset + channel] = std::byte(combinations[key][channel]);
            }
        }
    }
    return bytes;
}

QRectF shapeDocumentBounds(const QPainterPath& path, const QPainterPath& stroke,
    bool drawFill, const core::AffineTransform& localToDocument)
{
    if (path.isEmpty() || (!drawFill && stroke.isEmpty()))
        return {};
    const auto support=path.boundingRect().united(stroke.boundingRect()).adjusted(-2,-2,2,2);
    if(!localToDocument.validOver({support.x(),support.y(),support.width(),support.height()}))
        throw std::invalid_argument("Projective horizon crosses shape support");
    const auto transform = qtTransform(localToDocument);
    auto rect = transform.map(drawFill ? path : stroke).boundingRect();
    if (drawFill && !stroke.isEmpty())
        rect = rect.united(transform.map(stroke).boundingRect());
    return rect.isEmpty() ? QRectF {} : rect.adjusted(-paddingPixels, -paddingPixels,
        paddingPixels, paddingPixels);
}

class QtShapeHitGeometry final : public core::ShapeHitGeometry {
public:
    explicit QtShapeHitGeometry(const core::ShapeLayer& shape)
        : path_(geometryPath(shape))
        , stroke_(shape.strokeEnabled ? strokePath(path_, shape.strokeWidth,shape.strokeJoin,shape.strokeCap,shape.strokeMiterLimit) : QPainterPath {})
        , bounds_(stroke_.isEmpty() ? path_.boundingRect()
                                  : path_.boundingRect().united(stroke_.boundingRect()))
        , closed_(shape.kind != core::ShapeKind::Line)
    {
    }

    bool hit(const core::AffineTransform& localToScreen, core::Vec2d screenPoint,
        double tolerancePixels) const override
    {
        if (path_.isEmpty() || !std::isfinite(screenPoint.x) || !std::isfinite(screenPoint.y)
            || !std::isfinite(localToScreen.m00) || !std::isfinite(localToScreen.m01)
            || !std::isfinite(localToScreen.m02) || !std::isfinite(localToScreen.m10)
            || !std::isfinite(localToScreen.m11) || !std::isfinite(localToScreen.m12))
            return false;
        const double tolerance = std::isfinite(tolerancePixels) ? std::max(0.0, tolerancePixels) : 0;
        const auto transform = qtTransform(localToScreen);
        const QPointF point(screenPoint.x, screenPoint.y);
        // mapRect is a conservative transformed local bound, without mapping
        // or stroking even one path element for distant hover events.
        const auto screenBounds = transform.mapRect(bounds_).adjusted(-tolerance, -tolerance,
            tolerance, tolerance);
        if (!screenBounds.contains(point))
            return false;
        if (const auto inverse = localToScreen.inverted()) {
            const auto local = inverse->map(screenPoint);
            const QPointF localPoint(local.x, local.y);
            if ((closed_ && path_.contains(localPoint)) || stroke_.contains(localPoint))
                return true;
        }
        if (tolerance <= 0)
            return false;

        // Expensive screen-space stroking is needed only for near-boundary
        // tolerance. Retain exactly one transform+tolerance entry, so normal
        // hover movement never remaps/re-strokes a large polygon each event.
        // The lock also makes prepared geometry safe for independent readers.
        const std::lock_guard lock(cacheMutex_);
        if (!screenCache_ || screenCache_->transform != localToScreen
            || screenCache_->tolerance != tolerance) {
            ScreenCache next;
            next.transform = localToScreen;
            next.tolerance = tolerance;
            if (!stroke_.isEmpty())
                next.strokeTolerance = strokePath(transform.map(stroke_), tolerance * 2);
            next.geometryTolerance = strokePath(transform.map(path_), tolerance * 2);
            screenCache_ = std::move(next);
        }
        return screenCache_->strokeTolerance.contains(point)
            || screenCache_->geometryTolerance.contains(point);
    }

private:
    struct ScreenCache {
        core::AffineTransform transform;
        double tolerance {0};
        QPainterPath strokeTolerance;
        QPainterPath geometryTolerance;
    };
    const QPainterPath path_;
    const QPainterPath stroke_;
    const QRectF bounds_;
    const bool closed_;
    mutable std::mutex cacheMutex_;
    mutable std::optional<ScreenCache> screenCache_;
};
} // namespace

double QtShapeRenderService::densityForScale(double scale) noexcept
{
    if (!std::isfinite(scale) || scale <= 0)
        return 1;
    // Affine rotation hypot() can land a few ulps above an exact tier.
    return std::clamp(std::exp2(std::ceil(std::log2(scale) - 1e-9)), .125, 8.0);
}

std::shared_ptr<const core::LayerRenderCache> QtShapeRenderService::render(
    const core::ShapeRenderRequest& request)
{
    const auto& shape = request.shape;
    if (!core::validShape(shape))
        throw std::invalid_argument("Shape geometry contains invalid coordinates or style");
    const double requested = std::isfinite(request.rasterDensity) && request.rasterDensity > 0
        ? request.rasterDensity : 1;
    const auto path = geometryPath(shape);
    const bool drawFill = shape.kind != core::ShapeKind::Line && shape.fillEnabled && shape.fillColor.alpha;
    const bool drawStroke = shape.strokeEnabled && shape.strokeWidth > 0 && shape.strokeColor.alpha;
    const auto stroke = drawStroke ? strokePath(path, shape.strokeWidth,shape.strokeJoin,shape.strokeCap,shape.strokeMiterLimit) : QPainterPath {};
    if (path.isEmpty() || (!drawFill && stroke.isEmpty()))
        return emptyCache(shape, requested);

    auto rect = drawFill ? path.boundingRect() : stroke.boundingRect();
    if (drawFill && !stroke.isEmpty())
        rect = rect.united(stroke.boundingRect());
    if (rect.isEmpty())
        return emptyCache(shape, requested);

    // Bound both dimensions and exact rounded pixel area, reserving a two-
    // physical-pixel AA fringe on every side even for downscaled giant shapes.
    double density = request.exactPixelBudget ? requested : std::min({ std::clamp(requested, 1e-6, 8.0),
        (maxEdge - 2 * paddingPixels) / rect.width(),
        (maxEdge - 2 * paddingPixels) / rect.height(),
        std::sqrt(maxPixels / (rect.width() * rect.height())) });
    if(request.exactPixelBudget) {
        const auto w=std::ceil(rect.width()*density+2*paddingPixels);
        const auto h=std::ceil(rect.height()*density+2*paddingPixels);
        const auto budget=std::min<std::size_t>(*request.exactPixelBudget,64ULL*1024*1024);
        if(!std::isfinite(w)||!std::isfinite(h)||w<1||h<1||w>32768||h>32768||w*h>double(budget))
            throw std::runtime_error("Shape output-resolution cache exceeds the merge/export budget");
    }
    const auto dimensions = [&](double d) {
        return std::pair { std::max(1, static_cast<int>(std::ceil(rect.width() * d + 2 * paddingPixels))),
            std::max(1, static_cast<int>(std::ceil(rect.height() * d + 2 * paddingPixels))) };
    };
    auto [width, height] = dimensions(density);
    while (!request.exactPixelBudget && static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)
        > static_cast<std::uint64_t>(maxPixels)) {
        density *= .999 * std::sqrt(maxPixels / (double(width) * height));
        std::tie(width, height) = dimensions(density);
    }
    rect.adjust(-paddingPixels / density, -paddingPixels / density,
        paddingPixels / density, paddingPixels / density);

    QTransform transform;
    transform.scale(density, density);
    transform.translate(-rect.x(), -rect.y());
    auto bytes = rasterizeShape(shape, path, stroke, drawFill, width, height, transform);

    auto cache = std::make_shared<core::LayerRenderCache>();
    cache->surface = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) }, std::move(bytes));
    cache->pixelsToLocal = { 1 / density, 0, rect.x(), 0, 1 / density, rect.y() };
    cache->logicalExtent = logicalExtent(shape);
    cache->density = density;
    cache->requestedDensity = requested;
    return cache;
}

core::RectD QtShapeRenderService::documentBounds(const core::ShapeLayer& shape,
    const core::AffineTransform& localToDocument) const
{
    if (!core::validShape(shape))
        throw std::invalid_argument("Shape geometry contains invalid coordinates or style");
    if (!localToDocument.inverted())
        throw std::invalid_argument("Invalid shape transform in document rasterization");
    const auto path = geometryPath(shape);
    const bool drawFill = shape.kind != core::ShapeKind::Line && shape.fillEnabled && shape.fillColor.alpha;
    const auto stroke = shape.strokeEnabled && shape.strokeColor.alpha
        ? strokePath(path, shape.strokeWidth,shape.strokeJoin,shape.strokeCap,shape.strokeMiterLimit) : QPainterPath {};
    const auto rect = shapeDocumentBounds(path, stroke, drawFill, localToDocument);
    return { rect.x(), rect.y(), rect.width(), rect.height() };
}

std::shared_ptr<const core::LayerRenderCache> QtShapeRenderService::renderDocument(
    const core::ShapeLayer& shape, const core::AffineTransform& localToDocument,
    std::size_t exactPixelBudget, std::optional<core::RectI> documentClip) const
{
    if (!core::validShape(shape))
        throw std::invalid_argument("Shape geometry contains invalid coordinates or style");
    const auto inverse = localToDocument.inverted();
    if (!inverse)
        throw std::invalid_argument("Invalid shape transform in document rasterization");
    const auto path = geometryPath(shape);
    const bool drawFill = shape.kind != core::ShapeKind::Line && shape.fillEnabled && shape.fillColor.alpha;
    const auto stroke = shape.strokeEnabled && shape.strokeColor.alpha
        ? strokePath(path, shape.strokeWidth,shape.strokeJoin,shape.strokeCap,shape.strokeMiterLimit) : QPainterPath {};
    auto rect = shapeDocumentBounds(path, stroke, drawFill, localToDocument);
    const auto localRect = shapeDocumentBounds(path, stroke, drawFill, {});
    const auto localSourceBounds = localRect.isEmpty() ? core::RectD { 0, 0, 1, 1 }
        : core::RectD { localRect.x(), localRect.y(), std::ceil(localRect.width()), std::ceil(localRect.height()) };
    if (documentClip)
        rect = rect.intersected(QRectF(documentClip->x, documentClip->y,
            documentClip->width, documentClip->height));
    if (rect.isEmpty()) {
        auto cache = emptyCache(shape, 1);
        cache->pixelsToLocal = *inverse;
        cache->rasterizedDocumentTransform = localToDocument;
        cache->localSourceBounds = localSourceBounds;
        return cache;
    }
    const double left = std::floor(rect.left()), top = std::floor(rect.top());
    const double width = std::ceil(rect.right()) - left, height = std::ceil(rect.bottom()) - top;
    const auto budget = std::min<std::size_t>(exactPixelBudget, 64ULL * 1024 * 1024);
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) || !std::isfinite(height)
        || width < 1 || height < 1 || width > 32768 || height > 32768 || width * height > double(budget))
        throw std::runtime_error("Shape document raster cache exceeds the merge/export budget");

    auto pixelsToLocal = core::composeTransform(*inverse,{1,0,left,0,1,top});
    auto localToPixels = core::composeTransform({1,0,-left,0,1,-top},localToDocument);
    auto bytes = rasterizeShape(shape, path, stroke, drawFill, int(width), int(height), qtTransform(localToPixels));
    auto cache = std::make_shared<core::LayerRenderCache>();
    cache->surface = std::make_shared<core::ContiguousRasterSurface>(core::Extent2u {
        std::uint32_t(width), std::uint32_t(height) }, std::move(bytes));
    cache->pixelsToLocal = pixelsToLocal;
    cache->logicalExtent = logicalExtent(shape);
    cache->rasterizedDocumentTransform = localToDocument;
    cache->documentOrigin = { left, top };
    cache->localSourceBounds = localSourceBounds;
    return cache;
}

std::shared_ptr<const core::ShapeHitGeometry> QtShapeRenderService::prepareHit(
    const core::ShapeLayer& shape) const
{
    return std::make_shared<QtShapeHitGeometry>(shape);
}

bool QtShapeRenderService::hit(const core::ShapeLayer& shape,
    const core::AffineTransform& localToScreen, core::Vec2d screenPoint, double tolerancePixels) const
{
    return prepareHit(shape)->hit(localToScreen, screenPoint, tolerancePixels);
}

} // namespace imageeditor::ui
