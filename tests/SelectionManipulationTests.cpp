#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <numbers>
#include <set>
#include <tuple>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool result, const char* expression, int line)
{
    if (!result) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using Plane = std::vector<std::uint8_t>;
using UnitEdge = std::array<int, 4>;
using Edges = std::set<UnitEdge>;

Plane read(const SelectionMask& mask)
{
    const auto extent = mask.extent();
    Plane result(std::size_t(extent.width) * extent.height);
    for (unsigned y = 0; y < extent.height; ++y)
        for (unsigned x = 0; x < extent.width; ++x)
            result[std::size_t(y) * extent.width + x]
                = mask.coverageAtDocumentPixel(int(x), int(y));
    return result;
}

std::uint8_t at(const Plane& source, Extent2u extent, int x, int y)
{
    if (x < 0 || y < 0 || x >= int(extent.width) || y >= int(extent.height)) return 0;
    return source[std::size_t(y) * extent.width + unsigned(x)];
}

Plane morphologyAxis(const Plane& source, Extent2u extent, int amount, bool horizontal)
{
    if (!amount) return source;
    Plane output(source.size());
    const auto distance = std::abs(std::int64_t(amount));
    const int axisLength = int(horizontal ? extent.width : extent.height);
    // This scalar reference intentionally differs from a running-window
    // implementation: every destination gathers all in-range candidates.
    const int radius = int(std::min(distance, std::int64_t(axisLength)));
    for (int y = 0; y < int(extent.height); ++y) {
        for (int x = 0; x < int(extent.width); ++x) {
            const int position = horizontal ? x : y;
            std::uint8_t result = amount > 0 ? 0 : 255;
            if (amount < 0 && (std::int64_t(position) - distance < 0
                || std::int64_t(position) + distance >= axisLength)) {
                result = 0;
            } else {
                for (int d = -radius; d <= radius; ++d) {
                    const auto candidate = at(source, extent,
                        x + (horizontal ? d : 0), y + (horizontal ? 0 : d));
                    result = amount > 0 ? std::max(result, candidate)
                                        : std::min(result, candidate);
                }
            }
            output[std::size_t(y) * extent.width + unsigned(x)] = result;
        }
    }
    return output;
}

Plane morphology(const Plane& source, Extent2u extent, int horizontal, int vertical)
{
    return morphologyAxis(morphologyAxis(source, extent, horizontal, true), extent, vertical, false);
}

Plane translated(const Plane& source, Extent2u extent, int dx, int dy)
{
    Plane output(source.size());
    for (int y = 0; y < int(extent.height); ++y) {
        for (int x = 0; x < int(extent.width); ++x) {
            const auto sx = std::int64_t(x) - dx;
            const auto sy = std::int64_t(y) - dy;
            if (sx >= 0 && sy >= 0 && sx < extent.width && sy < extent.height)
                output[std::size_t(y) * extent.width + unsigned(x)]
                    = source[std::size_t(sy) * extent.width + std::size_t(sx)];
        }
    }
    return output;
}

Edges expectedEdges(const Plane& source, Extent2u extent)
{
    Edges edges;
    for (int y = 0; y < int(extent.height); ++y) {
        for (int x = 0; x < int(extent.width); ++x) {
            if (at(source, extent, x, y) < 128) continue;
            if (at(source, extent, x, y - 1) < 128) edges.insert({x, y, x + 1, y});
            if (at(source, extent, x, y + 1) < 128) edges.insert({x, y + 1, x + 1, y + 1});
            if (at(source, extent, x - 1, y) < 128) edges.insert({x, y, x, y + 1});
            if (at(source, extent, x + 1, y) < 128) edges.insert({x + 1, y, x + 1, y + 1});
        }
    }
    return edges;
}

Edges expandEdges(const std::vector<SelectionEdge>& source, Extent2u extent)
{
    Edges edges;
    for (const auto& edge : source) {
        CHECK(std::isfinite(edge.from.x) && std::isfinite(edge.from.y)
            && std::isfinite(edge.to.x) && std::isfinite(edge.to.y));
        CHECK(edge.from.x == std::round(edge.from.x) && edge.from.y == std::round(edge.from.y));
        CHECK(edge.to.x == std::round(edge.to.x) && edge.to.y == std::round(edge.to.y));
        const int x0 = int(std::min(edge.from.x, edge.to.x));
        const int y0 = int(std::min(edge.from.y, edge.to.y));
        const int x1 = int(std::max(edge.from.x, edge.to.x));
        const int y1 = int(std::max(edge.from.y, edge.to.y));
        CHECK(x0 >= 0 && y0 >= 0 && x1 <= int(extent.width) && y1 <= int(extent.height));
        CHECK((x0 == x1) != (y0 == y1));
        if (y0 == y1) for (int x = x0; x < x1; ++x) edges.insert({x, y0, x + 1, y0});
        if (x0 == x1) for (int y = y0; y < y1; ++y) edges.insert({x0, y, x0, y + 1});
    }
    return edges;
}

Vec2d boundsCenter(const SelectionState& mask)
{
    const auto bounds = mask->bounds();
    return {bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5};
}

SelectionState islandsAndHole(Extent2u extent)
{
    auto mask = SelectionMask::rectangle(extent, {3, 4, 10, 9});
    mask = mask->combined(*SelectionMask::rectangle(extent, {6, 7, 4, 3}), SelectionOperation::Subtract);
    mask = mask->combined(*SelectionMask::rectangle(extent, {18, 15, 4, 3}, 190), SelectionOperation::Add);
    return mask->combined(*SelectionMask::rectangle(extent, {20, 2, 2, 3}, 64), SelectionOperation::Add);
}

void moveHitsActualCoverageAndLatchesOnlyTheUnmodifiedReplaceMode()
{
    const auto mask = islandsAndHole({32, 24});
    CHECK(selectionMoveHit(mask, {3.01, 4.01}, SelectionOperation::Replace, false, false));
    CHECK(selectionMoveHit(mask, {20.5, 2.5}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit(mask, {7.2, 8.1}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit(mask, {15.0, 10.0}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit(mask, {-0.1, 4}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit(mask, {32, 4}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit({}, {3.5, 4.5}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit(SelectionMask::filled({32, 24}, 0), {3.5, 4.5}, SelectionOperation::Replace, false, false));
    for (const auto mode : {SelectionOperation::Replace, SelectionOperation::Add,
             SelectionOperation::Subtract, SelectionOperation::Intersect}) {
        for (const bool shift : {false, true}) for (const bool alt : {false, true}) {
            CHECK(selectionMoveHit(mask, {4.5, 5.5}, mode, shift, alt)
                == (mode == SelectionOperation::Replace && !shift && !alt));
        }
    }
    CHECK(!selectionMoveHit(mask, {std::numeric_limits<double>::quiet_NaN(), 5}, SelectionOperation::Replace, false, false));
    CHECK(!selectionMoveHit(mask, {5, std::numeric_limits<double>::infinity()}, SelectionOperation::Replace, false, false));
}

void translationIsExactClippedAndAlwaysDerivesFromTheGestureBaseline()
{
    const Extent2u extent {32, 24};
    const auto mask = islandsAndHole(extent);
    const auto original = read(*mask);
    const auto revision = mask->revision();
    for (const auto [dx, dy] : std::array<std::array<int, 2>, 12> {{{0, 0}, {1, -1}, {-4, 8}, {20, 10},
             {-20, -10}, {32, 0}, {0, -24}, {100, 100}, {-100, 0}, {0, 100},
             {std::numeric_limits<int>::min(), 0}, {std::numeric_limits<int>::max(), 0}}}) {
        const auto actual = mask->translated(dx, dy);
        CHECK(actual->extent() == extent);
        CHECK(read(*actual) == translated(original, extent, dx, dy));
    }
    const auto offCanvasPreview = mask->translated(29, 0);
    CHECK(offCanvasPreview->bounds().empty());
    const auto returnedPreview = mask->translated(0, 0);
    CHECK(returnedPreview->equivalent(*mask));
    CHECK(mask->translated(20, 0)->translated(-20, 0)->bounds() != mask->bounds());
    CHECK(read(*mask) == original);
    CHECK(mask->revision() == revision);
}

void translatedPreviewMatchesIndependentPixelBoundaryIncludingNewCanvasEdges()
{
    const Extent2u extent {32, 24};
    const std::array masks {SelectionMask::filled(extent, 0), SelectionMask::filled(extent, 255),
        SelectionMask::filled(extent, 127), SelectionMask::filled(extent, 128), islandsAndHole(extent),
        SelectionMask::rectangle(extent, {0, 0, 1, 24})};
    for (const auto& mask : masks) {
        const auto bytes = read(*mask);
        for (const int dx : {-33, -20, -1, 0, 1, 10, 31, 33}) {
            for (const int dy : {-25, -15, -1, 0, 1, 10, 23, 25}) {
                const auto expected = expectedEdges(translated(bytes, extent, dx, dy), extent);
                const auto preview = translatedSelectionPreviewEdges(mask, dx, dy);
                CHECK(expandEdges(preview, extent) == expected);
                CHECK(expandEdges(mask->translated(dx, dy)->boundaryEdges(), extent) == expected);
            }
        }
    }
    CHECK(translatedSelectionPreviewEdges({}, 1, 2).empty());
}

void fractionalMovePreviewPreservesContoursAndClipsAtTheCanvas()
{
    const auto mask = SelectionMask::rectangle({32,24}, {2,3,9,7});
    for (const auto offset : {Vec2d{.5,1.5}, Vec2d{-2.5,-3.5}, Vec2d{25.5,18.5}}) {
        const auto edges = translatedSelectionPreviewEdges(mask, offset.x, offset.y);
        const double x0=std::max(0.0,2+offset.x), y0=std::max(0.0,3+offset.y);
        const double x1=std::min(32.0,11+offset.x), y1=std::min(24.0,10+offset.y);
        const std::vector<SelectionEdge> expected {{{x0,y0},{x1,y0}}, {{x0,y1},{x1,y1}},
            {{x0,y0},{x0,y1}}, {{x1,y0},{x1,y1}}};
        CHECK(edges == expected);
    }
    const auto maskWithHole=mask->combined(*SelectionMask::rectangle({32,24},{4,5,2,2}),SelectionOperation::Subtract);
    auto expected=maskWithHole->boundaryEdges();
    for(auto& e:expected){e.from=e.from+Vec2d{.5,.5};e.to=e.to+Vec2d{.5,.5};}
    CHECK(translatedSelectionPreviewEdges(maskWithHole,.5,.5)==expected);
    CHECK(translatedSelectionPreviewEdges(mask,std::numeric_limits<double>::quiet_NaN(),0).empty());
    CHECK(translatedSelectionPreviewEdges(mask,0,std::numeric_limits<double>::infinity()).empty());
}

void morphologyAmountsArePerSideAndAxesAreDocumentSpace()
{
    const Extent2u extent {200, 140};
    const auto rectangle = SelectionMask::rectangle(extent, {40, 40, 100, 50});
    CHECK(rectangle->adjusted(10, 10)->bounds() == RectI({30, 30, 120, 70}));
    CHECK(rectangle->adjusted(10, 0)->bounds() == RectI({30, 40, 120, 50}));
    CHECK(rectangle->adjusted(0, 10)->bounds() == RectI({40, 30, 100, 70}));
    CHECK(rectangle->adjusted(-10, -10)->bounds() == RectI({50, 50, 80, 30}));
    CHECK(rectangle->adjusted(-50, 0)->bounds().empty());
    CHECK(rectangle->adjusted(0, -25)->bounds().empty());
    CHECK(rectangle->adjusted(0, 0)->equivalent(*rectangle));
    CHECK(rectangle->bounds() == RectI({40, 40, 100, 50}));
}

void morphologyMatchesIndependentR8ReferenceForEverySignAndCanvasEdge()
{
    const Extent2u extent {23, 19};
    Plane noisy(std::size_t(extent.width) * extent.height);
    std::uint32_t seed = 0x3d327af1;
    for (auto& value : noisy) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        value = std::uint8_t(seed & 255U);
    }
    const std::array masks {islandsAndHole(extent), SelectionMask::fromR8(extent, noisy, extent.width),
        SelectionMask::filled(extent, 0), SelectionMask::filled(extent, 173),
        SelectionMask::rectangle(extent, {0, 0, 1, 19}, 129)};
    for (const auto& mask : masks) {
        const auto source = read(*mask);
        for (const int h : {-25, -5, -1, 0, 1, 4, 25}) {
            for (const int v : {-25, -4, -1, 0, 1, 5, 25}) {
                const auto adjusted = mask->adjusted(h, v);
                CHECK(adjusted->extent() == extent);
                CHECK(read(*adjusted) == morphology(source, extent, h, v));
            }
        }
        CHECK(read(*mask) == source);
    }
    const auto seedMask = SelectionMask::rectangle(extent, {11, 9, 1, 1}, 72);
    CHECK(seedMask->adjusted(1000, 1000)->equivalent(*SelectionMask::filled(extent, 72)));
    CHECK(seedMask->adjusted(-1000, -1000)->bounds().empty());
}

void holesAndDisconnectedRegionsUseCoverageMorphologyNotBoundsScaling()
{
    const Extent2u extent {40, 32};
    const auto baseline = islandsAndHole(extent);
    const auto grown = baseline->adjusted(1, 1);
    CHECK(grown->coverageAtDocumentPixel(6, 8) == 255); // hole shrinks inward
    CHECK(grown->coverageAtDocumentPixel(7, 8) == 0); // but its center remains
    CHECK(grown->coverageAtDocumentPixel(17, 15) == 190); // separate partial island grows
    CHECK(grown->coverageAtDocumentPixel(15, 12) == 0); // not one stretched bounds rectangle
    const auto shrunk = baseline->adjusted(-1, -1);
    CHECK(shrunk->coverageAtDocumentPixel(5, 8) == 0); // hole grows outward
    CHECK(shrunk->coverageAtDocumentPixel(19, 16) == 190);
    CHECK(shrunk->coverageAtDocumentPixel(20, 2) == 0); // thin island disappears
    const auto mixed = baseline->adjusted(2, -1);
    CHECK(read(*mixed) == morphology(read(*baseline), extent, 2, -1));
}

void tileSeamsDoNotChangeManipulationResults()
{
    const Extent2u extent {263, 259};
    auto source = SelectionMask::rectangle(extent, {121, 119, 24, 27}, 210);
    source = source->combined(*SelectionMask::rectangle(extent, {128, 128, 4, 3}, 100), SelectionOperation::Subtract);
    source = source->combined(*SelectionMask::rectangle(extent, {254, 248, 9, 11}, 88), SelectionOperation::Add);
    const auto bytes = read(*source);
    for (const auto [dx, dy] : std::array<std::array<int, 2>, 6> {{{1, 1}, {-1, -1}, {127, 0}, {-128, 1}, {0, 128}, {-129, -129}}}) {
        CHECK(read(*source->translated(dx, dy)) == translated(bytes, extent, dx, dy));
        CHECK(expandEdges(translatedSelectionPreviewEdges(source, dx, dy), extent)
            == expectedEdges(translated(bytes, extent, dx, dy), extent));
    }
    for (const auto [h, v] : std::array<std::array<int, 2>, 5> {{{1, 1}, {-1, -1}, {3, -2}, {-2, 3}, {129, 0}}})
        CHECK(read(*source->adjusted(h, v)) == morphology(bytes, extent, h, v));
}

void quarterTurnsAreLosslessAroundPixelAlignedAndOffsetPivots()
{
    const Extent2u extent {64, 64};
    auto source = SelectionMask::rectangle(extent, {17, 21, 23, 13}, 211);
    source = source->combined(*SelectionMask::rectangle(extent, {17, 21, 3, 4}, 44), SelectionOperation::Subtract);
    source = source->combined(*SelectionMask::rectangle(extent, {34, 28, 2, 3}), SelectionOperation::Add);
    const auto original = read(*source);
    for (const auto pivot : {boundsCenter(source), Vec2d {32, 32}, Vec2d {19, 29}}) {
        for (const int turns : {-3, -2, -1, 0, 1, 2, 3, 4, 8}) {
            Plane expected(original.size());
            const int quarter = (turns % 4 + 4) % 4;
            for (int y = 0; y < int(extent.height); ++y) for (int x = 0; x < int(extent.width); ++x) {
                auto delta = Vec2d {x + 0.5 - pivot.x, y + 0.5 - pivot.y};
                for (int turn = 0; turn < quarter; ++turn) delta = {-delta.y, delta.x};
                const int dx = int(std::lround(pivot.x + delta.x - 0.5));
                const int dy = int(std::lround(pivot.y + delta.y - 0.5));
                if (dx >= 0 && dy >= 0 && dx < int(extent.width) && dy < int(extent.height))
                    expected[std::size_t(dy) * extent.width + unsigned(dx)] = at(original, extent, x, y);
            }
            const auto actual = source->rotated(turns * 90.0, pivot);
            CHECK(read(*actual) == expected);
            CHECK(actual->extent() == extent);
        }
    }
    CHECK(read(*source) == original);
}

void arbitraryRotationRetainsCoverageAndUsesTheOriginalForRepeatedSteps()
{
    const Extent2u extent {96, 96};
    const auto source = SelectionMask::rectangle(extent, {28, 35, 31, 19});
    const auto pivot = boundsCenter(source);
    const auto original = read(*source);
    const auto originalArea = std::accumulate(original.begin(), original.end(), std::uint64_t {0});
    for (const double angle : {-179.0, -46.5, -1.0, 1.0, 17.25, 45.0, 123.0, 359.0}) {
        const auto rotated = source->rotated(angle, pivot);
        const auto output = read(*rotated);
        const auto area = std::accumulate(output.begin(), output.end(), std::uint64_t {0});
        CHECK(std::abs(double(area) - double(originalArea)) < double(originalArea) * 0.025 + 255);
        CHECK(std::any_of(output.begin(), output.end(), [](auto v) { return v > 0 && v < 255; }));
        CHECK(rotated->equivalent(*source->rotated(angle, pivot)));
        CHECK(rotated->equivalent(*source->rotated(angle + 360.0, pivot)));
    }
    SelectionState preview;
    for (int angle = 1; angle <= 30; ++angle) preview = source->rotated(angle, pivot);
    CHECK(preview->equivalent(*source->rotated(30, pivot)));
    CHECK(source->rotated(0, pivot)->equivalent(*source));
    CHECK(source->rotated(360, pivot)->equivalent(*source));
    CHECK(source->rotated(-720, pivot)->equivalent(*source));
    CHECK(read(*source) == original);

    const auto partial = SelectionMask::rectangle(extent, {28, 35, 31, 19}, 83)->rotated(23, pivot);
    const auto coverage = read(*partial);
    CHECK(*std::max_element(coverage.begin(), coverage.end()) == 83);
    CHECK(std::any_of(coverage.begin(), coverage.end(), [](auto v) { return v > 0 && v < 83; }));
    const auto empty = SelectionMask::filled(extent, 0);
    CHECK(empty->rotated(45, {48, 48})->bounds().empty());
}

void filteredRotationMatchesScalarReferenceAcrossTilesAndCanvasEdges()
{
    const Extent2u extent {263, 259};
    auto source = SelectionMask::rectangle(extent, {0, 0, 263, 259}, 211);
    source = source->combined(*SelectionMask::rectangle(extent, {121, 127, 41, 83}, 99),
        SelectionOperation::Subtract);
    const auto bytes = read(*source);
    for (const auto pivot : {Vec2d {131.5, 129.5}, Vec2d {-17, 23.5}}) {
        for (const double angle : {-137.25, -1.0, 0.25, 45.0, 179.5}) {
            const auto actual = read(*source->rotated(angle, pivot));
            const auto radians = angle * std::numbers::pi / 180.0;
            const auto c = std::cos(radians), s = std::sin(radians);
            for (int y = 0; y < int(extent.height); ++y) for (int x = 0; x < int(extent.width); ++x) {
                const double dx = x + 0.5 - pivot.x, dy = y + 0.5 - pivot.y;
                const double sx = pivot.x + c * dx + s * dy - 0.5;
                const double sy = pivot.y - s * dx + c * dy - 0.5;
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const double fx = sx - ix, fy = sy - iy;
                const double top = at(bytes, extent, ix, iy) * (1-fx) + at(bytes, extent, ix+1, iy) * fx;
                const double bottom = at(bytes, extent, ix, iy+1) * (1-fx) + at(bytes, extent, ix+1, iy+1) * fx;
                CHECK(actual[std::size_t(y) * extent.width + unsigned(x)]
                    == std::uint8_t(std::lround(top * (1-fy) + bottom * fy)));
            }
        }
    }
    CHECK(source->rotated(45, {1e9, -1e9})->bounds().empty());
    bool rejected = false;
    try { (void)source->rotated(std::numeric_limits<double>::infinity(), {0, 0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

void manipulationUsesExistingHistoryAndCancelledPreviewsPreserveRedo()
{
    const Extent2u extent {64, 64};
    Document document(CanvasSpec {.extent = extent});
    History history;
    auto surface = std::make_shared<ContiguousRasterSurface>(extent, Rgba8 {21, 55, 90, 200});
    auto layer = Layer::raster("Unchanged pixels", surface);
    const auto layerId = layer.id;
    const auto transform = layer.localToDocument;
    CHECK(document.insertLayer(0, std::move(layer)));
    const auto contentRevision = document.revision();
    const auto rasterRevision = surface->revision();
    const auto initial = SelectionMask::rectangle(extent, {12, 12, 16, 12});
    CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(initial)));
    CHECK(history.undo(document));
    CHECK(!document.selection());
    CHECK(history.redo(document));
    CHECK(document.selection()->equivalent(*initial));

    const std::array actions {initial->translated(3, -2), initial->translated(3, -2)->adjusted(2, -1),
        initial->translated(3, -2)->adjusted(2, -1)->rotated(19, {23, 16})};
    for (const auto& after : actions)
        CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(after, "Manipulate selection")));
    CHECK(history.undoDepth() == 4);
    for (int step = 2; step >= 0; --step) {
        CHECK(history.undo(document));
        CHECK(document.selection()->equivalent(*(step ? actions[std::size_t(step - 1)] : initial)));
    }
    CHECK(history.redoDepth() == 3);
    const auto memory = history.memoryUsed();
    const auto selectionRevision = document.selectionRevision();
    const auto before = document.selection();
    // Preview and cancellation deliberately never publish a new document mask.
    auto cancelled = before->translated(-30, 0);
    CHECK(cancelled->bounds().empty());
    cancelled = before->rotated(47, boundsCenter(before));
    CHECK(!cancelled->equivalent(*before));
    cancelled.reset();
    CHECK(document.selection() == before);
    CHECK(document.selectionRevision() == selectionRevision);
    CHECK(history.memoryUsed() == memory);
    CHECK(!history.execute(document, std::make_unique<SetSelectionCommand>(before->translated(0, 0))));
    CHECK(!history.execute(document, std::make_unique<SetSelectionCommand>(before->adjusted(0, 0))));
    CHECK(!history.execute(document, std::make_unique<SetSelectionCommand>(before->rotated(360, boundsCenter(before)))));
    CHECK(history.undoDepth() == 1);
    CHECK(history.redoDepth() == 3);
    for (const auto& after : actions) {
        CHECK(history.redo(document));
        CHECK(document.selection()->equivalent(*after));
    }
    CHECK(history.undo(document));
    CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(initial->translated(-2, 3))));
    CHECK(history.redoDepth() == 0);
    CHECK(document.revision() == contentRevision);
    CHECK(surface->revision() == rasterRevision);
    CHECK(document.layer(layerId)->localToDocument == transform);
    CHECK(std::get<RasterLayer>(document.layer(layerId)->payload).surface == surface);
}
} // namespace

int main()
{
    moveHitsActualCoverageAndLatchesOnlyTheUnmodifiedReplaceMode();
    translationIsExactClippedAndAlwaysDerivesFromTheGestureBaseline();
    translatedPreviewMatchesIndependentPixelBoundaryIncludingNewCanvasEdges();
    fractionalMovePreviewPreservesContoursAndClipsAtTheCanvas();
    morphologyAmountsArePerSideAndAxesAreDocumentSpace();
    morphologyMatchesIndependentR8ReferenceForEverySignAndCanvasEdge();
    holesAndDisconnectedRegionsUseCoverageMorphologyNotBoundsScaling();
    tileSeamsDoNotChangeManipulationResults();
    quarterTurnsAreLosslessAroundPixelAlignedAndOffsetPivots();
    arbitraryRotationRetainsCoverageAndUsesTheOriginalForRepeatedSteps();
    filteredRotationMatchesScalarReferenceAcrossTilesAndCanvasEdges();
    manipulationUsesExistingHistoryAndCancelledPreviewsPreserveRedo();
    if (failures) {
        std::cerr << failures << " selection manipulation test failures\n";
        return 1;
    }
    std::cout << "All selection manipulation tests passed\n";
    return 0;
}
