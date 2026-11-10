#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/core/LayerCommands.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
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

template<class Action> bool rejects(Action&& action)
{
    try { action(); }
    catch (const std::invalid_argument&) { return true; }
    catch (const std::out_of_range&) { return true; }
    catch (const std::overflow_error&) { return true; }
    return false;
}

std::vector<std::byte> nativePixels(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    const auto stride = std::size_t(extent.width) * surface.nativeBytesPerPixel();
    std::vector<std::byte> bytes(stride * extent.height);
    surface.copyNative({0, 0, int(extent.width), int(extent.height)}, bytes, stride);
    return bytes;
}

std::array<std::byte, 4> rgbaPixel(const RasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> bytes {};
    surface.copyRgba8({x, y, 1, 1}, bytes, 4);
    return bytes;
}

DirtySet writeNative(RasterSurface& surface, RectI region,
    std::span<const std::byte> pixels, std::size_t stride)
{
    const RasterPatch patch {region, pixels, stride};
    return surface.replaceNativeBatch(std::span<const RasterPatch>(&patch, 1));
}

DirtySet writeGray(RasterEditTransaction& edit, RectI region, std::uint8_t value)
{
    std::vector<std::byte> bytes(std::size_t(region.width) * std::size_t(region.height) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        bytes[i] = bytes[i + 1] = bytes[i + 2] = std::byte(value);
        bytes[i + 3] = std::byte {255};
    }
    return edit.writeRgba8(region, bytes, std::size_t(region.width) * 4);
}

struct Fixture {
    Document document;
    std::shared_ptr<ScalarRasterSurface> surface;
    LayerId id {};
    History history;
    explicit Fixture(Extent2u extent, std::uint8_t value = 0)
        : document(CanvasSpec {.extent = extent})
        , surface(std::make_shared<ScalarRasterSurface>(extent, value))
    {
        auto layer = Layer::raster("Numeric data", surface);
        id = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
};

void everyByteSurvivesNativeAndRgbaAccess()
{
    ScalarRasterSurface surface({256, 3}, 0);
    CHECK(surface.dataKind() == RasterDataKind::Scalar);
    CHECK(surface.nativeBytesPerPixel() == 1);
    std::vector<std::byte> source(3 * 263, std::byte {77});
    for (std::size_t y = 0; y < 3; ++y)
        for (std::size_t x = 0; x < 256; ++x)
            source[y * 263 + x] = std::byte((x + y * 71) % 256);
    (void)writeNative(surface, {0, 0, 256, 3}, source, 263);

    std::vector<std::byte> destination(3 * 269, std::byte {99});
    surface.copyNative({0, 0, 256, 3}, destination, 269);
    for (std::size_t y = 0; y < 3; ++y) {
        for (std::size_t x = 0; x < 256; ++x) {
            const auto expected = source[y * 263 + x];
            CHECK(destination[y * 269 + x] == expected);
            CHECK(rgbaPixel(surface, int(x), int(y))
                == (std::array<std::byte, 4> {expected, expected, expected, std::byte {255}}));
        }
        CHECK(destination[y * 269 + 256] == std::byte {99});
    }
    // Zero is valid opaque scalar data, including when the entire plane is zero.
    ScalarRasterSurface black({13, 7}, 0);
    CHECK(black.contentBounds() == (RectI {0, 0, 13, 7}));
    CHECK(rgbaPixel(black, 0, 0)[3] == std::byte {255});
}

void packedDataRetainsHiddenRgbAndColorDefaults()
{
    std::vector<std::byte> source(256 * 4);
    for (std::size_t x = 0; x < 256; ++x) {
        source[x * 4] = std::byte(x);
        source[x * 4 + 1] = std::byte(255 - x);
        source[x * 4 + 2] = std::byte((x * 71) % 256);
        source[x * 4 + 3] = std::byte {0};
    }
    ContiguousRasterSurface packed({256, 1}, source, RasterDataKind::PackedData);
    CHECK(packed.dataKind() == RasterDataKind::PackedData);
    CHECK(packed.nativeBytesPerPixel() == 4);
    CHECK(nativePixels(packed) == source);
    ContiguousRasterSurface colour({256, 1}, source);
    CHECK(colour.dataKind() == RasterDataKind::Color);
    CHECK(nativePixels(colour) == source);
    CHECK(rejects([&] { ContiguousRasterSurface invalid({2, 2}, Rgba8 {}, RasterDataKind::Scalar); }));
}

void memoryAccountingTracksUniformAndSharedTiles()
{
    auto uniform = std::make_shared<ScalarRasterSurface>(Extent2u {2048, 2048}, 0);
    CHECK(uniform->allocatedPixelBytes() == 0);
    CHECK(uniform->memoryCost() < 65536);
    AddLayerCommand add(Layer::raster("Uniform numeric data", uniform), 0);
    CHECK(add.memoryCost() < 65536);
    std::vector<std::byte> data(256 * 256);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = std::byte(i % 256);
    auto original = std::make_shared<ScalarRasterSurface>(Extent2u {256,256}, data, 256);
    auto clone = original->clone();
    std::unordered_set<const void*> allocations;
    const auto first = original->memoryCost(allocations);
    const auto second = clone->memoryCost(allocations);
    CHECK(first == original->memoryCost());
    CHECK(second < 4096);
    CHECK(original->memoryCost(allocations) == 0);
    const std::byte replacement {77};
    (void)writeNative(*clone, {1,1,1,1}, {&replacement,1}, 1);
    allocations.clear();
    const auto total = original->memoryCost(allocations) + clone->memoryCost(allocations);
    CHECK(total == original->memoryCost() + clone->memoryCost()
        - original->snapshot()->sharedPixelBytes(*clone->snapshot()));
}

void dirtyUpdatesAndNativeSwapStayRegional()
{
    ScalarRasterSurface surface({257, 131}, 17);
    const auto initial = surface.revision();
    std::array<std::byte, 4> first {std::byte {0}, std::byte {128}, std::byte {254}, std::byte {255}};
    const std::array<RasterPatch, 2> patches {{
        {{127, 127, 2, 2}, first, 2},
        {{256, 130, 1, 1}, std::span<const std::byte>(first).last(1), 1},
    }};
    const auto dirty = surface.replaceNativeBatch(patches);
    CHECK(dirty.revision == initial + 1);
    CHECK(!dirty.fullRefresh);
    CHECK(!surface.dirtySince(initial).fullRefresh);
    CHECK(surface.dirtySince(initial).regions.size() == 2);
    CHECK(surface.dirtySince(surface.revision()).empty());
    CHECK(surface.dirtySince(0).fullRefresh);
    const auto after = nativePixels(surface);

    std::array<std::byte, 4> alternate {std::byte {99}, std::byte {99}, std::byte {99}, std::byte {99}};
    const auto expected = alternate;
    MutableRasterPatch swap {{127, 127, 2, 2}, alternate, 2};
    (void)surface.swapNativeBatch(std::span<MutableRasterPatch>(&swap, 1));
    CHECK(alternate == first);
    CHECK(rgbaPixel(surface, 127, 127)[0] == expected[0]);
    (void)surface.swapNativeBatch(std::span<MutableRasterPatch>(&swap, 1));
    CHECK(alternate == expected);
    CHECK(nativePixels(surface) == after);
}

void snapshotsShareUntouchedTilesWithoutSharingEdits()
{
    constexpr auto side = ScalarPlane::tileSize;
    const Extent2u extent {side * 2, side * 2};
    ScalarRasterSurface uniform(extent, 73);
    CHECK(uniform.allocatedPixelBytes() == 0);
    CHECK(uniform.snapshot()->constantValue({0, 0, int(extent.width), int(extent.height)}) == 73);
    CHECK(uniform.memoryCost() < std::size_t(extent.width) * extent.height);

    std::vector<std::byte> source(std::size_t(extent.width) * extent.height);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = std::byte(i % 251);
    ScalarRasterSurface surface(extent, source, extent.width);
    const auto frozen = surface.snapshot();
    const auto clone = surface.clone();
    CHECK(clone->id() != surface.id());
    CHECK(clone->snapshot() == frozen);
    CHECK(frozen->sharedPixelBytes(*clone->snapshot()) == source.size());
    const std::array<std::byte, 1> value {std::byte {255}};
    (void)writeNative(surface, {1, 1, 1, 1}, value, 1);
    CHECK(surface.snapshot() != frozen);
    CHECK(surface.snapshot()->sharedPixelBytes(*frozen) == source.size() - std::size_t(side) * side);
    CHECK(frozen->at(1, 1) == std::to_integer<std::uint8_t>(source[extent.width + 1]));
    CHECK(nativePixels(*clone) == source);
    CHECK(rgbaPixel(surface, 1, 1)[0] == std::byte {255});
    const std::array<std::byte, 1> other {std::byte {254}};
    (void)writeNative(*clone, {int(side + 1), int(side + 1), 1, 1}, other, 1);
    CHECK(surface.snapshot()->at(side + 1, side + 1) == frozen->at(side + 1, side + 1));
    CHECK(clone->snapshot()->at(1, 1) == frozen->at(1, 1));

    // Missing coordinates are not silently turned into a valid black donor.
    CHECK(rejects([&] { (void)frozen->at(extent.width, 0); }));
    CHECK(rejects([&] { (void)frozen->at(0, extent.height); }));
    const auto revision = surface.revision();
    CHECK(writeNative(surface, {1, 1, 1, 1}, value, 1).empty());
    CHECK(surface.revision() == revision);
}

void invalidBatchesCannotPartiallyChangeData()
{
    ScalarRasterSurface surface({8, 8}, 31);
    const auto before = nativePixels(surface);
    const auto revision = surface.revision();
    std::array<std::byte, 4> valid {std::byte {201}, std::byte {201}, std::byte {201}, std::byte {255}};
    std::array<std::byte, 4> coloured {std::byte {201}, std::byte {202}, std::byte {201}, std::byte {255}};
    std::array<std::byte, 4> transparent {std::byte {0}, std::byte {0}, std::byte {0}, std::byte {0}};
    const std::array<RasterPatch, 2> rgbaBatch {{
        {{0, 0, 1, 1}, valid, 4}, {{1, 0, 1, 1}, coloured, 4},
    }};
    CHECK(rejects([&] { (void)surface.replaceRgba8Batch(rgbaBatch); }));
    CHECK(nativePixels(surface) == before);
    CHECK(surface.revision() == revision);
    CHECK(rejects([&] { (void)surface.replaceRgba8({0, 0, 1, 1}, transparent, 4); }));

    const std::array<RasterPatch, 2> nativeBatch {{
        {{0, 0, 1, 1}, std::span<const std::byte>(valid).first(1), 1},
        {{7, 7, 2, 1}, std::span<const std::byte>(valid).first(2), 2},
    }};
    CHECK(rejects([&] { (void)surface.replaceNativeBatch(nativeBatch); }));
    CHECK(rejects([&] { (void)writeNative(surface, {0, 0, 2, 2}, valid, 1); }));
    CHECK(rejects([&] { (void)writeNative(surface, {0, 0, 1, 2}, valid,
        std::numeric_limits<std::size_t>::max()); }));
    CHECK(nativePixels(surface) == before);
    CHECK(surface.revision() == revision);
    auto swapBytes = valid;
    const auto swapBefore = swapBytes;
    std::array<MutableRasterPatch, 2> invalidSwap {{
        {{0, 0, 1, 1}, std::span<std::byte>(swapBytes).first(1), 1},
        {{0, 0, std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}, swapBytes, 1},
    }};
    CHECK(rejects([&] { (void)surface.swapNativeBatch(invalidSwap); }));
    CHECK(swapBytes == swapBefore);
    CHECK(nativePixels(surface) == before);
    CHECK(surface.revision() == revision);
}

void selectionCannotHideInvalidScalarWrites()
{
    Fixture fixture({8, 8}, 27);
    CHECK(fixture.document.setSelection(SelectionMask::filled({8, 8}, 128)));
    const auto before = nativePixels(*fixture.surface);
    const auto revision = fixture.surface->revision();
    RasterEditTransaction edit(fixture.document, fixture.id, "Invalid scalar producer");
    std::array<std::byte, 4> coloured {std::byte {0}, std::byte {255}, std::byte {0}, std::byte {255}};
    std::array<std::byte, 4> translucent {std::byte {128}, std::byte {128}, std::byte {128}, std::byte {128}};
    CHECK(rejects([&] { (void)edit.writeRgba8({0, 0, 1, 1}, coloured, 4); }));
    CHECK(rejects([&] { (void)edit.writeRgba8({0, 0, 1, 1}, translucent, 4); }));
    CHECK(nativePixels(*fixture.surface) == before);
    CHECK(fixture.surface->revision() == revision);
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::NoChanges);
    CHECK(!fixture.history.canUndo());
}

void scalarTransactionsAreNumericAndCompact()
{
    Fixture fixture({128, 128});
    const auto selection = SelectionMask::rectangle({128, 128}, {5, 5, 2, 1}, 128);
    CHECK(fixture.document.setSelection(selection));
    const auto selectionRevision = fixture.document.selectionRevision();
    RasterEditTransaction edit(fixture.document, fixture.id, "Numeric edit");
    (void)writeGray(edit, {4, 5, 4, 1}, 255);
    CHECK(edit.capturedPixelBytes() <= 64U * 64U);
    CHECK(rgbaPixel(*fixture.surface, 4, 5)[0] == std::byte {0});
    CHECK(rgbaPixel(*fixture.surface, 5, 5)[0] == std::byte {128});
    CHECK(rgbaPixel(*fixture.surface, 6, 5)[0] == std::byte {128});
    CHECK(rgbaPixel(*fixture.surface, 7, 5)[0] == std::byte {0});
    CHECK(rgbaPixel(*fixture.surface, 5, 5)[3] == std::byte {255});
    std::array<std::byte, 16> original {};
    edit.copyOriginalRgba8({4, 5, 4, 1}, original, original.size());
    for (std::size_t offset = 0; offset < original.size(); offset += 4) {
        CHECK(original[offset] == std::byte {0});
        CHECK(original[offset + 1] == std::byte {0});
        CHECK(original[offset + 2] == std::byte {0});
        CHECK(original[offset + 3] == std::byte {255});
    }
    CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.latestUndoMemoryCost() < 64U * 64U * 2U);
    CHECK(fixture.document.selection() == selection);
    CHECK(fixture.document.selectionRevision() == selectionRevision);
    const auto memory = fixture.history.memoryUsed();
    CHECK(fixture.history.undo(fixture.document));
    CHECK(rgbaPixel(*fixture.surface, 5, 5)[0] == std::byte {0});
    CHECK(fixture.history.redo(fixture.document));
    CHECK(rgbaPixel(*fixture.surface, 5, 5)[0] == std::byte {128});
    CHECK(fixture.history.memoryUsed() == memory);
}

void cancelAndNoOpPreserveRedo()
{
    Fixture fixture({70, 70}, 37);
    const auto before = nativePixels(*fixture.surface);
    {
        RasterEditTransaction edit(fixture.document, fixture.id, "Edit");
        (void)writeGray(edit, {68, 68, 2, 2}, 129);
        CHECK(edit.capturedPixelBytes() == 6U * 6U);
        CHECK(edit.commit(fixture.history) == RasterEditCommitResult::Committed);
    }
    CHECK(fixture.history.undo(fixture.document));
    CHECK(nativePixels(*fixture.surface) == before);
    const auto redo = fixture.history.redoDepth();
    {
        RasterEditTransaction edit(fixture.document, fixture.id, "No-op");
        (void)writeGray(edit, {68, 68, 2, 2}, 37);
        CHECK(edit.commit(fixture.history) == RasterEditCommitResult::NoChanges);
    }
    CHECK(fixture.history.redoDepth() == redo);
    {
        RasterEditTransaction edit(fixture.document, fixture.id, "Cancelled");
        (void)writeGray(edit, {63, 63, 7, 7}, 0);
        edit.cancel();
        CHECK(!edit.active());
    }
    CHECK(nativePixels(*fixture.surface) == before);
    CHECK(fixture.history.redoDepth() == redo);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(rgbaPixel(*fixture.surface, 69, 69)[0] == std::byte {129});
}

void unavailableTargetRollsBackAndResolvedCoverageIsNotAppliedTwice()
{
    Fixture fixture({8, 8}, 11);
    const auto before = nativePixels(*fixture.surface);
    RasterEditTransaction abandoned(fixture.document, fixture.id, "Detached target");
    (void)writeGray(abandoned, {0, 0, 2, 2}, 244);
    auto removed = fixture.document.takeLayer(fixture.id);
    CHECK(removed.has_value());
    CHECK(abandoned.commit(fixture.history) == RasterEditCommitResult::TargetUnavailable);
    CHECK(nativePixels(*fixture.surface) == before);
    CHECK(!fixture.history.canUndo());
    CHECK(fixture.document.insertLayer(0, std::move(removed->layer)));
    CHECK(fixture.document.setSelection(SelectionMask::filled({8, 8}, 128)));
    RasterEditTransaction resolved(fixture.document, fixture.id, "Resolved coverage");
    std::array<std::byte, 4> finalValue {std::byte {133}, std::byte {133}, std::byte {133}, std::byte {255}};
    const RasterPatch patch {{0, 0, 1, 1}, finalValue, 4};
    (void)resolved.writeSelectionResolvedRgba8Batch(std::span<const RasterPatch>(&patch, 1));
    CHECK(rgbaPixel(*fixture.surface, 0, 0)[0] == std::byte {133});
    CHECK(resolved.commit(fixture.history) == RasterEditCommitResult::Committed);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(nativePixels(*fixture.surface) == before);
}

} // namespace

int main()
{
    try {
        everyByteSurvivesNativeAndRgbaAccess();
        packedDataRetainsHiddenRgbAndColorDefaults();
        memoryAccountingTracksUniformAndSharedTiles();
        dirtyUpdatesAndNativeSwapStayRegional();
        snapshotsShareUntouchedTilesWithoutSharingEdits();
        invalidBatchesCannotPartiallyChangeData();
        selectionCannotHideInvalidScalarWrites();
        scalarTransactionsAreNumericAndCompact();
        cancelAndNoOpPreserveRedo();
        unavailableTargetRollsBackAndResolvedCoverageIsNotAppliedTwice();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (failures) {
        std::cerr << failures << " scalar storage assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All scalar storage tests passed\n";
    return EXIT_SUCCESS;
}
