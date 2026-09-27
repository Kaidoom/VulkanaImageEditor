#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/CropGeometry.hpp"
#include "imageeditor/core/LayerCropSession.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace c = imageeditor::core;
namespace {
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        if (failures < 60) std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
void close(double actual, double expected, double tolerance = 1e-8)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        if (failures < 60) std::cerr << "Value mismatch: " << actual << " vs " << expected << '\n';
        ++failures;
    }
}

c::LayerCrop crop(c::RectD frame, std::array<double, 4> corners)
{
    c::LayerCrop result {frame}; result.corners = corners; return result;
}
double cross(c::Vec2d a, c::Vec2d b) { return a.x * b.y - a.y * b.x; }

// Independent double-precision oracle: clip the manually constructed crop
// boundary against the four oriented pixel edges. Production clips the pixel
// against eight crop planes and computes area with a cyclic sum; this oracle
// uses the opposite clipping direction and a triangle fan. It calls no
// shipping crop geometry, coverage or containment helper.
double referenceCoverage(const c::LayerCrop& frame, c::Vec2d center,
    c::Vec2d dx, c::Vec2d dy)
{
    const double determinant = cross(dx, dy);
    if (frame.width <= 0 || frame.height <= 0 || determinant == 0) return 0;
    auto cuts = frame.corners;
    for (auto& cut : cuts) cut = std::min(std::max(0.0, cut), std::min(frame.width, frame.height) / 2);
    const double left = frame.x - center.x, top = frame.y - center.y;
    const double right = left + frame.width, bottom = top + frame.height;
    std::vector<c::Vec2d> polygon {{left + cuts[0], top}, {right - cuts[1], top},
        {right, top + cuts[1]}, {right, bottom - cuts[2]},
        {right - cuts[2], bottom}, {left + cuts[3], bottom},
        {left, bottom - cuts[3]}, {left, top + cuts[0]}};
    const std::array footprint {(dx + dy) * -.5, (dx - dy) * .5,
        (dx + dy) * .5, (dy - dx) * .5};
    const double orientation = determinant > 0 ? 1 : -1;
    for (std::size_t edgeIndex = 0; edgeIndex < footprint.size(); ++edgeIndex) {
        if (polygon.empty()) return 0;
        const auto origin = footprint[edgeIndex];
        const auto edge = footprint[(edgeIndex + 1) % footprint.size()] - origin;
        auto distance = [&](c::Vec2d p) { return orientation * cross(edge, p - origin); };
        std::vector<c::Vec2d> next;
        auto previous = polygon.back(); double previousDistance = distance(previous);
        for (const auto point : polygon) {
            const double currentDistance = distance(point);
            if ((previousDistance >= 0) != (currentDistance >= 0))
                next.push_back(previous + (point - previous) *
                    (previousDistance / (previousDistance - currentDistance)));
            if (currentDistance >= 0) next.push_back(point);
            previous = point; previousDistance = currentDistance;
        }
        polygon = std::move(next);
    }
    double twiceArea = 0;
    for (std::size_t i = 2; i < polygon.size(); ++i)
        twiceArea += cross(polygon[i - 1] - polygon[0], polygon[i] - polygon[0]);
    return std::clamp(std::abs(twiceArea) / (2 * std::abs(determinant)), 0.0, 1.0);
}

std::vector<std::byte> pixels(const c::RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4);
    surface.copyRgba8({0, 0, int(extent.width), int(extent.height)}, result,
        std::size_t(extent.width) * 4);
    return result;
}
struct Fixture {
    c::Document document {{{128, 128}, 96}};
    c::History history;
    std::shared_ptr<c::ContiguousRasterSurface> source;
    c::LayerId id;
    explicit Fixture(std::optional<c::LayerCrop> initial = {}, c::AffineTransform transform = {})
        : source(std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {24, 20}, c::Rgba8 {43, 81, 127, 191}))
    {
        auto layer = c::Layer::raster("Chamfer source", source);
        id = layer.id; layer.crop = initial; layer.localToDocument = transform;
        CHECK(document.insertLayer(0, std::move(layer))); document.markSaved();
    }
    std::optional<c::LayerCrop> current() const { return document.layer(id)->crop; }
};

void coverageMatchesIndependentOracle()
{
    const std::array cases {crop({.25, -.75, 13.5, 9.25}, {0, 0, 0, 0}),
        crop({.25, -.75, 13.5, 9.25}, {4.1, 0, 0, 0}),
        crop({.25, -.75, 13.5, 9.25}, {0, 3.25, 0, 0}),
        crop({.25, -.75, 13.5, 9.25}, {0, 0, 2.7, 0}),
        crop({.25, -.75, 13.5, 9.25}, {0, 0, 0, 1.8}),
        crop({.25, -.75, 13.5, 9.25}, {4.1, 3.25, 2.7, 1.8}),
        crop({.25, -.75, 13.5, 9.25}, {100, 100, 100, 100})};
    struct Footprint { c::Vec2d dx, dy; };
    const std::array<Footprint, 6> footprints {{{{1, 0}, {0, 1}},
        {{.8, .6}, {-.6, .8}}, {{1.2, .35}, {.6, .9}},
        {{-1.1, .2}, {.45, 1.3}}, {{.05, .1}, {-2.7, 1.5}},
        {{9, 2}, {1, 7}}}};
    std::mt19937 random(0xC4A4FEU);
    std::uniform_real_distribution<double> x(-5, 19), y(-5, 14);
    for (const auto& frame : cases) for (const auto footprint : footprints)
        for (int sample = 0; sample < 180; ++sample) {
            const c::Vec2d center {x(random), y(random)};
            close(c::layerCropCoverage(frame, center, footprint.dx, footprint.dy),
                referenceCoverage(frame, center, footprint.dx, footprint.dy), 6e-5);
        }
    const auto diamond = crop({0, 0, 8, 8}, {4, 4, 4, 4});
    close(c::layerCropCoverage(diamond, {4, 4}, {8, 0}, {0, 8}), .5);
    close(c::layerCropCoverage(diamond, {2.5, 1.5}, {1, 0}, {0, 1}), .5);
    close(c::layerCropCoverage(diamond, {1.5, 1.5}, {1, 0}, {0, 1}), 0);
    close(c::layerCropCoverage(diamond, {4, 4}, {0, 0}, {0, 1}), 0);
    close(c::layerCropCoverage(crop({0, 0, 0, 8}, {4, 4, 4, 4}), {0, 4}, {1, 0}, {0, 1}), 0);
    for (const double invalid : {-1.0, std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()}) {
        auto malformed = diamond; malformed.corners[2] = invalid;
        CHECK(!c::validLayerCrop(malformed));
        close(c::layerCropCoverage(malformed, {4, 4}, {1, 0}, {0, 1}), 0);
    }
}

void containmentBoundsAndBinaryEdges()
{
    const auto diamond = crop({0, 0, 8, 8}, {4, 4, 4, 4});
    CHECK(!diamond.contains({1, 1})); CHECK(diamond.contains({2, 2}));
    CHECK(!diamond.contains({7, 1})); CHECK(!diamond.contains({7, 7}));
    CHECK(!diamond.contains({1, 7})); CHECK(diamond.contains({4, 4}));
    CHECK(!diamond.contains({8, 4}));
    CHECK(c::croppedBounds(diamond, {0, 0, 1, 1}).empty());
    CHECK(c::clippedCropPolygon(diamond, {0, 0, 1, 1}).size == 0);
    const auto visible = c::croppedBounds(diamond, {0, 0, 3, 3});
    close(visible.x, 1); close(visible.y, 1); close(visible.width, 2); close(visible.height, 2);
    const std::array gates {diamond, crop({.25, -.5, 8.5, 7.75}, {3.5, .5, 2, 3})};
    for (const auto& gate : gates) for (int y = -1; y <= 8; ++y) for (int x = -1; x <= 9; ++x)
        CHECK(c::cropAllowsTexel(gate, x, y) ==
            (referenceCoverage(gate, {x + .5, y + .5}, {1, 0}, {0, 1}) > 1e-12));
    CHECK(!c::cropAllowsTexel(diamond, 1, 1)); // Diagonal tangent only.
    CHECK(c::cropAllowsTexel(diamond, 2, 1)); // Half-visible texel is editable.
    auto almostTangent = crop({0, 0, 8, 8}, {2 - 1e-9, 0, 0, 0});
    CHECK(c::cropAllowsTexel(almostTangent, 0, 0)); // Must not round to float AA zero.
    almostTangent.corners[0] = 2;
    CHECK(!c::cropAllowsTexel(almostTangent, 0, 0));
    CHECK(c::cropAllowsTexel({}, -100, -100));
}

void rasterWritesUseBinaryGateAndPinFullDescriptor()
{
    const auto originalCrop = crop({0, 0, 8, 8}, {3.5, 3.5, 3.5, 3.5});
    Fixture f(originalCrop);
    const auto before = pixels(*f.source);
    c::RasterEditTransaction edit(f.document, f.id, "Chamfer masked write");
    std::vector<std::byte> replacement(8 * 8 * 4);
    const std::array<std::byte, 4> newPixel {std::byte {199}, std::byte {37}, std::byte {73}, std::byte {83}};
    for (std::size_t i = 0; i < replacement.size(); i += 4)
        std::copy(newPixel.begin(), newPixel.end(), replacement.begin() + std::ptrdiff_t(i));
    (void)edit.writeRgba8({0, 0, 8, 8}, replacement, 8 * 4);
    const auto after = pixels(*f.source);
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 24; ++x) {
        const bool eligible = referenceCoverage(originalCrop, {x + .5, y + .5}, {1, 0}, {0, 1}) > 1e-12;
        const auto offset = std::size_t(y * 24 + x) * 4;
        for (std::size_t channel = 0; channel < 4; ++channel)
            CHECK(after[offset + channel] == (eligible ? newPixel[channel] : before[offset + channel]));
    }
    CHECK(edit.commit(f.history) == c::RasterEditCommitResult::Committed);
    CHECK(f.history.undo(f.document)); CHECK(pixels(*f.source) == before);
    CHECK(f.history.redo(f.document)); CHECK(pixels(*f.source) == after);

    c::RasterEditTransaction pinned(f.document, f.id, "Pinned chamfer");
    c::PinnedDocumentSampler sampler(f.document, {}, c::ColorSampleSource::MergedVisible);
    const auto snapshot = f.document.snapshot();
    const auto documentRevision = f.document.revision();
    auto invalid = originalCrop; invalid.corners[1] = -1;
    CHECK(!f.document.setLayerCrop(f.id, invalid));
    CHECK(f.document.revision() == documentRevision); CHECK(f.current() == originalCrop);
    auto changed = originalCrop; changed.corners[0] = 1;
    CHECK(f.document.setLayerCrop(f.id, changed));
    CHECK(!pinned.targetAvailable()); CHECK(!sampler.matches(f.document));
    CHECK(snapshot.layersBottomToTop.front().crop == originalCrop);
    (void)pinned.writeRgba8({0, 0, 8, 8}, replacement, 8 * 4);
    CHECK(pixels(*f.source) == after);
}

void allCornerGesturesUseLayerLocalCoordinatesAndOneHistoryAction()
{
    const std::array<c::AffineTransform, 3> matrices {{{}, {0, -1.2, 55, .8, 0, 9},
        {-1.1, .3, 65.25, .2, 1.4, 20.5}}};
    const auto initial = crop({2, 3, 16, 12}, {1, 2, 3, 4});
    const std::array<c::Vec2d, 4> corners {{{2, 3}, {18, 3}, {18, 15}, {2, 15}}};
    for (const auto matrix : matrices) for (std::size_t index = 0; index < 4; ++index) {
        Fixture f(initial, matrix); const auto before = pixels(*f.source);
        const auto revision = f.source->revision();
        c::LayerCropSession session(f.document, f.id);
        const auto handle = c::TransformHandle(index * 2);
        const c::Vec2d inward {index == 0 || index == 3 ? .5 : -.5, index < 2 ? .75 : -.75};
        CHECK(session.beginDrag(handle, matrix.map(corners[index]), true));
        CHECK(session.dragTo(matrix.map(corners[index] + inward * .5), {}, false));
        CHECK(session.dragTo(matrix.map(corners[index] + inward), {}, false));
        session.endDrag();
        CHECK(f.current()); if (!f.current()) continue;
        CHECK(static_cast<const c::RectD&>(*f.current()) == static_cast<const c::RectD&>(initial));
        for (std::size_t i = 0; i < 4; ++i)
            close(f.current()->corners[i], initial.corners[i] + (i == index ? 1.25 : 0));
        const auto result = f.current();
        CHECK(session.pendingHistory().undoDepth() == 1);
        CHECK(session.undo()); CHECK(f.current() == initial);
        CHECK(session.redo()); CHECK(f.current() == result);
        CHECK(session.commit(f.history) == c::TransformCommitResult::Committed);
        CHECK(f.history.undoDepth() == 1); CHECK(f.history.latestUndoMemoryCost() < 1024);
        CHECK(f.history.undo(f.document)); CHECK(f.current() == initial); CHECK(!f.document.isModified());
        CHECK(f.history.redo(f.document)); CHECK(f.current() == result);
        CHECK(f.document.layer(f.id)->localToDocument == matrix);
        CHECK(f.source->revision() == revision); CHECK(pixels(*f.source) == before);
    }
}

void altSideResizeNumericBoundsAndCancelRetainFullCrop()
{
    const auto initial = crop({2, 3, 16, 12}, {1, 2, 3, 4});
    Fixture f(initial); const auto state = f.document.contentState();
    c::LayerCropSession session(f.document, f.id);
    CHECK(session.beginDrag(c::TransformHandle::TopLeft, {2, 3}, true));
    CHECK(session.dragTo({3, 4}, {false, true}, false)); session.endDrag();
    CHECK(f.current()->corners == std::array<double, 4>({3, 2, 5, 4}));
    CHECK(session.pendingHistory().undoDepth() == 1);
    CHECK(session.beginDrag(c::TransformHandle::Right, {18, 9}, true));
    CHECK(session.dragTo({22, 9}, {}, false)); session.endDrag();
    close(f.current()->width, 20);
    CHECK(f.current()->corners == std::array<double, 4>({3, 2, 5, 4}));
    CHECK(session.setRect({-1, -2, 4, 4})); CHECK(session.completeAction());
    CHECK(f.current()->corners == std::array<double, 4>({3, 2, 5, 4}));
    CHECK(f.current()->resolvedCorners() == std::array<double, 4>({2, 2, 2, 2}));
    CHECK(session.setRect({2, 3, 16, 12})); CHECK(session.completeAction());
    CHECK(f.current()->resolvedCorners() == std::array<double, 4>({3, 2, 5, 4}));
    const auto beforeDrag = f.current();
    CHECK(session.beginDrag(c::TransformHandle::BottomRight, {18, 15}, true));
    CHECK(session.dragTo({100, 100}, {}, false)); close(f.current()->corners[2], 0);
    session.cancelDrag(); CHECK(f.current() == beforeDrag);
    CHECK(session.removeCrop()); CHECK(!f.current());
    CHECK(session.undo()); CHECK(f.current() == beforeDrag);
    session.cancel(); CHECK(f.current() == initial);
    CHECK(f.document.contentState() == state); CHECK(!f.document.isModified());
    CHECK(!f.history.canUndo());
}

void clickAndOutAndBackPreserveAbsentAndClampedRawCuts()
{
    const std::array<std::optional<c::LayerCrop>, 3> inputs {{std::nullopt,
        crop({2, 3, 16, 12}, {1, 2, 3, 4}), crop({2, 3, 4, 4}, {7, 8, 9, 10})}};
    for (const auto& initial : inputs) for (int index = 0; index < 4; ++index) {
        Fixture f(initial);
        c::LayerCropSession session(f.document, f.id);
        const auto frame = session.frame();
        const c::Vec2d press {index == 0 || index == 3 ? frame.x : frame.right(),
            index < 2 ? frame.y : frame.bottom()};
        const auto handle = c::TransformHandle(index * 2);
        CHECK(session.beginDrag(handle, press, true));
        CHECK(session.dragTo(press, {}, false)); session.endDrag();
        CHECK(f.current() == initial); CHECK(session.pendingHistory().undoDepth() == 0);
        const c::Vec2d inward {index == 0 || index == 3 ? 1.0 : -1.0, index < 2 ? 1.0 : -1.0};
        CHECK(session.beginDrag(handle, press, true));
        CHECK(session.dragTo(press + inward, {}, false));
        CHECK(session.dragTo(press, {}, false)); session.endDrag();
        CHECK(f.current() == initial); CHECK(session.pendingHistory().undoDepth() == 0);
        CHECK(session.commit(f.history) == c::TransformCommitResult::NoChange);
        CHECK(!f.document.isModified()); CHECK(!f.history.canUndo());
    }
}

void typedSourcesAndCachesAreNeverBakedByChamfer()
{
    for (const bool isText : {true, false}) {
        c::Document document({{128, 128}, 96}); c::History history;
        c::TextLayer text; text.utf8 = "Retained editable text";
        c::ShapeLayer shape; shape.size = {11.5, 7.25}; shape.strokeEnabled = true;
        auto layer = isText ? c::Layer::text("Text", text) : c::Layer::shape("Shape", shape);
        auto cache = std::make_shared<c::LayerRenderCache>();
        cache->surface = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {24, 20}, c::Rgba8 {13, 29, 47, 173});
        cache->pixelsToLocal = {.5, 0, -1, 0, .5, -2}; cache->density = 2;
        cache->logicalExtent = {12, 10}; layer.renderCache = cache;
        layer.localToDocument = {-1.1, .3, 40, .2, 1.4, 30};
        CHECK(document.insertLayer(0, layer)); document.markSaved();
        const auto original = *document.layer(layer.id);
        const auto before = pixels(*cache->surface); const auto revision = cache->surface->revision();
        c::LayerCropSession session(document, layer.id); CHECK(session.active());
        const auto press = original.localToDocument.map({-1, -2});
        CHECK(session.beginDrag(c::TransformHandle::TopLeft, press, true));
        CHECK(session.dragTo(original.localToDocument.map({0, -1}), {}, false)); session.endDrag();
        CHECK(document.layer(layer.id)->crop->corners[0] > 1.99);
        CHECK(session.commit(history) == c::TransformCommitResult::Committed);
        const auto result = document.layer(layer.id)->crop;
        CHECK(history.undo(document)); CHECK(!document.layer(layer.id)->crop);
        CHECK(history.redo(document)); CHECK(document.layer(layer.id)->crop == result);
        const auto* after = document.layer(layer.id);
        CHECK(after->renderCache == cache); CHECK(after->localToDocument == original.localToDocument);
        CHECK(after->textRevision == original.textRevision); CHECK(after->shapeRevision == original.shapeRevision);
        CHECK(after->adjustments == original.adjustments); CHECK(after->adjustmentRevision == original.adjustmentRevision);
        if (isText) CHECK(std::get<c::TextLayer>(after->payload) == std::get<c::TextLayer>(original.payload));
        else CHECK(std::get<c::ShapeLayer>(after->payload) == std::get<c::ShapeLayer>(original.payload));
        CHECK(cache->surface->revision() == revision); CHECK(pixels(*cache->surface) == before);
        CHECK(history.latestUndoMemoryCost() < 1024);
    }
}
}

int main()
{
    coverageMatchesIndependentOracle();
    containmentBoundsAndBinaryEdges();
    rasterWritesUseBinaryGateAndPinFullDescriptor();
    allCornerGesturesUseLayerLocalCoordinatesAndOneHistoryAction();
    altSideResizeNumericBoundsAndCancelRetainFullCrop();
    clickAndOutAndBackPreserveAbsentAndClampedRawCuts();
    typedSourcesAndCachesAreNeverBakedByChamfer();
    if (failures) std::cerr << failures << " layer chamfer checks failed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
