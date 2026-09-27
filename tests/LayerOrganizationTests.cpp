#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/LayerTransform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace imageeditor::core;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

std::vector<LayerId> leafOrder(const Document& document)
{
    std::vector<LayerId> ids;
    for (const auto& layer : document.layers()) ids.push_back(layer.id);
    return ids;
}

bool nearMatrix(const AffineTransform& a, const AffineTransform& b)
{
    for (const auto p : { Vec2d {}, Vec2d { 1, 0 }, Vec2d { 0, 1 } }) {
        const auto difference = a.map(p) - b.map(p);
        if (std::hypot(difference.x, difference.y) > 1e-9) return false;
    }
    return true;
}

struct Fixture {
    EditorSession session;
    std::array<LayerId, 6> ids;
    LayerId folder = makeLayerId(), group = makeLayerId(), nested = makeLayerId();
    Fixture()
    {
        auto document = std::make_unique<Document>(CanvasSpec { .extent = { 64, 64 } });
        for (std::size_t i = 0; i < ids.size(); ++i) {
            Layer layer = Layer::raster("Raster " + std::to_string(i),
                std::make_shared<ContiguousRasterSurface>(Extent2u { 8, 8 }, Rgba8 { 80, 120, 160, 180 }));
            if (i == 1) {
                TextLayer text;
                text.utf8 = "Editable text";
                layer = Layer::text("Text", std::move(text));
                auto cache = std::make_shared<LayerRenderCache>();
                cache->logicalExtent = { 24, 12 };
                cache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 24, 12 });
                layer.renderCache = std::move(cache);
            } else if (i == 2) {
                ShapeLayer shape;
                shape.kind = ShapeKind::RoundedRectangle;
                shape.size = { 20, 12 };
                shape.strokeEnabled = true;
                shape.strokeWidth = 4;
                layer = Layer::shape("Shape", shape);
            }
            layer.visible = i != 3;
            layer.opacity = i == 1 ? 0.6F : 1.0F;
            layer.localToDocument = i == 2
                ? AffineTransform { -1.3, 0.2, 40, 0.4, 0.7, 6 }
                : AffineTransform { 1, 0, double(i * 3), 0, 1, double(i * 2) };
            ids[i] = layer.id;
            CHECK(document->insertLayer(i, std::move(layer)));
        }
        session.replaceDocument(std::move(document));
        session.document()->markSaved();
    }
    LayerTree organized() const
    {
        return { { folder, ids[4], ids[5] }, {
            { folder, "Folder", ContainerKind::Folder, ColorLabel::Blue, { ids[0], group } },
            { group, "Group", ContainerKind::Group, ColorLabel::Orange, { ids[1], nested } },
            { nested, "Nested group", ContainerKind::Group, ColorLabel::Purple, { ids[2], ids[3] } },
        } };
    }
    void organize()
    {
        auto& document = *session.document();
        CHECK(document.replaceStructure(document.tree(), organized()));
    }
};

void treeTraversalAndNormalization()
{
    Fixture f;
    auto tree = f.organized();
    CHECK(tree.orderedLeaves(f.ids) == std::vector<LayerId>(f.ids.begin(), f.ids.end()));
    CHECK((tree.descendants(f.folder, true) == std::vector<LayerId> { f.ids[0] }));
    CHECK((tree.descendants(f.folder) == std::vector<LayerId> { f.ids[0], f.ids[1], f.ids[2], f.ids[3] }));
    CHECK((tree.descendants(f.group) == std::vector<LayerId> { f.ids[1], f.ids[2], f.ids[3] }));
    CHECK(tree.isAncestor(f.folder, f.ids[3]));
    CHECK(tree.isAncestor(f.group, f.nested));
    CHECK(!tree.isAncestor(f.nested, f.group));
    const std::array redundant { f.ids[3], f.nested, f.group, f.ids[1], f.group, LayerId { 0 } };
    CHECK((tree.normalize(redundant) == std::vector<LayerId> { f.group }));
    const std::array unordered { f.ids[5], f.ids[4], f.group };
    CHECK((tree.normalize(unordered) == std::vector<LayerId> { f.group, f.ids[4], f.ids[5] }));
    CHECK(tree.consecutiveSiblings(std::array { f.ids[5], f.ids[4] }));
    CHECK(tree.consecutiveSiblings(std::array { f.nested, f.ids[1] }));
    CHECK(!tree.consecutiveSiblings(std::array { f.group, f.ids[4] }));
    CHECK(!tree.consecutiveSiblings(std::array { f.folder, f.ids[5] }));
    CHECK(!tree.consecutiveSiblings(std::span<const LayerId> { }));

    const auto original = tree;
    CHECK(!tree.reparent(std::array { f.folder }, { f.nested, 0 }));
    CHECK(!tree.reparent(std::array { f.group }, { f.group, 0 }));
    CHECK(!tree.reparent(std::array { f.ids[0] }, { f.ids[1], 0 }));
    CHECK(!tree.reparent(std::array { f.ids[0] }, { f.folder, 200 }));
    CHECK(tree == original);
    CHECK(tree.reparent(std::array { f.ids[5], f.ids[4] }, { f.folder, 1 }));
    CHECK((tree.container(f.folder)->children == std::vector<LayerId> { f.ids[0], f.ids[4], f.ids[5], f.group }));
    CHECK((tree.orderedLeaves(f.ids) == std::vector<LayerId> { f.ids[0], f.ids[4], f.ids[5], f.ids[1], f.ids[2], f.ids[3] }));
    CHECK(!tree.reparent(std::array { f.ids[4], f.ids[5] }, { f.folder, 3 }));
    CHECK(tree.dissolve(f.group));
    CHECK(!tree.container(f.group));
    CHECK((tree.container(f.folder)->children == std::vector<LayerId> { f.ids[0], f.ids[4], f.ids[5], f.ids[1], f.nested }));
    CHECK(tree.dissolve(f.nested));
    CHECK(tree.dissolve(f.folder));
    CHECK(tree.containers.empty());
    CHECK((tree.roots == std::vector<LayerId> { f.ids[0], f.ids[4], f.ids[5], f.ids[1], f.ids[2], f.ids[3] }));
    CHECK(!tree.dissolve(f.folder));
}

void malformedTreesRejected()
{
    Fixture f;
    auto rejects = [&f](LayerTree tree) {
        bool threw = false;
        try { (void)tree.orderedLeaves(f.ids); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    };
    auto tree = f.organized();
    tree.roots.push_back(f.ids[0]); // Duplicate leaf membership.
    rejects(tree);
    tree = f.organized();
    tree.container(f.nested)->children.push_back(f.folder); // Containment cycle.
    rejects(tree);
    tree = f.organized();
    tree.container(f.nested)->children.pop_back(); // Orphan leaf.
    rejects(tree);
    tree = f.organized();
    tree.containers.push_back({ makeLayerId(), "Orphan folder", ContainerKind::Folder, ColorLabel::None, {} });
    rejects(tree);
    tree = f.organized();
    tree.container(f.folder)->children.push_back(makeLayerId());
    rejects(tree);
    tree = f.organized();
    tree.containers.front().name.clear();
    rejects(tree);
    tree = f.organized();
    tree.containers.front().colorLabel = static_cast<ColorLabel>(255);
    rejects(tree);

    // Deep nesting is bounded independently of leaf count.
    tree = f.session.document()->tree();
    for (std::size_t depth = 0; depth <= LayerTree::maxDepth; ++depth) {
        const auto id = makeLayerId();
        auto children = std::move(tree.roots);
        tree.roots = { id };
        tree.containers.push_back({ id, "Nested", ContainerKind::Folder, ColorLabel::None, std::move(children) });
    }
    rejects(tree);
}

void documentAdmissionAndTraversal()
{
    Fixture f;
    auto& document = *f.session.document();
    const auto original = document.tree();
    const auto raster = std::get<RasterLayer>(document.layer(f.ids[0])->payload).surface;
    const auto pixelsRevision = raster->revision();
    f.organize();
    CHECK(document.containsItem(f.group) && !document.containsLayer(f.group));
    CHECK(document.containsItem(f.folder) && !document.layer(f.folder));
    CHECK(leafOrder(document) == std::vector<LayerId>(f.ids.begin(), f.ids.end()));
    CHECK((document.expandedLayers(std::array { f.group, f.ids[2], f.nested })
        == std::vector<LayerId> { f.ids[1], f.ids[2], f.ids[3] }));
    CHECK(document.canvasTarget(f.ids[0]) == f.ids[0]);
    CHECK(document.canvasTarget(f.ids[1]) == f.group);
    CHECK(document.canvasTarget(f.ids[3]) == f.group);
    CHECK(document.canvasTarget(f.ids[4]) == f.ids[4]);

    const auto before = document.tree();
    const auto revision = document.revision();
    CHECK(!document.replaceStructure(before, before));
    CHECK(!document.replaceStructure(original, original)); // Stale expected structure.
    auto invalid = before;
    invalid.roots.push_back(f.ids[0]);
    try { CHECK(!document.replaceStructure(before, invalid)); }
    catch (const std::invalid_argument&) { } // Either admission policy is safe.
    CHECK(document.tree() == before && document.revision() == revision);
    CHECK(leafOrder(document) == std::vector<LayerId>(f.ids.begin(), f.ids.end()));
    CHECK(std::get<RasterLayer>(document.layer(f.ids[0])->payload).surface == raster);
    CHECK(raster->revision() == pixelsRevision && raster->dirtySince(pixelsRevision).empty());
    const auto snapshot = document.snapshot();
    CHECK(snapshot.layersBottomToTop.size() == f.ids.size());
    for (std::size_t i = 0; i < f.ids.size(); ++i) CHECK(snapshot.layersBottomToTop[i].id == f.ids[i]);
}

void structuralHistoryRestoresSelection()
{
    Fixture f;
    auto& session = f.session;
    auto& document = *session.document();
    const auto original = document.tree();
    const LayerSelectionState before { { f.ids[2], f.ids[1] }, f.ids[1], f.ids[5] };
    const LayerSelectionState after { { f.group }, f.group, f.group };
    session.setLayerSelection(before.ids, before.primary, before.anchor);
    session.setActiveTool(ToolId::Brush);
    auto grouped = original;
    grouped.roots = { f.ids[0], f.group, f.ids[3], f.ids[4], f.ids[5] };
    grouped.containers.push_back({ f.group, "Group", ContainerKind::Group, ColorLabel::Green, { f.ids[1], f.ids[2] } });
    CHECK(session.execute(std::make_unique<LayerStructureCommand>("Group layers", document, grouped,
        std::vector<LayerId> {}, std::vector<Layer> {}, before, after)));
    CHECK(document.tree() == grouped);
    CHECK(session.selectedLayers() == after.ids && session.activeLayer() == after.primary && session.selectionAnchor() == after.anchor);
    CHECK(document.isModified() && session.history().undoDepth() == 1);
    const auto memory = session.history().memoryUsed();
    CHECK(memory >= original.memoryCost() + grouped.memoryCost());
    CHECK(session.undo());
    CHECK(document.tree() == original && !document.isModified());
    CHECK(session.selectedLayers() == before.ids && session.activeLayer() == before.primary && session.selectionAnchor() == before.anchor);
    CHECK(session.activeTool() == ToolId::Brush);
    CHECK(session.history().memoryUsed() == memory);
    CHECK(session.redo());
    CHECK(document.tree() == grouped && session.selectedLayers() == after.ids);
    CHECK(session.undo());
    const auto redoDepth = session.history().redoDepth();
    const auto content = document.contentState();
    CHECK(!session.execute(std::make_unique<LayerStructureCommand>("No change", document, document.tree())));
    CHECK(session.history().redoDepth() == redoDepth && document.contentState() == content);
    CHECK(session.selectedLayers() == before.ids);
    CHECK(session.redo());
}

void metadataAndCollapsedSelection()
{
    Fixture f;
    f.organize();
    auto& session = f.session;
    auto& document = *session.document();
    session.setActiveLayer(f.group);
    CHECK(session.activeLayer() == f.group);
    const auto content = document.contentState();
    const auto revision = document.revision();
    session.setLayerSelection(std::array { f.ids[1], f.folder }, f.folder, f.ids[1]);
    // Hidden descendants can remain selected; the range uses displayed rows only.
    const std::array collapsedRows { f.ids[5], f.ids[4], f.folder };
    session.selectLayerRange(f.ids[4], collapsedRows);
    CHECK((session.selectedLayers() == std::vector<LayerId> { f.ids[4] }));
    CHECK(document.contentState() == content && document.revision() == revision);
    session.setActiveLayer(f.folder);
    session.selectLayerRange(f.ids[5], collapsedRows);
    CHECK((session.selectedLayers() == std::vector<LayerId> { f.ids[5], f.ids[4], f.folder }));
    CHECK(session.execute(std::make_unique<SetItemMetadataCommand>(f.folder, "Renamed folder", ColorLabel::Red)));
    CHECK(document.tree().container(f.folder)->name == "Renamed folder");
    CHECK(document.tree().container(f.folder)->colorLabel == ColorLabel::Red);
    CHECK(session.undo());
    CHECK(document.tree().container(f.folder)->name == "Folder");
    CHECK(document.tree().container(f.folder)->colorLabel == ColorLabel::Blue);
    const auto redoDepth = session.history().redoDepth();
    CHECK(!session.execute(std::make_unique<SetItemMetadataCommand>(f.folder, "Folder", ColorLabel::Blue)));
    CHECK(session.history().redoDepth() == redoDepth);
    CHECK(session.redo());
    const auto name = document.layer(f.ids[0])->name;
    CHECK(session.execute(std::make_unique<SetItemMetadataCommand>(f.ids[0], "Renamed raster", ColorLabel::Purple)));
    CHECK(document.layer(f.ids[0])->name == "Renamed raster");
    CHECK(document.layer(f.ids[0])->colorLabel == static_cast<std::uint8_t>(ColorLabel::Purple));
    CHECK(session.undo());
    CHECK(document.layer(f.ids[0])->name == name && document.layer(f.ids[0])->colorLabel == 0);
    const auto beforeEmpty = document.tree();
    CHECK(!session.execute(std::make_unique<SetItemMetadataCommand>(f.folder, "", ColorLabel::None)));
    CHECK(document.tree() == beforeEmpty);
}

void mixedNestedTransformsRetainContent()
{
    Fixture f;
    f.organize();
    auto& document = *f.session.document();
    const auto tree = document.tree();
    std::array<AffineTransform, 6> matrices;
    for (std::size_t i = 0; i < f.ids.size(); ++i) matrices[i] = document.layer(f.ids[i])->localToDocument;
    const auto text = std::get<TextLayer>(document.layer(f.ids[1])->payload);
    const auto textCache = document.layer(f.ids[1])->renderCache;
    const auto shape = std::get<ShapeLayer>(document.layer(f.ids[2])->payload);
    const auto raster = std::get<RasterLayer>(document.layer(f.ids[3])->payload).surface;
    const auto rasterRevision = raster->revision();
    const auto targets = document.expandedLayers(std::array { f.group, f.nested, f.ids[3] });
    LayerTransformSession transform(document, targets);
    CHECK(transform.active());
    CHECK(transform.beginDrag(TransformHandle::Move, { 10, 10 }));
    CHECK(transform.dragTo({ 21, 3 }, {}, false));
    transform.endDrag();
    CHECK(transform.commit(f.session.history()) == TransformCommitResult::Committed);
    for (std::size_t i = 0; i < f.ids.size(); ++i) {
        auto expected = matrices[i];
        if (i >= 1 && i <= 3) { expected.m02 += 11; expected.m12 -= 7; }
        CHECK(nearMatrix(document.layer(f.ids[i])->localToDocument, expected));
    }
    CHECK(!document.layer(f.ids[3])->visible); // Hidden members still moved.
    CHECK(std::get<TextLayer>(document.layer(f.ids[1])->payload) == text);
    CHECK(std::get<ShapeLayer>(document.layer(f.ids[2])->payload) == shape);
    CHECK(document.layer(f.ids[1])->renderCache == textCache);
    CHECK(std::get<RasterLayer>(document.layer(f.ids[3])->payload).surface == raster);
    CHECK(raster->revision() == rasterRevision && raster->dirtySince(rasterRevision).empty());
    CHECK(document.tree() == tree && f.session.history().undoDepth() == 1);
    CHECK(f.session.undo());
    for (std::size_t i = 0; i < f.ids.size(); ++i) CHECK(document.layer(f.ids[i])->localToDocument == matrices[i]);
    CHECK(f.session.redo());
    std::array<AffineTransform, 6> moved;
    for (std::size_t i = 0; i < f.ids.size(); ++i) moved[i] = document.layer(f.ids[i])->localToDocument;
    auto ungrouped = tree;
    CHECK(ungrouped.dissolve(f.group));
    CHECK(document.replaceStructure(tree, ungrouped));
    for (std::size_t i = 0; i < f.ids.size(); ++i) CHECK(document.layer(f.ids[i])->localToDocument == moved[i]);
    CHECK(document.canvasTarget(f.ids[2]) == f.nested);
}

void atomicReplacementRetainsOriginals()
{
    Fixture f;
    f.organize();
    auto& session = f.session;
    auto& document = *session.document();
    const auto beforeTree = document.tree();
    const auto retainedRaster = std::get<RasterLayer>(document.layer(f.ids[3])->payload).surface;
    const auto retainedText = std::get<TextLayer>(document.layer(f.ids[1])->payload);
    const auto retainedShape = std::get<ShapeLayer>(document.layer(f.ids[2])->payload);
    const auto retainedMatrix = document.layer(f.ids[2])->localToDocument;
    const LayerSelectionState beforeSelection { { f.group, f.ids[0] }, f.group, f.ids[0] };
    session.setLayerSelection(beforeSelection.ids, beforeSelection.primary, beforeSelection.anchor);
    auto merged = Layer::raster("Merged", std::make_shared<ContiguousRasterSurface>(Extent2u { 32, 24 }));
    merged.localToDocument = { 1, 0, -8, 0, 1, -6 };
    const auto mergedId = merged.id;
    auto afterTree = beforeTree;
    afterTree.container(f.folder)->children = { f.ids[0], mergedId };
    std::erase_if(afterTree.containers, [&f](const auto& item) { return item.id == f.group || item.id == f.nested; });
    const LayerSelectionState afterSelection { { mergedId }, mergedId, mergedId };
    CHECK(session.execute(std::make_unique<LayerStructureCommand>("Merge layers", document, afterTree,
        std::vector<LayerId> { f.ids[1], f.ids[2], f.ids[3] }, std::vector<Layer> { merged }, beforeSelection, afterSelection)));
    CHECK(!document.containsItem(f.group) && !document.containsLayer(f.ids[1]));
    CHECK(document.containsLayer(mergedId) && document.tree() == afterTree);
    CHECK(session.selectedLayers() == afterSelection.ids);
    const auto retainedCost = session.history().memoryUsed();
    CHECK(retainedCost >= 32U * 24U * 4U + 8U * 8U * 4U);
    CHECK(session.undo());
    CHECK(document.tree() == beforeTree && !document.containsLayer(mergedId));
    CHECK(std::get<RasterLayer>(document.layer(f.ids[3])->payload).surface == retainedRaster);
    CHECK(std::get<TextLayer>(document.layer(f.ids[1])->payload) == retainedText);
    CHECK(std::get<ShapeLayer>(document.layer(f.ids[2])->payload) == retainedShape);
    CHECK(document.layer(f.ids[2])->localToDocument == retainedMatrix);
    CHECK(session.selectedLayers() == beforeSelection.ids && session.activeLayer() == beforeSelection.primary
        && session.selectionAnchor() == beforeSelection.anchor);
    CHECK(session.history().memoryUsed() == retainedCost);
    CHECK(session.redo());
    CHECK(document.tree() == afterTree && session.selectedLayers() == afterSelection.ids);
    CHECK(session.history().memoryUsed() == retainedCost);
}

void existingLayerCommandsKeepParents()
{
    Fixture f;
    f.organize();
    auto& session = f.session;
    auto& document = *session.document();
    const auto original = document.tree();
    CHECK(session.execute(std::make_unique<RemoveLayerCommand>(f.ids[2])));
    CHECK((document.tree().container(f.nested)->children == std::vector<LayerId> { f.ids[3] }));
    CHECK(session.undo());
    CHECK(document.tree() == original);
    auto added = Layer::raster("Added inside group", std::make_shared<ContiguousRasterSurface>(Extent2u { 4, 4 }));
    const auto addedId = added.id;
    CHECK(session.execute(std::make_unique<AddLayerCommand>(added, 2)));
    CHECK((document.tree().container(f.nested)->children == std::vector<LayerId> { addedId, f.ids[2], f.ids[3] }));
    const auto withAdded = document.tree();
    CHECK(session.undo());
    CHECK(document.tree() == original);
    CHECK(session.redo());
    CHECK(document.tree() == withAdded);
    CHECK(session.undo());
    CHECK(session.execute(std::make_unique<MoveLayerCommand>(f.ids[2], 5)));
    CHECK(document.tree().placement(f.ids[2])->parent == 0);
    CHECK(session.undo());
    CHECK(document.tree() == original);
}

void emptyFolderAndExplicitEmptySelection()
{
    Fixture f;
    auto& session = f.session;
    auto& document = *session.document();
    const auto original = document.tree();
    auto withFolder = original;
    withFolder.roots.push_back(f.folder);
    withFolder.containers.push_back({ f.folder, "Empty", ContainerKind::Folder, ColorLabel::None, {} });
    const LayerSelectionState before { { f.ids[0] }, f.ids[0], f.ids[0] };
    const LayerSelectionState empty {};
    session.setLayerSelection(before.ids, before.primary, before.anchor);
    CHECK(session.execute(std::make_unique<LayerStructureCommand>("Add empty folder", document, withFolder,
        std::vector<LayerId> {}, std::vector<Layer> {}, before, empty)));
    CHECK(document.tree().descendants(f.folder).empty());
    CHECK(session.selectedLayers().empty() && !session.activeLayer() && !session.selectionAnchor());
    CHECK(session.undo());
    CHECK(session.selectedLayers() == before.ids);
    CHECK(session.redo());
    CHECK(session.selectedLayers().empty());
    auto dissolved = withFolder;
    CHECK(dissolved.dissolve(f.folder));
    CHECK(dissolved == original);
    CHECK(session.execute(std::make_unique<LayerStructureCommand>("Remove empty folder", document, dissolved)));
    CHECK(document.tree() == original && leafOrder(document) == original.roots);
    CHECK(session.undo());
    CHECK(document.tree() == withFolder);
}

void groupMembershipDisappearanceInvalidatesOnlyOwnedGeometry()
{
    for (const bool removeNested : { false, true }) {
        Fixture f;
        f.organize();
        auto& document = *f.session.document();
        std::array<AffineTransform, 6> originals;
        for (std::size_t i = 0; i < originals.size(); ++i) originals[i] = document.layer(f.ids[i])->localToDocument;
        LayerTransformSession transform(document, f.group); // Container ID, not pre-expanded leaves.
        CHECK(transform.active());
        CHECK(transform.beginDrag(TransformHandle::Move, { 0, 0 }));
        CHECK(transform.dragTo({ 5, -3 }, {}, false));
        transform.endDrag();
        auto changed = document.tree();
        CHECK(changed.dissolve(removeNested ? f.nested : f.group));
        CHECK(f.session.execute(std::make_unique<LayerStructureCommand>("External ungroup", document, changed)));
        const auto externalState = document.contentState();
        CHECK(!transform.targetAvailable());
        CHECK(!transform.beginDrag(TransformHandle::Move, { 5, -3 }));
        CHECK(transform.commit(f.session.history()) == TransformCommitResult::TargetUnavailable);
        CHECK(document.tree() == changed && document.contentState() == externalState);
        CHECK(f.session.history().undoDepth() == 1); // No partial transform publication.
        for (std::size_t i = 0; i < originals.size(); ++i) {
            CHECK(document.containsLayer(f.ids[i]));
            CHECK(document.layer(f.ids[i])->localToDocument == originals[i]);
        }
    }
}

void groupChildReorderDoesNotRetargetTransform()
{
    Fixture f;
    f.organize();
    auto& document = *f.session.document();
    CHECK(!LayerTransformSession(document, f.folder).active());
    LayerTransformSession transform(document, std::array { f.group, f.nested, f.ids[3] });
    CHECK(transform.active());
    const auto originalText = document.layer(f.ids[1])->localToDocument;
    const auto originalHidden = document.layer(f.ids[3])->localToDocument;
    CHECK(transform.beginDrag(TransformHandle::Move, { 0, 0 }));
    CHECK(transform.dragTo({ 5, 2 }, {}, false));
    transform.endDrag();
    auto reordered = document.tree();
    std::ranges::reverse(reordered.container(f.group)->children);
    std::ranges::reverse(reordered.container(f.nested)->children);
    CHECK(document.replaceStructure(document.tree(), reordered));
    CHECK(transform.targetAvailable()); // Same membership, different stack order.
    CHECK(transform.beginDrag(TransformHandle::Move, { 5, 2 }));
    CHECK(transform.dragTo({ 9, 6 }, {}, false));
    transform.endDrag();
    CHECK(transform.commit(f.session.history()) == TransformCommitResult::Committed);
    auto expectedText = originalText; expectedText.m02 += 9; expectedText.m12 += 6;
    auto expectedHidden = originalHidden; expectedHidden.m02 += 9; expectedHidden.m12 += 6;
    CHECK(nearMatrix(document.layer(f.ids[1])->localToDocument, expectedText));
    CHECK(nearMatrix(document.layer(f.ids[3])->localToDocument, expectedHidden));
    CHECK(document.tree() == reordered && f.session.history().undoDepth() == 2);
    CHECK(f.session.undo() && f.session.undo());
    CHECK(document.tree() == reordered);
    CHECK(document.layer(f.ids[1])->localToDocument == originalText);
    CHECK(document.layer(f.ids[3])->localToDocument == originalHidden);
}

std::vector<std::pair<LayerId, bool>> visibilityBits(const Document& document)
{
    std::vector<std::pair<LayerId, bool>> bits;
    for (const auto& layer : document.layers()) bits.emplace_back(layer.id, layer.visible);
    for (const auto& item : document.tree().containers) bits.emplace_back(item.id, item.visible);
    return bits;
}

void inheritedVisibilityKeepsLocalEyeState()
{
    Fixture f;
    f.organize();
    auto& session = f.session;
    auto& document = *session.document();
    const auto initial = visibilityBits(document);
    const auto shape = std::get<ShapeLayer>(document.layer(f.ids[2])->payload);
    const auto text = std::get<TextLayer>(document.layer(f.ids[1])->payload);
    const auto textCache = document.layer(f.ids[1])->renderCache;
    const auto raster = std::get<RasterLayer>(document.layer(f.ids[0])->payload).surface;
    const auto pixelsRevision = raster->revision();
    const LayerSelectionState selected { { f.folder, f.ids[0], f.nested, f.ids[3] }, f.folder, f.ids[0] };
    session.setLayerSelection(selected.ids, selected.primary, selected.anchor);
    document.markSaved();
    const auto revision = document.revision();
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document, selected.ids, VisibilityOperation::Hide)));
    CHECK(document.revision() == revision + 1 && session.history().undoDepth() == 1);
    CHECK(document.itemVisibility(f.folder) == false);
    CHECK(document.itemVisibility(f.group) == true && document.itemVisibility(f.nested) == true);
    for (std::size_t i = 0; i < f.ids.size(); ++i) {
        CHECK(document.itemVisibility(f.ids[i]) == (i != 3));
        CHECK(document.isEffectivelyVisible(f.ids[i]) == (i >= 4));
    }
    CHECK(!document.isEffectivelyVisible(f.folder) && !document.isEffectivelyVisible(f.group));
    CHECK(!document.itemVisibility(0) && !document.itemVisibility(makeLayerId()));
    CHECK(!document.isEffectivelyVisible(0) && !document.isEffectivelyVisible(makeLayerId()));
    const auto snapshot = document.snapshot();
    for (std::size_t i = 0; i < f.ids.size(); ++i)
        CHECK(snapshot.layersBottomToTop[i].visible == (i >= 4));
    CHECK(session.layerSelectionState() == selected);
    CHECK(std::get<ShapeLayer>(document.layer(f.ids[2])->payload) == shape);
    CHECK(std::get<TextLayer>(document.layer(f.ids[1])->payload) == text);
    CHECK(document.layer(f.ids[1])->renderCache == textCache);
    CHECK(raster->revision() == pixelsRevision && raster->dirtySince(pixelsRevision).empty());
    const auto hidden = visibilityBits(document);
    CHECK(session.undo() && visibilityBits(document) == initial && !document.isModified());
    CHECK(session.redo() && visibilityBits(document) == hidden);
    CHECK(session.execute(std::make_unique<SetLayerVisibilityCommand>(f.group, false)));
    CHECK(!document.isEffectivelyVisible(f.ids[1]));
    // The child can be edited independently, but showing it never opens a parent gate.
    CHECK(session.execute(std::make_unique<SetLayerVisibilityCommand>(f.ids[0], false)));
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::array { f.ids[0] }, VisibilityOperation::Show)));
    CHECK(document.itemVisibility(f.ids[0]) == true && !document.isEffectivelyVisible(f.ids[0]));
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::array { f.folder, f.group, f.ids[3] }, VisibilityOperation::Show)));
    CHECK(document.isEffectivelyVisible(f.ids[0]));
    CHECK(document.itemVisibility(f.group) == false && !document.isEffectivelyVisible(f.ids[1]));
    CHECK(document.itemVisibility(f.ids[3]) == false); // Selected ancestor owns the command.
}

void isolateAndShowAllVisibilityHistory()
{
    Fixture f;
    f.organize();
    auto& session = f.session;
    auto& document = *session.document();
    const std::array closed {
        ItemVisibilityUpdate { f.folder, true, false },
        ItemVisibilityUpdate { f.group, true, false },
        ItemVisibilityUpdate { f.nested, true, false }
    };
    CHECK(document.setItemVisibilities(closed));
    const auto original = visibilityBits(document);
    const LayerSelectionState selection { { f.ids[5], f.ids[3], f.nested }, f.nested, f.ids[5] };
    session.setLayerSelection(selection.ids, selection.primary, selection.anchor);
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        selection.ids, VisibilityOperation::Isolate)));
    CHECK(session.history().undoDepth() == 1 && session.layerSelectionState() == selection);
    CHECK(document.itemVisibility(f.folder) == true && document.itemVisibility(f.group) == true);
    CHECK(document.itemVisibility(f.nested) == true && document.itemVisibility(f.ids[5]) == true);
    CHECK(document.itemVisibility(f.ids[0]) == false && document.itemVisibility(f.ids[1]) == false);
    CHECK(document.itemVisibility(f.ids[4]) == false);
    CHECK(document.itemVisibility(f.ids[2]) == true && document.isEffectivelyVisible(f.ids[2]));
    CHECK(document.itemVisibility(f.ids[3]) == false && !document.isEffectivelyVisible(f.ids[3]));
    const auto isolated = visibilityBits(document);
    CHECK(session.undo() && visibilityBits(document) == original);
    CHECK(session.redo() && visibilityBits(document) == isolated);
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::span<const LayerId> {}, VisibilityOperation::ShowAll)));
    for (const auto& [id, own] : visibilityBits(document)) CHECK(own && document.isEffectivelyVisible(id));
    CHECK(session.layerSelectionState() == selection && session.history().undoDepth() == 2);
    CHECK(session.undo() && visibilityBits(document) == isolated);
    const auto redo = session.history().redoDepth();
    const auto content = document.contentState();
    CHECK(!session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::array { f.ids[0] }, VisibilityOperation::Hide)));
    CHECK(session.history().redoDepth() == redo && document.contentState() == content);
    CHECK(session.redo());
    CHECK(!session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::span<const LayerId> {}, VisibilityOperation::ShowAll)));
    CHECK(session.history().undoDepth() == 2);

    // An individually selected hidden descendant is made visible. Its required
    // ancestors open, and unrelated branches are hidden at their highest gate.
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::array { f.ids[3] }, VisibilityOperation::Isolate)));
    CHECK(document.isEffectivelyVisible(f.ids[3]));
    for (const auto id : f.ids) CHECK(document.isEffectivelyVisible(id) == (id == f.ids[3]));
    CHECK(document.itemVisibility(f.folder) == true && document.itemVisibility(f.group) == true
        && document.itemVisibility(f.nested) == true);
    CHECK(session.undo());
    // No selected targets is not an implicit hide-everything command.
    const auto beforeEmpty = visibilityBits(document);
    const auto emptyRedo = session.history().redoDepth();
    for (const auto operation : { VisibilityOperation::Hide, VisibilityOperation::Show, VisibilityOperation::Isolate })
        CHECK(!session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
            std::span<const LayerId> {}, operation)));
    CHECK(visibilityBits(document) == beforeEmpty && session.history().redoDepth() == emptyRedo);
    CHECK(session.execute(std::make_unique<SetItemsVisibilityCommand>(document,
        std::array { f.folder }, VisibilityOperation::Hide)));
    CHECK(session.history().redoDepth() == 0); // A real divergent edit clears redo.
    CHECK(document.itemVisibility(f.group) == true && document.itemVisibility(f.ids[3]) == true);
}

void visibilityBatchAdmissionIsAtomic()
{
    Fixture f;
    f.organize();
    auto& document = *f.session.document();
    const auto before = visibilityBits(document);
    const auto revision = document.revision();
    const std::array missing {
        ItemVisibilityUpdate { f.folder, true, false },
        ItemVisibilityUpdate { makeLayerId(), true, false }
    };
    CHECK(!document.setItemVisibilities(missing));
    const std::array stale {
        ItemVisibilityUpdate { f.folder, true, false },
        ItemVisibilityUpdate { f.ids[3], true, false } // Its original own eye is already false.
    };
    CHECK(!document.setItemVisibilities(stale));
    CHECK(visibilityBits(document) == before && document.revision() == revision);

    SetItemsVisibilityCommand disappearance(document, std::array { f.ids[4], f.ids[5] }, VisibilityOperation::Hide);
    CHECK(document.takeLayer(f.ids[5]).has_value());
    const auto afterRemoval = visibilityBits(document);
    const auto removalRevision = document.revision();
    CHECK(!disappearance.apply(document));
    CHECK(visibilityBits(document) == afterRemoval && document.revision() == removalRevision);

    SetItemsVisibilityCommand guarded(document, std::array { f.folder, f.ids[4] }, VisibilityOperation::Hide);
    CHECK(guarded.apply(document));
    CHECK(document.setItemVisibilities(std::array { ItemVisibilityUpdate { f.ids[4], false, true } }));
    const auto externallyChanged = visibilityBits(document);
    const auto externallyChangedRevision = document.revision();
    CHECK(!guarded.undo(document));
    CHECK(visibilityBits(document) == externallyChanged && document.revision() == externallyChangedRevision);
    CHECK(document.itemVisibility(f.folder) == false); // No first-target partial undo.
    CHECK(document.setItemVisibilities(std::array { ItemVisibilityUpdate { f.ids[4], true, false } }));
    CHECK(guarded.undo(document));
    CHECK(document.setItemVisibilities(std::array { ItemVisibilityUpdate { f.ids[4], true, false } }));
    const auto beforeStaleRedo = visibilityBits(document);
    CHECK(!guarded.apply(document));
    CHECK(visibilityBits(document) == beforeStaleRedo && document.itemVisibility(f.folder) == true);
}

void effectiveVisibilityMatchesSamplingAndTargeting()
{
    Document document(CanvasSpec { .extent = { 16, 16 } });
    auto background = Layer::raster("Background", std::make_shared<ContiguousRasterSurface>(Extent2u { 16, 16 }, Rgba8 { 0, 0, 255, 255 }));
    auto foreground = Layer::raster("Foreground", std::make_shared<ContiguousRasterSurface>(Extent2u { 8, 8 }, Rgba8 { 255, 0, 0, 255 }));
    auto text = Layer::text("Text", TextLayer {});
    auto textCache = std::make_shared<LayerRenderCache>();
    textCache->logicalExtent = { 8, 8 };
    textCache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 8, 8 }, Rgba8 { 0, 255, 0, 255 });
    text.renderCache = textCache;
    ShapeLayer geometry;
    geometry.kind = ShapeKind::Ellipse;
    geometry.size = { 8, 8 };
    auto shape = Layer::shape("Shape", geometry);
    auto shapeCache = std::make_shared<LayerRenderCache>(*textCache);
    shapeCache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 8, 8 }, Rgba8 { 255, 255, 0, 255 });
    shape.renderCache = shapeCache;
    const auto bottom = background.id, rasterId = foreground.id, textId = text.id, shapeId = shape.id;
    CHECK(document.insertLayer(0, std::move(background)));
    CHECK(document.insertLayer(1, std::move(foreground)));
    CHECK(document.insertLayer(2, std::move(text)));
    CHECK(document.insertLayer(3, std::move(shape)));
    const auto folder = makeLayerId(), group = makeLayerId();
    LayerTree tree { { bottom, folder }, {
        { folder, "Folder", ContainerKind::Folder, ColorLabel::None, { rasterId, group } },
        { group, "Group", ContainerKind::Group, ColorLabel::None, { textId, shapeId } }
    } };
    CHECK(document.replaceStructure(document.tree(), tree));
    const Vec2d point { 4.5, 4.5 };
    CHECK(hitTestRasterLayer(document, point) == shapeId);
    PinnedDocumentSampler beforeHide(document, rasterId, ColorSampleSource::MergedVisible);
    CHECK((beforeHide.sample(point) == Rgba8 { 255, 255, 0, 255 }));
    SetLayerVisibilityCommand hideFolder(folder, false);
    CHECK(hideFolder.apply(document));
    CHECK(!beforeHide.matches(document));
    const auto sample = sampleDocumentColor(document, rasterId, point, ColorSampleSource::MergedVisible);
    CHECK((sample.available() && sample.color == Rgba8 { 0, 0, 255, 255 }));
    CHECK(sample.texelsRead <= 4); // No hidden raster, text, or shape is sampled.
    PinnedDocumentSampler hidden(document, rasterId, ColorSampleSource::MergedVisible);
    CHECK(hidden.matches(document) && hidden.sample(point) == sample.color);
    CHECK(hitTestRasterLayer(document, point) == bottom);
    const auto snapshot = document.snapshot();
    CHECK(snapshot.layersBottomToTop[0].visible);
    for (std::size_t i = 1; i < snapshot.layersBottomToTop.size(); ++i)
        CHECK(!snapshot.layersBottomToTop[i].visible);
    // Active-layer sampling is deliberately independent of all visibility gates.
    CHECK((sampleDocumentColor(document, rasterId, point, ColorSampleSource::ActiveLayer).color == Rgba8 { 255, 0, 0, 255 }));
    PinnedDocumentSampler active(document, rasterId, ColorSampleSource::ActiveLayer);
    CHECK((active.sample(point) == Rgba8 { 255, 0, 0, 255 }));
    CHECK(hideFolder.undo(document));
    CHECK(!hidden.matches(document) && hitTestRasterLayer(document, point) == shapeId);
    SetLayerVisibilityCommand hideGroup(group, false);
    CHECK(hideGroup.apply(document));
    CHECK(hitTestRasterLayer(document, point) == rasterId);
    CHECK((sampleDocumentColor(document, rasterId, point, ColorSampleSource::MergedVisible).color == Rgba8 { 255, 0, 0, 255 }));
    CHECK(document.itemVisibility(textId) == true && document.itemVisibility(shapeId) == true);
    CHECK(document.layer(textId)->renderCache == textCache && document.layer(shapeId)->renderCache == shapeCache);
}

void hiddenContainerDissolutionPreservesAppearanceAndExactUndo()
{
    // A folder has an immediate raster and group; the group has immediate
    // editable text and another group. Both must transfer only the removed gate.
    for (const bool dissolveGroup : { false, true }) {
        Fixture f;
        f.organize();
        auto& session = f.session;
        auto& document = *session.document();
        const auto id = dissolveGroup ? f.group : f.folder;
        CHECK(document.setLayerVisibility(id, false));
        const auto originalTree = document.tree();
        const auto originalBits = visibilityBits(document);
        const LayerSelectionState beforeSelection { { id, f.ids[5] }, id, f.ids[5] };
        session.setLayerSelection(beforeSelection.ids, beforeSelection.primary, beforeSelection.anchor);
        document.markSaved();
        std::array<bool, 6> effective;
        std::array<AffineTransform, 6> matrices;
        for (std::size_t i = 0; i < f.ids.size(); ++i) {
            effective[i] = document.isEffectivelyVisible(f.ids[i]);
            matrices[i] = document.layer(f.ids[i])->localToDocument;
        }
        const auto raster = std::get<RasterLayer>(document.layer(f.ids[0])->payload).surface;
        const auto rasterRevision = raster->revision();
        const auto text = std::get<TextLayer>(document.layer(f.ids[1])->payload);
        const auto textCache = document.layer(f.ids[1])->renderCache;
        const auto shape = std::get<ShapeLayer>(document.layer(f.ids[2])->payload);
        auto afterTree = originalTree;
        const auto children = afterTree.container(id)->children;
        std::vector<ItemVisibilityUpdate> leafUpdates;
        for (const auto child : children) {
            if (auto* container = afterTree.container(child)) container->visible = false;
            else if (document.layer(child)->visible) leafUpdates.push_back({ child, true, false });
        }
        CHECK(leafUpdates.size() == 1);
        CHECK(afterTree.dissolve(id));
        auto afterSelection = beforeSelection;
        afterSelection.ids = children;
        afterSelection.ids.push_back(f.ids[5]);
        afterSelection.primary = children.back();
        const LayerStructureCommand withoutVisibility("Remove hidden container", document, afterTree,
            {}, {}, beforeSelection, afterSelection);
        auto command = std::make_unique<LayerStructureCommand>("Remove hidden container", document, afterTree,
            std::vector<LayerId> {}, std::vector<Layer> {}, beforeSelection, afterSelection, leafUpdates);
        CHECK(command->memoryCost() >= withoutVisibility.memoryCost()
            + 2 * leafUpdates.size() * sizeof(ItemVisibilityUpdate));
        const auto revision = document.revision();
        CHECK(session.execute(std::move(command)));
        CHECK(document.revision() == revision + 1 && session.history().undoDepth() == 1);
        CHECK(document.tree() == afterTree && !document.containsItem(id));
        CHECK(session.layerSelectionState() == afterSelection);
        for (const auto child : children) CHECK(document.itemVisibility(child) == false);
        // No recursive flattening of the inherited state into descendant eyes.
        CHECK(document.itemVisibility(f.ids[2]) == true && document.itemVisibility(f.ids[3]) == false);
        if (!dissolveGroup) {
            CHECK(document.itemVisibility(f.nested) == true);
            CHECK(document.itemVisibility(f.ids[1]) == true);
        }
        for (std::size_t i = 0; i < f.ids.size(); ++i) {
            CHECK(document.isEffectivelyVisible(f.ids[i]) == effective[i]);
            CHECK(document.layer(f.ids[i])->localToDocument == matrices[i]);
        }
        CHECK(raster->revision() == rasterRevision && raster->dirtySince(rasterRevision).empty());
        CHECK(document.layer(f.ids[1])->renderCache == textCache);
        CHECK(std::get<TextLayer>(document.layer(f.ids[1])->payload) == text);
        CHECK(std::get<ShapeLayer>(document.layer(f.ids[2])->payload) == shape);
        const auto afterBits = visibilityBits(document);
        const auto memory = session.history().memoryUsed();
        CHECK(session.undo());
        CHECK(document.tree() == originalTree && visibilityBits(document) == originalBits);
        CHECK(!document.isModified() && session.layerSelectionState() == beforeSelection);
        CHECK(session.history().memoryUsed() == memory);
        CHECK(session.redo());
        CHECK(document.tree() == afterTree && visibilityBits(document) == afterBits);
        CHECK(session.layerSelectionState() == afterSelection && session.history().memoryUsed() == memory);
    }
}

void retainedVisibilityStructuralAdmissionIsAtomic()
{
    Fixture f;
    f.organize();
    auto& session = f.session;
    auto& document = *session.document();
    const auto originalTree = document.tree();
    const auto originalBits = visibilityBits(document);
    const auto revision = document.revision();
    auto afterTree = originalTree;
    CHECK(afterTree.dissolve(f.folder));
    const auto rejects = [&](std::vector<ItemVisibilityUpdate> updates) {
        CHECK(!document.replaceStructure(originalTree, afterTree, {}, {}, updates));
        CHECK(document.tree() == originalTree && visibilityBits(document) == originalBits);
        CHECK(document.revision() == revision && leafOrder(document) == std::vector<LayerId>(f.ids.begin(), f.ids.end()));
    };
    rejects({ { f.ids[0], true, false }, { makeLayerId(), true, false } });
    rejects({ { f.ids[0], true, false }, { f.group, true, false } }); // Containers belong in the replacement tree.
    rejects({ { f.ids[0], true, false }, { f.ids[3], true, false } }); // Later stale leaf guard.
    rejects({ { f.ids[0], true, false }, { f.ids[0], true, false } });
    const std::array update { ItemVisibilityUpdate { f.ids[0], true, false } };
    auto withoutLeaf = afterTree;
    std::erase(withoutLeaf.roots, f.ids[0]);
    CHECK(!document.replaceStructure(originalTree, withoutLeaf, std::array { f.ids[0] }, {}, update));
    CHECK(document.tree() == originalTree && visibilityBits(document) == originalBits);
    const std::array replacementLeaf { *document.layer(f.ids[0]) };
    CHECK(!document.replaceStructure(originalTree, afterTree, std::array { f.ids[0] }, replacementLeaf, update));
    CHECK(document.tree() == originalTree && visibilityBits(document) == originalBits);

    const LayerSelectionState originalSelection { { f.folder }, f.folder, f.folder };
    const LayerSelectionState afterSelection { { f.ids[0], f.group }, f.group, f.ids[0] };
    session.setLayerSelection(originalSelection.ids, originalSelection.primary, originalSelection.anchor);
    CHECK(session.execute(std::make_unique<LayerStructureCommand>("Dissolve with retained visibility", document,
        afterTree, std::vector<LayerId> {}, std::vector<Layer> {}, originalSelection, afterSelection,
        std::vector<ItemVisibilityUpdate>(update.begin(), update.end()))));
    CHECK(document.itemVisibility(f.ids[0]) == false);
    CHECK(document.setLayerVisibility(f.ids[0], true)); // External mutation makes the undo guard stale.
    const auto staleUndoBits = visibilityBits(document);
    const auto staleUndoRevision = document.revision();
    const auto staleUndoState = document.contentState();
    const auto memory = session.history().memoryUsed();
    CHECK(!session.undo());
    CHECK(document.tree() == afterTree && visibilityBits(document) == staleUndoBits);
    CHECK(document.revision() == staleUndoRevision && document.contentState() == staleUndoState);
    CHECK(session.history().undoDepth() == 1 && session.history().redoDepth() == 0);
    CHECK(session.layerSelectionState() == afterSelection && session.history().memoryUsed() == memory);
    CHECK(document.setLayerVisibility(f.ids[0], false));
    CHECK(session.undo());
    CHECK(document.tree() == originalTree && visibilityBits(document) == originalBits);
    CHECK(document.setLayerVisibility(f.ids[0], false)); // A stale redo must not dissolve anything either.
    const auto staleRedoBits = visibilityBits(document);
    const auto staleRedoRevision = document.revision();
    CHECK(!session.redo());
    CHECK(document.tree() == originalTree && visibilityBits(document) == staleRedoBits);
    CHECK(document.revision() == staleRedoRevision && session.history().undoDepth() == 0 && session.history().redoDepth() == 1);
    CHECK(session.layerSelectionState() == originalSelection && session.history().memoryUsed() == memory);
}
}

void duplicateMixedHierarchy()
{
    Fixture f; f.organize();
    auto& s=f.session; auto& d=*s.document();
    s.setLayerSelection(std::array{f.folder,f.ids[2],f.ids[5]},f.ids[2]);
    const auto before=s.layerSelectionState(); const auto tree=d.tree();
    CHECK(s.execute(duplicateLayerItems(d,before)));
    CHECK(d.layers().size()==11); // Four descendants plus independent leaf, once.
    CHECK(s.selectedLayers().size()==2 && d.tree().containers.size()==6);
    const auto copy=*s.activeLayer();
    CHECK(d.tree().container(copy) && d.tree().container(copy)->name=="Folder copy");
    const auto descendants=d.tree().descendants(copy);
    CHECK(descendants.size()==4);
    for (std::size_t i=0;i<4;++i) {
        const auto& original=*d.layer(f.ids[i]); const auto& cloned=*d.layer(descendants[i]);
        CHECK(original.id!=cloned.id && original.payload.index()==cloned.payload.index());
        CHECK(original.localToDocument==cloned.localToDocument && original.visible==cloned.visible);
        if (const auto* a=std::get_if<RasterLayer>(&original.payload)) {
            auto b=std::get<RasterLayer>(cloned.payload).surface;
            CHECK(a->surface!=b && a->surface->id()!=b->id());
            std::array<std::byte,4> pixel{}, other{};
            a->surface->copyRgba8({0,0,1,1},pixel,4); b->copyRgba8({0,0,1,1},other,4); CHECK(pixel==other);
        }
        if (const auto* a=std::get_if<TextLayer>(&original.payload)) CHECK(*a==std::get<TextLayer>(cloned.payload));
        if (const auto* a=std::get_if<ShapeLayer>(&original.payload)) CHECK(*a==std::get<ShapeLayer>(cloned.payload));
    }
    const auto after=s.layerSelectionState();
    CHECK(s.undo()); CHECK(d.tree()==tree && s.layerSelectionState()==before);
    CHECK(s.redo()); CHECK(s.layerSelectionState()==after && d.containsItem(copy));
}

void stagedDuplicateTransformsCommitAtomically()
{
    Fixture f; f.organize();
    auto& s=f.session; auto& d=*s.document();
    s.setLayerSelection(std::array{f.folder,f.ids[2],f.ids[5]},f.ids[2]);
    const auto before=s.layerSelectionState(); const auto tree=d.tree();
    const auto content=d.contentState(); const auto depth=s.history().undoDepth();
    auto command=duplicateLayerItems(d,before);
    CHECK(command && !command->captureAddedLayerTransforms(d));
    CHECK(command->apply(d));
    const auto after=*command->layerSelectionAfter(false);
    const auto leaves=d.expandedLayers(after.ids);
    CHECK(leaves.size()==5);
    std::vector<AffineTransform> matrices;
    for (const auto id:leaves) {
        auto matrix=d.layer(id)->localToDocument;
        matrix.m02+=17.25; matrix.m12-=9.5;
        CHECK(d.setLayerTransform(id,matrix)); matrices.push_back(matrix);
    }
    CHECK(command->captureAddedLayerTransforms(d));
    CHECK(d.contentState()==content && s.history().undoDepth()==depth);
    CHECK(command->undo(d));
    CHECK(d.tree()==tree && d.contentState()==content);
    CHECK(s.execute(std::move(command)));
    CHECK(s.history().undoDepth()==depth+1 && s.layerSelectionState()==after);
    for (std::size_t i=0;i<leaves.size();++i) CHECK(d.layer(leaves[i])->localToDocument==matrices[i]);
    CHECK(s.undo()); CHECK(d.tree()==tree && s.layerSelectionState()==before && d.contentState()==content);
    CHECK(s.redo()); CHECK(s.layerSelectionState()==after);
    for (std::size_t i=0;i<leaves.size();++i) CHECK(d.layer(leaves[i])->localToDocument==matrices[i]);
}

int main()
{
    stagedDuplicateTransformsCommitAtomically();
    duplicateMixedHierarchy();
    treeTraversalAndNormalization();
    malformedTreesRejected();
    documentAdmissionAndTraversal();
    structuralHistoryRestoresSelection();
    metadataAndCollapsedSelection();
    mixedNestedTransformsRetainContent();
    atomicReplacementRetainsOriginals();
    existingLayerCommandsKeepParents();
    emptyFolderAndExplicitEmptySelection();
    groupMembershipDisappearanceInvalidatesOnlyOwnedGeometry();
    groupChildReorderDoesNotRetargetTransform();
    inheritedVisibilityKeepsLocalEyeState();
    isolateAndShowAllVisibilityHistory();
    visibilityBatchAdmissionIsAtomic();
    effectiveVisibilityMatchesSamplingAndTargeting();
    hiddenContainerDissolutionPreservesAppearanceAndExactUndo();
    retainedVisibilityStructuralAdmissionIsAtomic();
    if (failures) return EXIT_FAILURE;
    std::cout << "Layer organization tests passed\n";
    return EXIT_SUCCESS;
}
