#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/RasterSurface.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

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

struct Fixture {
    Document document;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId layerId {0};
    History history;

    explicit Fixture(Extent2u extent, Rgba8 fill = {0, 0, 0, 0})
        : document(CanvasSpec {.extent = extent})
        , surface(std::make_shared<ContiguousRasterSurface>(extent, fill))
    {
        auto layer = Layer::raster("Pixels", surface);
        layerId = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
};

std::vector<std::byte> solidPatch(RectI region, Rgba8 color)
{
    std::vector<std::byte> result(
        static_cast<std::size_t>(region.width * region.height) * 4U);
    for (std::size_t offset = 0; offset < result.size(); offset += 4) {
        result[offset] = static_cast<std::byte>(color.red);
        result[offset + 1] = static_cast<std::byte>(color.green);
        result[offset + 2] = static_cast<std::byte>(color.blue);
        result[offset + 3] = static_cast<std::byte>(color.alpha);
    }
    return result;
}

DirtySet writeSolid(RasterEditTransaction& transaction, RectI region, Rgba8 color)
{
    const auto patch = solidPatch(region, color);
    return transaction.writeRgba8(region, patch,
        static_cast<std::size_t>(region.width) * 4U);
}

Rgba8 readPixel(const RasterSurface& surface, std::int32_t x, std::int32_t y)
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

void oneTileCommitUndoRedo()
{
    Fixture fixture({128, 128});
    const auto initialRevision = fixture.surface->revision();
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Brush stroke");
    const auto liveDirty = writeSolid(edit, {4, 7, 6, 3}, {12, 34, 56, 180});
    CHECK(edit.capturedTileCount() == 1);
    CHECK(edit.capturedPixelBytes() == 64U * 64U * 4U);
    CHECK(liveDirty.revision == initialRevision + 1);
    CHECK(liveDirty.regions == std::vector<RectI>({{4, 7, 6, 3}}));
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(readPixel(*fixture.surface, 5, 8) == Rgba8({12, 34, 56, 180}));

    const auto committedRevision = fixture.surface->revision();
    CHECK(fixture.history.undo(fixture.document));
    CHECK(readPixel(*fixture.surface, 5, 8) == Rgba8({0, 0, 0, 0}));
    CHECK(fixture.surface->revision() == committedRevision + 1);
    const auto undoDirty = fixture.surface->dirtySince(committedRevision);
    CHECK(!undoDirty.fullRefresh);
    CHECK(undoDirty.regions == std::vector<RectI>({{4, 7, 6, 3}}));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(readPixel(*fixture.surface, 5, 8) == Rgba8({12, 34, 56, 180}));
}

void repeatedWritesCaptureEachTileOnce()
{
    Fixture fixture({128, 128});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Repeated writes");
    (void)writeSolid(edit, {2, 2, 8, 8}, {255, 0, 0, 255});
    (void)writeSolid(edit, {40, 40, 8, 8}, {0, 255, 0, 255});
    (void)writeSolid(edit, {4, 4, 2, 2}, {0, 0, 255, 255});
    CHECK(edit.capturedTileCount() == 1);
    CHECK(edit.capturedPixelBytes() == 64U * 64U * 4U);
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 1);
}

void manyTilesAndSparseDiagonalStaySparse()
{
    Fixture fixture({512, 512});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Sparse diagonal");
    std::vector<std::array<std::byte, 4>> colors(8);
    std::vector<RasterPatch> patches;
    for (std::int32_t index = 0; index < 8; ++index) {
        colors[static_cast<std::size_t>(index)] = {
            std::byte {0x44}, std::byte {0x77}, std::byte {0xAA}, std::byte {0xFF}};
        patches.push_back({{index * 64, index * 64, 1, 1},
            colors[static_cast<std::size_t>(index)], 4});
    }
    const auto before = fixture.surface->revision();
    const auto dirty = edit.writeRgba8Batch(patches);
    CHECK(fixture.surface->revision() == before + 1);
    CHECK(dirty.regions.size() == 8);
    CHECK(edit.capturedTileCount() == 8);
    CHECK(edit.capturedPixelBytes() == 8U * 64U * 64U * 4U);
    CHECK(edit.capturedPixelBytes() < 512U * 512U * 4U);
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undo(fixture.document));
    for (std::int32_t index = 0; index < 8; ++index) {
        CHECK(readPixel(*fixture.surface, index * 64, index * 64)
            == Rgba8({0, 0, 0, 0}));
    }
}

void edgeClippingUsesCorrectPatchOffsetAndClippedTileBytes()
{
    Fixture fixture({70, 70});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Edge clip");
    RectI region {-2, -2, 4, 4};
    auto patch = solidPatch(region, {9, 8, 7, 255});
    // Give every source pixel a unique red component so clipping alignment is
    // unambiguous. Surface (0,0) must receive source (2,2), index 10.
    for (std::size_t pixel = 0; pixel < 16; ++pixel) {
        patch[pixel * 4] = static_cast<std::byte>(pixel);
    }
    const auto dirty = edit.writeRgba8(region, patch, 16);
    CHECK(dirty.regions == std::vector<RectI>({{0, 0, 2, 2}}));
    CHECK(readPixel(*fixture.surface, 0, 0).red == 10);
    CHECK(readPixel(*fixture.surface, 1, 1).red == 15);
    CHECK(edit.capturedTileCount() == 1);
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);

    RasterEditTransaction edgeEdit(fixture.document, fixture.layerId, "Clipped edge tile");
    (void)writeSolid(edgeEdit, {68, 68, 2, 2}, {1, 2, 3, 4});
    CHECK(edgeEdit.capturedPixelBytes() == 6U * 6U * 4U);
    edgeEdit.cancel();
}

void cancelRestoresAndCreatesNoHistory()
{
    Fixture fixture({128, 128}, {10, 20, 30, 40});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Cancelled stroke");
    (void)writeSolid(edit, {55, 55, 20, 20}, {200, 100, 50, 255});
    const auto beforeCancel = fixture.surface->revision();
    edit.cancel();
    CHECK(!edit.active());
    CHECK(readPixel(*fixture.surface, 60, 60) == Rgba8({10, 20, 30, 40}));
    CHECK(fixture.surface->revision() == beforeCancel + 1);
    CHECK(!fixture.history.canUndo());
}

void noOpCommitDoesNotClearRedo()
{
    Fixture fixture({64, 64}, {1, 2, 3, 255});
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<SetLayerVisibilityCommand>(fixture.layerId, false)));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.history.canRedo());
    const auto redoDepth = fixture.history.redoDepth();

    RasterEditTransaction equalEdit(fixture.document, fixture.layerId, "No-op");
    (void)writeSolid(equalEdit, {1, 1, 4, 4}, {1, 2, 3, 255});
    CHECK(equalEdit.commit(fixture.history) == RasterEditCommitResult::NoChanges);
    CHECK(fixture.history.redoDepth() == redoDepth);

    RasterEditTransaction revertedEdit(fixture.document, fixture.layerId, "Reverted");
    (void)writeSolid(revertedEdit, {1, 1, 4, 4}, {9, 8, 7, 6});
    (void)writeSolid(revertedEdit, {1, 1, 4, 4}, {1, 2, 3, 255});
    CHECK(revertedEdit.commit(fixture.history) == RasterEditCommitResult::NoChanges);
    CHECK(fixture.history.redoDepth() == redoDepth);

    RasterEditTransaction divergent(fixture.document, fixture.layerId, "Divergent");
    (void)writeSolid(divergent, {1, 1, 1, 1}, {80, 90, 100, 255});
    CHECK(divergent.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(!fixture.history.canRedo());
}

void deletionDuringTransactionRestoresDetachedSurface()
{
    Fixture fixture({64, 64}, {3, 4, 5, 255});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Interrupted");
    (void)writeSolid(edit, {10, 10, 8, 8}, {200, 0, 0, 255});
    auto removed = fixture.document.takeLayer(fixture.layerId);
    CHECK(removed.has_value());
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::TargetUnavailable);
    const auto& raster = std::get<RasterLayer>(removed->layer.payload);
    CHECK(readPixel(*raster.surface, 11, 11) == Rgba8({3, 4, 5, 255}));
    CHECK(!fixture.history.canUndo());
}

void deletionAfterCommitComposesWithHistory()
{
    Fixture fixture({64, 64});
    auto second = Layer::raster("Other",
        std::make_shared<ContiguousRasterSurface>(Extent2u {64, 64}));
    CHECK(fixture.document.insertLayer(1, std::move(second)));
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Brush stroke");
    (void)writeSolid(edit, {3, 3, 2, 2}, {11, 22, 33, 255});
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.execute(fixture.document,
        std::make_unique<RemoveLayerCommand>(fixture.layerId)));
    CHECK(!fixture.document.containsLayer(fixture.layerId));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.document.containsLayer(fixture.layerId));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(readPixel(*fixture.surface, 3, 3) == Rgba8({0, 0, 0, 0}));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(readPixel(*fixture.surface, 3, 3) == Rgba8({11, 22, 33, 255}));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(!fixture.document.containsLayer(fixture.layerId));
}

void missingTargetLeavesUndoStackIntact()
{
    Fixture fixture({64, 64});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Brush stroke");
    (void)writeSolid(edit, {0, 0, 1, 1}, {1, 1, 1, 255});
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    auto removed = fixture.document.takeLayer(fixture.layerId);
    CHECK(removed.has_value());
    CHECK(!fixture.history.undo(fixture.document));
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.document.insertLayer(removed->index, std::move(removed->layer)));
    CHECK(fixture.history.undo(fixture.document));
}

void historyMemoryAccountingIsExactAndSwapInvariant()
{
    Fixture fixture({128, 128});
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Brush stroke");
    (void)writeSolid(edit, {2, 3, 5, 7}, {1, 2, 3, 4});
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    const auto expected = sizeof(RasterEditCommand)
        + sizeof(RasterEditCommand::TileSnapshot)
        + 64U * 64U * 4U + sizeof(RectI);
    CHECK(fixture.history.latestUndoMemoryCost() == expected);
    CHECK(fixture.history.memoryUsed() == expected);
    const auto retained = fixture.history.memoryUsed();
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.history.memoryUsed() == retained);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.history.memoryUsed() == retained);
}

void batchedWritesUseOneRevisionBeyondOldJournalLimit()
{
    Fixture fixture({512, 8});
    constexpr std::size_t patchCount = 65;
    std::vector<std::array<std::byte, 4>> pixels(patchCount);
    std::vector<RasterPatch> patches;
    patches.reserve(patchCount);
    for (std::size_t index = 0; index < patchCount; ++index) {
        pixels[index] = {std::byte {0xCC}, std::byte {0x44},
            std::byte {0x22}, std::byte {0xFF}};
        patches.push_back({{static_cast<std::int32_t>(index * 7U), 2, 1, 1},
            pixels[index], 4});
    }
    const auto uploaded = fixture.surface->revision();
    const auto dirty = fixture.surface->replaceRgba8Batch(patches);
    CHECK(fixture.surface->revision() == uploaded + 1);
    CHECK(dirty.regions.size() == patchCount);
    const auto cacheDirty = fixture.surface->dirtySince(uploaded);
    CHECK(!cacheDirty.fullRefresh);
    CHECK(cacheDirty.regions.size() == patchCount);
}

class HalfMask final : public SelectionMaskInput {
public:
    [[nodiscard]] Revision revision() const noexcept override { return 7; }
    [[nodiscard]] std::uint8_t coverageAtDocumentPixel(
        std::int32_t x, std::int32_t) const noexcept override
    {
        return x < 2 ? 0 : 128;
    }
};

void reservedSelectionMaskInputConstrainsWrites()
{
    Fixture fixture({8, 8}, {0, 0, 0, 0});
    HalfMask mask;
    RasterEditTransaction edit(fixture.document, fixture.layerId, "Masked edit",
        {.journalTileSize = 64, .selectionMask = &mask});
    (void)writeSolid(edit, {0, 0, 4, 1}, {200, 100, 50, 255});
    CHECK(readPixel(*fixture.surface, 0, 0) == Rgba8({0, 0, 0, 0}));
    // Coverage scales alpha, not straight RGB. Blending straight bytes here
    // would leave dark fringes when painting over a transparent target.
    CHECK(readPixel(*fixture.surface, 3, 0) == Rgba8({200, 100, 50, 128}));
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
}

} // namespace

int main()
{
    oneTileCommitUndoRedo();
    repeatedWritesCaptureEachTileOnce();
    manyTilesAndSparseDiagonalStaySparse();
    edgeClippingUsesCorrectPatchOffsetAndClippedTileBytes();
    cancelRestoresAndCreatesNoHistory();
    noOpCommitDoesNotClearRedo();
    deletionDuringTransactionRestoresDetachedSurface();
    deletionAfterCommitComposesWithHistory();
    missingTargetLeavesUndoStackIntact();
    historyMemoryAccountingIsExactAndSwapInvariant();
    batchedWritesUseOneRevisionBeyondOldJournalLimit();
    reservedSelectionMaskInputConstrainsWrites();

    if (failures != 0) {
        std::cerr << failures << " raster transaction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All raster edit transaction tests passed\n";
    return EXIT_SUCCESS;
}
