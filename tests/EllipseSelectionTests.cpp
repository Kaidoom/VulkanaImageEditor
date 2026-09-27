#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/EllipseCoverageRasterizer.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <set>
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
#define CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

template <class Function>
bool throws(Function&& function)
{
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

SelectionState raster(Extent2u canvas, const EllipseGeometry& ellipse,
    std::size_t budget = 65536)
{
    EllipseCoverageRasterizer job(canvas, ellipse);
    std::size_t calls = 0;
    while (!job.finished()) {
        job.step(budget);
        if (++calls > 2000000) {
            CHECK(false);
            break;
        }
    }
    CHECK(job.region() == ellipse.region(canvas));
    CHECK(job.coverage().size() == job.stride() * std::size_t(job.region().height));
    return SelectionMask::fromR8Region(canvas, job.region(), job.coverage(),
        job.stride());
}

double area(const SelectionState& mask)
{
    std::uint64_t sum = 0;
    const auto bounds = mask->bounds();
    for (int y = bounds.y; y < bounds.bottom(); ++y)
        for (int x = bounds.x; x < bounds.right(); ++x)
            sum += mask->coverageAtDocumentPixel(x, y);
    return double(sum) / 255.0;
}

// Independent numerical oracle: regular two-dimensional integration of the
// implicit conic, not production's analytic cell-area integration.
int oracle(const EllipseGeometry& ellipse, int x, int y)
{
    constexpr int samples = 256;
    const auto center = ellipse.center();
    const auto radii = (ellipse.maximum() - ellipse.minimum()) * 0.5;
    if (radii.x <= 0 || radii.y <= 0)
        return 0;
    unsigned covered = 0;
    for (int sy = 0; sy < samples; ++sy) {
        const auto dy = (y + (sy + 0.5) / samples - center.y) / radii.y;
        for (int sx = 0; sx < samples; ++sx) {
            const auto dx = (x + (sx + 0.5) / samples - center.x) / radii.x;
            covered += dx * dx + dy * dy <= 1.0;
        }
    }
    return int(std::lround(double(covered) * 255.0 / (samples * samples)));
}

using UnitEdge = std::array<int, 4>;
std::set<UnitEdge> unitEdges(const std::vector<SelectionEdge>& edges,
    Extent2u extent)
{
    std::set<UnitEdge> result;
    for (const auto& edge : edges) {
        CHECK(std::isfinite(edge.from.x) && std::isfinite(edge.from.y));
        CHECK(std::isfinite(edge.to.x) && std::isfinite(edge.to.y));
        CHECK(edge.from.x == std::round(edge.from.x) && edge.from.y == std::round(edge.from.y));
        CHECK(edge.to.x == std::round(edge.to.x) && edge.to.y == std::round(edge.to.y));
        const auto x0 = int(std::min(edge.from.x, edge.to.x));
        const auto y0 = int(std::min(edge.from.y, edge.to.y));
        const auto x1 = int(std::max(edge.from.x, edge.to.x));
        const auto y1 = int(std::max(edge.from.y, edge.to.y));
        CHECK(x0 >= 0 && y0 >= 0 && x1 <= int(extent.width) && y1 <= int(extent.height));
        CHECK((x0 == x1) != (y0 == y1));
        if (x0 == x1)
            for (int y = y0; y < y1; ++y)
                CHECK(result.insert({ x0, y, x0, y + 1 }).second);
        if (y0 == y1)
            for (int x = x0; x < x1; ++x)
                CHECK(result.insert({ x, y0, x + 1, y0 }).second);
    }
    return result;
}

void dragBoundsDirectionsAndCircleConstraint()
{
    const Vec2d low { 2.125, 3.375 }, high { 15.625, 10.875 };
    const auto expected = raster({ 20, 20 }, EllipseGeometry(low, high));
    for (const auto start :
        { low, Vec2d { low.x, high.y }, Vec2d { high.x, low.y }, high }) {
        const Vec2d end { start.x == low.x ? high.x : low.x,
            start.y == low.y ? high.y : low.y };
        const EllipseGeometry geometry(start, end);
        CHECK(geometry.minimum() == low);
        CHECK(geometry.maximum() == high);
        CHECK(geometry.center() == (low + high) * 0.5);
        CHECK(raster({ 20, 20 }, geometry)->equivalent(*expected));
    }
    for (const double sx : { -1.0, 1.0 }) {
        for (const double sy : { -1.0, 1.0 }) {
            const Vec2d start { 20.25, 20.75 };
            const auto end = start + Vec2d { 7.5 * sx, 12.75 * sy };
            const EllipseGeometry circle(start, end, true);
            const auto size = circle.maximum() - circle.minimum();
            CHECK(size == Vec2d({ 12.75, 12.75 }));
            const auto constrained = start + Vec2d { 12.75 * sx, 12.75 * sy };
            CHECK(circle.minimum() == Vec2d({ std::min(start.x, constrained.x), std::min(start.y, constrained.y) }));
            CHECK(circle.maximum() == Vec2d({ std::max(start.x, constrained.x), std::max(start.y, constrained.y) }));
            CHECK(raster({ 48, 48 }, circle)
                    ->equivalent(
                        *raster({ 48, 48 }, EllipseGeometry(start, constrained))));
        }
    }
    // Constraint does not quantize the original document-space position.
    const EllipseGeometry horizontal({ 3.125, 4.375 }, { 7.875, 4.375 }, true);
    CHECK(!horizontal.empty());
    CHECK(horizontal.minimum() == Vec2d({ 3.125, 4.375 }));
    CHECK(horizontal.maximum() == Vec2d({ 7.875, 9.125 }));
}

void exactAreaAndSymmetricCoverage()
{
    for (const double diameter :
        { 0.125, 0.5, 1.0, 2.0, 3.0, 7.0, 12.5, 27.75, 64.0, 128.0 }) {
        const double pad = diameter <= 1 ? 2.25 : 3.0;
        const EllipseGeometry circle({ pad, pad }, { pad + diameter, pad + diameter });
        const auto canvasSide = std::uint32_t(std::ceil(pad + diameter + 3));
        const auto mask = raster({ canvasSide, canvasSide }, circle);
        unsigned partial = 0;
        const auto region = circle.region({ canvasSide, canvasSide });
        for (int y = region.y; y < region.bottom(); ++y)
            for (int x = region.x; x < region.right(); ++x) {
                const auto value = mask->coverageAtDocumentPixel(x, y);
                partial += value > 0 && value < 255;
                CHECK(value == mask->coverageAtDocumentPixel(y, x));
            }
        const auto analyticArea = std::numbers::pi * diameter * diameter * 0.25;
        // Each quantized boundary cell contributes at most half an R8 unit.
        // Include cells rounded down to 0/up to 255 using a perimeter bound.
        const double boundaryBound = std::max(4.0, 4.0 * std::ceil(diameter) + 4.0);
        CHECK(std::abs(area(mask) - analyticArea) <= boundaryBound / 510.0 + 1.0e-7);
        CHECK(partial > 0);
    }
    const Extent2u canvas { 36, 36 };
    for (const auto radii :
        { Vec2d { 14, 9 }, Vec2d { 0.125, 12 }, Vec2d { 12, 0.25 }, Vec2d { 3.25, 9.75 } }) {
        const EllipseGeometry ellipse(Vec2d { 18, 18 } - radii, Vec2d { 18, 18 } + radii);
        const auto mask = raster(canvas, ellipse);
        const auto transpose = raster(canvas, EllipseGeometry({ 18 - radii.y, 18 - radii.x }, { 18 + radii.y, 18 + radii.x }));
        for (int y = 0; y < 36; ++y)
            for (int x = 0; x < 36; ++x) {
                const auto value = mask->coverageAtDocumentPixel(x, y);
                CHECK(value == mask->coverageAtDocumentPixel(35 - x, y));
                CHECK(value == mask->coverageAtDocumentPixel(x, 35 - y));
                CHECK(value == transpose->coverageAtDocumentPixel(y, x));
            }
    }
}

void independentCoverageOracleTinyNarrowAndSubpixel()
{
    const Extent2u extent { 9, 9 };
    const std::array<EllipseGeometry, 7> fixtures {
        EllipseGeometry({ 1.125, 1.25 }, { 7.625, 7.75 }),
        EllipseGeometry({ 1.375, 3.375 }, { 7.75, 3.5 }),
        EllipseGeometry({ 4.125, 0.25 }, { 4.375, 8.75 }),
        EllipseGeometry({ 2.8, 3.9 }, { 3.2, 4.1 }),
        EllipseGeometry({ -3.125, -1.25 }, { 6.375, 6.75 }),
        EllipseGeometry({ 2.0625, 2.03125 }, { 2.125, 2.09375 }),
        EllipseGeometry({ 0.01, 7.0001 }, { 8.99, 7.0099 })
    };
    for (const auto& ellipse : fixtures) {
        const auto mask = raster(extent, ellipse);
        bool partial = false;
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 9; ++x) {
                const auto value = mask->coverageAtDocumentPixel(x, y);
                CHECK(value == ellipse.coverageAt(x, y));
                CHECK(std::abs(int(value) - oracle(ellipse, x, y)) <= 1);
                partial = partial || (value > 0 && value < 255);
            }
        CHECK(partial);
    }
    // Vary phase at much finer precision than ordinary pixel coordinates.
    // Narrow subpixel ellipses must retain coverage rather than disappear at
    // an unlucky sample-row alignment.
    for (int phase = 0; phase < 32; ++phase) {
        const auto y = 2.0 + phase / 32.0;
        const EllipseGeometry thin({ 1.125, y }, { 7.875, y + 0.0625 });
        const auto mask = raster(extent, thin);
        CHECK(!mask->bounds().empty());
        CHECK(std::abs(area(mask) - std::numbers::pi * 6.75 * 0.0625 * 0.25) < 0.015);
    }
}

void unclippedGeometryAndFarOffCanvasCancellationSafety()
{
    const Extent2u extent { 12, 10 };
    for (const auto& ellipse :
        { EllipseGeometry({ -9, -4 }, { 9, 14 }),
            EllipseGeometry({ -10.125, 2.375 }, { 17.875, 8.125 }),
            EllipseGeometry({ 5.5, -500 }, { 8.5, 500 }) }) {
        const auto mask = raster(extent, ellipse);
        for (int y = 0; y < int(extent.height); ++y)
            for (int x = 0; x < int(extent.width); ++x)
                CHECK(mask->coverageAtDocumentPixel(x, y) == ellipse.coverageAt(x, y));
        CHECK(unitEdges(ellipse.previewEdges(extent), extent) == unitEdges(mask->boundaryEdges(), extent));
    }
    const EllipseGeometry original({ -9, -4 }, { 9, 14 });
    const EllipseGeometry wronglyClamped({ 0, 0 }, { 9, 10 });
    CHECK(!raster(extent, original)->equivalent(*raster(extent, wronglyClamped)));
    CHECK(raster(extent, original)->coverageAtDocumentPixel(0, 4) == 255);
    CHECK(raster(extent, wronglyClamped)->coverageAtDocumentPixel(0, 0) == 0);
    const auto contained = raster(extent, EllipseGeometry({ -1000000, -1000000 }, { 1000000, 1000000 }));
    CHECK(contained->equivalent(*SelectionMask::filled(extent, 255)));
    CHECK(raster(extent, EllipseGeometry({ -1000, -1000 }, { -900, -800 }))
            ->bounds()
            .empty());
    // The origin lies on a huge circle's leftmost tip. Analytic subtraction
    // must remain stable far from the center (no area cancellation blow-up).
    const EllipseGeometry distant({ 0, -100000000 }, { 200000000, 100000000 });
    const auto distantMask = raster(extent, distant);
    for (int y = 0; y < 10; ++y) {
        CHECK(distantMask->coverageAtDocumentPixel(0, y) >= 254);
        CHECK(distantMask->coverageAtDocumentPixel(1, y) == 255);
    }
}

void previewMatchesCommittedMaskAndChunkDelivery()
{
    const Extent2u extent { 72, 56 };
    const std::array<EllipseGeometry, 8> fixtures {
        EllipseGeometry({ 5.125, 6.25 }, { 65.375, 49.875 }),
        EllipseGeometry({ 3.25, 20.125 }, { 60.875, 20.875 }),
        EllipseGeometry({ 35.125, 1.25 }, { 35.875, 53.625 }),
        EllipseGeometry({ -45.5, -15.25 }, { 64.25, 46.5 }),
        EllipseGeometry({ 25.125, 26.5 }, { 25.875, 27.25 }),
        EllipseGeometry({ 25, 26 }, { 25, 27 }),
        EllipseGeometry({ -1000, -1000 }, { 1000, 1000 }),
        EllipseGeometry({ 3.03125, 5.0625 }, { 57.46875, 46.4375 })
    };
    for (const auto& geometry : fixtures) {
        const auto expected = raster(extent, geometry);
        const auto preview = unitEdges(geometry.previewEdges(extent), extent);
        CHECK(preview == unitEdges(expected->boundaryEdges(), extent));
        for (const std::size_t budget : { 1U, 7U, 257U, 4096U, 1000000U })
            CHECK(raster(extent, geometry, budget)->equivalent(*expected));
        for (const auto zoom : { 0.125, 0.5, 1.0, 1.5, 2.0, 8.0 }) {
            const Vec2d viewportOffset { 1024.25, -720.125 };
            const auto viewA = geometry.minimum() * zoom + viewportOffset;
            const auto viewB = geometry.maximum() * zoom + viewportOffset;
            const EllipseGeometry recovered((viewA - viewportOffset) * (1.0 / zoom),
                (viewB - viewportOffset) * (1.0 / zoom));
            CHECK(raster(extent, recovered)->equivalent(*expected));
            CHECK(unitEdges(recovered.previewEdges(extent), extent) == preview);
        }
    }
    // This crosses 128px mask tiles; preview merging need not share traversal
    // order, but the final selected pixel-edge set must be identical.
    const Extent2u tiledExtent { 300, 270 };
    const EllipseGeometry tiled({ 19.25, 11.125 }, { 288.875, 253.375 });
    CHECK(unitEdges(tiled.previewEdges(tiledExtent), tiledExtent) == unitEdges(raster(tiledExtent, tiled)->boundaryEdges(), tiledExtent));
}

void fixedSeedClippedAndEccentricPreviewParity()
{
    const Extent2u extent { 37, 29 };
    std::uint32_t seed = 0x5e1ec710U;
    const auto next = [&] {
        seed = seed * 1664525U + 1013904223U;
        return seed;
    };
    for (unsigned i = 0; i < 160; ++i) {
        const Vec2d start { double(int(next() % 1536U) - 512) / 16.0,
            double(int(next() % 1280U) - 512) / 16.0 };
        const double dx = (1.0 + next() % 1024U) / (i % 3 == 0 ? 2048.0 : 16.0);
        const double dy = (1.0 + next() % 1024U) / (i % 3 == 1 ? 2048.0 : 16.0);
        const EllipseGeometry geometry(start, start + Vec2d { dx, dy });
        const auto mask = raster(extent, geometry, 31);
        CHECK(unitEdges(geometry.previewEdges(extent), extent) == unitEdges(mask->boundaryEdges(), extent));
        const auto reverse = raster(extent, EllipseGeometry(start + Vec2d { dx, dy }, start), 113);
        CHECK(reverse->equivalent(*mask));
        for (int y = 0; y < int(extent.height); ++y)
            for (int x = 0; x < int(extent.width); ++x)
                CHECK(mask->coverageAtDocumentPixel(x, y) == geometry.coverageAt(x, y));
    }
}

void degeneraciesLimitsAndLocalizedWork()
{
    const Extent2u extent { 8, 8 };
    for (const auto& geometry :
        { EllipseGeometry({ 2, 3 }, { 2, 3 }), EllipseGeometry({ 2, 3 }, { 6, 3 }),
            EllipseGeometry({ 2, 3 }, { 2, 7 }),
            EllipseGeometry({ 2.125, 3.375 }, { 2.125, 3.375 }, true) }) {
        CHECK(geometry.empty());
        CHECK(raster(extent, geometry)->bounds().empty());
        CHECK(geometry.previewEdges(extent).empty());
    }
    for (const auto coordinate : { std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
             1000000001.0, -1000000001.0 }) {
        CHECK(throws([&] { (void)EllipseGeometry({ coordinate, 1 }, { 3, 4 }); }));
        CHECK(throws([&] { (void)EllipseGeometry({ 1, 2 }, { 3, coordinate }); }));
    }
    CHECK(throws([&] {
        EllipseCoverageRasterizer job({ 32769, 8 }, EllipseGeometry({ 1, 1 }, { 4, 4 }));
    }));
    CHECK(throws([&] {
        EllipseCoverageRasterizer job({ 9000, 9000 },
            EllipseGeometry({ 0, 0 }, { 9000, 9000 }));
    }));
    EllipseCoverageRasterizer local(
        { 5120, 2880 }, EllipseGeometry({ 200.125, 300.25 }, { 240.625, 320.875 }));
    CHECK(local.region() == RectI({ 200, 300, 41, 21 }));
    while (!local.finished())
        local.step(13);
    CHECK(local.stats().writtenPixels <= 41U * 21U);
    CHECK(local.stats().boundaryPixels < 41U * 21U);
    CHECK(local.memoryBytes() < 64U * 1024U);
    CHECK(local.coverage().size() == 41U * 21U);
    CHECK(local.finished());
    const auto completed = std::vector<std::uint8_t>(local.coverage().begin(),
        local.coverage().end());
    local.step(1);
    CHECK(std::equal(completed.begin(), completed.end(), local.coverage().begin(),
        local.coverage().end()));
}

void combinationsHistoryCancellationAndAdjustments()
{
    const Extent2u extent { 64, 56 };
    Document document(CanvasSpec { .extent = extent });
    History history;
    const auto base = SelectionMask::rectangle(extent, { 12, 10, 26, 27 });
    const EllipseGeometry geometry({ 5.25, 6.5 }, { 49.75, 45.25 });
    const auto incoming = raster(extent, geometry);
    CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(base)));
    for (const auto operation :
        { SelectionOperation::Replace, SelectionOperation::Add,
            SelectionOperation::Subtract, SelectionOperation::Intersect }) {
        const auto combined = combineSelection(base, incoming, operation);
        CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(combined, "Ellipse selection")));
        CHECK(document.selection()->equivalent(*combined));
        CHECK(history.undoLabel() == "Ellipse selection");
        CHECK(history.undo(document));
        CHECK(document.selection() == base);
        CHECK(history.redo(document));
        CHECK(document.selection()->equivalent(*combined));
        CHECK(history.undo(document));
    }
    const auto revision = document.selectionRevision();
    const auto redoDepth = history.redoDepth();
    {
        EllipseCoverageRasterizer cancelled(extent, geometry);
        cancelled.step(1);
        CHECK(!cancelled.finished());
        (void)geometry.previewEdges(extent);
        CHECK(document.selection() == base);
    }
    CHECK(document.selectionRevision() == revision);
    CHECK(history.redoDepth() == redoDepth);
    CHECK(!history.execute(document, std::make_unique<SetSelectionCommand>(base, "Ellipse selection")));
    CHECK(history.redoDepth() == redoDepth);
    const auto hole = incoming->combined(*raster(extent, EllipseGeometry({ 19, 18 }, { 33, 32 })),
        SelectionOperation::Subtract);
    CHECK(!selectionMoveHit(hole, { 26.5, 25.5 }, SelectionOperation::Replace,
        false, false));
    CHECK(selectionMoveHit(hole, { 15.5, 25.5 }, SelectionOperation::Replace, false,
        false));
    CHECK(!selectionMoveHit(hole, { 15.5, 25.5 }, SelectionOperation::Replace, true,
        false));
    const auto original = incoming;
    CHECK(original->translated(60, 0)->bounds().empty());
    CHECK(original->translated(0, 0)->equivalent(*incoming));
    CHECK(original->translated(1, -2)->coverageAtDocumentPixel(16, 18) == original->coverageAtDocumentPixel(15, 20));
    const auto grown = original->adjusted(2, 3);
    CHECK(grown->bounds().width == original->bounds().width + 4);
    CHECK(grown->bounds().height == original->bounds().height + 6);
    const auto shrunken = original->adjusted(-2, -3);
    CHECK(shrunken->bounds().width < original->bounds().width);
    CHECK(shrunken->bounds().height < original->bounds().height);
    CHECK(original->rotated(0, geometry.center())->equivalent(*original));
    CHECK(!original->rotated(37, geometry.center())->bounds().empty());
    const auto resized = original->resized({ 32, 28 });
    for (int y = 0; y < 28; ++y)
        for (int x = 0; x < 32; ++x)
            CHECK(resized->coverageAtDocumentPixel(x, y) == original->coverageAtDocumentPixel(x, y));
}

std::vector<std::byte> pixels(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4U);
    surface.copyRgba8({ 0, 0, int(extent.width), int(extent.height) }, result,
        std::size_t(extent.width) * 4U);
    return result;
}
Rgba8 pixel(const RasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> bytes { };
    surface.copyRgba8({ x, y, 1, 1 }, bytes, 4);
    return { std::to_integer<std::uint8_t>(bytes[0]),
        std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]),
        std::to_integer<std::uint8_t>(bytes[3]) };
}

void transformedBrushEraseFillAndCopyShareCoverage()
{
    const Extent2u canvas { 64, 64 };
    const auto selection = raster(canvas, EllipseGeometry({ 22.25, 15.5 }, { 35.75, 30.25 }));
    const std::array<AffineTransform, 3> transforms { { { 1, 0, 20, 0, 1, 18 },
        { 0, -2, 36, 1.5, 0, 18 },
        { -1.5, 0, 34, 0, 1.5, 18 } } };
    for (const auto mapping : transforms) {
        for (int operation = 0; operation < 3; ++operation) {
            Document document(CanvasSpec { .extent = canvas });
            const Rgba8 original { 20, 40, 60, 180 };
            auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 12, 9 }, original);
            auto layer = Layer::raster("Transformed ellipse target", surface);
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
                    operation == 0
                        ? BrushCompositeMode::Paint
                        : BrushCompositeMode::Erase);
                const NormalizedPointerSample sample { .documentPosition = { 32, 32 },
                    .timestampMicroseconds = 1000,
                    .pressure = 1.0,
                    .pointerType = PointerType::Mouse,
                    .buttons = PointerButtonPrimary };
                CHECK(stroke.begin(sample));
                CHECK(stroke.end(sample, history) == RasterEditCommitResult::Committed);
            }
            unsigned changed = 0, partial = 0;
            for (int y = 0; y < 9; ++y) {
                for (int x = 0; x < 12; ++x) {
                    const auto doc = mapping.map({ x + 0.5, y + 0.5 });
                    const auto coverage = selection->coverageAtDocumentPixel(
                        int(std::floor(doc.x)), int(std::floor(doc.y)));
                    const auto actual = pixel(*surface, x, y);
                    CHECK(coverage ? actual != original : actual == original);
                    changed += actual != original;
                    partial += coverage > 0 && coverage < 255;
                    if (operation == 1 && coverage)
                        CHECK(std::abs(
                                  int(actual.alpha) - int(std::lround(180.0 * (1.0 - coverage / 255.0))))
                            <= 1);
                }
            }
            CHECK(changed > 0 && partial > 0);
            CHECK(history.undoDepth() == 1);
            CHECK(document.selection() == selection);
            CHECK(document.layer(id)->localToDocument == mapping);
            const auto edited = pixels(*surface);
            CHECK(history.undo(document));
            for (int y = 0; y < 9; ++y)
                for (int x = 0; x < 12; ++x)
                    CHECK(pixel(*surface, x, y) == original);
            CHECK(history.redo(document));
            CHECK(pixels(*surface) == edited);
        }
    }
    // A one-pixel ellipse makes partial extraction and opacity accounting
    // unambiguous, including a flipped/scaled source layer.
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec { .extent = { 32, 32 } });
    auto surface = std::make_shared<ContiguousRasterSurface>(
        Extent2u { 1, 1 }, Rgba8 { 180, 90, 45, 128 });
    auto layer = Layer::raster("Scaled original", surface);
    const auto id = layer.id;
    layer.localToDocument = { -10, 0, 20, 0, 10, 10 };
    layer.opacity = 0.4F;
    CHECK(document->insertLayer(0, std::move(layer)));
    const auto copyMask = raster({ 32, 32 }, EllipseGeometry({ 14, 14 }, { 15, 15 }));
    CHECK(copyMask->coverageAtDocumentPixel(14, 14) == 200);
    CHECK(document->setSelection(copyMask));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(id);
    CHECK(session.execute(std::make_unique<LayerViaCopyCommand>(id, id)));
    const auto resultId = session.activeLayer();
    CHECK(resultId && *resultId != id);
    const auto* result = session.document()->layer(*resultId);
    const auto copied = std::get<RasterLayer>(result->payload).surface;
    CHECK(copied->extent() == Extent2u({ 1, 1 }));
    CHECK(result->localToDocument.map({ 0, 0 }) == Vec2d({ 14, 14 }));
    CHECK(pixel(*copied, 0, 0) == Rgba8({ 180, 90, 45, 100 }));
    CHECK(result->opacity == 0.4F);
    CHECK(pixel(*surface, 0, 0) == Rgba8({ 180, 90, 45, 128 }));
    CHECK(session.document()->selection() == copyMask);
    CHECK(session.undo());
    CHECK(session.activeLayer() == id);
    CHECK(session.redo());
    CHECK(session.activeLayer() == resultId);
}
} // namespace

int main()
{
    dragBoundsDirectionsAndCircleConstraint();
    exactAreaAndSymmetricCoverage();
    independentCoverageOracleTinyNarrowAndSubpixel();
    unclippedGeometryAndFarOffCanvasCancellationSafety();
    previewMatchesCommittedMaskAndChunkDelivery();
    fixedSeedClippedAndEccentricPreviewParity();
    degeneraciesLimitsAndLocalizedWork();
    combinationsHistoryCancellationAndAdjustments();
    transformedBrushEraseFillAndCopyShareCoverage();
    std::cout << "Ellipse selection tests: " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
