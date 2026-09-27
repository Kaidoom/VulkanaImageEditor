#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerCropSession.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace c = imageeditor::core;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

bool near(double a, double b) { return std::isfinite(a) && std::abs(a - b) < 1e-8; }
bool near(c::Vec2d a, c::Vec2d b) { return near(a.x, b.x) && near(a.y, b.y); }
bool near(c::RectD a, c::RectD b)
{ return near(a.x, b.x) && near(a.y, b.y) && near(a.width, b.width) && near(a.height, b.height); }
std::vector<std::byte> pixels(const c::RasterSurface& surface)
{
    const auto e = surface.extent();
    std::vector<std::byte> bytes(std::size_t(e.width) * e.height * 4);
    surface.copyRgba8({0, 0, int(e.width), int(e.height)}, bytes, std::size_t(e.width) * 4);
    return bytes;
}

struct Fixture {
    c::Document document {{{512, 512}, 96}};
    c::History history;
    std::shared_ptr<c::ContiguousRasterSurface> source;
    c::LayerId id;
    explicit Fixture(c::AffineTransform transform = {}, std::optional<c::RectD> crop = {},
        c::Extent2u extent = {100, 80})
        : source(std::make_shared<c::ContiguousRasterSurface>(extent, c::Rgba8 {43, 81, 127, 191}))
    {
        auto layer = c::Layer::raster("Crop source", source);
        id = layer.id; layer.localToDocument = transform; layer.crop = crop;
        CHECK(document.insertLayer(0, std::move(layer)));
        document.markSaved();
    }
    std::optional<c::RectD> crop() const { return document.layer(id)->crop; }
};

void eachActionPublishesItsOwnCommandAndLocalRedoBranch()
{
    Fixture f;
    CHECK(f.history.execute(f.document, std::make_unique<c::SetLayerVisibilityCommand>(f.id, false)));
    const auto precedingMemory = f.history.memoryUsed();
    CHECK(f.history.execute(f.document, std::make_unique<c::SetLayerOpacityCommand>(f.id, .25F)));
    CHECK(f.history.undo(f.document));
    f.document.markSaved();
    const auto entryState = f.document.contentState();
    const auto externalMemory = f.history.memoryUsed();
    const auto externalRedo = std::string(f.history.redoLabel());
    c::LayerCropSession session(f.document, f.id);
    CHECK(session.active()); CHECK(session.targetAvailable()); CHECK(session.layerId() == f.id);
    CHECK(near(session.frame(), {0, 0, 100, 80}));
    CHECK(!session.undo()); CHECK(!session.redo());
    const c::RectD first {10, 20, 60, 40};
    CHECK(session.setRect(first)); CHECK(session.completeAction());
    CHECK(session.pendingHistory().undoDepth() == 1);
    CHECK(f.document.isModified());
    CHECK(session.beginDrag(c::TransformHandle::Move, {20, 30}));
    CHECK(session.dragTo({27, 19}, {}, false)); session.endDrag();
    const c::RectD moved {17, 9, 60, 40};
    CHECK(near(session.frame(), moved));
    CHECK(session.pendingHistory().undoDepth() == 2);
    CHECK(session.removeCrop()); CHECK(!f.crop());
    CHECK(session.pendingHistory().undoDepth() == 3);
    const auto pendingMemory = session.pendingHistory().memoryUsed();
    const auto commandMemory = session.pendingHistory().latestUndoMemoryCost();
    CHECK(pendingMemory == 3 * commandMemory);
    CHECK(f.history.undoDepth() == 1); CHECK(f.history.redoDepth() == 1);
    CHECK(f.history.memoryUsed() == externalMemory); CHECK(f.history.redoLabel() == externalRedo);
    CHECK(session.undo()); CHECK(f.crop() == moved);
    CHECK(session.undo()); CHECK(f.crop() == first);
    CHECK(session.redo()); CHECK(f.crop() == moved);
    CHECK(session.pendingHistory().undoDepth() == 2); CHECK(session.pendingHistory().redoDepth() == 1);
    CHECK(session.commit(f.history) == c::TransformCommitResult::Committed);
    CHECK(!session.active()); CHECK(session.pendingHistory().memoryUsed() == 0);
    CHECK(f.history.undoDepth() == 3); CHECK(f.history.redoDepth() == 1);
    CHECK(f.history.memoryUsed() == precedingMemory + pendingMemory);
    CHECK(f.history.redo(f.document)); CHECK(!f.crop());
    CHECK(f.history.undo(f.document)); CHECK(f.crop() == moved);
    CHECK(f.history.undo(f.document)); CHECK(f.crop() == first);
    CHECK(f.history.undo(f.document)); CHECK(!f.crop());
    CHECK(f.document.contentState() == entryState); CHECK(!f.document.isModified());
    CHECK(f.history.memoryUsed() == precedingMemory + pendingMemory);
}

void cancellationAndAllUndoneApplyPreserveExternalRedo()
{
    Fixture f;
    CHECK(f.history.execute(f.document, std::make_unique<c::SetLayerOpacityCommand>(f.id, .75F)));
    CHECK(f.history.execute(f.document, std::make_unique<c::SetLayerVisibilityCommand>(f.id, false)));
    CHECK(f.history.undo(f.document)); f.document.markSaved();
    const auto entryState = f.document.contentState();
    const auto memory = f.history.memoryUsed();
    const auto redoLabel = std::string(f.history.redoLabel());
    {
        c::LayerCropSession session(f.document, f.id);
        CHECK(session.setRect({2, 3, 40, 30})); CHECK(session.completeAction());
        CHECK(session.setRect({5, 7, 20, 10})); CHECK(session.completeAction());
        CHECK(session.undo());
        CHECK(session.setRect({11, 13, 30, 20})); CHECK(session.completeAction());
        CHECK(!session.pendingHistory().canRedo());
        const auto completed = f.crop();
        CHECK(session.beginDrag(c::TransformHandle::Move, {0, 0}));
        CHECK(session.dragTo({91, -27}, {}, false));
        CHECK(session.dragging()); session.cancelDrag();
        CHECK(!session.dragging()); CHECK(f.crop() == completed);
        CHECK(session.pendingHistory().undoDepth() == 2);
        session.cancel(); CHECK(!session.active()); CHECK(!f.crop());
        CHECK(session.pendingHistory().memoryUsed() == 0);
    }
    CHECK(f.document.contentState() == entryState); CHECK(!f.document.isModified());
    CHECK(f.history.undoDepth() == 1); CHECK(f.history.redoDepth() == 1);
    CHECK(f.history.memoryUsed() == memory); CHECK(f.history.redoLabel() == redoLabel);
    {
        c::LayerCropSession session(f.document, f.id);
        CHECK(session.setRect({4, 5, 40, 30})); CHECK(session.completeAction());
        CHECK(session.setRect({6, 7, 20, 10})); CHECK(session.completeAction());
        CHECK(session.undo()); CHECK(session.undo()); CHECK(!f.crop());
        CHECK(session.pendingHistory().redoDepth() == 2);
        CHECK(session.commit(f.history) == c::TransformCommitResult::NoChange);
    }
    CHECK(f.document.contentState() == entryState); CHECK(!f.document.isModified());
    CHECK(f.history.undoDepth() == 1); CHECK(f.history.redoDepth() == 1);
    CHECK(f.history.memoryUsed() == memory); CHECK(f.history.redoLabel() == redoLabel);
    {
        c::LayerCropSession abandoned(f.document, f.id);
        CHECK(abandoned.setRect({8, 9, 20, 30}));
    }
    CHECK(!f.crop()); CHECK(f.document.contentState() == entryState);
    CHECK(f.history.redo(f.document)); CHECK(!f.document.layer(f.id)->visible);
}

void uncroppedClickAndOutAndBackAreNoOps()
{
    for (int h = int(c::TransformHandle::TopLeft); h <= int(c::TransformHandle::Move); ++h) {
        Fixture f;
        c::LayerCropSession session(f.document, f.id);
        const auto handle = c::TransformHandle(h);
        const auto handles = c::geometryTransformHandles(session.frameTransform(), {100, 80});
        const auto press = handle == c::TransformHandle::Move ? c::Vec2d {50, 40} : handles[std::size_t(h)];
        CHECK(session.beginDrag(handle, press));
        CHECK(session.dragTo(press, {}, false)); session.endDrag();
        CHECK(!f.crop()); CHECK(session.pendingHistory().undoDepth() == 0);
        CHECK(session.beginDrag(handle, press));
        CHECK(session.dragTo(press + c::Vec2d {9, 11}, {}, false));
        CHECK(session.dragTo(press, {}, false)); session.endDrag();
        CHECK(!f.crop()); CHECK(session.pendingHistory().undoDepth() == 0);
        CHECK(!session.removeCrop());
        CHECK(session.commit(f.history) == c::TransformCommitResult::NoChange);
        CHECK(f.history.memoryUsed() == 0); CHECK(!f.document.isModified());
    }
    Fixture empty({}, c::RectD {3, 4, 0, 0});
    c::LayerCropSession session(empty.document, empty.id);
    CHECK(session.active()); CHECK(session.frame().width > 0); CHECK(session.frame().height > 0);
    CHECK(empty.crop()->empty());
    CHECK(session.commit(empty.history) == c::TransformCommitResult::NoChange);
    CHECK(empty.crop() == c::RectD({3, 4, 0, 0}));
}

void crossingHandlesNeverFlipsOrRebasesSourceContent()
{
    const std::array<c::AffineTransform, 4> matrices {{{}, {0, -1.2, 130, .8, 0, 20},
        {-1.1, .3, 140.25, .2, 1.4, 120.5}, {1, .7, 17, .3, 1.2, -5}}};
    for (const auto matrix : matrices) {
        Fixture f(matrix);
        const auto bytes = pixels(*f.source); const auto revision = f.source->revision();
        c::LayerCropSession session(f.document, f.id);
        CHECK(session.beginDrag(c::TransformHandle::TopLeft, matrix.map({0, 0})));
        CHECK(session.dragTo(matrix.map({130, 100}), {}, false)); session.endDrag();
        CHECK(near(session.frame(), {100, 80, 30, 20}));
        CHECK(near(session.frameTransform().map({0, 0}), matrix.map({100, 80})));
        CHECK(f.document.layer(f.id)->localToDocument == matrix);
        CHECK(session.undo()); CHECK(!f.crop());
        CHECK(session.beginDrag(c::TransformHandle::Right, matrix.map({100, 40})));
        CHECK(session.dragTo(matrix.map({-20, 40}), {}, false)); session.endDrag();
        CHECK(near(session.frame(), {-20, 0, 20, 80}));
        CHECK(f.document.layer(f.id)->localToDocument == matrix);
        CHECK(session.beginDrag(c::TransformHandle::Move, matrix.map({-10, 40})));
        CHECK(session.dragTo(matrix.map({-3, 29}), {}, false)); session.endDrag();
        CHECK(near(session.frame(), {-13, -11, 20, 80}));
        CHECK(session.commit(f.history) == c::TransformCommitResult::Committed);
        CHECK(f.document.layer(f.id)->localToDocument == matrix);
        CHECK(f.source->revision() == revision); CHECK(pixels(*f.source) == bytes);
        CHECK(f.source->dirtySince(revision).empty());
    }
}

void aspectConstraintShiftAndAltKeepIndependentExpectedAnchors()
{
    struct Case { c::TransformModifiers modifiers; bool locked; c::RectD expected; };
    const std::array<Case, 5> cases {{{{}, true, {10, 20, 112, 56}},
        {{true, false}, false, {10, 20, 112, 56}},
        {{true, false}, true, {10, 20, 100, 80}},
        {{false, true}, true, {-22, 4, 144, 72}},
        {{false, true}, false, {-10, -20, 120, 120}}}};
    for (const auto& test : cases) {
        Fixture f({}, c::RectD {10, 20, 80, 40});
        c::LayerCropSession session(f.document, f.id);
        CHECK(session.beginDrag(c::TransformHandle::BottomRight, {90, 60}));
        CHECK(session.dragTo({110, 100}, test.modifiers, test.locked)); session.endDrag();
        const auto result = session.frame(); CHECK(near(result, test.expected));
        if (test.modifiers.alt) CHECK(near(c::Vec2d {result.x + result.width / 2, result.y + result.height / 2}, {50, 40}));
        else CHECK(near(c::Vec2d {result.x, result.y}, {10, 20}));
        CHECK(f.document.layer(f.id)->localToDocument == c::AffineTransform {});
        session.cancel(); CHECK(f.crop() == c::RectD({10, 20, 80, 40}));
    }
}

void typedGeometryAndDensityCachesStayUnchanged()
{
    for (bool isText : {true, false}) {
        c::Document document({{128, 128}, 96}); c::History history;
        c::TextLayer text; text.utf8 = "Crop never deletes text"; text.defaultStyle.sizePixels = 17.5;
        c::ShapeLayer shape; shape.size = {11.5, 7.25}; shape.strokeEnabled = true; shape.strokeWidth = 2.5;
        auto layer = isText ? c::Layer::text("Text", text) : c::Layer::shape("Shape", shape);
        auto cache = std::make_shared<c::LayerRenderCache>();
        cache->surface = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {24, 20}, c::Rgba8 {13, 29, 47, 0});
        cache->pixelsToLocal = {.5, 0, -1, 0, .5, -2}; cache->density = 2;
        cache->logicalExtent = {12, 10}; layer.renderCache = cache;
        layer.localToDocument = {-1.1, .3, 40, .2, 1.4, 30};
        const auto bytes = pixels(*cache->surface);
        const auto revision = cache->surface->revision();
        CHECK(document.insertLayer(0, layer)); document.markSaved();
        // Layer construction/admission normalizes text runs. Compare crop
        // edits with canonical admitted data, not the pre-normalized fixture.
        const auto original = *document.layer(layer.id);
        c::LayerCropSession session(document, layer.id);
        CHECK(session.active()); CHECK(near(session.frame(), {-1, -2, 12, 10}));
        CHECK(session.setRect({-.5, -.25, 5.75, 4.125})); CHECK(session.completeAction());
        CHECK(session.removeCrop()); CHECK(session.undo());
        CHECK(session.commit(history) == c::TransformCommitResult::Committed);
        CHECK(history.undo(document)); CHECK(history.redo(document));
        const auto* after = document.layer(layer.id);
        CHECK(after->renderCache == cache); CHECK(after->localToDocument == original.localToDocument);
        CHECK(after->textRevision == original.textRevision); CHECK(after->shapeRevision == original.shapeRevision);
        CHECK(after->adjustments == original.adjustments); CHECK(after->adjustmentRevision == original.adjustmentRevision);
        if (isText) CHECK(std::get<c::TextLayer>(after->payload) == std::get<c::TextLayer>(original.payload));
        else CHECK(std::get<c::ShapeLayer>(after->payload) == std::get<c::ShapeLayer>(original.payload));
        CHECK(cache->surface->revision() == revision); CHECK(pixels(*cache->surface) == bytes);
        CHECK(cache->surface->dirtySince(revision).empty());
        CHECK(history.latestUndoMemoryCost() < 1024); // Inline metadata, not source/cache pixels.
    }
}

void emptyCroppedGroupMembersMoveWithoutContributingBounds()
{
    for (bool selectGroup : {false, true}) {
        c::Document document({{512, 512}, 96}); c::History history;
        const auto source = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {20, 10}, c::Rgba8 {41, 73, 127, 211});
        const auto bytes = pixels(*source); const auto revision = source->revision();
        auto empty = c::Layer::raster("Empty crop, still a group member", source);
        empty.localToDocument = {1, .2, -400, .1, 1, 300};
        empty.crop = c::RectD {5, 3, 0, 2};
        auto visible = c::Layer::raster("Visible cropped member", source);
        visible.localToDocument = {1, 0, 40, 0, 1, 30};
        visible.crop = c::RectD {2, 1, 8, 6};
        CHECK(document.insertLayer(0, empty)); CHECK(document.insertLayer(1, visible));
        const auto group = c::makeLayerId();
        auto tree = document.tree();
        tree.containers.push_back({group, "Mixed group", c::ContainerKind::Group, c::ColorLabel::None, {empty.id, visible.id}});
        tree.roots = {group}; CHECK(document.replaceStructure(document.tree(), tree));
        document.markSaved();
        const auto selection = selectGroup ? std::vector<c::LayerId> {group} : std::vector<c::LayerId> {empty.id, visible.id};
        c::LayerTransformSession transform(document, selection);
        CHECK(transform.active()); CHECK(transform.grouped()); CHECK(transform.targetAvailable());
        // The distant empty member must neither disable Ctrl+T nor enlarge its frame.
        CHECK(near(transform.geometryExtent().width, 8)); CHECK(near(transform.geometryExtent().height, 6));
        CHECK(near(transform.transform().map({0, 0}), {42, 31}));
        CHECK(transform.beginDrag(c::TransformHandle::Move, {46, 34}));
        CHECK(transform.dragTo({59, 27}, {}, false)); transform.endDrag();
        const c::AffineTransform delta {1, 0, 13, 0, 1, -7};
        const auto emptyMoved = c::composeAffine(delta, empty.localToDocument);
        const auto visibleMoved = c::composeAffine(delta, visible.localToDocument);
        CHECK(document.layer(empty.id)->localToDocument == emptyMoved);
        CHECK(document.layer(visible.id)->localToDocument == visibleMoved);
        CHECK(document.layer(empty.id)->crop == empty.crop); CHECK(document.layer(visible.id)->crop == visible.crop);
        CHECK(transform.pendingHistory().undoDepth() == 1);
        CHECK(transform.commit(history) == c::TransformCommitResult::Committed);
        CHECK(history.undo(document));
        CHECK(document.layer(empty.id)->localToDocument == empty.localToDocument);
        CHECK(document.layer(visible.id)->localToDocument == visible.localToDocument);
        CHECK(!document.isModified());
        CHECK(history.redo(document));
        CHECK(document.layer(empty.id)->localToDocument == emptyMoved);
        CHECK(document.layer(visible.id)->localToDocument == visibleMoved);
        CHECK(document.setLayerCrop(empty.id, {}));
        CHECK(document.layer(empty.id)->localToDocument == emptyMoved);
        CHECK(document.layer(visible.id)->localToDocument == visibleMoved);
        CHECK(c::layerInteractionBounds(*document.layer(empty.id)) == c::RectD({0, 0, 20, 10}));
        CHECK(source->revision() == revision); CHECK(pixels(*source) == bytes);
    }
}

void invalidCropAdmissionIsAtomicForEveryLayerType()
{
    Fixture f;
    CHECK(f.history.execute(f.document, std::make_unique<c::SetLayerVisibilityCommand>(f.id, false)));
    CHECK(f.history.undo(f.document));
    const auto tree = f.document.tree(); const auto revision = f.document.revision();
    const auto contentState = f.document.contentState(); const auto historyMemory = f.history.memoryUsed();
    const auto sourceRevision = f.source->revision(); const auto bytes = pixels(*f.source);
    c::TextLayer text; text.utf8 = "Invalid crop fixture";
    c::ShapeLayer shape; shape.size = {12, 9};
    const std::array<c::Layer, 3> types {c::Layer::raster("Raster", f.source),
        c::Layer::text("Text", text), c::Layer::shape("Shape", shape)};
    const std::array<c::RectD, 5> invalid {{{0, 0, -1, 1}, {0, 0, 1, -1}, {1e9, 0, 1, 1},
        {0, -1e9 - 1, 1, 1}, {0, 0, std::numeric_limits<double>::infinity(), 1}}};
    for (const auto& type : types) for (const auto crop : invalid) {
        auto layer = type; layer.crop = crop;
        CHECK(!f.document.insertLayer(1, layer));
        CHECK(!f.history.execute(f.document, std::make_unique<c::AddLayerCommand>(layer, 1)));
        // Batch admission must validate all additions before removing an old layer.
        auto valid = c::Layer::raster("Valid alongside invalid", f.source);
        const std::array added {valid, layer}; const std::array removed {f.id};
        auto replacement = tree; replacement.roots = {valid.id, layer.id};
        CHECK(!f.document.replaceStructure(tree, replacement, removed, added));
        CHECK(f.document.tree() == tree); CHECK(f.document.layers().size() == 1);
        CHECK(f.document.containsLayer(f.id)); CHECK(!f.document.containsLayer(layer.id));
        CHECK(f.document.revision() == revision); CHECK(f.document.contentState() == contentState);
        CHECK(f.history.undoDepth() == 0); CHECK(f.history.redoDepth() == 1);
        CHECK(f.history.memoryUsed() == historyMemory);
    }
    CHECK(f.source->revision() == sourceRevision); CHECK(pixels(*f.source) == bytes);
    auto explicitlyEmpty = c::Layer::raster("Valid empty crop", f.source);
    explicitlyEmpty.crop = c::RectD {-1e9, 1e9, 0, 0};
    CHECK(f.document.insertLayer(1, explicitlyEmpty));
    CHECK(f.document.layer(explicitlyEmpty.id)->crop == explicitlyEmpty.crop);
}

void invalidEntryAndUnavailableTargetsFailSafely()
{
    for (const auto matrix : {c::AffineTransform {1e-8, 0, 0, 0, 1, 0},
             c::AffineTransform {1, 1, 0, 1, 1 + 1e-10, 0},
             c::AffineTransform {0, 0, 0, 0, 0, 0}}) {
        Fixture f;
        // Imported/pathological state can bypass the ordinary transform UI.
        f.document.layer(f.id)->localToDocument = matrix;
        c::LayerCropSession session(f.document, f.id);
        CHECK(!session.active()); CHECK(!session.targetAvailable());
        CHECK(!session.setRect({1, 2, 3, 4}));
        CHECK(session.commit(f.history) == c::TransformCommitResult::TargetUnavailable);
        CHECK(!f.crop()); CHECK(!f.document.isModified());
    }
    Fixture f;
    c::LayerCropSession session(f.document, f.id);
    CHECK(!session.beginDrag(c::TransformHandle::Rotate, {0, 0}));
    CHECK(!session.beginDrag(c::TransformHandle::None, {0, 0}));
    CHECK(!session.beginDrag(c::TransformHandle::Move, {std::numeric_limits<double>::quiet_NaN(), 0}));
    CHECK(!session.setRect({0, 0, -1, 1}));
    CHECK(!session.setRect({1e9, 0, 1, 1}));
    CHECK(session.setRect({3, 4, 20, 30})); CHECK(session.completeAction());
    CHECK(f.document.takeLayer(f.id));
    CHECK(!session.targetAvailable()); CHECK(!session.setRect({1, 2, 3, 4}));
    CHECK(!session.undo()); CHECK(!session.redo());
    CHECK(session.commit(f.history) == c::TransformCommitResult::TargetUnavailable);
    CHECK(!session.active()); CHECK(!f.document.containsLayer(f.id));
    CHECK(f.history.undoDepth() == 0); CHECK(f.history.memoryUsed() == 0);
    session.cancel(); // Idempotent even after target deletion and failed Apply.
}
} // namespace

int main()
{
    eachActionPublishesItsOwnCommandAndLocalRedoBranch();
    cancellationAndAllUndoneApplyPreserveExternalRedo();
    uncroppedClickAndOutAndBackAreNoOps();
    crossingHandlesNeverFlipsOrRebasesSourceContent();
    aspectConstraintShiftAndAltKeepIndependentExpectedAnchors();
    typedGeometryAndDensityCachesStayUnchanged();
    emptyCroppedGroupMembersMoveWithoutContributingBounds();
    invalidCropAdmissionIsAtomicForEveryLayerType();
    invalidEntryAndUnavailableTargetsFailSafely();
    if (failures) return EXIT_FAILURE;
    std::cout << "Layer crop session tests passed\n";
    return EXIT_SUCCESS;
}
