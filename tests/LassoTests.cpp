#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/FreehandSelectionPath.hpp"
#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
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
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <class Function> bool throws(Function&& function)
{
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

SelectionState raster(Extent2u canvas, std::span<const Vec2d> points, std::size_t budget = 65536)
{
    PolygonCoverageRasterizer job(canvas, points);
    unsigned calls = 0;
    while (!job.finished()) {
        job.step(budget);
        if (++calls > 2000000) {
            CHECK(false);
            break;
        }
    }
    CHECK(job.coverage().size() == job.stride() * std::size_t(job.region().height));
    return SelectionMask::fromR8Region(canvas, job.region(), job.coverage(), job.stride());
}

std::vector<std::byte> pixels(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4U);
    surface.copyRgba8(
        { 0, 0, int(extent.width), int(extent.height) }, result, std::size_t(extent.width) * 4U);
    return result;
}
Rgba8 pixel(const RasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> bytes { };
    surface.copyRgba8({ x, y, 1, 1 }, bytes, 4);
    return { std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3]) };
}

// Deliberately independent oracle: point-in-polygon at a regular 2-D grid.
// Production integrates horizontal intervals and uses an active-edge table;
// this oracle neither sorts crossings nor shares its interval/coverage code.
bool pointInside(std::span<const Vec2d> polygon, Vec2d point)
{
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto a = polygon[i], b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y)) {
            const auto crossing = a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (point.x < crossing)
                inside = !inside;
        }
    }
    return inside;
}
int oracle(std::span<const Vec2d> polygon, int x, int y)
{
    constexpr int samples = 128;
    int covered = 0;
    for (int sy = 0; sy < samples; ++sy)
        for (int sx = 0; sx < samples; ++sx)
            covered += pointInside(polygon, { x + (sx + 0.5) / samples, y + (sy + 0.5) / samples });
    return int(std::lround(double(covered) * 255.0 / (samples * samples)));
}

void capturePreservesGeometryAndHasExplicitLimits()
{
    FreehandSelectionPath path;
    CHECK(path.append({ -1.25, 2.75 }));
    CHECK(!path.append({ -1.25, 2.75 }));
    CHECK(path.append({ 0.25, 2.75 }));
    CHECK(path.append({ 3.25, 2.75 }));
    CHECK(path.points().size() == 2);
    CHECK(path.points().front() == Vec2d({ -1.25, 2.75 }));
    CHECK(path.points().back() == Vec2d({ 3.25, 2.75 }));
    CHECK(path.append({ 0.25, 2.75 })); // Backtracking must retain the turning point.
    CHECK(path.points().size() == 3);
    CHECK(path.append({ 0.25, 2.75000001 })); // A sharp/subpixel turn is not filtered.
    CHECK(path.points().size() == 4);
    const auto before = std::vector<Vec2d>(path.points().begin(), path.points().end());
    CHECK(throws([&] { path.append({ std::numeric_limits<double>::quiet_NaN(), 0 }); }));
    CHECK(throws([&] { path.append({ 0, std::numeric_limits<double>::infinity() }); }));
    CHECK(throws([&] { path.append({ FreehandSelectionPath::maximumCoordinate + 1, 0 }); }));
    CHECK(std::equal(before.begin(), before.end(), path.points().begin(), path.points().end()));
    FreehandSelectionPath longPath;
    for (std::size_t i = 0; i < FreehandSelectionPath::maximumPoints; ++i)
        CHECK(longPath.append({ double(i), double(i % 2) }));
    CHECK(longPath.points().size() == FreehandSelectionPath::maximumPoints);
    CHECK(throws([&] { longPath.append({ 65536, 0 }); }));
    CHECK(longPath.points().size() == FreehandSelectionPath::maximumPoints);
}

void rectangleAnalyticCoverageAndDocumentClipping()
{
    const Extent2u canvas { 10, 9 };
    const std::vector<Vec2d> points { { 1.25, 2.5 }, { 4.75, 2.5 }, { 4.75, 6.25 }, { 1.25, 6.25 } };
    const auto mask = raster(canvas, points);
    CHECK(mask->bounds() == RectI({ 1, 2, 4, 5 }));
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 10; ++x) {
            const auto horizontal = std::max(0.0, std::min(x + 1.0, 4.75) - std::max(double(x), 1.25));
            const auto vertical = std::max(0.0, std::min(y + 1.0, 6.25) - std::max(double(y), 2.5));
            CHECK(mask->coverageAtDocumentPixel(x, y) == std::lround(255.0 * horizontal * vertical));
        }
    }
    const std::vector<Vec2d> outside { { -8.0, -8.0 }, { 18.0, -8.0 }, { 18.0, 17.0 }, { -8.0, 17.0 } };
    CHECK(raster(canvas, outside)->equivalent(*SelectionMask::filled(canvas, 255)));
    // Clamping vertices would change this slanted boundary and wrongly include (0,0).
    const std::vector<Vec2d> crossing { { -6.0, 6.0 }, { 5.0, -4.0 }, { 15.0, 5.0 }, { 4.0, 13.0 } };
    const auto crossingMask = raster(canvas, crossing);
    CHECK(crossingMask->coverageAtDocumentPixel(0, 0) < 255);
    CHECK(crossingMask->coverageAtDocumentPixel(4, 4) == 255);
    const std::vector<Vec2d> distant { { -100, -100 }, { -20, -100 }, { -20, -20 }, { -100, -20 } };
    CHECK(raster(canvas, distant)->bounds().empty());
}

void concaveCrossedAndWindingIndependentCoverage()
{
    const Extent2u canvas { 9, 8 };
    const std::vector<std::vector<Vec2d>> fixtures {
        { { 1.125, 1.25 }, { 7.625, 1.25 }, { 7.625, 3.125 }, { 3.75, 3.125 }, { 3.75, 6.75 },
            { 1.125, 6.75 } },
        { { 1.0, 1.0 }, { 7.0, 7.0 }, { 1.0, 7.0 }, { 7.0, 1.0 } }, // Signed area is zero, not empty.
        { { -2.25, 2.125 }, { 5.125, -1.25 }, { 9.875, 6.5 }, { 3.25, 4.375 }, { 1.5, 9.25 } },
        { { 1.0, 1.0 }, { 7.0, 1.0 }, { 7.0, 7.0 }, { 1.0, 7.0 }, { 1.0, 1.0 }, { 3.0, 3.0 }, { 5.0, 3.0 },
            { 5.0, 5.0 }, { 3.0, 5.0 }, { 3.0, 3.0 }, { 1.0, 1.0 } }, // Hole + retraced bridge.
    };
    for (const auto& points : fixtures) {
        const auto expected = raster(canvas, points);
        CHECK(!expected->bounds().empty());
        for (int y = 0; y < int(canvas.height); ++y)
            for (int x = 0; x < int(canvas.width); ++x)
                CHECK(std::abs(int(expected->coverageAtDocumentPixel(x, y)) - oracle(points, x, y)) <= 3);
        auto reversed = points;
        std::reverse(reversed.begin(), reversed.end());
        CHECK(raster(canvas, reversed)->equivalent(*expected));
        for (std::size_t offset = 1; offset < points.size(); ++offset) {
            auto rotated = points;
            std::rotate(rotated.begin(), rotated.begin() + std::ptrdiff_t(offset), rotated.end());
            CHECK(raster(canvas, rotated, 1 + offset)->equivalent(*expected));
        }
        std::vector<Vec2d> duplicates;
        duplicates.reserve(points.size() + 2);
        duplicates.push_back(points.front());
        for (const auto point : points)
            duplicates.push_back(point);
        duplicates.push_back(points.front());
        CHECK(raster(canvas, duplicates)->equivalent(*expected));
    }
    const auto eight = raster(canvas, fixtures[1]);
    CHECK(eight->coverageAtDocumentPixel(3, 1) == 255);
    CHECK(eight->coverageAtDocumentPixel(3, 6) == 255);
    CHECK(eight->coverageAtDocumentPixel(1, 3) == 0);
    const auto hole = raster(canvas, fixtures[3]);
    CHECK(hole->coverageAtDocumentPixel(2, 2) == 255);
    CHECK(hole->coverageAtDocumentPixel(3, 3) == 0);
}

void degeneraciesTinyPhasesAndHalfOpenEdges()
{
    const Extent2u canvas { 8, 8 };
    for (const std::vector<Vec2d>& points : std::vector<std::vector<Vec2d>> { { }, { { 2.5, 3.5 } },
             { { 2, 3 }, { 6, 3 } }, { { 1, 1 }, { 3, 3 }, { 5, 5 }, { 2, 2 } },
             { { 2, 2 }, { 2, 2 }, { 2, 2 } }, { { 1, 4 }, { 6, 4 }, { 2, 4 }, { 5, 4 } } }) {
        CHECK(raster(canvas, points)->bounds().empty());
    }
    for (int phase = 0; phase < 16; ++phase) {
        const double p = phase / 16.0;
        const std::vector<Vec2d> tiny { { 2 + p, 2 + p }, { 2.2 + p, 2 + p }, { 2 + p, 2.2 + p } };
        const auto mask = raster(canvas, tiny, 3);
        int sum = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                sum += mask->coverageAtDocumentPixel(x, y);
        CHECK(sum >= 4 && sum <= 6); // Triangle area .02, or 5.1 coverage-byte units.
    }
    // Horizontal AA phases are the worst case for midpoint-only Y coverage.
    // A thin strip must not collapse into coarse 16/64-level alpha banding.
    for (int phase = 0; phase < 32; ++phase) {
        for (const double height : { 1.0 / 32.0, 0.213 }) {
            const double top = 2.0 + phase / 32.0;
            const std::vector<Vec2d> strip { { 1.125, top }, { 6.875, top }, { 6.875, top + height },
                { 1.125, top + height } };
            const auto mask = raster(canvas, strip);
            for (int y = 0; y < 8; ++y) {
                const double overlap
                    = std::max(0.0, std::min(y + 1.0, top + height) - std::max(double(y), top));
                CHECK(std::abs(int(mask->coverageAtDocumentPixel(3, y)) - int(std::lround(overlap * 255.0)))
                    <= 1);
            }
        }
    }
    // Exact scanline midpoint vertices use the same half-open edge ownership
    // whichever direction the closed path is supplied in.
    const double middle = 3.0 + 0.5 / PolygonCoverageRasterizer::subrowsPerPixel;
    std::vector<Vec2d> aligned { { 1.25, middle }, { 4.5, 1.0 }, { 6.25, middle }, { 4.5, 6.0 } };
    const auto original = raster(canvas, aligned);
    std::reverse(aligned.begin(), aligned.end());
    CHECK(raster(canvas, aligned, 1)->equivalent(*original));
}

void deliveryZoomAndWorkBudgetParity()
{
    const Extent2u canvas { 64, 48 };
    const std::vector<Vec2d> corners { { 5.125, 6.25 }, { 51.125, 6.25 }, { 51.125, 39.25 },
        { 24.125, 21.25 }, { 5.125, 39.25 } };
    FreehandSelectionPath sparse, dense;
    for (const auto p : corners)
        sparse.append(p);
    for (std::size_t edge = 0; edge < corners.size(); ++edge) {
        const auto from = corners[edge], to = corners[(edge + 1) % corners.size()];
        for (int sample = 0; sample < 128; ++sample)
            dense.append(from + (to - from) * (double(sample) / 128));
    }
    const auto expected = raster(canvas, sparse.points());
    CHECK(raster(canvas, dense.points())->equivalent(*expected));
    for (const auto zoom : { 0.125, 0.5, 1.0, 1.5, 2.0, 8.0 }) {
        std::vector<Vec2d> mapped;
        const Vec2d viewportOffset { 32.25, -20.125 };
        for (const auto p : corners) {
            const auto viewport = p * zoom + viewportOffset;
            mapped.push_back((viewport - viewportOffset) * (1.0 / zoom));
        }
        CHECK(raster(canvas, mapped)->equivalent(*expected));
    }
    for (const std::size_t budget : { 1U, 257U, 4096U, 1000000U })
        CHECK(raster(canvas, corners, budget)->equivalent(*expected));

    PolygonCoverageRasterizer bounded({ 5120, 2880 }, corners);
    CHECK(bounded.region() == RectI({ 5, 6, 47, 34 }));
    while (!bounded.finished())
        bounded.step(4096);
    CHECK(bounded.stats().writtenPixels <= 47U * 34U);
    CHECK(bounded.memoryBytes() < 128 * 1024);
    CHECK(bounded.coverage().size() < 2048);
}

void unsafeWorkRejectedAndCroppedMaskOwnsPixels()
{
    const std::vector<Vec2d> huge { { 0, 0 }, { 9000, 0 }, { 9000, 9000 }, { 0, 9000 } };
    CHECK(throws([&] { PolygonCoverageRasterizer job({ 9000, 9000 }, huge); }));
    std::vector<Vec2d> excessiveEdges;
    for (int i = 0; i < 100; ++i) {
        excessiveEdges.push_back({ double(i), 0 });
        excessiveEdges.push_back({ double(i) + 0.5, 8000 });
    }
    CHECK(throws([&] { PolygonCoverageRasterizer job({ 8000, 8000 }, excessiveEdges); }));
    const std::vector<Vec2d> nonFinite { { 0, 0 }, { 1, 1 }, { NAN, 2 } };
    CHECK(throws([&] { PolygonCoverageRasterizer job({ 8, 8 }, nonFinite); }));
    std::array<std::uint8_t, 8> bytes { 0, 128, 99, 99, 255, 64, 99, 99 };
    const auto mask = SelectionMask::fromR8Region({ 1024, 1024 }, { 400, 500, 2, 2 }, bytes, 4);
    CHECK(mask->bounds() == RectI({ 400, 500, 2, 2 }));
    CHECK(mask->coverageAtDocumentPixel(401, 500) == 128);
    CHECK(mask->coverageAtDocumentPixel(400, 501) == 255);
    CHECK(mask->coverageAtDocumentPixel(399, 501) == 0);
    bytes.fill(0);
    CHECK(mask->coverageAtDocumentPixel(400, 501) == 255);
    CHECK(throws([&] { (void)SelectionMask::fromR8Region({ 8, 8 }, { 0, 0, 2, 2 }, bytes, 1); }));
    CHECK(throws([&] {
        (void)SelectionMask::fromR8Region(
            { 8, 8 }, { 0, 0, 2, 2 }, bytes, std::numeric_limits<std::size_t>::max());
    }));
    const std::array<std::uint8_t, 15> tiledBytes { 10, 20, 30, 99, 99, 40, 50, 60, 99, 99, 70, 80, 90, 99,
        99 };
    const auto tiled = SelectionMask::fromR8Region({ 256, 256 }, { 127, 127, 3, 3 }, tiledBytes, 5);
    CHECK(tiled->bounds() == RectI({ 127, 127, 3, 3 }));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            CHECK(tiled->coverageAtDocumentPixel(127 + x, 127 + y) == tiledBytes[std::size_t(y * 5 + x)]);
    CHECK(tiled->coverageAtDocumentPixel(126, 127) == 0);
    CHECK(tiled->coverageAtDocumentPixel(130, 129) == 0);
    const auto clipped = SelectionMask::fromR8Region({ 8, 8 }, { -1, -1, 3, 3 }, tiledBytes, 5);
    CHECK(clipped->bounds() == RectI({ 0, 0, 2, 2 }));
    CHECK(clipped->coverageAtDocumentPixel(0, 0) == 50);
    CHECK(clipped->coverageAtDocumentPixel(1, 1) == 90);
    CHECK(throws([&] {
        (void)SelectionMask::fromR8Region({ 8, 8 }, { std::numeric_limits<int>::max(), 0, 2, 2 }, bytes, 4);
    }));
}

void selectionCombinationHistoryAndCancelledProducer()
{
    const Extent2u extent { 32, 24 };
    Document document(CanvasSpec { .extent = extent });
    auto surface = std::make_shared<ContiguousRasterSurface>(extent, Rgba8 { 12, 34, 56, 200 });
    CHECK(document.insertLayer(0, Layer::raster("Unchanged pixels", surface)));
    const auto bytes = pixels(*surface);
    const auto revision = surface->revision();
    const auto base = SelectionMask::rectangle(extent, { 2, 2, 8, 8 });
    const std::vector<Vec2d> polygon { { 6.25, 4.25 }, { 19.75, 4.25 }, { 19.75, 18.75 }, { 6.25, 18.75 } };
    const auto incoming = raster(extent, polygon);
    History history;
    CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(base)));
    for (const auto operation : { SelectionOperation::Replace, SelectionOperation::Add,
             SelectionOperation::Subtract, SelectionOperation::Intersect }) {
        const auto combined = combineSelection(base, incoming, operation);
        CHECK(
            history.execute(document, std::make_unique<SetSelectionCommand>(combined, "Lasso selection")));
        CHECK(history.undoLabel() == "Lasso selection");
        CHECK(document.selection()->equivalent(*combined));
        CHECK(history.undo(document));
        CHECK(document.selection()->equivalent(*base));
        CHECK(history.redo(document));
        CHECK(document.selection()->equivalent(*combined));
        CHECK(history.undo(document));
    }
    CHECK(history.redoDepth() == 1);
    const auto selectionRevision = document.selectionRevision();
    {
        PolygonCoverageRasterizer cancelled(extent, polygon);
        cancelled.step(1);
        CHECK(!cancelled.finished());
        CHECK(document.selection() == base); // Producing preview/coverage never commits a selection.
    }
    CHECK(document.selectionRevision() == selectionRevision);
    CHECK(history.redoDepth() == 1);
    CHECK(!history.execute(document, std::make_unique<SetSelectionCommand>(base, "Lasso selection")));
    CHECK(history.redoDepth() == 1);
    CHECK(surface->revision() == revision);
    CHECK(pixels(*surface) == bytes);
}

NormalizedPointerSample sample(Vec2d point)
{
    return { .documentPosition = point,
        .timestampMicroseconds = 1000,
        .pressure = 1.0,
        .pointerType = PointerType::Mouse,
        .buttons = PointerButtonPrimary };
}

void transformedPaintEraseAndFillReuseMask()
{
    const Extent2u canvas { 64, 64 };
    const std::vector<Vec2d> polygon { { 23.25, 15.5 }, { 32.75, 15.5 }, { 32.75, 29.25 }, { 28.25, 29.25 },
        { 28.25, 21.75 }, { 23.25, 21.75 } };
    const auto selection = raster(canvas, polygon);
    const std::array<AffineTransform, 3> transforms { { { 1, 0, 20, 0, 1, 18 }, { 0, -2, 36, 1.5, 0, 18 },
        { -1.5, 0, 34, 0, 1.5, 18 } } };
    for (const auto mapping : transforms) {
        for (int operation = 0; operation < 3; ++operation) {
            Document document(CanvasSpec { .extent = canvas });
            const Rgba8 original { 20, 40, 60, 180 };
            auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 12, 9 }, original);
            auto layer = Layer::raster("Transformed lasso target", surface);
            const auto id = layer.id;
            layer.localToDocument = mapping;
            CHECK(document.insertLayer(0, std::move(layer)));
            CHECK(document.setSelection(selection));
            History history;
            if (operation == 2) {
                FillOperation fill(document, id, { .color = { 230, 30, 80, 255 } });
                while (fill.state() == FillState::Discovering || fill.state() == FillState::Applying)
                    fill.step(128);
                CHECK(fill.commit(history) == RasterEditCommitResult::Committed);
            } else {
                auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
                settings.sizePixels = 128;
                settings.hardness = settings.opacity = settings.flow = 1.0;
                settings.foreground = { 230, 30, 80, 255 };
                settings.pressureToSize = settings.pressureToFlow = false;
                BasicPixelBrushStroke stroke(document, id, settings,
                    operation == 0 ? BrushCompositeMode::Paint : BrushCompositeMode::Erase);
                CHECK(stroke.begin(sample({ 32, 32 })));
                CHECK(stroke.end(sample({ 32, 32 }), history) == RasterEditCommitResult::Committed);
            }
            bool changed = false;
            const auto editedSurface=std::get<RasterLayer>(document.layer(id)->payload).surface;
            const auto origin=document.layer(id)->rasterOrigin;
            for (int y = 0; y < 9; ++y) {
                for (int x = 0; x < 12; ++x) {
                    const auto doc = mapping.map({ x + 0.5, y + 0.5 });
                    const auto coverage = selection->coverageAtDocumentPixel(
                        int(std::floor(doc.x)), int(std::floor(doc.y)));
                    const auto actual = pixel(*editedSurface, x-int(origin.x), y-int(origin.y));
                    CHECK(coverage ? actual != original : actual == original);
                    changed = changed || actual != original;
                    if (operation == 1 && coverage)
                        CHECK(
                            std::abs(int(actual.alpha) - int(std::lround(180.0 * (1.0 - coverage / 255.0))))
                            <= 1);
                }
            }
            CHECK(changed);
            CHECK(history.undoDepth() == 1);
            CHECK(document.selection() == selection);
            CHECK(document.layer(id)->localToDocument == mapping);
            const auto edited = pixels(*editedSurface);
            CHECK(history.undo(document));
            for (int y = 0; y < 9; ++y)
                for (int x = 0; x < 12; ++x)
                    CHECK(pixel(*surface, x, y) == original);
            CHECK(history.redo(document));
            CHECK(pixels(*std::get<RasterLayer>(document.layer(id)->payload).surface) == edited);
        }
    }
}

void copyUsesPartialLassoCoverageWithoutDoubleOpacity()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec { .extent = { 32, 32 } });
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 1, 1 }, Rgba8 { 180, 90, 45, 128 });
    auto layer = Layer::raster("Scaled original", surface);
    const auto id = layer.id;
    layer.localToDocument = { -10, 0, 20, 0, 10, 10 };
    layer.opacity = 0.4F;
    CHECK(document->insertLayer(0, std::move(layer)));
    const std::vector<Vec2d> points { { 14, 14 }, { 14.5, 14 }, { 14.5, 15 }, { 14, 15 } };
    const auto mask = raster({ 32, 32 }, points);
    CHECK(mask->coverageAtDocumentPixel(14, 14) == 128);
    CHECK(document->setSelection(mask));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(id);
    CHECK(session.execute(std::make_unique<LayerViaCopyCommand>(id, id)));
    const auto resultId = session.activeLayer();
    CHECK(resultId && *resultId != id);
    const auto* result = session.document()->layer(*resultId);
    const auto copied = std::get<RasterLayer>(result->payload).surface;
    CHECK(copied->extent() == Extent2u({ 1, 1 }));
    CHECK(result->localToDocument.map({ 0, 0 }) == Vec2d({ 14, 14 }));
    CHECK(pixel(*copied, 0, 0) == Rgba8({ 180, 90, 45, 64 }));
    CHECK(result->opacity == 0.4F);
    CHECK(pixel(*surface, 0, 0) == Rgba8({ 180, 90, 45, 128 }));
    CHECK(session.document()->selection() == mask);
    CHECK(session.undo());
    CHECK(session.activeLayer() == id);
    CHECK(session.redo());
    CHECK(session.activeLayer() == resultId);
}

void optionalContactSheet()
{
    const auto* filename = std::getenv("IMAGEEDITOR_TEST_LASSO_SHEET");
    if (!filename || !*filename)
        return;
    // PGM is intentionally dependency-free. Rows: concave / crossing / clipped;
    // hole / subpixel diagonal / narrow strips. Each cell shows exact R8 alpha.
    const std::vector<std::vector<Vec2d>> fixtures { { { 1.125, 1.25 }, { 7.625, 1.25 }, { 7.625, 3.125 },
                                                         { 3.75, 3.125 }, { 3.75, 6.75 }, { 1.125, 6.75 } },
        { { 1, 1 }, { 7, 7 }, { 1, 7 }, { 7, 1 } }, { { -6, 6 }, { 5, -4 }, { 15, 5 }, { 4, 13 } },
        { { 1, 1 }, { 7, 1 }, { 7, 7 }, { 1, 7 }, { 1, 1 }, { 3, 3 }, { 5, 3 }, { 5, 5 }, { 3, 5 },
            { 3, 3 }, { 1, 1 } },
        { { 1.125, 1.625 }, { 7.625, 5.125 }, { 7.25, 6.25 }, { 1.25, 3.125 } },
        { { 1.125, 1.25 }, { 7.875, 1.25 }, { 7.875, 1.75 }, { 1.125, 1.75 }, { 1.125, 3.25 },
            { 7.875, 3.25 }, { 7.875, 3.5 }, { 1.125, 3.5 }, { 1.125, 5.25 }, { 7.875, 5.25 },
            { 7.875, 5.375 }, { 1.125, 5.375 } } };
    constexpr int width = 636, height = 400, scale = 20;
    std::vector<std::uint8_t> image(width * height, 22);
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto mask = raster({ 9, 8 }, fixtures[i]);
        const int originX = 16 + int(i % 3) * 212;
        const int originY = 24 + int(i / 3) * 200;
        for (int y = -1; y <= 8 * scale; ++y) {
            for (int x = -1; x <= 9 * scale; ++x) {
                int value = 110;
                if (x >= 0 && y >= 0 && x < 9 * scale && y < 8 * scale) {
                    const int backdrop = ((x / scale + y / scale) % 2) ? 44 : 56;
                    const double alpha = mask->coverageAtDocumentPixel(x / scale, y / scale) / 255.0;
                    value = int(std::lround(backdrop + (230 - backdrop) * alpha));
                }
                image[std::size_t(originY + y) * width + std::size_t(originX + x)] = std::uint8_t(value);
            }
        }
    }
    std::ofstream stream(filename, std::ios::binary);
    stream << "P5\n# Lasso: concave, figure-eight, clipped, hole, subpixel diagonal, thin strips\n"
           << width << ' ' << height << "\n255\n";
    stream.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    CHECK(stream.good());
    std::cout << "Lasso contact sheet: " << filename << '\n';
}
}

int main()
{
    capturePreservesGeometryAndHasExplicitLimits();
    rectangleAnalyticCoverageAndDocumentClipping();
    concaveCrossedAndWindingIndependentCoverage();
    degeneraciesTinyPhasesAndHalfOpenEdges();
    deliveryZoomAndWorkBudgetParity();
    unsafeWorkRejectedAndCroppedMaskOwnsPixels();
    selectionCombinationHistoryAndCancelledProducer();
    transformedPaintEraseAndFillReuseMask();
    copyUsesPartialLassoCoverageWithoutDoubleOpacity();
    optionalContactSheet();
    std::cout << "Lasso tests: " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
