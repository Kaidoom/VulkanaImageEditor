#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/RasterSurface.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

void checkNear(double actual, double expected, double tolerance,
    const char* expression, int line)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "FAIL line " << line << ": " << expression
                  << " (actual " << actual << ", expected " << expected
                  << ", tolerance " << tolerance << ")\n";
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
#define CHECK_NEAR(actual, expected, tolerance) \
    checkNear((actual), (expected), (tolerance), #actual " ~= " #expected, __LINE__)

constexpr double kGeometryTolerance = 1.0e-8;

bool pointNear(Vec2d actual, Vec2d expected, double tolerance = kGeometryTolerance)
{
    return std::isfinite(actual.x) && std::isfinite(actual.y)
        && std::abs(actual.x - expected.x) <= tolerance
        && std::abs(actual.y - expected.y) <= tolerance;
}

bool affineNear(const AffineTransform& actual, const AffineTransform& expected,
    double tolerance = kGeometryTolerance)
{
    return std::abs(actual.m00 - expected.m00) <= tolerance
        && std::abs(actual.m01 - expected.m01) <= tolerance
        && std::abs(actual.m02 - expected.m02) <= tolerance
        && std::abs(actual.m10 - expected.m10) <= tolerance
        && std::abs(actual.m11 - expected.m11) <= tolerance
        && std::abs(actual.m12 - expected.m12) <= tolerance;
}

bool relativeNear(double actual, double expected, double tolerance = 1.0e-12)
{
    return std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected)
            <= tolerance * std::max({1.0, std::abs(actual), std::abs(expected)});
}

Vec2d pointAtAngle(Vec2d center, double radius, double degrees)
{
    const auto angle = degrees * std::numbers::pi / 180.0;
    return {center.x + std::cos(angle) * radius,
        center.y + std::sin(angle) * radius};
}

double dot(Vec2d left, Vec2d right)
{
    return left.x * right.x + left.y * right.y;
}

struct Fixture {
    Document document;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId layerId {0};
    History history;

    explicit Fixture(Extent2u surfaceExtent = {100, 80},
        Rgba8 fill = {0, 0, 0, 0}, Extent2u canvasExtent = {512, 512})
        : document(CanvasSpec {.extent = canvasExtent})
        , surface(std::make_shared<ContiguousRasterSurface>(surfaceExtent, fill))
    {
        auto layer = Layer::raster("Transform target", surface);
        layerId = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }

    [[nodiscard]] const AffineTransform& transform() const
    {
        const auto* found = document.layer(layerId);
        CHECK(found != nullptr);
        static const AffineTransform fallback;
        return found ? found->localToDocument : fallback;
    }
};

// Deliberately mixed, already transformed targets. Fractional shape dimensions
// must not be rounded into the shared frame; cache texels are not text geometry.
struct GroupFixture {
    Document document {CanvasSpec {.extent = {512, 512}}};
    History history;
    std::array<LayerId, 3> ids {};
    std::array<Extent2d, 3> sizes {{{32, 24}, {24, 18}, {31.5, 19.25}}};
    std::array<AffineTransform, 3> entry {{
        {1.0, 0.35, 11.25, 0.15, 1.1, 13.5},
        {0.0, -1.2, 160.5, 0.8, 0.24, 20.75},
        {-1.1, 0.3, 140.25, 0.2, 1.4, 120.5},
    }};
    std::shared_ptr<ContiguousRasterSurface> raster
        = std::make_shared<ContiguousRasterSurface>(Extent2u {32, 24},
            Rgba8 {39, 71, 113, 193});
    std::shared_ptr<LayerRenderCache> textCache
        = std::make_shared<LayerRenderCache>();
    std::shared_ptr<LayerRenderCache> shapeCache
        = std::make_shared<LayerRenderCache>();
    LayerId unselected {0};

    GroupFixture()
    {
        auto rasterLayer = Layer::raster("Group raster", raster);
        TextLayer text;
        text.utf8 = "Grouped text";
        auto textLayer = Layer::text("Group text", std::move(text));
        textCache->logicalExtent = {24, 18};
        textCache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u {48, 36});
        textCache->pixelsToLocal = {0.5, 0, 0, 0, 0.5, 0};
        textCache->density = 2;
        textLayer.renderCache = textCache;
        ShapeLayer shape;
        shape.kind = ShapeKind::RoundedRectangle;
        shape.size = sizes[2];
        shape.strokeEnabled = true;
        shape.strokeWidth = 7.5;
        auto shapeLayer = Layer::shape("Group shape", shape);
        shapeCache->logicalExtent = {32, 20};
        shapeCache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u {48, 40});
        shapeCache->pixelsToLocal = {1, 0, -8, 0, 1, -8};
        shapeLayer.renderCache = shapeCache;
        std::array<Layer, 3> layers {std::move(rasterLayer),
            std::move(textLayer), std::move(shapeLayer)};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            ids[index] = layers[index].id;
            layers[index].localToDocument = entry[index];
            CHECK(document.insertLayer(index, std::move(layers[index])));
        }
        auto other = Layer::raster("Not selected",
            std::make_shared<ContiguousRasterSurface>(Extent2u {2, 2}));
        unselected = other.id;
        CHECK(document.insertLayer(3, std::move(other)));
    }

    [[nodiscard]] std::array<AffineTransform, 3> matrices() const
    {
        std::array<AffineTransform, 3> result {};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const auto* layer = document.layer(ids[index]);
            CHECK(layer != nullptr);
            if (layer) result[index] = layer->localToDocument;
        }
        return result;
    }

    void checkDelta(const AffineTransform& frame, const AffineTransform& entryFrame) const
    {
        const auto inverse = entryFrame.inverted();
        CHECK(inverse.has_value());
        if (!inverse) return;
        const auto delta = composeAffine(frame, *inverse);
        const auto actual = matrices();
        for (std::size_t index = 0; index < ids.size(); ++index)
            CHECK(affineNear(actual[index], composeAffine(delta, entry[index])));
        CHECK(document.layer(unselected)->localToDocument == AffineTransform {});
    }
};

Rgba8 pixelAt(const RasterSurface& surface, std::int32_t x, std::int32_t y)
{
    std::array<std::byte, 4> bytes {};
    surface.copyRgba8({x, y, 1, 1}, bytes, 4);
    return {
        std::to_integer<std::uint8_t>(bytes[0]),
        std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]),
        std::to_integer<std::uint8_t>(bytes[3]),
    };
}

NormalizedPointerSample pointerSample(Vec2d point, std::uint64_t timestamp = 0)
{
    return {
        .documentPosition = point,
        .timestampMicroseconds = timestamp,
        .pressure = 1.0,
        .tiltX = 0.0,
        .tiltY = 0.0,
        .rotationDegrees = 0.0,
        .barrelRotationDegrees = 0.0,
        .pointerType = PointerType::Mouse,
        .buttons = PointerButtonPrimary,
        .modifiers = PointerModifierNone,
    };
}

BrushSettings hardRound(Rgba8 color)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = 5.0;
    settings.hardness = 1.0;
    settings.opacity = 1.0;
    settings.flow = 1.0;
    settings.spacingPercent = 10.0;
    settings.foreground = color;
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    return settings;
}

RasterEditCommitResult applyDot(Fixture& fixture, Vec2d documentPoint,
    BrushCompositeMode mode, Rgba8 color)
{
    BasicPixelBrushStroke stroke(fixture.document, fixture.layerId,
        hardRound(color), mode);
    CHECK(stroke.valid());
    const auto sample = pointerSample(documentPoint);
    CHECK(stroke.begin(sample));
    return stroke.end(sample, fixture.history);
}

void multipleGesturesKeepIndividualCommands()
{
    Fixture fixture;
    const auto original = fixture.transform();
    LayerTransformSession session(fixture.document, fixture.layerId);
    CHECK(session.active());

    CHECK(session.beginDrag(TransformHandle::Move, {7.0, 11.0}));
    CHECK(session.dragTo({27.0, 38.0}, {}, false));
    session.endDrag();
    const auto translated = session.transform();
    CHECK(session.pendingHistory().undoDepth() == 1);

    auto handles = transformHandles(session.transform(), session.extent());
    const auto scalePress = handles[4];
    CHECK(session.beginDrag(TransformHandle::BottomRight, scalePress));
    CHECK(session.dragTo(scalePress + Vec2d {40.0, 20.0}, {}, false));
    session.endDrag();
    const auto scaled = session.transform();
    CHECK(session.pendingHistory().undoDepth() == 2);

    const auto center = session.values().center;
    CHECK(session.beginDrag(TransformHandle::Rotate,
        center + Vec2d {50.0, 0.0}));
    CHECK(session.dragTo(center + Vec2d {0.0, 50.0}, {}, false));
    session.endDrag();

    const auto finalTransform = session.transform();
    CHECK(!affineNear(finalTransform, original));
    CHECK(session.pendingHistory().undoDepth() == 3);
    CHECK(fixture.history.undoDepth() == 0);
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 3);
    CHECK(fixture.history.undoLabel() == std::string_view("Layer transform"));
    const auto retainedMemory = fixture.history.memoryUsed();
    CHECK(retainedMemory > 0);

    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == scaled);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == translated);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == original);
    CHECK(fixture.history.memoryUsed() == retainedMemory);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.transform() == translated);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.transform() == scaled);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.transform() == finalTransform);
    CHECK(fixture.history.memoryUsed() == retainedMemory);
}

void cancellationNoOpAndRedoAreExact()
{
    Fixture fixture;
    const auto original = fixture.transform();
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.layerId, false)));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.history.canRedo());
    const auto redoDepth = fixture.history.redoDepth();

    {
        LayerTransformSession cancelled(fixture.document, fixture.layerId);
        auto values = cancelled.values();
        values.center = values.center + Vec2d {31.0, -17.0};
        values.rotationDegrees = 23.0;
        CHECK(cancelled.setValues(values));
        CHECK(fixture.transform() != original);
        cancelled.cancel();
        CHECK(fixture.transform() == original);
    }
    CHECK(fixture.history.redoDepth() == redoDepth);

    {
        LayerTransformSession noOp(fixture.document, fixture.layerId);
        CHECK(noOp.commit(fixture.history) == TransformCommitResult::NoChange);
    }
    CHECK(fixture.history.redoDepth() == redoDepth);

    {
        LayerTransformSession dragCancel(fixture.document, fixture.layerId);
        CHECK(dragCancel.beginDrag(TransformHandle::Move, {4.0, 5.0}));
        CHECK(dragCancel.dragTo({14.0, 25.0}, {}, false));
        dragCancel.endDrag();
        const auto firstGesture = dragCancel.transform();
        CHECK(dragCancel.beginDrag(TransformHandle::Move, {14.0, 25.0}));
        CHECK(dragCancel.dragTo({99.0, -33.0}, {}, false));
        dragCancel.cancelDrag();
        CHECK(dragCancel.transform() == firstGesture);
        dragCancel.cancel();
        CHECK(fixture.transform() == original);
    }
    CHECK(fixture.history.redoDepth() == redoDepth);

    LayerTransformSession divergent(fixture.document, fixture.layerId);
    auto changed = divergent.values();
    changed.center.x += 9.0;
    CHECK(divergent.setValues(changed));
    CHECK(divergent.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(!fixture.history.canRedo());
}

void pendingBranchPublishesBothStacksAndExactMemory()
{
    Fixture fixture;
    const auto entry = fixture.transform();
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.layerId, false)));
    const auto precedingMemory = fixture.history.memoryUsed();
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerOpacityCommand>(fixture.layerId, 0.25F)));
    CHECK(fixture.history.undo(fixture.document));
    const auto originalMemory = fixture.history.memoryUsed();
    const auto originalRedo = std::string(fixture.history.redoLabel());

    LayerTransformSession session(fixture.document, fixture.layerId);
    CHECK(!session.undo());
    CHECK(!session.redo());
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(session.beginDrag(TransformHandle::Move, {0, 0}));
    CHECK(session.dragTo({11, 17}, {}, false));
    session.endDrag();
    const auto moved = session.transform();
    auto numeric = session.values();
    numeric.scaleX = 1.75;
    numeric.scaleY = -0.8;
    CHECK(session.setValues(numeric));
    CHECK(session.completeAction());
    const auto scaled = session.transform();
    numeric = session.values();
    numeric.rotationDegrees += 53;
    CHECK(session.setValues(numeric));
    CHECK(session.completeAction());
    const auto rotated = session.transform();
    CHECK(fixture.history.memoryUsed() == originalMemory);
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.redoLabel() == originalRedo);
    const auto pendingMemory = session.pendingHistory().memoryUsed();
    const auto actionMemory = session.pendingHistory().latestUndoMemoryCost();
    CHECK(pendingMemory == 3 * actionMemory);

    CHECK(session.undo());
    CHECK(session.transform() == scaled);
    CHECK(session.undo());
    CHECK(session.transform() == moved);
    CHECK(session.redo());
    CHECK(session.transform() == scaled);
    CHECK(session.active());
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.pendingHistory().redoDepth() == 1);
    CHECK(session.pendingHistory().memoryUsed() == pendingMemory);
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 3);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.memoryUsed() == precedingMemory + pendingMemory);
    CHECK(session.pendingHistory().memoryUsed() == 0);

    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.transform() == rotated);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == scaled);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == moved);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == entry);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.document.layer(fixture.layerId)->visible);
    CHECK(fixture.history.memoryUsed() == precedingMemory + pendingMemory);
}

void cancellingDivergentActionsPreservesPreexistingHistoryAndRedo()
{
    Fixture fixture;
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerOpacityCommand>(fixture.layerId, 0.75F)));
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.layerId, false)));
    CHECK(fixture.history.undo(fixture.document));
    const auto memory = fixture.history.memoryUsed();
    const auto undoLabel = std::string(fixture.history.undoLabel());
    const auto redoLabel = std::string(fixture.history.redoLabel());
    const auto entry = fixture.transform();

    LayerTransformSession session(fixture.document, fixture.layerId);
    CHECK(session.flip(true));
    const auto first = session.transform();
    CHECK(session.flip(false));
    CHECK(session.undo());
    CHECK(session.transform() == first);
    CHECK(session.pendingHistory().redoDepth() == 1);
    auto values = session.values();
    values.center.x += 37;
    CHECK(session.setValues(values));
    CHECK(session.completeAction());
    CHECK(!session.pendingHistory().canRedo());
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.undo());
    CHECK(session.transform() == first);
    CHECK(session.beginDrag(TransformHandle::Move, {7, 9}));
    CHECK(session.dragTo({67, 99}, {}, false));
    session.cancel();

    CHECK(fixture.transform() == entry);
    CHECK(fixture.history.memoryUsed() == memory);
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.undoLabel() == undoLabel);
    CHECK(fixture.history.redoLabel() == redoLabel);
    CHECK(!session.pendingHistory().canUndo());
    CHECK(!session.pendingHistory().canRedo());
    CHECK(fixture.history.redo(fixture.document));
    CHECK(!fixture.document.layer(fixture.layerId)->visible);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.history.undo(fixture.document));
    CHECK_NEAR(fixture.document.layer(fixture.layerId)->opacity, 1.0, 0.0);
}

void allUndoneAndNoOpActionsPreserveExistingRedo()
{
    Fixture fixture;
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.layerId, false)));
    CHECK(fixture.history.undo(fixture.document));
    const auto memory = fixture.history.memoryUsed();
    const auto original = fixture.transform();
    LayerTransformSession session(fixture.document, fixture.layerId);
    CHECK(session.flip(true));
    CHECK(session.flip(false));
    CHECK(session.undo());
    CHECK(session.undo());
    CHECK(session.transform() == original);
    CHECK(!session.undo());
    CHECK(session.pendingHistory().redoDepth() == 2);

    CHECK(session.beginDrag(TransformHandle::Move, {1, 2}));
    CHECK(session.dragTo({1, 2}, {}, false));
    session.endDrag();
    CHECK(session.pendingHistory().redoDepth() == 2);
    auto equivalentValues = session.values();
    equivalentValues.rotationDegrees += 360;
    equivalentValues.center.x += 1.0e-10;
    CHECK(session.setValues(equivalentValues));
    CHECK(!session.completeAction());
    CHECK(session.transform() == original);
    CHECK(session.pendingHistory().redoDepth() == 2);
    CHECK(session.commit(fixture.history) == TransformCommitResult::NoChange);
    CHECK(fixture.transform() == original);
    CHECK(fixture.history.undoDepth() == 0);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.memoryUsed() == memory);
    CHECK(fixture.history.redo(fixture.document));
}

void numericScrubbingGroupsAtCompletionAndPreservesSignedValues()
{
    Fixture fixture;
    const auto original = fixture.transform();
    LayerTransformSession session(fixture.document, fixture.layerId);
    for (int step = 1; step <= 100; ++step) {
        auto value = session.values();
        value.center.x = 50.0 + step * 0.5;
        CHECK(session.setValues(value));
        CHECK(session.pendingHistory().undoDepth() == 0);
    }
    CHECK(session.completeAction());
    CHECK(!session.completeAction());
    CHECK(session.pendingHistory().undoDepth() == 1);
    const auto positioned = session.transform();

    CHECK(session.flip(true));
    const auto flipped = session.transform();
    const auto flippedValues = session.values();
    CHECK(flippedValues.scaleX < 0);
    CHECK(flippedValues.scaleY > 0);
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.undo());
    CHECK(session.transform() == positioned);
    CHECK(session.values().scaleX > 0);
    CHECK(session.redo());
    CHECK(session.transform() == flipped);
    CHECK_NEAR(session.values().scaleX, flippedValues.scaleX, 0.0);
    CHECK_NEAR(session.values().scaleY, flippedValues.scaleY, 0.0);
    CHECK_NEAR(session.values().rotationDegrees, flippedValues.rotationDegrees, 0.0);

    CHECK(session.undo());
    auto numeric = session.values();
    numeric.rotationDegrees = -47.5;
    CHECK(session.setValues(numeric));
    CHECK(session.completeAction());
    CHECK(!session.pendingHistory().canRedo());
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.beginDrag(TransformHandle::Move, {10, 10}));
    CHECK(session.dragTo({60, 90}, {}, false));
    session.cancelDrag();
    CHECK(session.pendingHistory().undoDepth() == 2);
    const auto committedRotation = session.transform();

    // A click returning to the same values must retain exact action bits so
    // the previous command remains undoable, including a rotated matrix.
    numeric = session.values();
    numeric.center.x += 1.0e-10;
    CHECK(session.setValues(numeric));
    CHECK(!session.completeAction());
    CHECK(session.transform() == committedRotation);
    CHECK(session.undo());
    CHECK(session.transform() == positioned);
    CHECK(session.undo());
    CHECK(session.transform() == original);
    CHECK(session.redo());
    CHECK(session.transform() == positioned);
    CHECK(session.redo());
    CHECK(session.transform() == committedRotation);
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 2);
}

void completedActionsReturningToEntryRemainIndividuallyUndoable()
{
    Fixture fixture;
    const auto original = fixture.transform();
    LayerTransformSession session(fixture.document, fixture.layerId);
    CHECK(session.flip(true));
    const auto flipped = session.transform();
    CHECK(session.flip(true));
    CHECK(session.transform() == original);
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 2);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == flipped);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.transform() == original);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.transform() == original);
}

void stableLayerAndSurfaceIdentityProtectHistory()
{
    {
        Fixture fixture;
        LayerTransformSession interrupted(fixture.document, fixture.layerId);
        auto changed = interrupted.values();
        changed.center.x += 25.0;
        CHECK(interrupted.setValues(changed));
        const auto liveTransform = interrupted.transform();
        auto removed = fixture.document.takeLayer(fixture.layerId);
        CHECK(removed.has_value());
        CHECK(!interrupted.targetAvailable());
        CHECK(interrupted.commit(fixture.history)
            == TransformCommitResult::TargetUnavailable);
        CHECK(!fixture.history.canUndo());
        CHECK(removed && removed->layer.localToDocument == liveTransform);
    }

    {
        Fixture fixture;
        auto other = Layer::raster("Keep one layer",
            std::make_shared<ContiguousRasterSurface>(Extent2u {8, 8}));
        CHECK(fixture.document.insertLayer(1, std::move(other)));
        const auto original = fixture.transform();
        LayerTransformSession transform(fixture.document, fixture.layerId);
        auto changed = transform.values();
        changed.center = changed.center + Vec2d {14.0, 8.0};
        changed.scaleY = -0.75;
        CHECK(transform.setValues(changed));
        const auto transformed = transform.transform();
        CHECK(transform.commit(fixture.history) == TransformCommitResult::Committed);
        CHECK(fixture.history.execute(fixture.document,
            std::make_unique<RemoveLayerCommand>(fixture.layerId)));
        CHECK(!fixture.document.containsLayer(fixture.layerId));
        CHECK(fixture.history.undo(fixture.document));
        CHECK(fixture.transform() == transformed);
        CHECK(fixture.history.undo(fixture.document));
        CHECK(fixture.transform() == original);
        CHECK(fixture.history.redo(fixture.document));
        CHECK(fixture.transform() == transformed);
        CHECK(fixture.history.redo(fixture.document));
        CHECK(!fixture.document.containsLayer(fixture.layerId));
    }

    {
        Fixture fixture;
        LayerTransformSession transform(fixture.document, fixture.layerId);
        auto changed = transform.values();
        changed.center.x += 18.0;
        CHECK(transform.setValues(changed));
        CHECK(transform.commit(fixture.history) == TransformCommitResult::Committed);
        auto removed = fixture.document.takeLayer(fixture.layerId);
        CHECK(removed.has_value());
        if (removed) {
            auto replacement = std::move(removed->layer);
            std::get<RasterLayer>(replacement.payload).surface
                = std::make_shared<ContiguousRasterSurface>(Extent2u {100, 80});
            CHECK(fixture.document.insertLayer(removed->index,
                std::move(replacement)));
        }
        CHECK(!fixture.history.undo(fixture.document));
        CHECK(fixture.history.undoDepth() == 1);
    }
}

TransformValues challengingValues()
{
    return {
        .center = {260.0, 230.0},
        .scaleX = 1.7,
        .scaleY = -0.8,
        .rotationDegrees = 31.0,
        .shear = 0.27,
    };
}

void rotatedFlippedShearedAnchorsStayFixed()
{
    Fixture fixture;
    const auto initial = transformFromValues(challengingValues(), {100, 80});
    CHECK(fixture.document.setLayerTransform(fixture.layerId, initial));

    {
        LayerTransformSession corner(fixture.document, fixture.layerId);
        const Vec2d anchor {0.0, 0.0};
        const Vec2d handle {100.0, 80.0};
        const auto fixedAnchor = initial.map(anchor);
        const auto press = initial.map(handle);
        CHECK(corner.beginDrag(TransformHandle::BottomRight, press));
        CHECK(corner.dragTo(press + Vec2d {73.0, -29.0}, {}, false));
        CHECK(pointNear(corner.transform().map(anchor), fixedAnchor));
        CHECK_NEAR(corner.values().shear, challengingValues().shear, 1.0e-12);
        CHECK(corner.values().scaleY < 0.0);
        corner.cancel();
        CHECK(fixture.transform() == initial);
    }

    {
        LayerTransformSession centered(fixture.document, fixture.layerId);
        const Vec2d localCenter {50.0, 40.0};
        const auto fixedCenter = initial.map(localCenter);
        const auto press = initial.map({0.0, 0.0});
        CHECK(centered.beginDrag(TransformHandle::TopLeft, press));
        CHECK(centered.dragTo(press + Vec2d {-41.0, 36.0},
            {.alt = true}, false));
        CHECK(pointNear(centered.transform().map(localCenter), fixedCenter));
        CHECK_NEAR(centered.values().shear, challengingValues().shear, 1.0e-12);
        centered.cancel();
    }

    {
        LayerTransformSession edge(fixture.document, fixture.layerId);
        const Vec2d anchor {100.0, 40.0};
        const auto fixedAnchor = initial.map(anchor);
        const auto press = initial.map({0.0, 40.0});
        CHECK(edge.beginDrag(TransformHandle::Left, press));
        CHECK(edge.dragTo(press + Vec2d {25.0, 17.0}, {}, false));
        CHECK(pointNear(edge.transform().map(anchor), fixedAnchor));
        CHECK_NEAR(edge.values().scaleY, challengingValues().scaleY, 1.0e-10);
        edge.cancel();
    }
}

void individualDragCancellationRestoresExactShearedMatrices()
{
    Fixture fixture;
    const auto imported = transformFromValues(challengingValues(), {100, 80});
    CHECK(fixture.document.setLayerTransform(fixture.layerId, imported));
    LayerTransformSession session(fixture.document, fixture.layerId);

    const auto corner = imported.map({100.0, 80.0});
    CHECK(session.beginDrag(TransformHandle::BottomRight, corner));
    CHECK(session.dragTo(corner + Vec2d {61.0, -37.0}, {}, false));
    session.cancelDrag();
    CHECK(session.transform() == imported);
    CHECK(fixture.transform() == imported);

    CHECK(session.beginDrag(TransformHandle::Move, {9.0, 13.0}));
    CHECK(session.dragTo({34.0, 41.0}, {}, false));
    session.endDrag();
    const auto completedGesture = session.transform();
    const auto handles = transformHandles(completedGesture, {100, 80});
    CHECK(session.beginDrag(TransformHandle::TopLeft, handles[0]));
    CHECK(session.dragTo(handles[0] + Vec2d {-28.0, 19.0},
        {.alt = true}, false));
    session.cancelDrag();
    CHECK(session.transform() == completedGesture);
    CHECK(fixture.transform() == completedGesture);
    session.cancel();
    CHECK(fixture.transform() == imported);
}

void aspectConstraintUsesPressTimeProjectionAndShiftToggle()
{
    Fixture fixture;
    const auto initialValues = challengingValues();
    const auto initial = transformFromValues(initialValues, {100, 80});
    CHECK(fixture.document.setLayerTransform(fixture.layerId, initial));
    LayerTransformSession session(fixture.document, fixture.layerId);

    const Vec2d anchor {0.0, 0.0};
    const Vec2d handle {100.0, 80.0};
    const auto press = initial.map(handle);
    const Vec2d delta {46.0, -18.0};
    const auto diagonal = initial.map(handle) - initial.map(anchor);
    const auto expectedFactor = 1.0 + dot(delta, diagonal) / dot(diagonal, diagonal);
    CHECK(session.beginDrag(TransformHandle::BottomRight, press));
    CHECK(session.dragTo(press + delta, {.shift = true}, false));
    const auto locked = session.transform();
    CHECK_NEAR(session.values().scaleX / initialValues.scaleX,
        expectedFactor, 1.0e-10);
    CHECK_NEAR(session.values().scaleY / initialValues.scaleY,
        expectedFactor, 1.0e-10);
    CHECK(pointNear(locked.map(anchor), initial.map(anchor)));

    CHECK(session.dragTo(press + delta, {}, false));
    const auto unlocked = session.transform();
    CHECK(!affineNear(unlocked, locked, 1.0e-5));
    CHECK(session.dragTo(press + delta, {.shift = true}, false));
    CHECK(affineNear(session.transform(), locked));

    // Aspect Lock is a default that Shift temporarily inverts, rather than a
    // second cumulative constraint state.
    CHECK(session.dragTo(press + delta, {}, true));
    CHECK(affineNear(session.transform(), locked));
    CHECK(session.dragTo(press + delta, {.shift = true}, true));
    CHECK(affineNear(session.transform(), unlocked));
    session.cancel();

    LayerTransformSession edge(fixture.document, fixture.layerId);
    const Vec2d edgeHandle {50.0, 0.0};
    const Vec2d edgeAnchor {50.0, 80.0};
    const auto edgePress = initial.map(edgeHandle);
    const auto edgeDiagonal = initial.map(edgeHandle) - initial.map(edgeAnchor);
    const auto edgeDelta = edgeDiagonal * 0.35;
    CHECK(edge.beginDrag(TransformHandle::Top, edgePress));
    CHECK(edge.dragTo(edgePress + edgeDelta, {.shift = true}, false));
    CHECK_NEAR(edge.values().scaleX / initialValues.scaleX, 1.35, 1.0e-10);
    CHECK_NEAR(edge.values().scaleY / initialValues.scaleY, 1.35, 1.0e-10);
    CHECK(pointNear(edge.transform().map(edgeAnchor), initial.map(edgeAnchor)));
}

void constrainedScaleClampsOneCommonFactorAtNumericLimits()
{
    const auto run = [](double scaleX, double scaleY, double requestedFactor,
                         double expectedFactor) {
        Fixture fixture;
        TransformValues initialValues {
            .center = {220.0, 210.0},
            .scaleX = scaleX,
            .scaleY = scaleY,
            .rotationDegrees = 19.0,
            .shear = 0.18,
        };
        const auto initial = transformFromValues(initialValues, {100, 80});
        CHECK(fixture.document.setLayerTransform(fixture.layerId, initial));
        LayerTransformSession session(fixture.document, fixture.layerId);
        const Vec2d anchor {0.0, 0.0};
        const Vec2d handle {100.0, 80.0};
        const auto fixedAnchor = initial.map(anchor);
        const auto press = initial.map(handle);
        const auto diagonal = press - fixedAnchor;
        CHECK(session.beginDrag(TransformHandle::BottomRight, press));
        CHECK(session.dragTo(press + diagonal * (requestedFactor - 1.0),
            {.shift = true}, false));
        CHECK_NEAR(session.values().scaleX / scaleX, expectedFactor, 1.0e-10);
        CHECK_NEAR(session.values().scaleY / scaleY, expectedFactor, 1.0e-10);
        CHECK(pointNear(session.transform().map(anchor), fixedAnchor, 1.0e-5));
        CHECK(session.transform().inverted().has_value());
    };

    // X reaches the maximum first; Y must use the same common factor.
    run(900.0, -0.5, 2.0, kMaximumLayerScale / 900.0);
    // X reaches the minimum first; Y must not continue shrinking alone.
    run(1.0e-5, -0.1, 0.01, kMinimumLayerScale / 1.0e-5);
    // Signed crossing clamps magnitude once and flips both axes together.
    run(900.0, -0.5, -2.0, -kMaximumLayerScale / 900.0);
}

void signedScaleCrossingAndBoundsRemainInvertible()
{
    Fixture fixture;
    LayerTransformSession session(fixture.document, fixture.layerId);
    const Vec2d handle {100.0, 40.0};
    const Vec2d anchor {0.0, 40.0};
    CHECK(session.beginDrag(TransformHandle::Right, handle));

    CHECK(session.dragTo(anchor, {}, false));
    CHECK_NEAR(session.values().scaleX, kMinimumLayerScale, 0.0);
    CHECK(session.transform().inverted().has_value());
    CHECK(pointNear(session.transform().map(anchor), anchor));

    CHECK(session.dragTo({-10.0, 40.0}, {}, false));
    CHECK_NEAR(session.values().scaleX, -0.1, 1.0e-12);
    CHECK(session.transform().inverted().has_value());
    CHECK(pointNear(session.transform().map(anchor), anchor));

    CHECK(session.dragTo({200000.0, 40.0}, {}, false));
    CHECK_NEAR(session.values().scaleX, kMaximumLayerScale, 0.0);
    CHECK(session.transform().inverted().has_value());
    session.cancel();

    LayerTransformSession numeric(fixture.document, fixture.layerId);
    auto values = numeric.values();
    values.scaleX = 0.0;
    values.scaleY = -2.0 * kMaximumLayerScale;
    CHECK(numeric.setValues(values));
    CHECK_NEAR(numeric.values().scaleX, kMinimumLayerScale, 0.0);
    CHECK_NEAR(numeric.values().scaleY, -kMaximumLayerScale, 0.0);
    CHECK(numeric.transform().inverted().has_value());
    const auto center = numeric.values().center;
    const auto beforeFlip = numeric.transform();
    CHECK(numeric.flip(true));
    CHECK(numeric.values().scaleX < 0.0);
    CHECK(pointNear(numeric.values().center, center));
    CHECK(numeric.flip(true));
    CHECK(numeric.transform() == beforeFlip);

    auto invalid = numeric.values();
    invalid.center.x = std::numeric_limits<double>::infinity();
    CHECK(!numeric.setValues(invalid));
    invalid = numeric.values();
    invalid.rotationDegrees = std::numeric_limits<double>::quiet_NaN();
    CHECK(!numeric.setValues(invalid));
}

void affineDecompositionIsFiniteAndRoundTripsWithoutIdentityLoss()
{
    const AffineTransform arbitrary {
        .m00 = -1.2, .m01 = 0.7, .m02 = 43.0,
        .m10 = 0.4, .m11 = 2.3, .m12 = -17.0};
    const auto decomposed = valuesFromTransform(arbitrary, {100, 80});
    CHECK(decomposed.has_value());
    if (decomposed) {
        CHECK(decomposed->scaleX > 0.0);
        CHECK(decomposed->scaleY < 0.0);
        CHECK(affineNear(transformFromValues(*decomposed, {100, 80}),
            arbitrary, 1.0e-12));
    }

    // Normalized inversion accepts uniformly huge, well-conditioned data;
    // decomposition must not overflow by multiplying the determinant first.
    const AffineTransform huge {
        .m00 = 1.0e200, .m01 = 0.0, .m02 = 0.0,
        .m10 = 0.0, .m11 = 1.0e200, .m12 = 0.0};
    const auto hugeValues = valuesFromTransform(huge, {1, 1});
    CHECK(hugeValues.has_value());
    if (hugeValues) {
        CHECK(relativeNear(hugeValues->scaleX, 1.0e200));
        CHECK(relativeNear(hugeValues->scaleY, 1.0e200));
        const auto roundTrip = transformFromValues(*hugeValues, {1, 1});
        CHECK(relativeNear(roundTrip.m00, huge.m00));
        CHECK(relativeNear(roundTrip.m11, huge.m11));
        CHECK(relativeNear(roundTrip.m01, huge.m01));
        CHECK(relativeNear(roundTrip.m10, huge.m10));
    }

    Document hugeDocument(CanvasSpec {.extent = {1, 1}});
    auto hugeSurface = std::make_shared<ContiguousRasterSurface>(Extent2u {1, 1});
    auto hugeLayer = Layer::raster("External huge transform", hugeSurface);
    const auto hugeLayerId = hugeLayer.id;
    CHECK(hugeDocument.insertLayer(0, std::move(hugeLayer)));
    CHECK(hugeDocument.setLayerTransform(hugeLayerId, huge));
    LayerTransformSession unsupported(hugeDocument, hugeLayerId);
    CHECK(!unsupported.active());
    CHECK(hugeDocument.layer(hugeLayerId)->localToDocument == huge);
}

void transformsNeverReplaceOrReviseRasterStorage()
{
    Fixture fixture({32, 24}, {11, 29, 47, 173}, {256, 256});
    const auto surfaceId = fixture.surface->id();
    const auto surfaceRevision = fixture.surface->revision();
    const auto* initialLayer = fixture.document.layer(fixture.layerId);
    CHECK(initialLayer != nullptr);
    const auto initialName = initialLayer ? initialLayer->name : std::string {};
    const auto initialVisibility = initialLayer && initialLayer->visible;
    const auto initialOpacity = initialLayer ? initialLayer->opacity : 0.0F;

    LayerTransformSession session(fixture.document, fixture.layerId);
    auto values = session.values();
    values.center = {113.0, 97.0};
    values.scaleX = -2.5;
    values.scaleY = 1.75;
    values.rotationDegrees = 48.0;
    CHECK(session.setValues(values));
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    const auto transformed = fixture.transform();

    const auto assertStorageIdentity = [&] {
        const auto* layer = fixture.document.layer(fixture.layerId);
        CHECK(layer != nullptr);
        const auto* raster = layer ? std::get_if<RasterLayer>(&layer->payload) : nullptr;
        CHECK(raster != nullptr);
        CHECK(raster && raster->surface.get() == fixture.surface.get());
        CHECK(raster && raster->surface->id() == surfaceId);
        CHECK(fixture.surface->revision() == surfaceRevision);
        CHECK(pixelAt(*fixture.surface, 3, 4) == Rgba8({11, 29, 47, 173}));
        CHECK(layer && layer->name == initialName);
        CHECK(layer && layer->visible == initialVisibility);
        CHECK(layer && layer->opacity == initialOpacity);
    };
    assertStorageIdentity();
    CHECK(fixture.history.undo(fixture.document));
    assertStorageIdentity();
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.transform() == transformed);
    assertStorageIdentity();
}

void rotationWrapAndRelativeSnapAreStable()
{
    {
        Fixture fixture;
        LayerTransformSession wrap(fixture.document, fixture.layerId);
        const auto center = wrap.values().center;
        CHECK(wrap.beginDrag(TransformHandle::Rotate,
            pointAtAngle(center, 80.0, 170.0)));
        CHECK(wrap.dragTo(pointAtAngle(center, 80.0, 179.0), {}, false));
        CHECK_NEAR(wrap.values().rotationDegrees, 9.0, 1.0e-10);
        CHECK(wrap.dragTo(pointAtAngle(center, 80.0, -179.0), {}, false));
        CHECK_NEAR(wrap.values().rotationDegrees, 11.0, 1.0e-10);
        CHECK(wrap.dragTo(pointAtAngle(center, 80.0, -160.0), {}, false));
        CHECK_NEAR(wrap.values().rotationDegrees, 30.0, 1.0e-10);
    }

    {
        Fixture fixture;
        LayerTransformSession reverse(fixture.document, fixture.layerId);
        const auto center = reverse.values().center;
        CHECK(reverse.beginDrag(TransformHandle::Rotate,
            pointAtAngle(center, 60.0, -170.0)));
        CHECK(reverse.dragTo(pointAtAngle(center, 60.0, 170.0), {}, false));
        CHECK_NEAR(reverse.values().rotationDegrees, -20.0, 1.0e-10);
    }

    {
        Fixture fixture;
        auto initialValues = challengingValues();
        initialValues.rotationDegrees = 37.0;
        const auto initial = transformFromValues(initialValues, {100, 80});
        CHECK(fixture.document.setLayerTransform(fixture.layerId, initial));
        LayerTransformSession snap(fixture.document, fixture.layerId);
        const auto center = snap.values().center;
        CHECK(snap.beginDrag(TransformHandle::Rotate,
            pointAtAngle(center, 70.0, 0.0)));
        CHECK(snap.dragTo(pointAtAngle(center, 70.0, 20.0),
            {.shift = true}, false));
        // Shift aligns the resolved layer angle to absolute document axes.
        CHECK_NEAR(snap.values().rotationDegrees, 60.0, 1.0e-10);
        CHECK(snap.dragTo(pointAtAngle(center, 70.0, 20.0), {}, false));
        CHECK_NEAR(snap.values().rotationDegrees, 57.0, 1.0e-10);
        CHECK(pointNear(snap.values().center, initialValues.center));
    }
}

void hitTestingUsesLogicalPixelsAtEveryZoom()
{
    const auto documentHandles = transformHandles({}, {100, 80});
    for (const double zoom : {0.5, 1.0, 7.0, 24.0}) {
        std::array<Vec2d, 8> logical {};
        for (std::size_t index = 0; index < logical.size(); ++index) {
            logical[index] = documentHandles[index] * zoom
                + Vec2d {37.0, 19.0};
        }
        CHECK(hitTestTransform(logical,
            logical[0] + Vec2d {-5.0, -5.0}) == TransformHandle::TopLeft);
        CHECK(hitTestTransform(logical,
            logical[4] + Vec2d {5.0, 5.0}) == TransformHandle::BottomRight);
    }

    // Below the point where handles overlap, the hit remains usable even
    // though no unique semantic corner can be inferred from screen position.
    std::array<Vec2d, 8> compact {};
    for (std::size_t index = 0; index < compact.size(); ++index) {
        compact[index] = documentHandles[index] * 0.08 + Vec2d {37.0, 19.0};
    }
    CHECK(hitTestTransform(compact, compact[0]) != TransformHandle::None);

    const std::array<Vec2d, 8> square {{{0, 0}, {50, 0}, {100, 0}, {100, 50},
        {100, 100}, {50, 100}, {0, 100}, {0, 50}}};
    CHECK(hitTestTransform(square, {50, 50}) == TransformHandle::Move);
    CHECK(hitTestTransform(square, {-15, -15}) == TransformHandle::Rotate);
    CHECK(hitTestTransform(square, {-40, -40}) == TransformHandle::None);
    CHECK(hitTestTransform(square,
        {std::numeric_limits<double>::quiet_NaN(), 0.0}) == TransformHandle::None);
}

void brushEraserAndSamplerUseTheCommittedTransform()
{
    constexpr Extent2u surfaceExtent {16, 16};
    constexpr Extent2u canvasExtent {96, 64};
    const Vec2d localPixelCenter {2.5, 3.5};

    {
        Fixture paint(surfaceExtent, {17, 29, 41, 0}, canvasExtent);
        const auto original = paint.transform();
        LayerTransformSession transform(paint.document, paint.layerId);
        auto values = transform.values();
        values.center = {48.0, 32.0};
        values.scaleX = -1.0;
        values.scaleY = 1.0;
        CHECK(transform.setValues(values));
        const auto transformed = transform.transform();
        const auto documentPoint = transformed.map(localPixelCenter);
        CHECK(pointNear(documentPoint, {53.5, 27.5}));
        CHECK(transform.commit(paint.history) == TransformCommitResult::Committed);

        auto sampled = sampleDocumentColor(paint.document, paint.layerId,
            documentPoint, ColorSampleSource::ActiveLayer);
        CHECK(sampled.available());
        CHECK(sampled.color == Rgba8({0, 0, 0, 0})); // Sampling canonicalizes transparent RGB.
        CHECK(applyDot(paint, documentPoint, BrushCompositeMode::Paint,
            {231, 47, 83, 255}) == RasterEditCommitResult::Committed);
        CHECK(pixelAt(*paint.surface, 2, 3) == Rgba8({231, 47, 83, 255}));
        sampled = sampleDocumentColor(paint.document, paint.layerId,
            documentPoint, ColorSampleSource::ActiveLayer);
        CHECK(sampled.color == Rgba8({231, 47, 83, 255}));
        CHECK(paint.history.undoDepth() == 2);
        CHECK(paint.history.undo(paint.document));
        CHECK(pixelAt(*paint.surface, 2, 3) == Rgba8({17, 29, 41, 0}));
        CHECK(paint.transform() == transformed);
        CHECK(paint.history.undo(paint.document));
        CHECK(paint.transform() == original);
        CHECK(paint.history.redo(paint.document));
        CHECK(paint.transform() == transformed);
        CHECK(paint.history.redo(paint.document));
        CHECK(pixelAt(*paint.surface, 2, 3) == Rgba8({231, 47, 83, 255}));
    }

    {
        Fixture erase(surfaceExtent, {61, 117, 203, 255}, canvasExtent);
        LayerTransformSession transform(erase.document, erase.layerId);
        auto values = transform.values();
        values.center = {48.0, 32.0};
        values.scaleX = -1.0;
        CHECK(transform.setValues(values));
        const auto documentPoint = transform.transform().map(localPixelCenter);
        CHECK(transform.commit(erase.history) == TransformCommitResult::Committed);
        CHECK(applyDot(erase, documentPoint, BrushCompositeMode::Erase,
            {255, 0, 255, 0}) == RasterEditCommitResult::Committed);
        CHECK(pixelAt(*erase.surface, 2, 3) == Rgba8({61, 117, 203, 0}));
        const auto active = sampleDocumentColor(erase.document, erase.layerId,
            documentPoint, ColorSampleSource::ActiveLayer);
        CHECK(active.color == Rgba8({0, 0, 0, 0})); // Stored erased pixels remain unchanged above.
        const auto merged = sampleDocumentColor(erase.document, {},
            documentPoint, ColorSampleSource::MergedVisible);
        CHECK(merged.color == Rgba8({0, 0, 0, 0}));
    }

    // Exact quarter-turn/reflection matrix proves sampling does not merely
    // understand translated or axis-aligned positive-scale layers.
    {
        Fixture sampled(surfaceExtent, {9, 18, 27, 255}, canvasExtent);
        const AffineTransform reflectedQuarterTurn {
            .m00 = 0.0, .m01 = 1.0, .m02 = 20.0,
            .m10 = 1.0, .m11 = 0.0, .m12 = 10.0};
        CHECK(sampled.document.setLayerTransform(sampled.layerId,
            reflectedQuarterTurn));
        const auto documentPoint = reflectedQuarterTurn.map(localPixelCenter);
        const auto result = sampleDocumentColor(sampled.document, sampled.layerId,
            documentPoint, ColorSampleSource::ActiveLayer);
        CHECK(result.available());
        CHECK(result.color == Rgba8({9, 18, 27, 255}));
        CHECK(result.texelsRead == 1);
    }
}

void groupFrameUsesTightMixedLayerGeometryWithoutChangingContent()
{
    GroupFixture fixture;
    const auto text = std::get<TextLayer>(fixture.document.layer(fixture.ids[1])->payload);
    const auto shape = std::get<ShapeLayer>(fixture.document.layer(fixture.ids[2])->payload);
    const auto rasterRevision = fixture.raster->revision();
    const auto textRevision = fixture.document.layer(fixture.ids[1])->textRevision;
    const auto shapeRevision = fixture.document.layer(fixture.ids[2])->shapeRevision;
    const auto documentRevision = fixture.document.revision();
    LayerTransformSession session(fixture.document, fixture.ids);
    CHECK(session.active());
    CHECK(session.grouped());
    CHECK(fixture.document.revision() == documentRevision);
    CHECK(fixture.matrices() == fixture.entry);

    Vec2d minimum {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec2d maximum {-minimum.x, -minimum.y};
    for (std::size_t index = 0; index < fixture.ids.size(); ++index) {
        const auto size = fixture.sizes[index];
        for (const auto corner : {Vec2d {0, 0}, Vec2d {size.width, 0},
                 Vec2d {size.width, size.height}, Vec2d {0, size.height}}) {
            const auto p = fixture.entry[index].map(corner);
            minimum.x = std::min(minimum.x, p.x);
            minimum.y = std::min(minimum.y, p.y);
            maximum.x = std::max(maximum.x, p.x);
            maximum.y = std::max(maximum.y, p.y);
        }
    }
    CHECK(pointNear(session.transform().map({0, 0}), minimum));
    CHECK_NEAR(session.geometryExtent().width, maximum.x - minimum.x, 1e-12);
    CHECK_NEAR(session.geometryExtent().height, maximum.y - minimum.y, 1e-12);
    CHECK(pointNear(session.values().center, (minimum + maximum) * 0.5));
    CHECK_NEAR(session.values().scaleX, 1, 0);
    CHECK_NEAR(session.values().scaleY, 1, 0);
    CHECK_NEAR(session.values().rotationDegrees, 0, 0);
    const auto entryFrame = session.transform();
    auto values = session.values();
    values.center = values.center + Vec2d {17.25, -9.5};
    values.rotationDegrees = 36.5;
    values.scaleX = -1.3;
    values.scaleY = 0.75;
    CHECK(session.setValues(values));
    fixture.checkDelta(session.transform(), entryFrame);
    const auto transformed = fixture.matrices();
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 1); // Not one entry per target.
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == fixture.entry);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.matrices() == transformed);
    CHECK(fixture.raster->revision() == rasterRevision);
    CHECK(pixelAt(*fixture.raster, 3, 4) == Rgba8({39, 71, 113, 193}));
    CHECK(std::get<RasterLayer>(fixture.document.layer(fixture.ids[0])->payload).surface
        == fixture.raster);
    CHECK(std::get<TextLayer>(fixture.document.layer(fixture.ids[1])->payload) == text);
    CHECK(std::get<ShapeLayer>(fixture.document.layer(fixture.ids[2])->payload) == shape);
    CHECK(fixture.document.layer(fixture.ids[1])->textRevision == textRevision);
    CHECK(fixture.document.layer(fixture.ids[2])->shapeRevision == shapeRevision);
    CHECK(fixture.document.layer(fixture.ids[1])->renderCache == fixture.textCache);
    CHECK(fixture.document.layer(fixture.ids[2])->renderCache == fixture.shapeCache);
}

void groupAnchorsSignedCrossingAndSharedShearAreStable()
{
    GroupFixture fixture;
    LayerTransformSession session(fixture.document, fixture.ids);
    const auto entryFrame = session.transform();
    const auto size = session.geometryExtent();
    const auto origin = entryFrame.map({0, 0});
    const auto press = entryFrame.map({size.width, size.height});
    CHECK(session.beginDrag(TransformHandle::BottomRight, press));
    CHECK(session.dragTo(press + Vec2d {37, 11}, {.shift = true}, false));
    CHECK_NEAR(session.values().scaleX, session.values().scaleY, 1e-12);
    CHECK(pointNear(session.transform().map({0, 0}), origin));
    fixture.checkDelta(session.transform(), entryFrame);
    session.endDrag();
    const auto scaled = fixture.matrices();
    CHECK(session.pendingHistory().undoDepth() == 1);

    const auto center = session.values().center;
    CHECK(session.beginDrag(TransformHandle::Rotate, pointAtAngle(center, 50, 170)));
    CHECK(session.dragTo(pointAtAngle(center, 50, -160), {}, false));
    CHECK_NEAR(session.values().rotationDegrees, 30, 1e-10);
    fixture.checkDelta(session.transform(), entryFrame);
    session.endDrag();
    const auto rotated = fixture.matrices();
    const auto centeredPress = session.transform().map({0, 0});
    CHECK(session.beginDrag(TransformHandle::TopLeft, centeredPress));
    CHECK(session.dragTo(centeredPress + Vec2d {-23, 17}, {.alt = true}, false));
    CHECK(pointNear(session.values().center, center));
    fixture.checkDelta(session.transform(), entryFrame);
    session.cancelDrag();
    CHECK(fixture.matrices() == rotated);
    CHECK(session.undo());
    CHECK(fixture.matrices() == scaled);
    CHECK(session.undo());
    CHECK(fixture.matrices() == fixture.entry);

    const auto right = entryFrame.map({size.width, size.height / 2});
    const auto opposite = entryFrame.map({0, size.height / 2});
    CHECK(session.beginDrag(TransformHandle::Right, right));
    CHECK(session.dragTo(opposite, {}, false));
    CHECK_NEAR(session.values().scaleX, kMinimumLayerScale, 0);
    CHECK(pointNear(session.transform().map({0, size.height / 2}), opposite));
    for (const auto& matrix : fixture.matrices()) CHECK(matrix.inverted().has_value());
    CHECK(session.dragTo(opposite - Vec2d {size.width / 2, 0}, {}, false));
    CHECK_NEAR(session.values().scaleX, -0.5, 1e-12);
    fixture.checkDelta(session.transform(), entryFrame);
    session.endDrag();
    CHECK(!session.pendingHistory().canRedo());
    CHECK(session.pendingHistory().undoDepth() == 1);
    const auto crossed = fixture.matrices();
    CHECK(session.flip(false));
    CHECK(session.values().scaleX < 0 && session.values().scaleY < 0);
    fixture.checkDelta(session.transform(), entryFrame);
    CHECK(session.undo());
    CHECK(fixture.matrices() == crossed);
    session.cancel();
    CHECK(fixture.matrices() == fixture.entry);
}

void groupNumericActionsAndPendingHistoryKeepExactUndoSteps()
{
    GroupFixture fixture;
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerOpacityCommand>(fixture.unselected, 0.4F)));
    LayerTransformSession session(fixture.document, fixture.ids);
    const auto entryFrame = session.transform();
    const auto entryCenter = session.values().center;
    for (int step = 1; step <= 40; ++step) {
        auto values = session.values();
        values.center = entryCenter + Vec2d {step * 0.5, -step * 0.25};
        CHECK(session.setValues(values));
        CHECK(!session.pendingHistory().canUndo());
    }
    CHECK(session.completeAction());
    CHECK(!session.completeAction());
    const auto moved = fixture.matrices();
    CHECK(session.flip(true));
    const auto flipped = fixture.matrices();
    auto values = session.values();
    values.rotationDegrees = -71.25;
    CHECK(session.setValues(values));
    CHECK(session.completeAction());
    const auto rotated = fixture.matrices();
    fixture.checkDelta(session.transform(), entryFrame);
    const auto pendingMemory = session.pendingHistory().memoryUsed();
    CHECK(pendingMemory == 3 * session.pendingHistory().latestUndoMemoryCost());
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(session.undo());
    CHECK(fixture.matrices() == flipped);
    CHECK(session.undo());
    CHECK(fixture.matrices() == moved);
    CHECK(session.redo());
    CHECK(fixture.matrices() == flipped);
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.pendingHistory().redoDepth() == 1);
    CHECK(session.active());
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 3);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.matrices() == rotated);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == flipped);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == moved);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == fixture.entry);
    CHECK(fixture.history.undo(fixture.document));
    CHECK_NEAR(fixture.document.layer(fixture.unselected)->opacity, 1, 0);
}

void groupNoOpsCancellationAndDivergencePreserveExternalRedo()
{
    GroupFixture fixture;
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.unselected, false)));
    CHECK(fixture.history.undo(fixture.document));
    const auto externalMemory = fixture.history.memoryUsed();
    const auto entryState = fixture.document.contentState();
    {
        LayerTransformSession session(fixture.document, fixture.ids);
        CHECK(session.beginDrag(TransformHandle::Move, {10, 20}));
        CHECK(session.dragTo({10, 20}, {}, false));
        session.endDrag();
        CHECK(!session.pendingHistory().canUndo());
        CHECK(session.flip(true));
        CHECK(session.flip(false));
        CHECK(session.undo());
        CHECK(session.undo());
        CHECK(fixture.matrices() == fixture.entry);
        auto values = session.values();
        values.rotationDegrees += 360;
        values.center.x += 1e-10;
        CHECK(session.setValues(values));
        CHECK(!session.completeAction());
        CHECK(session.pendingHistory().redoDepth() == 2);
        CHECK(session.commit(fixture.history) == TransformCommitResult::NoChange);
    }
    CHECK(fixture.history.undoDepth() == 0);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.memoryUsed() == externalMemory);
    CHECK(fixture.document.contentState() == entryState);
    {
        LayerTransformSession session(fixture.document, fixture.ids);
        CHECK(session.flip(true));
        CHECK(session.flip(false));
        CHECK(session.undo());
        auto values = session.values();
        values.center.x += 29;
        CHECK(session.setValues(values));
        CHECK(session.completeAction());
        CHECK(!session.pendingHistory().canRedo());
        CHECK(session.beginDrag(TransformHandle::Move, {3, 4}));
        CHECK(session.dragTo({23, 34}, {}, false));
        session.cancel();
    }
    CHECK(fixture.matrices() == fixture.entry);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.memoryUsed() == externalMemory);
    CHECK(fixture.document.contentState() == entryState);
    LayerTransformSession divergent(fixture.document, fixture.ids);
    CHECK(divergent.flip(true));
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(divergent.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(!fixture.history.canRedo());
}

void groupStableIdsSurviveReorderingAndInvalidTargetsRejectAtomically()
{
    GroupFixture fixture;
    LayerTransformSession session(fixture.document, fixture.ids);
    CHECK(fixture.document.moveLayer(fixture.ids[0], 3));
    CHECK(fixture.document.moveLayer(fixture.ids[2], 0));
    CHECK(session.targetAvailable());
    CHECK(session.flip(true));
    const auto transformed = fixture.matrices();
    CHECK(session.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.document.moveLayer(fixture.ids[1], 3));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == fixture.entry);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.matrices() == transformed);

    // A stale final target must reject before an earlier valid target is touched.
    auto external = transformed[2];
    external.m02 += 99;
    CHECK(fixture.document.setLayerTransform(fixture.ids[2], external));
    const auto invalidated = fixture.matrices();
    const auto revision = fixture.document.revision();
    const auto memory = fixture.history.memoryUsed();
    CHECK(!fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == invalidated);
    CHECK(fixture.document.revision() == revision);
    CHECK(fixture.history.memoryUsed() == memory);
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.document.setLayerTransform(fixture.ids[2], transformed[2]));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.matrices() == fixture.entry);

    // Redo has the same all-target guard, not only undo/application.
    auto removed = fixture.document.takeLayer(fixture.ids[2]);
    CHECK(removed.has_value());
    CHECK(!fixture.history.redo(fixture.document));
    CHECK(fixture.document.layer(fixture.ids[0])->localToDocument == fixture.entry[0]);
    CHECK(fixture.document.layer(fixture.ids[1])->localToDocument == fixture.entry[1]);
    CHECK(fixture.history.redoDepth() == 1);
    if (removed) CHECK(fixture.document.insertLayer(removed->index, std::move(removed->layer)));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.matrices() == transformed);
}

void removedGroupTargetCancelsOwnedSurvivorsWithoutResurrection()
{
    GroupFixture fixture;
    LayerTransformSession session(fixture.document, fixture.ids);
    CHECK(session.flip(true));
    CHECK(session.beginDrag(TransformHandle::Move, {1, 2}));
    CHECK(session.dragTo({31, 42}, {}, false));
    const auto preview = fixture.matrices();
    const auto removed = fixture.document.takeLayer(fixture.ids[2]);
    CHECK(removed.has_value());
    CHECK(removed && removed->layer.localToDocument == preview[2]);
    fixture.document.advanceContentState(); // An unrelated removal owns this state.
    const auto externalState = fixture.document.contentState();
    CHECK(!session.targetAvailable());
    CHECK(!session.dragTo({91, 102}, {}, false));
    CHECK(fixture.document.layer(fixture.ids[0])->localToDocument == preview[0]);
    CHECK(fixture.document.layer(fixture.ids[1])->localToDocument == preview[1]);
    CHECK(session.commit(fixture.history) == TransformCommitResult::TargetUnavailable);
    CHECK(!session.active());
    CHECK(!fixture.document.containsLayer(fixture.ids[2]));
    CHECK(fixture.document.layer(fixture.ids[0])->localToDocument == fixture.entry[0]);
    CHECK(fixture.document.layer(fixture.ids[1])->localToDocument == fixture.entry[1]);
    CHECK(fixture.document.contentState() == externalState);
    CHECK(!fixture.history.canUndo());
    CHECK(!session.pendingHistory().canUndo());

    GroupFixture changed;
    LayerTransformSession cancelled(changed.document, changed.ids);
    CHECK(cancelled.flip(false));
    auto external = changed.document.layer(changed.ids[1])->localToDocument;
    external.m12 += 13;
    CHECK(changed.document.setLayerTransform(changed.ids[1], external));
    cancelled.cancel();
    CHECK(changed.document.layer(changed.ids[0])->localToDocument == changed.entry[0]);
    CHECK(changed.document.layer(changed.ids[1])->localToDocument == external);
    CHECK(changed.document.layer(changed.ids[2])->localToDocument == changed.entry[2]);
}

void guardedTransformBatchNeverPublishesPartialChanges()
{
    GroupFixture fixture;
    std::array<LayerTransformUpdate, 3> updates {};
    for (std::size_t index = 0; index < updates.size(); ++index) {
        auto after = fixture.entry[index];
        after.m02 += 17;
        updates[index] = {fixture.ids[index], fixture.entry[index], after};
    }
    const auto validLast = updates.back();
    const auto revision = fixture.document.revision();
    updates.back().before.m12 += 1;
    CHECK(!fixture.document.setLayerTransforms(updates));
    CHECK(fixture.matrices() == fixture.entry);
    CHECK(fixture.document.revision() == revision);
    updates.back() = validLast;
    updates.back().after.m02 = std::numeric_limits<double>::quiet_NaN();
    CHECK(!fixture.document.setLayerTransforms(updates));
    CHECK(fixture.matrices() == fixture.entry);
    CHECK(fixture.document.revision() == revision);
    updates.back() = validLast;
    CHECK(fixture.document.setLayerTransforms(updates));
    for (std::size_t index = 0; index < updates.size(); ++index)
        CHECK(fixture.document.layer(fixture.ids[index])->localToDocument == updates[index].after);
}

} // namespace

int main()
{
    multipleGesturesKeepIndividualCommands();
    cancellationNoOpAndRedoAreExact();
    pendingBranchPublishesBothStacksAndExactMemory();
    cancellingDivergentActionsPreservesPreexistingHistoryAndRedo();
    allUndoneAndNoOpActionsPreserveExistingRedo();
    numericScrubbingGroupsAtCompletionAndPreservesSignedValues();
    completedActionsReturningToEntryRemainIndividuallyUndoable();
    stableLayerAndSurfaceIdentityProtectHistory();
    rotatedFlippedShearedAnchorsStayFixed();
    individualDragCancellationRestoresExactShearedMatrices();
    aspectConstraintUsesPressTimeProjectionAndShiftToggle();
    constrainedScaleClampsOneCommonFactorAtNumericLimits();
    signedScaleCrossingAndBoundsRemainInvertible();
    affineDecompositionIsFiniteAndRoundTripsWithoutIdentityLoss();
    transformsNeverReplaceOrReviseRasterStorage();
    rotationWrapAndRelativeSnapAreStable();
    hitTestingUsesLogicalPixelsAtEveryZoom();
    brushEraserAndSamplerUseTheCommittedTransform();
    groupFrameUsesTightMixedLayerGeometryWithoutChangingContent();
    groupAnchorsSignedCrossingAndSharedShearAreStable();
    groupNumericActionsAndPendingHistoryKeepExactUndoSteps();
    groupNoOpsCancellationAndDivergencePreserveExternalRedo();
    groupStableIdsSurviveReorderingAndInvalidTargetsRejectAtomically();
    removedGroupTargetCancelsOwnedSurvivorsWithoutResurrection();
    guardedTransformBatchNeverPublishesPartialChanges();

    if (failures != 0) {
        std::cerr << failures << " layer transform assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All layer transform tests passed\n";
    return EXIT_SUCCESS;
}
