#include "imageeditor/core/LocalBlurStroke.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"

#include <cmath>
#include <iostream>

using namespace imageeditor::core;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto e = surface.extent(); std::vector<std::byte> result(std::size_t(e.width) * e.height);
    surface.copyNative({0, 0, int(e.width), int(e.height)}, result, e.width); return result;
}
BrushSettings brush()
{
    auto value = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    value.sizePixels = 14; value.hardness = 1; value.flow = 1; value.opacity = .3;
    value.pressureToFlow = false; value.pressureToSize = false;
    value.foreground = {255, 0, 0, 32}; // A filter never uses the drawing colour.
    return value;
}
NormalizedPointerSample sample(Vec2d point, std::uint64_t time = 1)
{ return {.documentPosition = point, .timestampMicroseconds = time, .pressure = 1,
    .pointerType = PointerType::Pen, .buttons = PointerButtonPrimary}; }
LayerId add(Document& doc, const std::shared_ptr<ScalarRasterSurface>& surface)
{
    auto layer = Layer::raster("Numeric channel", surface); const auto id = layer.id;
    CHECK(doc.insertLayer(doc.layers().size(), std::move(layer))); return id;
}
std::vector<double> kernel(double radius)
{
    const int support = int(std::ceil(radius)); std::vector<double> result(std::size_t(2 * support + 1));
    const double sigma = radius / 3 * std::sqrt(2.0); double sum = 0;
    for (int x = -support; x <= support; ++x) {
        const auto low = std::max(-radius, x - .5), high = std::min(radius, x + .5);
        const auto weight = high > low ? std::erf(high / sigma) - std::erf(low / sigma) : 0;
        result[std::size_t(x + support)] = weight; sum += weight;
    }
    for (auto& weight : result) weight /= sum;
    return result;
}

void numericBlurAndHistory()
{
    const Extent2u e {64, 64};
    std::vector<std::byte> original(std::size_t(e.width) * e.height);
    for (std::uint32_t y = 0; y < e.height; ++y) for (std::uint32_t x = 32; x < e.width; ++x)
        original[std::size_t(y) * e.width + x] = std::byte {255};
    Document doc({e, 96}); auto surface = std::make_shared<ScalarRasterSurface>(e, original, e.width);
    const auto id = add(doc, surface); History history;
    auto untouched = std::make_shared<ScalarRasterSurface>(e, 73); add(doc, untouched);
    const auto beforeOther = untouched->snapshot();
    const auto weights = kernel(3);
    double numeric = 0;
    for (int dx = -3; dx <= 3; ++dx) if (31 - dx >= 32) numeric += weights[std::size_t(dx + 3)] * 255;
    LocalBlurStroke stroke(doc, id, brush(), BlurSettings{.strength=1, .radius=3});
    CHECK(stroke.begin(sample({31.5, 32.5})));
    CHECK(stroke.end(sample({31.5, 32.5}, 2), history) == RasterEditCommitResult::Committed);
    const auto actual = surface->snapshot()->at(31,32);
    CHECK(std::abs(double(actual) - numeric) <= 1);
    CHECK(actual < 128); // Colour-space blur would produce a much brighter edge.
    const auto after = bytes(*surface);
    CHECK(history.undoDepth() == 1 && untouched->snapshot() == beforeOther);
    CHECK(history.undo(doc)); CHECK(bytes(*surface) == original);
    CHECK(history.redo(doc)); CHECK(bytes(*surface) == after);

    CHECK(history.undo(doc));
    const auto selection = SelectionMask::filled(e, 128); doc.setSelection(selection);
    LocalBlurStroke soft(doc, id, brush(), BlurSettings{.strength=1, .radius=3});
    CHECK(soft.begin(sample({31.5, 32.5})));
    CHECK(soft.end(sample({31.5, 32.5}, 2), history) == RasterEditCommitResult::Committed);
    CHECK(std::abs(double(surface->snapshot()->at(31,32)) - numeric * 128 / 255) <= 1);
    CHECK(doc.selection() == selection);

    // The layer's numeric halo remains usable when it extends beyond canvas.
    Document smallCanvas({{32, 64}, 96});
    auto extended = std::make_shared<ScalarRasterSurface>(e, original, e.width);
    const auto extendedId = add(smallCanvas, extended); History extendedHistory;
    LocalBlurStroke edge(smallCanvas, extendedId, brush(), BlurSettings{.strength=1, .radius=3});
    CHECK(edge.begin(sample({31.5,32.5})));
    CHECK(edge.end(sample({31.5,32.5},2), extendedHistory) == RasterEditCommitResult::Committed);
    CHECK(std::abs(double(extended->snapshot()->at(31,32)) - numeric) <= 1);
}

void zeroContextAndCancellation()
{
    const Extent2u e {64, 64}; Document doc({e, 96});
    auto zero = std::make_shared<ScalarRasterSurface>(e, 0); const auto id = add(doc, zero); History history;
    LocalBlurStroke black(doc, id, brush(), BlurSettings{.strength=1, .radius=4});
    CHECK(black.begin(sample({32.5,32.5})));
    CHECK(black.end(sample({32.5,32.5}, 2), history) == RasterEditCommitResult::NoChanges);
    CHECK(!history.canUndo() && zero->snapshot()->at(32,32) == 0);

    const std::byte white {255}; const RasterPatch patch {{32,32,1,1}, {&white, 1}, 1};
    (void)zero->replaceNativeBatch(std::span(&patch,1)); const auto original = bytes(*zero);
    LocalBlurStroke cancel(doc, id, brush(), BlurSettings{.strength=1, .radius=4});
    CHECK(cancel.begin(sample({32.5,32.5})));
    cancel.cancel(); CHECK(bytes(*zero) == original && !history.canUndo());
    LocalBlurStroke early(doc, id, brush(), BlurSettings{.strength=1, .radius=4});
    CHECK(!early.begin(sample({32.5,32.5}), [] { return true; }));
    CHECK(bytes(*zero) == original && !history.canUndo());
}

void compactCaptureAndTransformedIdentity()
{
    const Extent2u e {512, 512}; std::vector<std::byte> original(std::size_t(e.width) * e.height);
    for (std::uint32_t y = 0; y < e.height; ++y) for (std::uint32_t x = 0; x < e.width; ++x)
        original[std::size_t(y) * e.width + x] = std::byte((x + y) % 256);
    auto surface = std::make_shared<ScalarRasterSurface>(e, original, e.width);
    const auto pinned = surface->snapshot();
    Document doc({e, 96}); const auto id = add(doc, surface); History history;
    LocalBlurStroke stroke(doc, id, brush(), BlurSettings{.strength=1, .radius=3});
    CHECK(stroke.begin(sample({128.5,128.5})));
    CHECK(stroke.filterStats().snapshotBytes < original.size() + 65536);
    CHECK(pinned->at(128,128) == std::to_integer<std::uint8_t>(original[128 * 512 + 128]));
    stroke.cancel(); CHECK(bytes(*surface) == original);
    CHECK(doc.setLayerTransform(id, {1.5, 0, .3, 0, .8, .7}));
    LocalBlurStroke identity(doc, id, brush(), BlurSettings{.strength=1, .radius=0});
    CHECK(identity.begin(sample({128.5,128.5})));
    CHECK(identity.end(sample({128.5,128.5},2), history) == RasterEditCommitResult::NoChanges);
    CHECK(bytes(*surface) == original);
}
}

int main()
{
    numericBlurAndHistory(); zeroContextAndCancellation(); compactCaptureAndTransformedIdentity();
    return failures ? 1 : 0;
}
