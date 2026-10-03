#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/core/SmartSelectionEvidence.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
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

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct Fixture {
    Document document;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId id {0};
    History history;

    explicit Fixture(Extent2u extent, Rgba8 color = {0, 0, 0, 0})
        : document(CanvasSpec {.extent = extent})
        , surface(std::make_shared<ContiguousRasterSurface>(extent, color))
    {
        auto layer = Layer::raster("Source", surface);
        id = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
};

Rgba8 pixel(const RasterSurface& surface, std::int32_t x, std::int32_t y)
{
    std::array<std::byte, 4> bytes {};
    surface.copyRgba8({x, y, 1, 1}, bytes, 4);
    return {std::to_integer<std::uint8_t>(bytes[0]),
        std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]),
        std::to_integer<std::uint8_t>(bytes[3])};
}

std::vector<std::byte> pixels(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4U);
    surface.copyRgba8({0, 0, static_cast<std::int32_t>(extent.width),
                         static_cast<std::int32_t>(extent.height)},
        result, std::size_t(extent.width) * 4U);
    return result;
}

DirtySet writeSolid(RasterEditTransaction& edit, RectI region, Rgba8 color)
{
    std::vector<std::byte> bytes(std::size_t(region.width) * std::size_t(region.height) * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
        bytes[offset] = std::byte {color.red};
        bytes[offset + 1] = std::byte {color.green};
        bytes[offset + 2] = std::byte {color.blue};
        bytes[offset + 3] = std::byte {color.alpha};
    }
    return edit.writeRgba8(region, bytes, std::size_t(region.width) * 4U);
}

NormalizedPointerSample sample(Vec2d point, std::uint64_t timestamp = 0)
{
    return {.documentPosition = point,
        .timestampMicroseconds = timestamp,
        .pressure = 1.0,
        .pointerType = PointerType::Mouse,
        .buttons = PointerButtonPrimary};
}

BrushSettings brush(double size = 12)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = size;
    settings.hardness = 1.0;
    settings.opacity = 1.0;
    settings.flow = 0.3;
    settings.spacingPercent = 10.0;
    settings.foreground = {220, 40, 80, 255};
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    settings.smoothing = BrushSmoothingMode::None;
    settings.tip.rotationMode = BrushTipRotationMode::Fixed;
    return settings;
}

void maskCoverageBoundsAndImmutableSnapshots()
{
    const Extent2u extent {9, 7};
    const auto empty = SelectionMask::filled(extent, 0);
    CHECK(empty->extent() == extent);
    CHECK(empty->bounds().empty());
    CHECK(empty->coverageAtDocumentPixel(0, 0) == 0);
    CHECK(empty->coverageAtDocumentPixel(-1, 0) == 0);
    CHECK(empty->coverageAtDocumentPixel(9, 0) == 0);
    const auto clipped = SelectionMask::rectangle(extent, {-2, 3, 6, 8}, 73);
    CHECK(clipped->bounds() == RectI({0, 3, 4, 4}));
    CHECK(clipped->coverageAtDocumentPixel(0, 3) == 73);
    CHECK(clipped->coverageAtDocumentPixel(4, 3) == 0);
    CHECK(clipped->coverageAtDocumentPixel(3, 6) == 73);

    // Input rows may be padded, and the immutable mask must not retain a view
    // into the caller's mutable memory.
    std::array<std::uint8_t, 18> bytes {
        0, 0, 0, 0, 99, 99,
        0, 1, 127, 0, 99, 99,
        0, 255, 128, 0, 99, 99};
    const auto coverage = SelectionMask::fromR8({4, 3}, bytes, 6);
    CHECK(coverage->bounds() == RectI({1, 1, 2, 2}));
    CHECK(coverage->coverageAtDocumentPixel(1, 1) == 1);
    CHECK(coverage->coverageAtDocumentPixel(2, 1) == 127);
    CHECK(coverage->coverageAtDocumentPixel(1, 2) == 255);
    CHECK(coverage->coverageAtDocumentPixel(2, 2) == 128);
    bytes.fill(255);
    CHECK(coverage->coverageAtDocumentPixel(0, 0) == 0);
    CHECK(coverage->coverageAtDocumentPixel(1, 1) == 1);
    CHECK(coverage->revision() > 0);
    CHECK(coverage->memoryCost() > 0);
    CHECK(coverage->equivalent(*coverage));
    CHECK(!empty->equivalent(*clipped));
}

void overflowingMaskStrideIsRejectedBeforeReading()
{
    const std::array<std::uint8_t, 4> bytes {};
    bool rejected = false;
    try {
        (void)SelectionMask::fromR8({2, 2}, bytes,
            std::numeric_limits<std::size_t>::max());
    } catch (const std::invalid_argument&) {
        rejected = true;
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    CHECK(rejected);
    // More than two rows exercises the product, not just the final addition.
    rejected = false;
    try {
        (void)SelectionMask::fromR8({2, 3}, bytes,
            std::numeric_limits<std::size_t>::max() / 2 + 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

void partialCoverageAlgebraAndDisconnectedRegions()
{
    const std::array<std::uint8_t, 4> a {0, 64, 192, 255};
    const std::array<std::uint8_t, 4> b {128, 192, 64, 255};
    const auto left = SelectionMask::fromR8({4, 1}, a, 4);
    const auto right = SelectionMask::fromR8({4, 1}, b, 4);
    const auto add = left->combined(*right, SelectionOperation::Add);
    const auto subtract = left->combined(*right, SelectionOperation::Subtract);
    const auto intersect = left->combined(*right, SelectionOperation::Intersect);
    const auto replace = left->combined(*right, SelectionOperation::Replace);
    const std::array<std::uint8_t, 4> expectedAdd {128, 192, 192, 255};
    const std::array<std::uint8_t, 4> expectedSubtract {0, 0, 128, 0};
    const std::array<std::uint8_t, 4> expectedIntersect {0, 64, 64, 255};
    for (std::int32_t x = 0; x < 4; ++x) {
        CHECK(add->coverageAtDocumentPixel(x, 0) == expectedAdd[std::size_t(x)]);
        CHECK(subtract->coverageAtDocumentPixel(x, 0) == expectedSubtract[std::size_t(x)]);
        CHECK(intersect->coverageAtDocumentPixel(x, 0) == expectedIntersect[std::size_t(x)]);
    }
    CHECK(replace->equivalent(*right));
    CHECK(left->coverageAtDocumentPixel(1, 0) == 64);
    CHECK(left->inverted()->inverted()->equivalent(*left));

    const Extent2u extent {16, 16};
    const auto all = SelectionMask::filled(extent, 255);
    const auto hole = SelectionMask::rectangle(extent, {4, 4, 8, 8});
    const auto ring = all->combined(*hole, SelectionOperation::Subtract);
    CHECK(ring->bounds() == RectI({0, 0, 16, 16}));
    CHECK(ring->coverageAtDocumentPixel(0, 0) == 255);
    CHECK(ring->coverageAtDocumentPixel(7, 7) == 0);
    const auto first = SelectionMask::rectangle(extent, {1, 2, 2, 3});
    const auto second = SelectionMask::rectangle(extent, {11, 12, 3, 2});
    const auto islands = first->combined(*second, SelectionOperation::Add);
    CHECK(islands->bounds() == RectI({1, 2, 13, 12}));
    CHECK(islands->coverageAtDocumentPixel(2, 3) == 255);
    CHECK(islands->coverageAtDocumentPixel(12, 12) == 255);
    CHECK(islands->coverageAtDocumentPixel(7, 7) == 0);
    CHECK(islands->combined(*first, SelectionOperation::Subtract)->equivalent(*second));
}

void inactiveAndActiveEmptyAreDistinct()
{
    Fixture fixture({8, 8});
    const auto empty = SelectionMask::filled({8, 8}, 0);
    const auto rect = SelectionMask::rectangle({8, 8}, {2, 2, 3, 3});
    CHECK(!fixture.document.selection());
    const auto revision = fixture.document.selectionRevision();
    CHECK(fixture.document.setSelection(empty));
    CHECK(fixture.document.selection());
    CHECK(fixture.document.selection()->bounds().empty());
    CHECK(fixture.document.selectionRevision() > revision);
    const auto emptyRevision = fixture.document.selectionRevision();
    CHECK(!fixture.document.setSelection(SelectionMask::filled({8, 8}, 0)));
    CHECK(fixture.document.selectionRevision() == emptyRevision);
    CHECK(fixture.document.setSelection({}));
    CHECK(!fixture.document.selection());
    CHECK(fixture.document.selectionRevision() > emptyRevision);

    CHECK(combineSelection({}, rect, SelectionOperation::Replace)->equivalent(*rect));
    CHECK(combineSelection({}, rect, SelectionOperation::Add)->equivalent(*rect));
    CHECK(combineSelection({}, rect, SelectionOperation::Intersect)->equivalent(*rect));
    CHECK(combineSelection({}, rect, SelectionOperation::Subtract)->equivalent(*rect->inverted()));
    CHECK(combineSelection(empty, rect, SelectionOperation::Intersect)->bounds().empty());
    CHECK(combineSelection(empty, rect, SelectionOperation::Subtract)->bounds().empty());
}

using UnitEdges = std::set<std::array<int, 4>>;

UnitEdges expandEdges(std::span<const SelectionEdge> edges)
{
    UnitEdges result;
    for (const auto& edge : edges) {
        CHECK(edge.from.x == edge.to.x || edge.from.y == edge.to.y);
        const auto x0 = int(std::min(edge.from.x, edge.to.x));
        const auto x1 = int(std::max(edge.from.x, edge.to.x));
        const auto y0 = int(std::min(edge.from.y, edge.to.y));
        const auto y1 = int(std::max(edge.from.y, edge.to.y));
        if (x0 == x1)
            for (int y = y0; y < y1; ++y) result.insert({x0, y, x0, y + 1});
        else
            for (int x = x0; x < x1; ++x) result.insert({x, y0, x + 1, y0});
    }
    return result;
}

UnitEdges referenceEdges(const SelectionMask& mask)
{
    UnitEdges edges;
    for (int y = 0; y < int(mask.extent().height); ++y) {
        for (int x = 0; x < int(mask.extent().width); ++x) {
            if (mask.coverageAtDocumentPixel(x, y) < 128) continue;
            if (mask.coverageAtDocumentPixel(x - 1, y) < 128) edges.insert({x, y, x, y + 1});
            if (mask.coverageAtDocumentPixel(x + 1, y) < 128) edges.insert({x + 1, y, x + 1, y + 1});
            if (mask.coverageAtDocumentPixel(x, y - 1) < 128) edges.insert({x, y, x + 1, y});
            if (mask.coverageAtDocumentPixel(x, y + 1) < 128) edges.insert({x, y + 1, x + 1, y + 1});
        }
    }
    return edges;
}

void rectangleAlignmentAndAnalyticBoundaryParity()
{
    const Extent2u extent {17, 13};
    CHECK(alignedSelectionRectangle({2.2, 3.8}, {12.7, 10.1}, extent) == RectI({2, 4, 11, 6}));
    CHECK(alignedSelectionRectangle({12.7, 10.1}, {2.2, 3.8}, extent) == RectI({2, 4, 11, 6}));
    CHECK(alignedSelectionRectangle({-10, -20}, {200, 300}, extent) == RectI({0, 0, 17, 13}));
    CHECK(alignedSelectionRectangle({2.2, 3.8}, {2.2, 3.8}, extent).empty());
    // Baseline includes disconnected regions, a hole, and values on both
    // sides of the contour threshold. Verify final and live preview geometry
    // independently against every adjacent pair of document pixels.
    std::vector<std::uint8_t> coverage(std::size_t(extent.width) * extent.height);
    std::uint32_t seed = 0xA17U;
    for (auto& value : coverage) {
        seed = seed * 1664525U + 1013904223U;
        value = static_cast<std::uint8_t>(seed >> 24U);
    }
    const auto mask = SelectionMask::fromR8(extent, coverage, extent.width);
    CHECK(expandEdges(mask->boundaryEdges()) == referenceEdges(*mask));
    const std::array<RectI, 6> rectangles {{
        {3, 4, 8, 6}, {-5, 7, 10, 12}, {0, 0, 17, 13},
        {5, 5, 1, 1}, {7, 3, 0, 4}, {15, 11, 6, 6},
    }};
    for (const auto& base : std::array<SelectionState, 3> {SelectionState {}, mask, SelectionMask::filled(extent, 0)}) {
        for (const auto rect : rectangles) {
            for (const auto operation : {SelectionOperation::Replace, SelectionOperation::Add,
                     SelectionOperation::Subtract, SelectionOperation::Intersect}) {
                const auto expected = combineSelection(base, SelectionMask::rectangle(extent, rect), operation);
                CHECK(expandEdges(expected->boundaryEdges()) == referenceEdges(*expected));
                const auto preview = expandEdges(rectangleSelectionPreviewEdges(base, rect, operation, extent));
                const auto reference = referenceEdges(*expected);
                if (preview != reference) {
                    std::cerr << "Preview parity: base=" << (!base ? "inactive" : base->bounds().empty() ? "empty" : "partial")
                              << " rect=" << rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height
                              << " operation=" << int(operation) << " actual edges=" << preview.size()
                              << " expected edges=" << reference.size() << '\n';
                }
                CHECK(preview == reference);
            }
        }
    }
    const auto all = SelectionMask::filled({5120, 2880}, 255);
    CHECK(all->boundaryEdges().size() == 4);
}

void selectionHistoryNoOpsAndSurfaceIsolation()
{
    Fixture fixture({16, 16}, {12, 34, 56, 200});
    const auto surfaceRevision = fixture.surface->revision();
    const auto surfaceId = fixture.surface->id();
    const auto rect = SelectionMask::rectangle({16, 16}, {2, 3, 5, 7});
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetSelectionCommand>(rect, "Rectangle selection")));
    CHECK(fixture.history.undoDepth() == 1);
    const auto selectedRevision = fixture.document.selectionRevision();
    const auto memory = fixture.history.memoryUsed();
    CHECK(memory > 0);
    CHECK(!fixture.history.execute(fixture.document,
        std::make_unique<SetSelectionCommand>(SelectionMask::rectangle({16, 16}, {2, 3, 5, 7}))));
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.document.selectionRevision() == selectedRevision);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(!fixture.document.selection());
    CHECK(fixture.document.selectionRevision() > selectedRevision);
    CHECK(fixture.history.memoryUsed() == memory);
    CHECK(!fixture.history.execute(fixture.document, std::make_unique<SetSelectionCommand>(SelectionState {})));
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.document.selection()->equivalent(*rect));
    CHECK(fixture.history.memoryUsed() == memory);
    CHECK(fixture.surface->revision() == surfaceRevision);
    CHECK(fixture.surface->id() == surfaceId);
    CHECK(fixture.surface->dirtySince(surfaceRevision).empty());
}

void selectionEvidenceHistoryIsAtomicAndSessionOnly()
{
    Fixture f({16, 16}, {12, 34, 56, 200});
    const auto sourceRevision = f.surface->revision();
    const auto contentRevision = f.document.revision();
    const auto contentState = f.document.contentState();
    f.document.markSaved();
    const auto mask = SelectionMask::rectangle({16, 16}, {1, 1, 12, 12});
    QuickSelectionHints hints {SelectionMask::rectangle({16, 16}, {3, 3, 2, 2}), {}, {}};
    const auto positive = std::make_shared<const SmartSelectionEvidence>(
        f.document, f.id, ColorSampleSource::MergedVisible, hints);
    CHECK(f.history.execute(f.document, std::make_unique<SetSelectionCommand>(mask, "Quick", positive)));
    CHECK(f.document.selectionEvidence() == positive);
    CHECK(f.history.latestUndoMemoryCost() >= positive->memoryCost() + mask->memoryCost());
    hints.background = SelectionMask::rectangle({16, 16}, {13, 3, 2, 2});
    const auto correction = std::make_shared<const SmartSelectionEvidence>(
        f.document, f.id, ColorSampleSource::MergedVisible, hints);
    // A useful negative hint outside currently inferred coverage is still an
    // action. Its visible R8 is unchanged, but subsequent inference must know it.
    CHECK(f.history.execute(f.document, std::make_unique<SetSelectionCommand>(mask, "Quick", correction)));
    CHECK(f.history.undoDepth() == 2);
    const auto memory = f.history.memoryUsed();
    CHECK(f.history.undo(f.document));
    CHECK(f.document.selection() == mask && f.document.selectionEvidence() == positive);
    CHECK(f.history.redo(f.document));
    CHECK(f.document.selection() == mask && f.document.selectionEvidence() == correction);
    CHECK(f.history.memoryUsed() == memory);
    // Inferred exclusion coverage is independent of hard foreground/background
    // samples. Its partial-coverage ceiling must survive the same history path.
    hints.rejected = SelectionMask::rectangle({16, 16}, {12, 2, 1, 5}, 130);
    const auto protectedBoundary = std::make_shared<const SmartSelectionEvidence>(
        f.document, f.id, ColorSampleSource::MergedVisible, hints);
    CHECK(!protectedBoundary->equivalent(*correction));
    CHECK(protectedBoundary->memoryCost() == correction->memoryCost() + hints.rejected->memoryCost());
    CHECK(f.history.execute(f.document, std::make_unique<SetSelectionCommand>(mask, "Quick", protectedBoundary)));
    CHECK(f.history.undoDepth() == 3);
    CHECK(f.history.undo(f.document));
    CHECK(f.document.selectionEvidence() == correction);
    CHECK(!std::dynamic_pointer_cast<const SmartSelectionEvidence>(f.document.selectionEvidence())->hints().rejected);
    CHECK(f.history.redo(f.document));
    CHECK(f.document.selectionEvidence() == protectedBoundary);
    CHECK(std::dynamic_pointer_cast<const SmartSelectionEvidence>(f.document.selectionEvidence())
        ->hints().rejected->coverageAtDocumentPixel(12, 3) == 130);
    CHECK(f.history.undo(f.document));
    CHECK(f.history.undo(f.document));
    const auto equivalent = std::make_shared<const SmartSelectionEvidence>(
        f.document, f.id, ColorSampleSource::MergedVisible, positive->hints());
    CHECK(!f.history.execute(f.document, std::make_unique<SetSelectionCommand>(mask, "Quick", equivalent)));
    CHECK(f.history.redoDepth() == 2 && f.document.selectionEvidence() == positive);
    CHECK(!f.history.execute(f.document, std::make_unique<SetSelectionCommand>(mask)));
    CHECK(f.history.redoDepth() == 2 && f.document.selectionEvidence() == positive);
    // An ordinary selection edit rebases hints; undo restores both identities.
    CHECK(f.history.execute(f.document, std::make_unique<SetSelectionCommand>(SelectionState {})));
    CHECK(!f.document.selection() && !f.document.selectionEvidence());
    CHECK(f.history.undo(f.document));
    CHECK(f.document.selection() == mask && f.document.selectionEvidence() == positive);
    CHECK(f.document.revision() == contentRevision && f.document.contentState() == contentState);
    CHECK(!f.document.isModified() && f.surface->revision() == sourceRevision);
    CHECK(positive->matches(f.document, f.id, ColorSampleSource::MergedVisible));
    CHECK(!positive->matches(f.document, f.id, ColorSampleSource::ActiveLayer));
    CHECK(f.document.setLayerOpacity(f.id, .5F));
    CHECK(!positive->matches(f.document, f.id, ColorSampleSource::MergedVisible));
}

void canvasResizeRestoresClippedSelectionWithHistory()
{
    Fixture fixture({12, 10}, {10, 20, 30, 255});
    const auto initial = SelectionMask::rectangle({12, 10}, {5, 4, 6, 5}, 177);
    CHECK(fixture.document.setSelection(initial));
    const auto originalPixels = pixels(*fixture.surface);
    const auto originalTransform = fixture.document.layer(fixture.id)->localToDocument;
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<ChangeCanvasSpecCommand>(CanvasSpec {.extent = {8, 6}})));
    CHECK(fixture.document.selection()->extent() == Extent2u({8, 6}));
    CHECK(fixture.document.selection()->bounds() == RectI({5, 4, 3, 2}));
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<ChangeCanvasSpecCommand>(CanvasSpec {.extent = {16, 14}})));
    CHECK(fixture.document.selection()->coverageAtDocumentPixel(10, 8) == 0);
    CHECK(fixture.document.selection()->coverageAtDocumentPixel(7, 5) == 177);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.document.canvas().extent == Extent2u({8, 6}));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.document.canvas().extent == Extent2u({12, 10}));
    CHECK(fixture.document.selection()->equivalent(*initial));
    CHECK(fixture.document.selection()->coverageAtDocumentPixel(10, 8) == 177);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.document.selection()->bounds() == RectI({5, 4, 3, 2}));
    CHECK(pixels(*fixture.surface) == originalPixels);
    CHECK(fixture.document.layer(fixture.id)->localToDocument == originalTransform);

    Fixture unrestricted({12, 10});
    CHECK(unrestricted.history.execute(unrestricted.document,
        std::make_unique<ChangeCanvasSpecCommand>(CanvasSpec {.extent = {20, 20}})));
    CHECK(!unrestricted.document.selection());
    CHECK(unrestricted.history.undo(unrestricted.document));
    CHECK(!unrestricted.document.selection());

    Fixture empty({12, 10});
    CHECK(empty.document.setSelection(SelectionMask::filled({12, 10}, 0)));
    CHECK(empty.history.execute(empty.document,
        std::make_unique<ChangeCanvasSpecCommand>(CanvasSpec {.extent = {20, 20}})));
    CHECK(empty.document.selection());
    CHECK(empty.document.selection()->bounds().empty());
    CHECK(empty.document.selection()->extent() == Extent2u({20, 20}));
}

void maskedTransactionsPinCoverageAndRemainIncremental()
{
    Fixture fixture({128, 128});
    const auto initial = SelectionMask::rectangle({128, 128}, {5, 6, 3, 2}, 128);
    CHECK(fixture.document.setSelection(initial));
    RasterEditTransaction edit(fixture.document, fixture.id, "Pinned selection");
    CHECK(edit.targetAvailable());
    CHECK(fixture.document.setSelection(SelectionMask::filled({128, 128}, 0)));
    CHECK(edit.targetAvailable());
    const auto initialRevision = fixture.surface->revision();
    const auto dirty = writeSolid(edit, {0, 0, 16, 16}, {200, 100, 50, 255});
    CHECK(!dirty.fullRefresh);
    CHECK(dirty.regions == std::vector<RectI>({{5, 6, 3, 2}}));
    CHECK(fixture.surface->revision() == initialRevision + 1);
    CHECK(pixel(*fixture.surface, 5, 6) == Rgba8({200, 100, 50, 128}));
    CHECK(pixel(*fixture.surface, 4, 6) == Rgba8({0, 0, 0, 0}));
    const auto firstWriteRevision = fixture.surface->revision();
    for (int i = 0; i < 20; ++i)
        CHECK(writeSolid(edit, {0, 0, 16, 16}, {200, 100, 50, 255}).empty());
    CHECK(fixture.surface->revision() == firstWriteRevision);
    CHECK(edit.capturedTileCount() == 1);
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 1);
    const auto beforeUndo = fixture.surface->revision();
    CHECK(fixture.history.undo(fixture.document));
    CHECK(pixel(*fixture.surface, 5, 6) == Rgba8({0, 0, 0, 0}));
    CHECK(fixture.surface->dirtySince(beforeUndo).regions == std::vector<RectI>({{5, 6, 3, 2}}));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(pixel(*fixture.surface, 5, 6) == Rgba8({200, 100, 50, 128}));
    CHECK(fixture.document.selection()->bounds().empty());

    // An explicit input service still overrides the document's active-empty
    // selection; an empty-document optimization must not ignore that override.
    const auto explicitMask = SelectionMask::rectangle({128, 128}, {40, 41, 1, 1});
    RasterEditTransaction overridden(fixture.document, fixture.id, "Explicit mask",
        {.journalTileSize = 64, .selectionMask = explicitMask.get()});
    CHECK(!writeSolid(overridden, {40, 41, 1, 1}, {70, 90, 110, 255}).empty());
    CHECK(overridden.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*fixture.surface, 40, 41) == Rgba8({70, 90, 110, 255}));
}

void emptySelectionAndCancellationPreserveRedo()
{
    Fixture fixture({16, 16}, {3, 6, 9, 120});
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.id, false)));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.document.setSelection(SelectionMask::filled({16, 16}, 0)));
    const auto before = pixels(*fixture.surface);
    const auto revision = fixture.surface->revision();
    RasterEditTransaction empty(fixture.document, fixture.id, "Empty selection");
    CHECK(writeSolid(empty, {0, 0, 16, 16}, {255, 0, 0, 255}).empty());
    CHECK(empty.capturedTileCount() == 0);
    CHECK(empty.capturedPixelBytes() == 0);
    CHECK(empty.commit(fixture.history) == RasterEditCommitResult::NoChanges);
    CHECK(fixture.surface->revision() == revision);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.document.setSelection(SelectionMask::rectangle({16, 16}, {4, 4, 5, 5}, 128)));
    RasterEditTransaction cancelled(fixture.document, fixture.id, "Cancelled masked edit");
    CHECK(!writeSolid(cancelled, {0, 0, 16, 16}, {200, 150, 100, 255}).empty());
    const auto beforeCancel = fixture.surface->revision();
    cancelled.cancel();
    CHECK(pixels(*fixture.surface) == before);
    CHECK(fixture.surface->dirtySince(beforeCancel).regions == std::vector<RectI>({{4, 4, 5, 5}}));
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.undoDepth() == 0);
}

void overlappingMaskedBatchPatchesKeepLastCandidate()
{
    Fixture fixture({4, 4}, {0, 0, 0, 255});
    CHECK(fixture.document.setSelection(SelectionMask::filled({4, 4}, 128)));
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.id, false)));
    CHECK(fixture.history.undo(fixture.document));
    const std::array<std::byte, 4> white {
        std::byte {255}, std::byte {255}, std::byte {255}, std::byte {255}};
    const std::array<std::byte, 4> black {
        std::byte {0}, std::byte {0}, std::byte {0}, std::byte {255}};
    const std::array patches {RasterPatch {{1, 1, 1, 1}, white, 4},
        RasterPatch {{1, 1, 1, 1}, black, 4}};
    RasterEditTransaction reverted(fixture.document, fixture.id, "Ordered masked no-op");
    (void)reverted.writeRgba8Batch(patches);
    CHECK(pixel(*fixture.surface, 1, 1) == Rgba8({0, 0, 0, 255}));
    CHECK(reverted.commit(fixture.history) == RasterEditCommitResult::NoChanges);
    CHECK(fixture.history.undoDepth() == 0);
    CHECK(fixture.history.redoDepth() == 1);

    const std::array<std::byte, 12> whiteRow {
        white[0], white[1], white[2], white[3],
        white[0], white[1], white[2], white[3],
        white[0], white[1], white[2], white[3]};
    const std::array<std::byte, 8> blackRow {
        black[0], black[1], black[2], black[3],
        black[0], black[1], black[2], black[3]};
    const std::array overlap {RasterPatch {{0, 2, 3, 1}, whiteRow, 12},
        RasterPatch {{1, 2, 2, 1}, blackRow, 8}};
    RasterEditTransaction edited(fixture.document, fixture.id, "Ordered masked overlap");
    (void)edited.writeRgba8Batch(overlap);
    CHECK(pixel(*fixture.surface, 0, 2) == Rgba8({188, 188, 188, 255}));
    CHECK(pixel(*fixture.surface, 1, 2) == Rgba8({0, 0, 0, 255}));
    CHECK(pixel(*fixture.surface, 2, 2) == Rgba8({0, 0, 0, 255}));
    CHECK(edited.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.redoDepth() == 0);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(pixel(*fixture.surface, 0, 2) == Rgba8({0, 0, 0, 255}));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(pixel(*fixture.surface, 0, 2) == Rgba8({188, 188, 188, 255}));
    CHECK(pixel(*fixture.surface, 1, 2) == Rgba8({0, 0, 0, 255}));
}

void partialCoverageUsesLinearLightAndStraightAlpha()
{
    Fixture paint({4, 4}, {0, 0, 0, 255});
    CHECK(paint.document.setSelection(SelectionMask::filled({4, 4}, 128)));
    RasterEditTransaction edit(paint.document, paint.id, "Linear coverage");
    (void)writeSolid(edit, {1, 1, 1, 1}, {255, 255, 255, 255});
    CHECK(pixel(*paint.surface, 1, 1) == Rgba8({188, 188, 188, 255}));
    CHECK(edit.commit(paint.history) == RasterEditCommitResult::Committed);

    Fixture erase({4, 4}, {20, 70, 180, 180});
    CHECK(erase.document.setSelection(SelectionMask::filled({4, 4}, 128)));
    RasterEditTransaction eraseEdit(erase.document, erase.id, "Partial erase");
    for (int i = 0; i < 10; ++i)
        (void)writeSolid(eraseEdit, {1, 1, 1, 1}, {20, 70, 180, 0});
    CHECK(pixel(*erase.surface, 1, 1) == Rgba8({20, 70, 180, 90}));
    CHECK(eraseEdit.commit(erase.history) == RasterEditCommitResult::Committed);
    CHECK(erase.history.undo(erase.document));
    CHECK(pixel(*erase.surface, 1, 1) == Rgba8({20, 70, 180, 180}));
}

void transformedBrushAndEraseRespectDocumentSelection()
{
    const std::array<AffineTransform, 4> transforms {{
        {1, 0, 20, 0, 1, 20},
        {0, -2, 40, 1.5, 0, 20},
        {-2, 0, 40, 0, 1.5, 20},
        {1, 0.3, 20, 0.1, 1.4, 20},
    }};
    for (const auto transform : transforms) {
        for (const auto mode : {BrushCompositeMode::Paint, BrushCompositeMode::Erase}) {
            Document document(CanvasSpec {.extent = {96, 96}});
            const Rgba8 original {20, 40, 60, 180};
            auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u {8, 6}, original);
            auto layer = Layer::raster("Transformed", surface);
            const auto id = layer.id;
            layer.localToDocument = transform;
            CHECK(document.insertLayer(0, std::move(layer)));
            const auto mask = SelectionMask::rectangle({96, 96}, {24, 0, 8, 96});
            CHECK(document.setSelection(mask));
            auto settings = brush(128);
            settings.flow = 1.0;
            History history;
            BasicPixelBrushStroke stroke(document, id, settings, mode);
            CHECK(stroke.begin(sample({32, 32})));
            CHECK(stroke.end(sample({32, 32}, 1000), history) == RasterEditCommitResult::Committed);
            // Painting can replace/extend storage; inspect the current surface
            // at the same canonical local pixels, not the retained undo base.
            const auto editedSurface=std::get<RasterLayer>(document.layer(id)->payload).surface;
            const auto origin=document.layer(id)->rasterOrigin;
            bool changed = false;
            for (std::int32_t y = 0; y < 6; ++y) {
                for (std::int32_t x = 0; x < 8; ++x) {
                    const auto doc = transform.map({double(x) + 0.5, double(y) + 0.5});
                    const bool selected = mask->coverageAtDocumentPixel(
                        static_cast<std::int32_t>(std::floor(doc.x)),
                        static_cast<std::int32_t>(std::floor(doc.y))) != 0;
                    const auto value = pixel(*editedSurface, x-int(origin.x), y-int(origin.y));
                    CHECK(selected ? value != original : value == original);
                    changed = changed || value != original;
                    if (mode == BrushCompositeMode::Erase && selected) {
                        CHECK(value.red == original.red);
                        CHECK(value.green == original.green);
                        CHECK(value.blue == original.blue);
                        CHECK(value.alpha < original.alpha);
                    }
                }
            }
            CHECK(changed);
            CHECK(document.selection() == mask);
            CHECK(document.layer(id)->localToDocument == transform);
            const auto edited = pixels(*editedSurface);
            CHECK(history.undo(document));
            for (std::int32_t y = 0; y < 6; ++y)
                for (std::int32_t x = 0; x < 8; ++x)
                    CHECK(pixel(*surface, x, y) == original);
            CHECK(history.redo(document));
            CHECK(pixels(*std::get<RasterLayer>(document.layer(id)->payload).surface) == edited);
        }
    }
}

std::vector<std::byte> maskedStroke(unsigned events, BrushCompositeMode mode,
    const SelectionState& selection)
{
    Fixture fixture({96, 48}, {30, 60, 90, 140});
    if (selection) CHECK(fixture.document.setSelection(selection));
    const auto original = pixels(*fixture.surface);
    const auto revision = fixture.surface->revision();
    BasicPixelBrushStroke stroke(fixture.document, fixture.id, brush(), mode);
    CHECK(stroke.begin(sample({8, 24})));
    for (unsigned i = 1; i < events; ++i) {
        const auto progress = double(i) / double(events);
        CHECK(stroke.append(sample({8 + 80 * progress, 24}, std::uint64_t(i) * 100000 / events)));
    }
    const bool empty = selection && selection->bounds().empty();
    CHECK(stroke.end(sample({88, 24}, 100000), fixture.history)
        == (empty ? RasterEditCommitResult::NoChanges : RasterEditCommitResult::Committed));
    CHECK(fixture.history.undoDepth() == (empty ? 0U : 1U));
    if (empty) {
        CHECK(stroke.stats().retainedStrokeTiles == 0);
        CHECK(stroke.stats().uploadedRegionBytes == 0);
        CHECK(stroke.stats().surfaceWriteBatches == 0);
        CHECK(fixture.surface->revision() == revision);
        CHECK(pixels(*fixture.surface) == original);
    }
    return pixels(*fixture.surface);
}

void partialMaskBrushIsIndependentOfInputDelivery()
{
    const auto partial = SelectionMask::rectangle({96, 48}, {15, 5, 70, 38}, 128);
    CHECK(maskedStroke(1, BrushCompositeMode::Paint, partial) == maskedStroke(80, BrushCompositeMode::Paint, partial));
    CHECK(maskedStroke(1, BrushCompositeMode::Erase, partial) == maskedStroke(80, BrushCompositeMode::Erase, partial));
    const auto full = SelectionMask::filled({96, 48}, 255);
    for (const auto mode : {BrushCompositeMode::Paint, BrushCompositeMode::Erase}) {
        CHECK(maskedStroke(80, mode, {}) == maskedStroke(80, mode, full));
        (void)maskedStroke(80, mode, SelectionMask::filled({96, 48}, 0));
    }
}

void copySelectedTransformedPixelsAndActiveLayerUndoRedo()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {64, 64}});
    auto bottom = Layer::raster("Bottom",
        std::make_shared<ContiguousRasterSurface>(Extent2u {64, 64}));
    CHECK(document->insertLayer(0, std::move(bottom)));
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u {1, 1}, Rgba8 {180, 90, 45, 128});
    auto source = Layer::raster("Scaled source", surface);
    const auto sourceId = source.id;
    source.opacity = 0.4F;
    source.localToDocument = {10, 0, 20, 0, 10, 20};
    CHECK(document->insertLayer(1, std::move(source)));
    auto upper = Layer::raster("Upper",
        std::make_shared<ContiguousRasterSurface>(Extent2u {1, 1}));
    const auto upperId = upper.id;
    CHECK(document->insertLayer(2, std::move(upper)));
    const auto selection = SelectionMask::rectangle({64, 64}, {24, 25, 1, 1}, 128);
    CHECK(document->setSelection(selection));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(sourceId);
    const auto originalBytes = pixels(*surface);
    const auto revision = surface->revision();
    CHECK(session.execute(std::make_unique<LayerViaCopyCommand>(sourceId, session.activeLayer())));
    CHECK(session.document()->layers().size() == 4);
    const auto createdId = session.activeLayer();
    CHECK(createdId && *createdId != sourceId);
    CHECK(session.document()->layers()[2].id == createdId);
    CHECK(session.document()->layers()[3].id == upperId);
    const auto* created = session.document()->layer(*createdId);
    const auto copiedSurface = std::get<RasterLayer>(created->payload).surface;
    CHECK(copiedSurface->extent() == Extent2u({1, 1}));
    CHECK(created->localToDocument.map({0, 0}) == Vec2d({24, 25}));
    CHECK(created->opacity == 0.4F);
    CHECK(pixel(*copiedSurface, 0, 0) == Rgba8({180, 90, 45, 64}));
    CHECK(pixels(*surface) == originalBytes);
    CHECK(surface->revision() == revision);
    CHECK(session.document()->selection() == selection);
    CHECK(session.history().undoDepth() == 1);
    CHECK(session.undo());
    CHECK(session.activeLayer() == sourceId);
    CHECK(!session.document()->containsLayer(*createdId));
    CHECK(session.document()->selection() == selection);
    CHECK(session.redo());
    CHECK(session.activeLayer() == createdId);
    CHECK(session.document()->layers()[2].id == createdId);
    CHECK(session.document()->selection() == selection);
}

void copiedRotatedAndFlippedLayersPreserveSampleAlignment()
{
    const std::array<AffineTransform, 3> transforms {{
        {0, -2, 30, 2, 0, 20},
        {-2, 0, 30, 0, 2, 20},
        {1.3, -0.5, 22.4, 0.75, 1.4, 20.6},
    }};
    for (const auto transform : transforms) {
        EditorSession session;
        auto document = std::make_unique<Document>(CanvasSpec {.extent = {64, 64}});
        auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u {4, 3});
        auto layer = Layer::raster("Pattern", surface);
        const auto id = layer.id;
        layer.opacity = 0.35F;
        layer.localToDocument = transform;
        CHECK(document->insertLayer(0, std::move(layer)));
        RasterEditTransaction paint(*document, id, "Test pattern");
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                (void)writeSolid(paint, {x, y, 1, 1},
                    {static_cast<std::uint8_t>(30 + x * 45),
                        static_cast<std::uint8_t>(40 + y * 70), 80,
                        static_cast<std::uint8_t>(80 + x * 35)});
        History setup;
        CHECK(paint.commit(setup) == RasterEditCommitResult::Committed);
        const auto selection = SelectionMask::rectangle({64, 64}, {21, 20, 10, 8});
        CHECK(document->setSelection(selection));
        session.replaceDocument(std::move(document));
        session.setActiveLayer(id);
        CHECK(session.execute(std::make_unique<LayerViaCopyCommand>(id, id)));
        const auto copy = session.activeLayer();
        CHECK(copy && *copy != id);
        CHECK(session.document()->layer(*copy)->opacity == 0.35F);
        for (int y = 20; y < 28; ++y) {
            for (int x = 21; x < 31; ++x) {
                const Vec2d point {double(x) + 0.5, double(y) + 0.5};
                const auto expected = sampleDocumentColor(*session.document(), id, point, ColorSampleSource::ActiveLayer);
                const auto actual = sampleDocumentColor(*session.document(), copy, point, ColorSampleSource::ActiveLayer);
                CHECK(actual.available());
                CHECK(expected.available());
                CHECK(actual.color == expected.color);
            }
        }
    }
}

void copyWithoutSelectionDuplicatesAndEmptyCopyPreservesRedo()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {32, 32}});
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u {4, 3}, Rgba8 {13, 26, 39, 100});
    auto layer = Layer::raster("Duplicate source", surface);
    const auto id = layer.id;
    const AffineTransform transform {-2, 0.3, 14, 0.2, 1.5, 8};
    layer.localToDocument = transform;
    layer.opacity = 0.7F;
    CHECK(document->insertLayer(0, std::move(layer)));
    session.replaceDocument(std::move(document));
    CHECK(session.execute(std::make_unique<LayerViaCopyCommand>(id, id)));
    const auto copy = *session.activeLayer();
    CHECK(copy != id);
    const auto* copied = session.document()->layer(copy);
    const auto copySurface = std::get<RasterLayer>(copied->payload).surface;
    CHECK(copySurface->id() != surface->id());
    CHECK(copySurface->extent() == surface->extent());
    CHECK(pixels(*copySurface) == pixels(*surface));
    CHECK(copied->localToDocument == transform);
    CHECK(copied->opacity == 0.7F);
    CHECK(session.undo());
    CHECK(session.activeLayer() == id);
    CHECK(session.document()->setSelection(SelectionMask::filled({32, 32}, 0)));
    CHECK(!session.execute(std::make_unique<LayerViaCopyCommand>(id, id)));
    CHECK(session.document()->layers().size() == 1);
    CHECK(session.history().redoDepth() == 1);
    CHECK(session.activeLayer() == id);
    CHECK(session.document()->setSelection(SelectionMask::rectangle({32, 32}, {0, 0, 1, 1})));
    CHECK(!session.execute(std::make_unique<LayerViaCopyCommand>(id, id)));
    CHECK(session.history().redoDepth() == 1);
    CHECK(!session.execute(std::make_unique<LayerViaCopyCommand>(LayerId {99999999}, id)));
    CHECK(session.history().redoDepth() == 1);
}

void highResolutionMaskOperationsStayBounded()
{
    const Extent2u extent {5120, 2880};
    const auto a = SelectionMask::rectangle(extent, {7, 9, 3, 5}, 80);
    const auto b = SelectionMask::rectangle(extent, {5100, 2850, 5, 7}, 180);
    const auto result = a->combined(*b, SelectionOperation::Add);
    CHECK(result->extent() == extent);
    CHECK(result->coverageAtDocumentPixel(8, 10) == 80);
    CHECK(result->coverageAtDocumentPixel(5102, 2853) == 180);
    CHECK(result->coverageAtDocumentPixel(2560, 1440) == 0);
    CHECK(result->bounds() == RectI({7, 9, 5098, 2848}));
    CHECK(result->memoryCost() < std::size_t(extent.width) * extent.height * 2U);
    const auto shrunk = result->resized({128, 128});
    CHECK(shrunk->bounds() == RectI({7, 9, 3, 5}));
    CHECK(shrunk->coverageAtDocumentPixel(8, 10) == 80);
    CHECK(shrunk->resized(extent)->coverageAtDocumentPixel(5102, 2853) == 0);
    CHECK(result->coverageAtDocumentPixel(5102, 2853) == 180);
}

} // namespace

void rememberedSelectionHistoryAndResize()
{
    Fixture f({32,24});
    auto& d=f.document; auto& h=f.history;
    const auto a=SelectionMask::rectangle({32,24},{10,8,20,12},137);
    const auto revision=d.revision();
    CHECK(h.execute(d,std::make_unique<SetSelectionCommand>(a)));
    CHECK(d.lastSelection()==a && d.revision()==revision);
    CHECK(h.execute(d,std::make_unique<SetSelectionCommand>(SelectionState{})));
    CHECK(!d.selection() && d.lastSelection()==a);
    // Transient previews and their cancellation never replace the saved mask.
    d.setSelection(SelectionMask::filled({32,24},255)); d.setSelection({});
    CHECK(d.lastSelection()==a);
    CHECK(h.execute(d,std::make_unique<SetSelectionCommand>(d.lastSelection())));
    CHECK(d.selection()==a); CHECK(h.undo(d));
    CHECK(!d.selection() && d.lastSelection()==a);
    CHECK(!h.execute(d,std::make_unique<SetSelectionCommand>(SelectionState{})));
    CHECK(h.redoDepth()==1);
    CHECK(h.execute(d,std::make_unique<ChangeCanvasSpecCommand>(CanvasSpec{{16,16}})));
    CHECK(d.lastSelection()->extent()==Extent2u({16,16}));
    CHECK(d.lastSelection()->coverageAtDocumentPixel(12,10)==137);
    CHECK(h.undo(d)); CHECK(d.lastSelection()==a);
    CHECK(h.execute(d,std::make_unique<ChangeCanvasSpecCommand>(CanvasSpec{{4,4}})));
    CHECK(!d.lastSelection()); CHECK(h.undo(d)); CHECK(d.lastSelection()==a);
}

int main()
{
    rememberedSelectionHistoryAndResize();
    maskCoverageBoundsAndImmutableSnapshots();
    overflowingMaskStrideIsRejectedBeforeReading();
    partialCoverageAlgebraAndDisconnectedRegions();
    inactiveAndActiveEmptyAreDistinct();
    rectangleAlignmentAndAnalyticBoundaryParity();
    selectionHistoryNoOpsAndSurfaceIsolation();
    selectionEvidenceHistoryIsAtomicAndSessionOnly();
    canvasResizeRestoresClippedSelectionWithHistory();
    maskedTransactionsPinCoverageAndRemainIncremental();
    emptySelectionAndCancellationPreserveRedo();
    overlappingMaskedBatchPatchesKeepLastCandidate();
    partialCoverageUsesLinearLightAndStraightAlpha();
    transformedBrushAndEraseRespectDocumentSelection();
    partialMaskBrushIsIndependentOfInputDelivery();
    copySelectedTransformedPixelsAndActiveLayerUndoRedo();
    copiedRotatedAndFlippedLayersPreserveSampleAlignment();
    copyWithoutSelectionDuplicatesAndEmptyCopyPreservesRedo();
    highResolutionMaskOperationsStayBounded();
    if (failures) {
        std::cerr << failures << " selection assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All selection tests passed\n";
    return EXIT_SUCCESS;
}
