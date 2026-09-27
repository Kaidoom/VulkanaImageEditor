#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/ShapeCommands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template<class Function> bool throwsInvalid(Function&& function)
{
    try { function(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

ShapeLayer polygon()
{
    ShapeLayer value;
    value.kind = ShapeKind::Polygon;
    value.size = {60.5, 40.25};
    value.points = {{0.25, 0.5}, {60.5, 20.25}, {30.125, 40.25}, {10.5, 8.75}};
    value.fillColor = {23, 87, 176, 119};
    value.strokeColor = {203, 64, 7, 82};
    value.strokeEnabled = true;
    value.strokeWidth = 7.75;
    return value;
}

struct Fixture {
    Document document {CanvasSpec {.extent = {100, 100}}};
    History history;
    LayerId id;
    explicit Fixture(ShapeLayer value = polygon())
    {
        auto layer = Layer::shape("Shape", std::move(value));
        id = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
    const ShapeLayer& content() const { return std::get<ShapeLayer>(document.layer(id)->payload); }
    bool edit(ShapeLayer after, std::uint64_t key = 0)
    {
        return history.execute(document, std::make_unique<SetShapeCommand>(id, content(), std::move(after), key));
    }
};

void canonicalDataAndStyleAreIndependent()
{
    for (const auto kind : {ShapeKind::Rectangle, ShapeKind::RoundedRectangle,
             ShapeKind::Ellipse, ShapeKind::Triangle}) {
        ShapeLayer shape;
        shape.kind = kind;
        shape.size = {0.125, 24.375};
        CHECK(validShape(shape));
        CHECK(hasShapeGeometry(shape));
        CHECK(shapeGeometryExtent(shape) == Extent2u({1, 25}));
        for (const bool fill : {false, true}) {
            for (const bool stroke : {false, true}) {
                shape.fillEnabled = fill;
                shape.strokeEnabled = stroke;
                shape.fillColor = {255, 0, 0, 0};
                shape.strokeColor = {0, 255, 0, 128};
                shape.strokeWidth = 0;
                shape.cornerRadius = 90;
                auto layer = Layer::shape("Independent shape style", shape);
                CHECK(std::get<ShapeLayer>(layer.payload) == shape);
                CHECK(layer.textRevision == 1);
                CHECK(layer.shapeRevision == 1);
                CHECK(!layer.renderCache);
                CHECK(!std::holds_alternative<RasterLayer>(layer.payload));
            }
        }
    }

    auto value = polygon();
    CHECK(validShape(value));
    CHECK(hasShapeGeometry(value));
    auto line = value;
    line.kind = ShapeKind::Line;
    line.size = {50.75, 0};
    line.points = {{0, 0}, {50.75, 0}};
    CHECK(validShape(line));
    CHECK(hasShapeGeometry(line));
    CHECK(shapeGeometryExtent(line) == Extent2u({51, 1}));
    line.size = {0, 50.75};
    line.points = {{0, 0}, {0, 50.75}};
    CHECK(hasShapeGeometry(line));
    CHECK(shapeGeometryExtent(line) == Extent2u({1, 51}));
}

void invalidAndDegenerateGeometry()
{
    const auto original = polygon();
    for (const auto invalid : {-1.0, maximumShapeDimension + 1,
             std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto shape = original;
        shape.size.width = invalid;
        CHECK(!validShape(shape));
        CHECK(!hasShapeGeometry(shape));
        CHECK(shapeGeometryExtent(shape).empty());
        CHECK(throwsInvalid([&] { (void)Layer::shape("Invalid", shape); }));
    }
    for (const auto invalid : {-1.0, maximumShapeStyleDimension + 1,
             std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto shape = original;
        shape.strokeWidth = invalid;
        CHECK(!validShape(shape));
        shape = original;
        shape.cornerRadius = invalid;
        CHECK(!validShape(shape));
    }
    for (const auto badPoint : {Vec2d{-0.01, 2}, Vec2d{61, 2}, Vec2d{2, 41},
             Vec2d{std::numeric_limits<double>::quiet_NaN(), 2}}) {
        auto shape = original;
        shape.points[1] = badPoint;
        CHECK(!validShape(shape));
    }
    auto shape = original;
    shape.kind = static_cast<ShapeKind>(99);
    CHECK(!validShape(shape));
    shape = original;
    shape.points.resize(maximumShapePoints + 1);
    CHECK(!validShape(shape));
    shape = original;
    shape.points.resize(2);
    CHECK(!validShape(shape));
    shape.kind = ShapeKind::Line;
    CHECK(validShape(shape));
    shape.points.pop_back();
    CHECK(!validShape(shape));

    shape = {};
    shape.points = {{0,0}};
    CHECK(!validShape(shape));
    shape.points.clear();
    shape.size = {0, 10};
    CHECK(validShape(shape));
    CHECK(!hasShapeGeometry(shape));
    shape.kind = ShapeKind::Line;
    shape.points = {{0,0}, {0,0}};
    CHECK(validShape(shape));
    CHECK(!hasShapeGeometry(shape));

    shape = original;
    shape.size = {10,10};
    shape.points = {{0,0}, {0,0}, {5,5}, {10,10}, {10,10}};
    CHECK(validShape(shape));
    CHECK(!hasShapeGeometry(shape));
    shape.points = {{0,0}, {10,10}, {0,10}, {10,0}};
    CHECK(hasShapeGeometry(shape)); // Figure-eight has zero signed area.
    std::reverse(shape.points.begin(), shape.points.end());
    CHECK(hasShapeGeometry(shape));
    shape.points = {{0,0}, {10,0}, {0.001,0.00001}};
    CHECK(hasShapeGeometry(shape)); // Acute corner is still real geometry.
}

void documentAdmissionRevisionsAndSnapshots()
{
    Fixture f;
    const auto original = f.content();
    auto* layer = f.document.layer(f.id);
    auto cache = std::make_shared<LayerRenderCache>();
    cache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u{75, 55});
    cache->pixelsToLocal = {1, 0, -7, 0, 1, -7};
    cache->logicalExtent = shapeGeometryExtent(original);
    cache->contentRevision = layer->shapeRevision;
    layer->renderCache = cache;
    const auto revision = f.document.revision();
    const auto shapeRevision = layer->shapeRevision;
    const auto textRevision = layer->textRevision;
    CHECK(!f.document.setLayerShape(f.id, original));
    CHECK(f.document.revision() == revision);
    CHECK(layer->renderCache == cache);

    const AffineTransform flippedRotated {0, -2.5, 40.25, -0.75, 0, -10.5};
    CHECK(f.document.setLayerTransform(f.id, flippedRotated));
    CHECK(f.content() == original);
    CHECK(layer->shapeRevision == shapeRevision);
    CHECK(layer->renderCache == cache);
    CHECK(f.document.setCanvas({.extent = {15, 18}}));
    CHECK(f.content() == original);
    CHECK(layer->localToDocument == flippedRotated);
    CHECK(layer->renderCache == cache); // Clipping is not geometry mutation.
    const auto snapshot = f.document.snapshot();
    CHECK(std::get<ShapeLayer>(snapshot.layersBottomToTop.front().payload) == original);
    CHECK(snapshot.layersBottomToTop.front().shapeRevision == shapeRevision);
    CHECK(snapshot.layersBottomToTop.front().localToDocument == flippedRotated);
    CHECK(snapshot.layersBottomToTop.front().renderCache == cache);

    auto invalid = original;
    invalid.strokeWidth = -1;
    const auto unchangedRevision = f.document.revision();
    CHECK(!f.document.setLayerShape(f.id, invalid));
    CHECK(!f.document.setLayerShape(makeLayerId(), original));
    CHECK(f.document.revision() == unchangedRevision);
    CHECK(layer->renderCache == cache);
    auto inserted = Layer::shape("Tampered invalid layer", original);
    std::get<ShapeLayer>(inserted.payload).strokeWidth = -1;
    CHECK(!f.document.insertLayer(1, std::move(inserted)));
    CHECK(f.document.layers().size() == 1);

    auto changed = original;
    changed.fillEnabled = false;
    CHECK(f.document.setLayerShape(f.id, changed));
    CHECK(layer->shapeRevision == shapeRevision + 1);
    CHECK(layer->textRevision == textRevision);
    CHECK(!layer->renderCache);
    CHECK(layer->localToDocument == flippedRotated);
    CHECK(std::get<ShapeLayer>(snapshot.layersBottomToTop.front().payload) == original);
}

void commandsGroupingNoOpsAndStableTargets()
{
    Fixture f;
    const auto original = f.content();
    auto changed = original;
    changed.strokeWidth = 20;
    CHECK(f.edit(changed, 10));
    changed.strokeWidth = 30;
    CHECK(f.edit(changed, 10));
    changed.strokeWidth = 40;
    CHECK(f.edit(changed, 10));
    CHECK(f.history.undoDepth() == 1);
    const auto memory = f.history.memoryUsed();
    CHECK(memory == f.history.latestUndoMemoryCost());
    CHECK(memory > sizeof(SetShapeCommand));
    CHECK(f.history.undo(f.document));
    CHECK(f.content() == original);
    CHECK(f.history.memoryUsed() == memory);
    const auto revision = f.document.revision();
    CHECK(!f.edit(original, 11));
    CHECK(f.document.revision() == revision);
    CHECK(f.history.redoDepth() == 1);
    CHECK(f.history.memoryUsed() == memory);
    CHECK(f.history.redo(f.document));
    CHECK(f.content() == changed);
    CHECK(f.history.memoryUsed() == memory);

    auto extra = Layer::shape("Another shape", ShapeLayer{});
    CHECK(f.document.insertLayer(0, std::move(extra)));
    CHECK(f.history.undo(f.document)); // Stable ID, not list index.
    CHECK(f.content() == original);
    auto divergent = original;
    divergent.fillColor = {17, 27, 37, 47};
    CHECK(f.edit(divergent));
    CHECK(!f.history.canRedo());
    CHECK(f.history.undoDepth() == 1);
    CHECK(!f.history.execute(f.document,
        std::make_unique<SetShapeCommand>(f.id, original, changed, 11)));
    CHECK(f.content() == divergent); // Stale baseline cannot overwrite data.

    auto removed = f.document.takeLayer(f.id);
    CHECK(removed.has_value());
    const auto retained = f.history.memoryUsed();
    CHECK(!f.history.undo(f.document));
    CHECK(f.history.undoDepth() == 1);
    CHECK(f.history.memoryUsed() == retained);
    CHECK(f.document.insertLayer(removed->index, std::move(removed->layer)));
    CHECK(f.history.undo(f.document));
    CHECK(f.content() == original);

    auto raster = Layer::raster("Raster", std::make_shared<ContiguousRasterSurface>(Extent2u{2,2}));
    const auto rasterId = raster.id;
    CHECK(f.document.insertLayer(0, std::move(raster)));
    CHECK(!f.document.setLayerShape(rasterId, original));
    CHECK(!f.history.execute(f.document,
        std::make_unique<SetShapeCommand>(rasterId, original, changed)));
    CHECK(f.history.redoDepth() == 1);
}

void liveCommitCancelAndCreationConsequences()
{
    Fixture f;
    const auto original = f.content();
    auto next = original;
    next.strokeWidth = 3;
    CHECK(f.edit(next));
    CHECK(f.history.undo(f.document));
    const auto memory = f.history.memoryUsed();
    const auto contentState = f.document.contentState();
    auto preview = original;
    preview.cornerRadius = 42;
    CHECK(f.document.setLayerShape(f.id, preview));
    CHECK(f.document.setLayerShape(f.id, original)); // Cancel restores baseline.
    CHECK(f.history.memoryUsed() == memory);
    CHECK(f.history.redoDepth() == 1);
    CHECK(f.document.contentState() == contentState);
    CHECK(f.document.setLayerShape(f.id, preview));
    std::unique_ptr<Command> completed = std::make_unique<SetShapeCommand>(f.id, original, preview);
    CHECK(f.history.adoptApplied(f.document, completed));
    CHECK(!completed);
    CHECK(f.history.undoDepth() == 1);
    CHECK(f.history.redoDepth() == 0);
    CHECK(f.history.undo(f.document));
    CHECK(f.content() == original);
    CHECK(f.history.redo(f.document));
    CHECK(f.content() == preview);
    std::unique_ptr<Command> noOp = std::make_unique<SetShapeCommand>(f.id, preview, preview);
    CHECK(!f.history.adoptApplied(f.document, noOp));
    CHECK(noOp != nullptr);

    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec{.extent = {100,100}});
    auto raster = Layer::raster("Background", std::make_shared<ContiguousRasterSurface>(Extent2u{100,100}));
    const auto previous = raster.id;
    CHECK(document->insertLayer(0, std::move(raster)));
    session.replaceDocument(std::move(document));
    auto shape = Layer::shape("Polygon", polygon());
    shape.localToDocument = {0, 2, -5, -0.5, 0, 20};
    const auto id = shape.id;
    const auto transform = shape.localToDocument;
    shape.renderCache = std::make_shared<LayerRenderCache>();
    CHECK(session.execute(std::make_unique<AddLayerCommand>(std::move(shape), 1, previous)));
    CHECK(session.activeLayer() == id);
    CHECK(session.document()->layers().size() == 2);
    CHECK(!session.document()->layer(id)->renderCache);
    CHECK(session.undo());
    CHECK(session.activeLayer() == previous);
    CHECK(!session.document()->layer(id));
    CHECK(session.redo());
    CHECK(session.activeLayer() == id);
    CHECK(session.document()->layer(id)->localToDocument == transform);
    CHECK(std::get<ShapeLayer>(session.document()->layer(id)->payload) == polygon());
    CHECK(session.execute(std::make_unique<RemoveLayerCommand>(id)));
    CHECK(session.undo());
    CHECK(std::get<ShapeLayer>(session.document()->layer(id)->payload) == polygon());
}

void pixelEditsNeverFlattenAndMemoryAccounting()
{
    Fixture f;
    const auto original = f.content();
    const auto revision = f.document.revision();
    RasterEditTransaction transaction(f.document, f.id, "Must not paint a shape");
    CHECK(!transaction.active());
    CHECK(!transaction.targetAvailable());
    CHECK(!transaction.capture({0, 0, 1, 1}));
    CHECK(transaction.capturedTileCount() == 0);
    CHECK(transaction.commit(f.history) != RasterEditCommitResult::Committed);
    FillOperation fill(f.document, f.id, FillOptions{.color = {255,0,0,255}});
    CHECK(fill.state() == FillState::Failed);
    CHECK(f.document.revision() == revision);
    CHECK(f.content() == original);
    CHECK(f.history.undoDepth() == 0);

    auto before = polygon();
    auto after = before;
    before.points.reserve(30);
    after.points.reserve(40);
    after.strokeWidth = 1;
    const auto expected = sizeof(SetShapeCommand) + before.points.capacity() * sizeof(Vec2d)
        + after.points.capacity() * sizeof(Vec2d);
    CHECK(shapeMemoryCost(before) == before.points.capacity() * sizeof(Vec2d));
    SetShapeCommand command(f.id, std::move(before), std::move(after));
    CHECK(command.memoryCost() == expected);
}

void geometryTargetingUsesActualInteriors()
{
    ShapeLayer shape;
    shape.size = {100, 50};
    shape.fillEnabled = false;
    shape.strokeEnabled = true;
    shape.kind = ShapeKind::Ellipse;
    CHECK(shapeContainsPoint(shape, {50,25}));
    CHECK(shapeContainsPoint(shape, {99.9,25}));
    CHECK(!shapeContainsPoint(shape, {2,2}));
    CHECK(!shapeContainsPoint(shape, {-0.1,25}));
    shape.kind = ShapeKind::RoundedRectangle;
    shape.cornerRadius = 20;
    CHECK(shapeContainsPoint(shape, {50,25}));
    CHECK(!shapeContainsPoint(shape, {1,1}));
    CHECK(shapeContainsPoint(shape, {20,0}));
    shape.kind = ShapeKind::Triangle;
    CHECK(shapeContainsPoint(shape, {50,1}));
    CHECK(!shapeContainsPoint(shape, {1,1}));
    CHECK(shapeContainsPoint(shape, {1,49.9}));
    shape.kind = ShapeKind::Polygon;
    shape.size = {10,10};
    shape.points = {{0,0}, {10,0}, {10,10}, {7,10}, {7,3}, {3,3}, {3,10}, {0,10}};
    CHECK(shapeContainsPoint(shape, {1,5}));
    CHECK(!shapeContainsPoint(shape, {5,5})); // Concave gap inside the bounds.
    CHECK(shapeContainsPoint(shape, {5,1}));
    std::reverse(shape.points.begin(), shape.points.end());
    CHECK(!shapeContainsPoint(shape, {5,5}));
    CHECK(shapeContainsPoint(shape, {5,1}));

    Fixture f(shape);
    const AffineTransform transform {0, -2, 50, -1.5, 0, 45};
    CHECK(f.document.setLayerTransform(f.id, transform));
    CHECK(hitTestRasterLayer(f.document, transform.map({1,5})) == f.id);
    CHECK(!hitTestRasterLayer(f.document, transform.map({5,5})));
    auto top = Layer::shape("Top rectangle", ShapeLayer{.size = {10,10}, .points = {}});
    top.localToDocument = transform;
    const auto topId = top.id;
    CHECK(f.document.insertLayer(1, std::move(top)));
    CHECK(hitTestRasterLayer(f.document, transform.map({5,5})) == topId);
    CHECK(f.document.setLayerVisibility(topId, false));
    CHECK(!hitTestRasterLayer(f.document, transform.map({5,5})));
    CHECK(f.document.setLayerVisibility(topId, true));
    CHECK(f.document.setLayerOpacity(topId, 0));
    CHECK(!hitTestRasterLayer(f.document, transform.map({5,5})));
}

void sharedTransformAndCacheSampling()
{
    Fixture f(ShapeLayer{.kind = ShapeKind::Ellipse, .size = {20, 10}, .points = {}});
    auto* layer = f.document.layer(f.id);
    const auto content = f.content();
    const auto original = layer->localToDocument;
    const auto shapeRevision = layer->shapeRevision;
    auto cache = std::make_shared<LayerRenderCache>();
    cache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u{60,40}, Rgba8{50,100,150,128});
    cache->pixelsToLocal = {0.5,0,-5,0,0.5,-5};
    cache->logicalExtent = {20,10};
    cache->contentRevision = shapeRevision;
    layer->renderCache = cache;
    CHECK(layerGeometryExtent(*layer) == Extent2u({20,10}));
    CHECK(renderedSurface(*layer) == cache->surface);

    {
        LayerTransformSession transform(f.document, f.id);
        CHECK(transform.active());
        CHECK(transform.beginDrag(TransformHandle::Move, {10,5}));
        CHECK(transform.dragTo({30,35}, {}, false));
        transform.endDrag();
        CHECK(transform.flip(true));
        auto values = transform.values();
        values.rotationDegrees = 45;
        values.scaleY = 2.5;
        CHECK(transform.setValues(values));
        CHECK(transform.completeAction());
        CHECK(transform.pendingHistory().undoDepth() == 3);
        CHECK(f.content() == content);
        CHECK(layer->shapeRevision == shapeRevision);
        CHECK(layer->renderCache == cache);
        CHECK(transform.commit(f.history) == TransformCommitResult::Committed);
    }
    const auto transformed = layer->localToDocument;
    CHECK(f.history.undoDepth() == 3);
    CHECK(f.history.undo(f.document));
    CHECK(f.history.undo(f.document));
    CHECK(f.history.undo(f.document));
    CHECK(layer->localToDocument == original);
    CHECK(f.history.redo(f.document));
    CHECK(f.history.redo(f.document));
    CHECK(f.history.redo(f.document));
    CHECK(layer->localToDocument == transformed);
    CHECK(layer->shapeRevision == shapeRevision);
    CHECK(layer->renderCache == cache);
    CHECK(f.document.setLayerOpacity(f.id, 0.5F));
    const auto point = layer->localToDocument.map({10,5});
    const auto sample = sampleDocumentColor(f.document, f.id, point, ColorSampleSource::MergedVisible);
    CHECK(sample.available());
    CHECK(sample.color == Rgba8({50,100,150,64}));
    CHECK(sample.texelsRead <= 4);
    const PinnedDocumentSampler pinned(f.document, f.id, ColorSampleSource::MergedVisible);
    CHECK(pinned.matches(f.document));
    CHECK(pinned.sample(point) == sample.color);
    const auto snapshot = f.document.snapshot();
    CHECK(renderTransform(snapshot.layersBottomToTop[0]) == renderTransform(*layer));
    const auto fromPixels = renderTransform(*layer).map({30,20});
    CHECK(std::hypot(fromPixels.x - point.x, fromPixels.y - point.y) < 1e-10);

    // An explicit session is cancellable without disturbing an existing redo
    // branch. Ordinary shape interactions use the same session per gesture.
    CHECK(f.history.undo(f.document));
    const auto baseline = layer->localToDocument;
    const auto depth = f.history.redoDepth();
    const auto contentState = f.document.contentState();
    {
        LayerTransformSession transform(f.document, f.id);
        CHECK(transform.beginDrag(TransformHandle::Move, {20,20}));
        CHECK(transform.dragTo({70,80}, {}, false));
        transform.endDrag();
        transform.cancel();
    }
    CHECK(layer->localToDocument == baseline);
    CHECK(f.history.redoDepth() == depth);
    CHECK(f.document.contentState() == contentState);
    {
        LayerTransformSession transform(f.document, f.id);
        CHECK(transform.beginDrag(TransformHandle::Move, {20,20}));
        CHECK(transform.dragTo({20,20}, {}, false));
        transform.endDrag();
        CHECK(transform.commit(f.history) == TransformCommitResult::NoChange);
    }
    CHECK(f.history.redoDepth() == depth);
    CHECK(f.content() == content);
}
} // namespace

int main()
{
    canonicalDataAndStyleAreIndependent();
    invalidAndDegenerateGeometry();
    documentAdmissionRevisionsAndSnapshots();
    commandsGroupingNoOpsAndStableTargets();
    liveCommitCancelAndCreationConsequences();
    pixelEditsNeverFlattenAndMemoryAccounting();
    geometryTargetingUsesActualInteriors();
    sharedTransformAndCacheSampling();
    std::cout << "Shape data, geometry admission and history: " << failures << " failure(s)\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
