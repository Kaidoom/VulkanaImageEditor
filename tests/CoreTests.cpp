#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/BrushAssetRegistry.hpp"
#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <cmath>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <stdexcept>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using namespace imageeditor::core;

static_assert(!std::is_copy_constructible_v<ContiguousRasterSurface>);
static_assert(!std::is_move_constructible_v<ContiguousRasterSurface>);

std::shared_ptr<ContiguousRasterSurface> makeSurface(std::uint32_t width = 4, std::uint32_t height = 4)
{
    return std::make_shared<ContiguousRasterSurface>(Extent2u {width, height}, Rgba8 {1, 2, 3, 255});
}

void rasterTracksDirtyRegions()
{
    auto surface = makeSurface();
    CHECK(surface->dirtySince(0).fullRefresh);
    CHECK(surface->dirtySince(surface->revision()).empty());

    const auto before = surface->revision();
    const std::vector<std::byte> pixel {
        std::byte {9}, std::byte {8}, std::byte {7}, std::byte {6},
    };
    surface->replaceRgba8({2, 1, 1, 1}, pixel, 4);
    CHECK(surface->revision() == before + 1);
    const auto dirty = surface->dirtySince(before);
    CHECK(!dirty.fullRefresh);
    CHECK(dirty.regions.size() == 1);
    CHECK(dirty.regions.front() == RectI({2, 1, 1, 1}));

    std::vector<std::byte> copied(4);
    surface->copyRgba8({2, 1, 1, 1}, copied, 4);
    CHECK(copied == pixel);
}

void rasterReplacementClipsWithoutMisaligningSource()
{
    auto surface = makeSurface(3, 3);
    std::vector<std::byte> source(3U * 2U * 4U);
    for (std::size_t pixel = 0; pixel < 6; ++pixel) {
        source[pixel * 4U] = static_cast<std::byte>(pixel);
        source[pixel * 4U + 3U] = std::byte {255};
    }
    surface->replaceRgba8({-1, 1, 3, 2}, source, 3U * 4U);

    std::vector<std::byte> copied(2U * 2U * 4U);
    surface->copyRgba8({0, 1, 2, 2}, copied, 2U * 4U);
    CHECK(std::to_integer<int>(copied[0]) == 1);
    CHECK(std::to_integer<int>(copied[4]) == 2);
    CHECK(std::to_integer<int>(copied[8]) == 4);
    CHECK(std::to_integer<int>(copied[12]) == 5);
}

void historyAppliesUndoRedoAndDiverges()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    const auto layer = Layer::raster("Layer", makeSurface(8, 8));
    const auto id = layer.id;
    CHECK(document->insertLayer(0, layer));
    session.replaceDocument(std::move(document));

    CHECK(session.execute(std::make_unique<SetLayerVisibilityCommand>(id, false)));
    CHECK(!session.document()->layer(id)->visible);
    CHECK(session.undo());
    CHECK(session.document()->layer(id)->visible);
    CHECK(session.redo());
    CHECK(!session.document()->layer(id)->visible);
    CHECK(session.undo());
    CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(id, 0.5F)));
    CHECK(!session.history().canRedo());
    CHECK(std::abs(session.document()->layer(id)->opacity - 0.5F) < 0.001F);
}

void foregroundColorIsEditorStateNotDocumentHistory()
{
    EditorSession session;
    const Rgba8 chosen {17, 91, 203, 144};
    session.setForegroundColor(chosen);
    CHECK(session.foregroundColor() == chosen);

    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    CHECK(document->insertLayer(0, Layer::raster("Layer", makeSurface(8, 8))));
    session.replaceDocument(std::move(document));
    CHECK(session.foregroundColor() == chosen);
    CHECK(!session.history().canUndo());
}

void opacityCommandsMergeIntoOneUndoStep()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    const auto layer = Layer::raster("Layer", makeSurface(8, 8));
    const auto id = layer.id;
    CHECK(document->insertLayer(0, layer));
    session.replaceDocument(std::move(document));
    constexpr std::uint64_t gesture = 42;
    CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(id, 0.75F, gesture)));
    CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(id, 0.25F, gesture)));
    CHECK(session.undo());
    CHECK(std::abs(session.document()->layer(id)->opacity - 1.0F) < 0.001F);
    CHECK(!session.history().canUndo());
    CHECK(session.redo());
    CHECK(std::abs(session.document()->layer(id)->opacity - 0.25F) < 0.001F);
}

void separateOpacityGesturesRemainSeparateUndoSteps()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    const auto layer = Layer::raster("Layer", makeSurface(8, 8));
    const auto id = layer.id;
    CHECK(document->insertLayer(0, layer));
    session.replaceDocument(std::move(document));
    CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(id, 0.75F, 1)));
    CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(id, 0.25F, 2)));
    CHECK(session.undo());
    CHECK(std::abs(session.document()->layer(id)->opacity - 0.75F) < 0.001F);
    CHECK(session.undo());
    CHECK(std::abs(session.document()->layer(id)->opacity - 1.0F) < 0.001F);
}

void historyBudgetCountsBothUndoStacksAndRetainedPixels()
{
    Document document(CanvasSpec {.extent = {8, 8}});
    const auto original = Layer::raster("Original", makeSurface(8, 8));
    CHECK(document.insertLayer(0, original));

    History history(1);
    auto added = Layer::raster("Retained pixels", makeSurface(8, 8));
    CHECK(history.execute(document, std::make_unique<AddLayerCommand>(added, 1)));
    const auto retainedCost = history.memoryUsed();
    CHECK(retainedCost > 8U * 8U * 4U);
    CHECK(history.undo(document));
    CHECK(history.memoryUsed() == retainedCost);
    CHECK(history.redo(document));
    CHECK(history.memoryUsed() == retainedCost);

    // The budget always retains the newest command but trims older commands.
    CHECK(history.execute(document,
        std::make_unique<SetLayerVisibilityCommand>(original.id, false)));
    CHECK(history.undo(document));
    CHECK(!history.canUndo());
}

void layerCommandsPreserveStableIdsAndOrdering()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    const auto bottom = Layer::raster("Bottom", makeSurface(8, 8));
    const auto top = Layer::raster("Top", makeSurface(8, 8));
    const auto bottomId = bottom.id;
    const auto topId = top.id;
    CHECK(document->insertLayer(0, bottom));
    session.replaceDocument(std::move(document));
    CHECK(session.execute(std::make_unique<AddLayerCommand>(top, 1)));
    CHECK(session.document()->layers().front().id == bottomId);
    CHECK(session.document()->layers().back().id == topId);
    CHECK(session.undo());
    CHECK(!session.document()->containsLayer(topId));
    CHECK(session.redo());
    CHECK(session.document()->layers().back().id == topId);
    session.setActiveLayer(bottomId);
    CHECK(session.execute(std::make_unique<RemoveLayerCommand>(topId)));
    CHECK(session.activeLayer() == bottomId);
    CHECK(session.undo());
    CHECK(session.document()->layers().back().id == topId);
}

void removingTheLastLayerIsRejected()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    const auto onlyLayer = Layer::raster("Only layer", makeSurface(8, 8));
    const auto onlyLayerId = onlyLayer.id;
    CHECK(document->insertLayer(0, onlyLayer));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(onlyLayerId);

    CHECK(!session.execute(std::make_unique<RemoveLayerCommand>(onlyLayerId)));
    CHECK(session.document()->layers().size() == 1);
    CHECK(session.document()->containsLayer(onlyLayerId));
    CHECK(session.activeLayer() == onlyLayerId);
    CHECK(!session.history().canUndo());
}

void layerReorderingIsUndoableAndPreservesIdentity()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {.extent = {8, 8}});
    const auto bottom = Layer::raster("Bottom", makeSurface(8, 8));
    const auto middle = Layer::raster("Middle", makeSurface(8, 8));
    const auto top = Layer::raster("Top", makeSurface(8, 8));
    CHECK(document->insertLayer(0, bottom));
    CHECK(document->insertLayer(1, middle));
    CHECK(document->insertLayer(2, top));
    session.replaceDocument(std::move(document));
    session.setActiveLayer(top.id);

    CHECK(session.execute(std::make_unique<MoveLayerCommand>(top.id, 0)));
    CHECK(session.document()->layers()[0].id == top.id);
    CHECK(session.document()->layers()[1].id == bottom.id);
    CHECK(session.document()->layers()[2].id == middle.id);
    CHECK(session.activeLayer() == top.id);

    CHECK(session.undo());
    CHECK(session.document()->layers()[0].id == bottom.id);
    CHECK(session.document()->layers()[1].id == middle.id);
    CHECK(session.document()->layers()[2].id == top.id);
    CHECK(session.activeLayer() == top.id);
    CHECK(session.redo());
    CHECK(session.document()->layers()[0].id == top.id);
}

void staleDirtyJournalFallsBackToFullRefresh()
{
    auto surface = makeSurface();
    const auto initial = surface->revision();
    const std::vector<std::byte> pixel {
        std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4},
    };
    for (int edit = 0; edit < 270; ++edit) {
        auto changedPixel = pixel;
        changedPixel[0] = static_cast<std::byte>((edit / 16) % 2 == 0 ? 11 : 12);
        (void)surface->replaceRgba8(
            {edit % 4, (edit / 4) % 4, 1, 1}, changedPixel, 4);
    }
    CHECK(surface->dirtySince(initial).fullRefresh);
}

void snapshotsShareRasterStorage()
{
    Document document(CanvasSpec {.extent = {16, 16}});
    auto surface = makeSurface(16, 16);
    CHECK(document.insertLayer(0, Layer::raster("Pixels", surface)));
    const auto snapshot = document.snapshot();
    CHECK(snapshot.layersBottomToTop.size() == 1);
    const auto& raster = std::get<RasterLayerSnapshot>(snapshot.layersBottomToTop.front().payload);
    CHECK(raster.surface.get() == surface.get());
}

void canvasResizeIsUndoableAndNeverCropsLayerData()
{
    EditorSession session;
    auto document = std::make_unique<Document>(
        CanvasSpec {.extent = {640, 480}, .dotsPerInch = 96.0});
    auto surface = makeSurface(900, 700);
    auto layer = Layer::raster("Larger than canvas", surface);
    layer.localToDocument = {
        .m00 = 0.9,
        .m01 = -0.2,
        .m02 = -175.0,
        .m10 = 0.2,
        .m11 = 0.9,
        .m12 = 83.0,
    };
    const auto layerId = layer.id;
    const auto originalTransform = layer.localToDocument;
    CHECK(document->insertLayer(0, layer));
    session.replaceDocument(std::move(document));

    const auto surfaceRevision = surface->revision();
    CHECK(session.execute(std::make_unique<ChangeCanvasSpecCommand>(
        CanvasSpec {.extent = {320, 240}, .dotsPerInch = 144.0})));
    CHECK(session.document()->canvas()
        == CanvasSpec({.extent = {320, 240}, .dotsPerInch = 144.0}));
    CHECK(session.document()->layer(layerId)->localToDocument.m02 == originalTransform.m02);
    CHECK(session.document()->layer(layerId)->localToDocument.m12 == originalTransform.m12);
    const auto& resizedRaster = std::get<RasterLayer>(session.document()->layer(layerId)->payload);
    CHECK(resizedRaster.surface.get() == surface.get());
    CHECK(surface->extent() == Extent2u({900, 700}));
    CHECK(surface->revision() == surfaceRevision);

    CHECK(session.undo());
    CHECK(session.document()->canvas()
        == CanvasSpec({.extent = {640, 480}, .dotsPerInch = 96.0}));
    CHECK(std::get<RasterLayer>(session.document()->layer(layerId)->payload).surface.get()
        == surface.get());
    CHECK(session.redo());
    CHECK(session.document()->canvas().extent == Extent2u({320, 240}));
}

void viewportZoomKeepsCursorAnchor()
{
    ViewportState viewport;
    const Extent2d document {1000.0, 800.0};
    const Extent2d view {1200.0, 900.0};
    const Vec2d cursor {263.0, 471.0};
    const auto before = viewport.viewportToDocument(cursor, document, view);
    viewport.zoomAround(cursor, 1.75, document, view);
    const auto after = viewport.viewportToDocument(cursor, document, view);
    CHECK(std::abs(before.x - after.x) < 1e-9);
    CHECK(std::abs(before.y - after.y) < 1e-9);

    viewport.fit(document, view);
    CHECK(viewport.zoom() > 0.0);
    viewport.reset100Percent();
    CHECK(viewport.zoom() == 1.0);
    CHECK(viewport.pan() == Vec2d({0.0, 0.0}));
}

void textLayerIsPlatformNeutralData()
{
    TextLayer text {
        .utf8 = "Editable text",
        .defaultStyle = {.font = {.family = "Noto Sans", .style = "Regular"},
            .sizePixels = 32.0, .color = {240, 240, 245, 255}},
        .runs = {},
        .paragraphs = {{0, TextAlignment::Center}},
    };
    const auto layer = Layer::text("Title", text);
    CHECK(std::get<TextLayer>(layer.payload).utf8 == "Editable text");
}

void brushAssetRegistryRejectsAmbiguousIdentityAndPaths()
{
    BrushAssetRecord tip {
        .id = "pack.tip.test.v1",
        .displayName = "Test Tip",
        .type = BrushAssetType::Tip,
        .relativePackagedPath = "tips/test.png",
        .coverageChannel = BrushCoverageChannel::Luminance,
        .invert = false,
        .seamless = std::nullopt,
        .defaultScalePixels = std::nullopt,
        .defaultRotationDegrees = 0.0,
        .revision = 1,
        .sha256 = std::string(64, 'a'),
    };
    const BrushAssetRegistry registry(BrushAssetRegistry::SupportedVersion,
        {tip});
    CHECK(registry.version() == 1);
    CHECK(registry.assets().size() == 1);
    CHECK(registry.contains("pack.tip.test.v1", BrushAssetType::Tip));
    CHECK(!registry.contains("pack.tip.test.v1", BrushAssetType::Grain));
    CHECK(registry.find("missing") == nullptr);

    bool duplicateRejected = false;
    try {
        (void)BrushAssetRegistry(1, {tip, tip});
    } catch (const std::invalid_argument&) {
        duplicateRejected = true;
    }
    CHECK(duplicateRejected);

    const std::array<std::string_view, 1> reservedIds {
        "pack.tip.test.v1"};
    bool crossSourceCollisionRejected = false;
    try {
        (void)BrushAssetRegistry(1, {tip}, reservedIds);
    } catch (const std::invalid_argument&) {
        crossSourceCollisionRejected = true;
    }
    CHECK(crossSourceCollisionRejected);

    auto traversal = tip;
    traversal.id = "pack.tip.traversal.v1";
    traversal.relativePackagedPath = "../outside.png";
    bool traversalRejected = false;
    try {
        (void)BrushAssetRegistry(1, {traversal});
    } catch (const std::invalid_argument&) {
        traversalRejected = true;
    }
    CHECK(traversalRejected);

    bool versionRejected = false;
    try {
        (void)BrushAssetRegistry(99, {tip});
    } catch (const std::invalid_argument&) {
        versionRejected = true;
    }
    CHECK(versionRejected);

    auto invalidType = tip;
    invalidType.id = "pack.tip.invalid-type.v1";
    invalidType.type = static_cast<BrushAssetType>(255);
    bool invalidTypeRejected = false;
    try {
        (void)BrushAssetRegistry(1, {invalidType});
    } catch (const std::invalid_argument&) {
        invalidTypeRejected = true;
    }
    CHECK(invalidTypeRejected);

    auto invalidCoverage = tip;
    invalidCoverage.id = "pack.tip.invalid-coverage.v1";
    invalidCoverage.coverageChannel
        = static_cast<BrushCoverageChannel>(255);
    bool invalidCoverageRejected = false;
    try {
        (void)BrushAssetRegistry(1, {invalidCoverage});
    } catch (const std::invalid_argument&) {
        invalidCoverageRejected = true;
    }
    CHECK(invalidCoverageRejected);

    auto nonSeamlessGrain = tip;
    nonSeamlessGrain.id = "pack.grain.nonseamless.v1";
    nonSeamlessGrain.type = BrushAssetType::Grain;
    nonSeamlessGrain.seamless = false;
    bool nonSeamlessGrainRejected = false;
    try {
        (void)BrushAssetRegistry(1, {nonSeamlessGrain});
    } catch (const std::invalid_argument&) {
        nonSeamlessGrainRejected = true;
    }
    CHECK(nonSeamlessGrainRejected);
}

} // namespace

int main()
{
    rasterTracksDirtyRegions();
    rasterReplacementClipsWithoutMisaligningSource();
    historyAppliesUndoRedoAndDiverges();
    foregroundColorIsEditorStateNotDocumentHistory();
    opacityCommandsMergeIntoOneUndoStep();
    separateOpacityGesturesRemainSeparateUndoSteps();
    historyBudgetCountsBothUndoStacksAndRetainedPixels();
    layerCommandsPreserveStableIdsAndOrdering();
    removingTheLastLayerIsRejected();
    layerReorderingIsUndoableAndPreservesIdentity();
    staleDirtyJournalFallsBackToFullRefresh();
    snapshotsShareRasterStorage();
    canvasResizeIsUndoableAndNeverCropsLayerData();
    viewportZoomKeepsCursorAnchor();
    textLayerIsPlatformNeutralData();
    brushAssetRegistryRejectsAmbiguousIdentityAndPaths();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All core tests passed\n";
    return EXIT_SUCCESS;
}
