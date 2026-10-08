#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/RegionFinder.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) { std::cerr << "FAIL " << line << ": " << expression << '\n'; ++failures; }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

template<class Exception, class Function> void throws(Function&& function)
{
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}

SmartReferenceImage solid(Extent2u extent, Rgba8 color)
{
    const auto size = std::size_t(extent.width) * extent.height;
    return {extent, std::vector<Rgba8>(size, color), std::vector<std::uint8_t>(size, 1)};
}
std::size_t index(const SmartReferenceImage& image, int x, int y)
{ return std::size_t(y) * image.extent.width + std::size_t(x); }
std::uint8_t at(const SelectionState& selection, int x, int y)
{ return selection ? selection->coverageAtDocumentPixel(x, y) : 0; }
bool same(const SelectionState& a, const SelectionState& b)
{ return (!a && !b) || (a && b && a->equivalent(*b)); }
std::size_t selected(const SelectionState& selection)
{
    if (!selection) return 0;
    std::size_t count = 0;
    const auto bounds = selection->bounds();
    for (int y = bounds.y; y < bounds.bottom(); ++y)
        for (int x = bounds.x; x < bounds.right(); ++x) count += at(selection, x, y) >= 128;
    return count;
}
SmartSelectionResult run(const SmartReferenceImage& image, std::span<const QuickHintDab> dabs, QuickSelectionHints hints = {}, SelectionState original = {},
    SelectionOperation operation = SelectionOperation::Replace)
{
    const std::atomic_bool cancelled {false};
    return buildQuickSelection(image, dabs, std::move(hints), std::move(original), operation, cancelled);
}
SmartSelectionResult dot(const SmartReferenceImage& image, double x, double y, double radius = 3, QuickSelectionHints hints = {}, SelectionState original = {},
    SelectionOperation operation = SelectionOperation::Replace)
{
    const std::array dabs {QuickHintDab {{x, y}, radius}};
    return run(image, dabs, std::move(hints), std::move(original), operation);
}
NormalizedPointerSample sample(Vec2d p, std::uint64_t time, std::uint32_t modifiers = 0)
{
    return {.documentPosition = p, .timestampMicroseconds = time, .pressure = 1,
        .pointerType = PointerType::Mouse, .buttons = PointerButtonPrimary, .modifiers = modifiers};
}
std::vector<QuickHintDab> line(Vec2d first, Vec2d last, double diameter, int events,
    double zoom = 1, std::uint32_t modifiers = 0)
{
    QuickSelectionPath path;
    ViewportState viewport;
    viewport.setZoom(zoom); viewport.setPan({17, -23});
    const auto normalized = [&](Vec2d p, std::uint64_t time) {
        const auto screen = viewport.documentToViewport(p, {256, 192}, {800, 600});
        return sample(viewport.viewportToDocument(screen, {256, 192}, {800, 600}), time, modifiers);
    };
    CHECK(path.begin(diameter, normalized(first, 0)));
    for (int i = 1; i < events; ++i) {
        const double t = double(i) / events;
        (void)path.append(normalized({first.x + (last.x - first.x) * t,
            first.y + (last.y - first.y) * t}, std::uint64_t(i) * 1000));
    }
    CHECK(path.end(normalized(last, std::uint64_t(events) * 1000)));
    return path.dabs();
}

// Smooth multicolor appearance with reproducible fine texture. A seed-relative
// flood fill at tolerance 30 cannot cover both ends, although the outer edge is
// much stronger than all interior changes. The brush teaches both appearances.
SmartReferenceImage multicolor(Extent2u extent, RectI subject)
{
    auto image = solid(extent, {8, 20, 85, 255});
    for (int y = subject.y; y < subject.bottom(); ++y) for (int x = subject.x; x < subject.right(); ++x) {
        const double t = double(x - subject.x) / std::max(1, subject.width - 1);
        const int texture = ((x / 3 + y / 3) % 2) * 8;
        image.pixels[index(image, x, y)] = {std::uint8_t(170 + int(t * 75) + texture),
            std::uint8_t(65 + int(t * 85) + texture), 20, 255};
    }
    return image;
}

void multicolorAndFloodFillComparison()
{
    const RectI object {24, 24, 120, 72};
    const auto image = multicolor({192, 128}, object);
    const auto dabs = line({38.5, 60.5}, {129.5, 60.5}, 6, 2);
    const auto result = run(image, dabs);
    CHECK(result.incoming && result.combined);
    CHECK(same(result.incoming, result.combined));
    std::size_t recovered = 0, leaked = 0;
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 192; ++x) {
        const bool inside = x >= object.x && x < object.right() && y >= object.y && y < object.bottom();
        if (at(result.combined, x, y) >= 128) (inside ? recovered : leaked)++;
    }
    CHECK(recovered > std::size_t(object.width * object.height) * 95 / 100);
    CHECK(leaked < 32);
    CHECK(at(result.combined, 80, 30) >= 128); // Thirty pixels beyond the brush.
    RegionFinder flood(image.extent, 38, 60, 30, [&](int x, int y) -> std::optional<Rgba8> {
        return image.pixels[index(image, x, y)];
    });
    while (!flood.step(4096)) {}
    CHECK(flood.pixelCount() < recovered * 3 / 4);
    CHECK(!flood.contains(130, 40) && at(result.combined, 130, 40) >= 128);
    CHECK(result.hints.foreground && !result.hints.background);
    CHECK(selected(result.hints.foreground) < selected(result.combined) / 5);
    CHECK(at(result.hints.foreground, 80, 30) == 0); // Inference remains unknown evidence.
    CHECK(!result.combined->boundaryEdges().empty());
    std::cout << "multicolor: quick=" << recovered << " subject=8640 flood_tolerance30="
        << flood.pixelCount() << " leaked=" << leaked << '\n';
}

void shortInputOnTexturedRegion()
{
    auto image=solid({240,180},{190,60,80,255});
    for(int y=20;y<160;++y)for(int x=24;x<216;++x) {
        const auto noise=std::uint8_t((unsigned(x)*1664525u+unsigned(y)*1013904223u)%5);
        image.pixels[index(image,x,y)]={std::uint8_t(8+noise),std::uint8_t(16+noise),std::uint8_t(22+noise),255};
    }
    // A brief mark in textured dark material must not choose its imposed
    // zero-cost footprint as the image boundary. No source-specific oracle.
    const auto result=dot(image,120.5,90.5,12);
    CHECK(selected(result.combined)>192*140*95/100);
    CHECK(at(result.combined,30,30)==255);
    CHECK(at(result.combined,20,90)==0);
    CHECK(!result.stats.limitedGrowth);
}

void thinFeaturesHolesAndDisconnectedObjects()
{
    auto image = solid({224, 144}, {4, 25, 90, 255});
    const Rgba8 gold {240, 165, 15, 255};
    for (int y = 20; y < 116; ++y) for (int x = 16; x < 120; ++x) {
        if (x >= 56 && x < 78 && y >= 54 && y < 78) continue;
        image.pixels[index(image, x, y)] = gold;
    }
    for (int x = 120; x < 150; ++x) image.pixels[index(image, x, 43)] = gold;
    for (int y = 36; y < 56; ++y) for (int x = 163; x < 183; ++x)
        image.pixels[index(image, x, y)] = gold;
    const std::array dabs {QuickHintDab {{38.5, 43.5}, 3}, QuickHintDab {{113.5, 43.5}, 3},
        QuickHintDab {{90.5, 96.5}, 3}};
    const auto result = run(image, dabs);
    CHECK(at(result.combined, 67, 65) == 0); // Hole is background without a closing operation.
    CHECK(at(result.combined, 51, 65) >= 128 && at(result.combined, 84, 65) >= 128);
    for (int x = 120; x < 149; ++x) CHECK(at(result.combined, x, 43) >= 128);
    CHECK(at(result.combined, 138, 41) == 0 && at(result.combined, 138, 45) == 0);
    CHECK(at(result.combined, 172, 44) == 0); // Matching color across a contrasting gap.
    const auto added = dot(image, 172.5, 44.5, 3, result.hints, result.combined, SelectionOperation::Add);
    CHECK(at(added.combined, 172, 44) == 255 && at(added.combined, 138, 43) >= 128);
    CHECK(at(added.combined, 67, 65) == 0);
}

void brushFirstLocalityAndCanvasEdge()
{
    const auto image = solid({384, 256}, {110, 110, 110, 255});
    const auto result = dot(image, 180.5, 128.5, 3);
    // With no visible boundary the connected region is the entire valid image,
    // not the old Reach disk. Artificial compute boundaries must disappear.
    CHECK(selected(result.combined) == 384 * 256);
    CHECK(result.combined->bounds() == RectI({0, 0, 384, 256}));
    CHECK(!result.hints.background); // Unknown canvas and its border never become hard BG.

    auto longImage = solid({1024, 384}, {5, 20, 90, 255});
    for (int y = 70; y < 102; ++y) for (int x = 22; x < 880; ++x)
        longImage.pixels[index(longImage, x, y)] = {230, 130, 30, 255};
    for (int y = 102; y < 310; ++y) for (int x = 848; x < 880; ++x)
        longImage.pixels[index(longImage, x, y)] = {230, 130, 30, 255};
    const auto longRegion = dot(longImage, 42.5, 84.5, 2);
    CHECK(at(longRegion.combined, 870, 295) == 255);
    CHECK(at(longRegion.combined, 840, 295) == 0);
    CHECK(at(longRegion.combined, 500, 150) == 0);
    CHECK(longRegion.stats.workRegion.width > 800);
    CHECK(longRegion.combined->bounds() == RectI({22, 70, 858, 240}));

    auto edgeImage = solid({96, 80}, {0, 0, 100, 255});
    for (int y = 0; y < 40; ++y) for (int x = 0; x < 42; ++x)
        edgeImage.pixels[index(edgeImage, x, y)] = {240, 165, 15, 255};
    const auto edge = dot(edgeImage, 7.5, 8.5, 3);
    CHECK(at(edge.combined, 0, 0) == 255 && at(edge.combined, 0, 30) >= 128);
    CHECK(at(edge.combined, 31, 0) >= 128 && at(edge.combined, 70, 5) == 0);
}

void correctionsAndHintSnapshots()
{
    const auto image = solid({160, 120}, {140, 140, 140, 255});
    const auto first = dot(image, 44.5, 60.5, 4);
    const auto corrected = dot(image, 78.5, 60.5, 6,
        first.hints, first.combined, SelectionOperation::Subtract);
    CHECK(at(corrected.combined, 78, 60) == 0 && at(corrected.hints.background, 78, 60) == 255);
    CHECK(at(corrected.hints.foreground, 44, 60) == 255);
    const auto refined = dot(image, 55.5, 42.5, 3,
        corrected.hints, corrected.combined, SelectionOperation::Add);
    CHECK(at(refined.combined, 78, 60) == 0 && at(refined.hints.background, 78, 60) == 255);
    CHECK(at(refined.combined, 44, 60) == 255 && at(refined.hints.foreground, 44, 60) == 255);
    const auto override = dot(image, 78.5, 60.5, 2,
        refined.hints, refined.combined, SelectionOperation::Add);
    CHECK(at(override.combined, 78, 60) == 255 && at(override.hints.background, 78, 60) == 0);
    CHECK(at(override.hints.foreground, 78, 60) == 255);
    CHECK(at(override.hints.background, 74, 60) == 255); // Only deliberate overlap clears BG.

    // The core receives immutable history snapshots. Restoring the pre-correction
    // snapshot must reproduce a branch in which the undone correction never ran.
    const auto afterUndo = dot(image, 55.5, 42.5, 3,
        first.hints, first.combined, SelectionOperation::Add);
    const auto firstAgain = dot(image, 44.5, 60.5, 4);
    const auto cleanBranch = dot(image, 55.5, 42.5, 3,
        firstAgain.hints, firstAgain.combined, SelectionOperation::Add);
    CHECK(same(afterUndo.combined, cleanBranch.combined));
    CHECK(!afterUndo.hints.background && at(afterUndo.combined, 78, 60) >= 128);
    const auto afterRedo = dot(image, 55.5, 42.5, 3,
        corrected.hints, corrected.combined, SelectionOperation::Add);
    CHECK(same(afterRedo.combined, refined.combined));
    CHECK(same(afterRedo.hints.background, refined.hints.background));
    CHECK(at(first.combined, 78, 60) == 255 && !first.hints.background);

    const auto replacement = dot(image, 18.5, 18.5, 3,
        corrected.hints, corrected.combined, SelectionOperation::Replace);
    CHECK(!replacement.hints.background && at(replacement.hints.foreground, 44, 60) == 0);
    CHECK(at(replacement.combined, 78, 60) == 255); // Replace reselects this flat connected region.
}

void weakBoundaryWithBackgroundEvidence()
{
    auto image = solid({160, 120}, {95, 100, 105, 255});
    for (int y = 25; y < 95; ++y) for (int x = 25; x < 85; ++x)
        image.pixels[index(image, x, y)] = {125, 130, 135, 255};
    const auto first = dot(image, 52.5, 60.5, 4);
    const auto corrected = dot(image, 101.5, 60.5, 4,
        first.hints, first.combined, SelectionOperation::Subtract);
    CHECK(at(corrected.combined, 101, 60) == 0);
    const auto refined = dot(image, 60.5, 45.5, 3,
        corrected.hints, corrected.combined, SelectionOperation::Add);
    CHECK(at(refined.combined, 52, 60) == 255 && at(refined.combined, 101, 60) == 0);
    CHECK(at(refined.hints.background, 101, 60) == 255);
}

void transparentRgbAndValidExtents()
{
    auto a = solid({128, 96}, {255, 0, 0, 0});
    auto b = solid({128, 96}, {0, 230, 90, 0});
    for (int y = 10; y < 86; ++y) for (int x = 48; x < 84; ++x) {
        a.pixels[index(a, x, y)] = {255, 100, 0, 255};
        b.pixels[index(b, x, y)] = {255, 100, 0, 255};
    }
    const auto transparentA = dot(a, 18.5, 40.5, 3);
    const auto transparentB = dot(b, 18.5, 40.5, 3);
    CHECK(same(transparentA.combined, transparentB.combined));
    CHECK(at(transparentA.combined, 65, 45) == 0);
    const auto opaque = dot(a, 65.5, 45.5, 3);
    CHECK(at(opaque.combined, 65, 20) == 255 && at(opaque.combined, 38, 45) == 0);
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 128; ++x)
        a.valid[index(a, x, y)] = std::uint8_t(x >= 10 && x < 40 && y >= 12 && y < 83);
    const auto cropped = dot(a, 22.5, 40.5, 3);
    CHECK(at(cropped.combined, 12, 20) == 255);
    CHECK(at(cropped.combined, 9, 40) == 0 && at(cropped.combined, 40, 40) == 0);
    CHECK(at(cropped.combined, 22, 11) == 0 && at(cropped.combined, 22, 83) == 0);
    CHECK(!dot(a, 90.5, 40.5).incoming);
}

void ratesZoomAndModifierResampling()
{
    const auto image = multicolor({192, 128}, {24, 24, 120, 72});
    const auto sparse = line({38.5, 39.5}, {129.5, 81.5}, 8, 1);
    const auto expected = run(image, sparse);
    for (int events : {7, 113}) for (double zoom : {.125, 1., 8.}) {
        const auto dense = line({38.5, 39.5}, {129.5, 81.5}, 8, events, zoom, PointerModifierShift);
        CHECK(dense.size() == sparse.size());
        if (dense.size() == sparse.size()) for (std::size_t i = 0; i < sparse.size(); ++i) {
            CHECK(std::abs(dense[i].center.x - sparse[i].center.x) < 1e-8);
            CHECK(std::abs(dense[i].center.y - sparse[i].center.y) < 1e-8);
            CHECK(dense[i].radius == sparse[i].radius);
        }
        const auto result = run(image, dense);
        CHECK(same(result.combined, expected.combined));
        CHECK(same(result.hints.foreground, expected.hints.foreground));
    }
}

void combinationCancellationAndHistory()
{
    const auto image = solid({128, 96}, {150, 100, 40, 255});
    const auto original = SelectionMask::rectangle(image.extent, {0, 0, 50, 96});
    const std::array dabs {QuickHintDab {{45.5, 40.5}, 3}};
    for (const auto operation : {SelectionOperation::Replace, SelectionOperation::Add,
        SelectionOperation::Subtract, SelectionOperation::Intersect}) {
        const auto result = run(image, dabs, {}, original, operation);
        CHECK(same(result.combined, combineSelection(original, result.incoming, operation)));
    }
    const std::atomic_bool cancelled {true};
    const auto cancelledResult = buildQuickSelection(image, dabs, {}, original,
        SelectionOperation::Add, cancelled);
    CHECK(!cancelledResult.incoming && !cancelledResult.combined);
    CHECK(!cancelledResult.hints.foreground && !cancelledResult.hints.background);
    CHECK(original->bounds() == RectI({0, 0, 50, 96}));
    CHECK(!run(image, {}).incoming);
    const auto small = run(image, dabs, {}, original, SelectionOperation::Add);
    (void)run(image, dabs, {}, original, SelectionOperation::Add);
    CHECK(same(small.combined, run(image, dabs, {}, original, SelectionOperation::Add).combined));

    EditorSession session;
    session.replaceDocument(std::make_unique<Document>(CanvasSpec {image.extent}));
    session.document()->markSaved();
    const auto content = session.document()->contentState();
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(original)));
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(small.combined)));
    CHECK(session.undo() && same(session.document()->selection(), original));
    CHECK(session.history().canRedo());
    CHECK(!session.execute(std::make_unique<SetSelectionCommand>(original)));
    CHECK(session.history().canRedo()); // No-op preserves redo.
    CHECK(session.redo() && same(session.document()->selection(), small.combined));
    CHECK(!session.document()->isModified() && session.document()->contentState() == content);
    // UI automatic hint invalidation/restore is covered by interaction tests;
    // this test establishes the core immutable-snapshot and history contracts.
}

void referenceRevisionAndReadOnlySnapshot()
{
    Document document({{24, 20}});
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u {16, 12}, Rgba8 {250, 50, 10, 255});
    auto layer = Layer::raster("Rendered reference", surface);
    const auto id = layer.id;
    layer.localToDocument = {.m02 = 3, .m12 = 4};
    layer.opacity = .5f;
    CHECK(document.insertLayer(0, std::move(layer)));
    document.markSaved();
    const auto revision = document.revision();
    const auto surfaceRevision = surface->revision();
    SmartSelectionReference reference(document, id, ColorSampleSource::ActiveLayer);
    CHECK(!reference.image() && reference.matches(document));
    while (!reference.step(13)) {}
    const auto image = reference.image();
    CHECK(image && reference.matches(document));
    CHECK(image->valid[index(*image, 3, 4)] && !image->valid[index(*image, 1, 1)]);
    CHECK(image->pixels[index(*image, 8, 8)].alpha >= 127 && image->pixels[index(*image, 8, 8)].alpha <= 128);
    CHECK(document.revision() == revision && surface->revision() == surfaceRevision && !document.isModified());
    CHECK(!document.selection());
    CHECK(document.setSelection(SelectionMask::rectangle(image->extent, {2, 2, 10, 10})));
    CHECK(reference.matches(document)); // Selection changes are not source changes.
    CHECK(document.setLayerOpacity(id, .75f));
    CHECK(!reference.matches(document));
    SmartSelectionReference fresh(document, id, ColorSampleSource::ActiveLayer);
    CHECK(fresh.step(32) == false);
    const std::array pixel {std::byte(10), std::byte(220), std::byte(30), std::byte(255)};
    (void)surface->replaceRgba8({0, 0, 1, 1}, pixel, 4);
    CHECK(!fresh.matches(document));
    // Completed snapshots remain coherent and independently usable after edits.
    CHECK(image->pixels[index(*image, 3, 4)].red == 250);
    CHECK(dot(*image, 8.5, 8.5, 2).combined != nullptr);
}

void invalidRequests()
{
    auto image = solid({32, 32}, {10, 20, 30, 255});
    const std::array invalid {QuickHintDab {{std::numeric_limits<double>::quiet_NaN(), 1}, 3}};
    throws<std::invalid_argument>([&] { (void)run(image, invalid); });
    const std::array radius {QuickHintDab {{4, 4}, 0}};
    throws<std::invalid_argument>([&] { (void)run(image, radius); });
    const std::array dab {QuickHintDab {{4, 4}, std::numeric_limits<double>::quiet_NaN()}};
    throws<std::invalid_argument>([&] {
        (void)run(image, dab);
    });
    const std::vector<QuickHintDab> excessive(16385, QuickHintDab {{4, 4}, 3});
    throws<std::length_error>([&] { (void)run(image, excessive); });
    image.valid.pop_back();
    throws<std::invalid_argument>([&] { (void)dot(image, 5, 5); });
    CHECK(!dot(solid({32, 32}, {}), -100, -100).incoming);
}

void persistentBasinsAndThinEdgeRefinement()
{
    const std::atomic_bool cancelled {false};
    const auto pick = [&](const SmartReferenceImage& image, Vec2d seed, double sensitivity) {
        const std::array dabs {QuickHintDab {seed, 1.1}};
        return buildQuickSelection(image, dabs, {}, {}, SelectionOperation::Replace, cancelled,
            {.edgeSensitivity = sensitivity});
    };
    // A stable shaded object adjoins a smaller, visibly different neighbor.
    // The biggest later escape encloses both; it is not the intended first basin.
    for (const unsigned alpha : {255U, 128U}) {
        auto image = solid({96, 72}, {230, 230, 230, std::uint8_t(alpha)});
        for (int y=12; y<60; ++y) for (int x=10; x<66; ++x) {
            const auto value = std::uint8_t(x < 50 ? 80 + (x-10)/4 : 112);
            image.pixels[index(image,x,y)] = {value,value,value,std::uint8_t(alpha)};
        }
        const auto result=pick(image,{25.5,36.5},.4);
        CHECK(at(result.combined,12,15)==255 && at(result.combined,47,56)==255);
        CHECK(at(result.combined,55,35)==0 && at(result.combined,5,35)==0);
        // Reversing scan/edge directions changes neither region nor coverage.
        auto mirrored=image;
        for(int y=0;y<72;++y)for(int x=0;x<96;++x)
            mirrored.pixels[index(mirrored,x,y)]=image.pixels[index(image,95-x,y)];
        const auto reversed=pick(mirrored,{70.5,36.5},.4);
        for(int y=0;y<72;++y)for(int x=0;x<96;++x)
            CHECK(at(result.combined,x,y)==at(reversed.combined,95-x,y));
    }
    for (const double sensitivity : {0.0,.4,1.0}) {
        // Wider samples see different colors across this dark rim, but must not
        // create a false edge ALONG its constant two-pixel interior.
        auto rim=solid({80,64},{150,110,165,255});
        for(int y=0;y<64;++y)for(int x=39;x<80;++x)
            rim.pixels[index(rim,x,y)]=x<41?Rgba8{20,20,20,255}:Rgba8{230,220,210,255};
        const auto result=pick(rim,{40.0,32.5},sensitivity);
        CHECK(at(result.combined,39,2)==255 && at(result.combined,40,61)==255);
        CHECK(at(result.combined,37,32)==0 && at(result.combined,43,32)==0);
        // A one-pixel-wide document exercises vertical cached-edge indexing.
        const auto column=pick(solid({1,32},{80,90,100,255}),{.5,15.5},sensitivity);
        CHECK(selected(column.combined)==32);
        auto tiny=solid({16,16},{15,20,30,255});
        for(int y=6;y<9;++y)for(int x=6;x<9;++x)
            tiny.pixels[index(tiny,x,y)]={240,230,220,255};
        CHECK(pick(tiny,{7.5,7.5},sensitivity).combined->bounds()==RectI({6,6,3,3}));
        // Invalid reference samples are not part of the halo, regardless of RGB.
        auto clipped=solid({32,32},{80,90,100,255});
        for(int y=0;y<32;++y)for(int x=0;x<32;++x)
            clipped.valid[index(clipped,x,y)]=(x>=7&&x<24&&y>=4&&y<28);
        const auto before=pick(clipped,{15.5,15.5},sensitivity);
        for(std::size_t i=0;i<clipped.pixels.size();++i)
            if(!clipped.valid[i])clipped.pixels[i]={255,0,255,255};
        CHECK(same(before.combined,pick(clipped,{15.5,15.5},sensitivity).combined));
        CHECK(before.combined->bounds()==RectI({7,4,17,24}));
    }
    const auto image=solid({16,16},{100,110,120,255});
    for(double invalid : {-1.0,1.1,std::numeric_limits<double>::quiet_NaN()})
        throws<std::invalid_argument>([&]{(void)pick(image,{8.5,8.5},invalid);});
}

void benchmark()
{
    using Clock = std::chrono::steady_clock;
    std::cout << "benchmark: warm immutable source, local solver plus mask/boundary creation; "
        "reference allocation is separately timed; RSS is process peak\n";
    for (const Extent2u extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}}) {
        const int cx = int(extent.width / 2), cy = int(extent.height / 2);
        const auto sourceStart = Clock::now();
        const auto image = multicolor(extent, {cx - 130, cy - 100, 260, 200});
        const double sourceMs = std::chrono::duration<double, std::milli>(Clock::now() - sourceStart).count();
        const auto dabs = line({double(cx - 105) + .5, double(cy) + .5},
            {double(cx + 105) + .5, double(cy) + .5}, 24, 48);
        std::array<double, 3> milliseconds {};
        SmartSelectionResult result;
        for (auto& elapsed : milliseconds) {
            const auto start = Clock::now();
            result = run(image, dabs);
            elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
        const auto correctionStart = Clock::now();
        const auto correction = dot(image, double(cx + 110) + .5, double(cy + 65) + .5, 8,
            result.hints, result.combined, SelectionOperation::Subtract);
        const double correctionMs = std::chrono::duration<double, std::milli>(Clock::now() - correctionStart).count();
        const auto refineStart = Clock::now();
        const auto refined = dot(image, double(cx - 60) + .5, double(cy - 45) + .5, 8,
            correction.hints, correction.combined, SelectionOperation::Add);
        const double refineMs = std::chrono::duration<double, std::milli>(Clock::now() - refineStart).count();
        const auto floodStart = Clock::now();
        RegionFinder flood(extent, cx - 105, cy, 30, [&](int x, int y) -> std::optional<Rgba8> {
            return image.pixels[index(image, x, y)];
        });
        while (!flood.step(65536)) {}
        const double floodMs = std::chrono::duration<double, std::milli>(Clock::now() - floodStart).count();
        long peakRssKiB = 0;
#if defined(__linux__)
        rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) == 0) peakRssKiB = usage.ru_maxrss;
#endif
        std::sort(milliseconds.begin(), milliseconds.end());
        std::cout << std::fixed << std::setprecision(2) << extent.width << 'x' << extent.height
            << " reference_ms=" << sourceMs << " reference_MiB="
            << double(image.pixels.capacity() * sizeof(Rgba8) + image.valid.capacity()) / (1024 * 1024)
            << " quick_min_median_max_ms=" << milliseconds[0] << ',' << milliseconds[1] << ',' << milliseconds[2]
            << " subtract_ms=" << correctionMs << " add_refine_ms=" << refineMs
            << " evaluated=" << result.stats.evaluatedPixels << " queue_pops=" << result.stats.queuePops
            << " solver_estimated_MiB=" << double(result.stats.workspaceBytes) / (1024 * 1024)
            << " incoming_mask_MiB=" << double(result.incoming->memoryCost()) / (1024 * 1024)
            << " selected=" << selected(result.combined) << " corrected_selected=" << selected(refined.combined)
            << " flood_ms=" << floodMs << " flood_selected=" << flood.pixelCount()
            << " flood_MiB=" << double(flood.memoryBytes()) / (1024 * 1024)
            << " process_peak_RSS_MiB=" << double(peakRssKiB) / 1024 << '\n';
        CHECK(result.stats.evaluatedPixels < 1'000'000 && selected(result.combined) > flood.pixelCount());
    }
}
void rejectedBridgeCannotSeedDisconnectedIsland()
{
    auto image=solid({96,48},{220,20,20,255});
    for(int y=0;y<48;++y)image.pixels[index(image,46,y)]={80,20,20,255};
    const auto result=dot(image,40.5,24.5,2);
    CHECK(at(result.incoming,44,24)>128);
    CHECK(at(result.incoming,46,24)<128);
    CHECK(at(result.incoming,50,24)==0);
    CHECK(at(result.incoming,64,24)==0);
}
void uniformBenchmark()
{
    for (const auto extent : {Extent2u {3840, 2160}, Extent2u {5120, 2880}}) {
        const auto image = solid(extent, {120, 130, 140, 255});
        const auto start = std::chrono::steady_clock::now();
        const auto result = dot(image, double(extent.width / 2) + .5, double(extent.height / 2) + .5, 3);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        CHECK(selected(result.combined) == std::size_t(extent.width) * extent.height);
        long peakRssKiB = 0;
#if defined(__linux__)
        rusage usage {};
        if (getrusage(RUSAGE_SELF, &usage) == 0) peakRssKiB = usage.ru_maxrss;
#endif
        std::cout << extent.width << 'x' << extent.height << " uniform_expanding_ms=" << elapsed
            << " evaluated=" << result.stats.evaluatedPixels << " workspace_MiB="
            << double(result.stats.workspaceBytes) / (1024 * 1024)
            << " process_peak_RSS_MiB=" << double(peakRssKiB) / 1024 << '\n';
    }
}
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") benchmark();
    else if (argc > 1 && std::string_view(argv[1]) == "--benchmark-uniform") uniformBenchmark();
    else {
        persistentBasinsAndThinEdgeRefinement();
        rejectedBridgeCannotSeedDisconnectedIsland();
        multicolorAndFloodFillComparison();
        shortInputOnTexturedRegion();
        thinFeaturesHolesAndDisconnectedObjects();
        brushFirstLocalityAndCanvasEdge();
        correctionsAndHintSnapshots();
        weakBoundaryWithBackgroundEvidence();
        transparentRgbAndValidExtents();
        ratesZoomAndModifierResampling();
        combinationCancellationAndHistory();
        referenceRevisionAndReadOnlySnapshot();
        invalidRequests();
    }
    if (failures) std::cerr << failures << " Quick Selection failure(s)\n";
    else std::cout << "Quick Selection tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
