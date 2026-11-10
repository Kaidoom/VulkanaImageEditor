#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) { std::cerr << "FAIL " << line << ": " << expression << '\n'; ++failures; }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Fixture {
    Document document {CanvasSpec {.extent = {16, 16}}};
    std::shared_ptr<ScalarRasterSurface> surface;
    LayerId id {0};
    History history;
    explicit Fixture(std::uint8_t value = 0)
        : surface(std::make_shared<ScalarRasterSurface>(Extent2u {16, 16}, value))
    {
        auto layer = Layer::raster("Scalar", surface);
        id = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
};

Rgba8 pixel(const RasterSurface& surface, int x = 7, int y = 7)
{
    std::array<std::byte, 4> bytes;
    surface.copyRgba8({x, y, 1, 1}, bytes, 4);
    return {std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3])};
}
std::vector<std::byte> data(const RasterSurface& surface)
{
    const auto e = surface.extent();
    std::vector<std::byte> result(std::size_t(e.width) * e.height * 4);
    surface.copyRgba8({0, 0, int(e.width), int(e.height)}, result, std::size_t(e.width) * 4);
    return result;
}
BrushSettings settings(double opacity = 1)
{
    auto result = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    result.sizePixels = 6; result.opacity = opacity; result.flow = 1; result.hardness = 1;
    result.pressureToSize = result.pressureToFlow = false;
    result.foreground = {255, 255, 255, 255};
    return result;
}
NormalizedPointerSample point(double x = 7.5, double y = 7.5)
{
    return {.documentPosition = {x, y}, .pressure = 1, .buttons = PointerButtonPrimary};
}
RasterEditCommitResult paint(Fixture& f, double opacity = 1,
    BrushCompositeMode mode = BrushCompositeMode::Paint)
{
    BasicPixelBrushStroke stroke(f.document, f.id, settings(opacity), mode);
    CHECK(stroke.valid()); CHECK(stroke.begin(point()));
    return stroke.end(point(), f.history);
}
void finish(FillOperation& fill)
{
    for (int i = 0; i < 1000; ++i) {
        const auto state = fill.step(4096);
        if (state == FillState::Ready) return;
        if (state == FillState::Failed || state == FillState::Cancelled) break;
    }
    CHECK(false && "fill did not finish");
}

void brushNumericAndHistory()
{
    Fixture f;
    auto other = std::make_shared<ScalarRasterSurface>(Extent2u {16, 16}, 93);
    CHECK(f.document.insertLayer(1, Layer::raster("Other channel", other)));
    const auto original = data(*f.surface), untouched = data(*other);
    CHECK(paint(f, .5) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface) == (Rgba8 {128, 128, 128, 255}));
    CHECK(data(*other) == untouched);
    const auto painted = data(*f.surface);
    CHECK(f.history.undoDepth() == 1);
    CHECK(f.history.undo(f.document)); CHECK(data(*f.surface) == original);
    CHECK(f.history.redo(f.document)); CHECK(data(*f.surface) == painted);
    CHECK(paint(f, 1, BrushCompositeMode::Erase) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface) == (Rgba8 {0, 0, 0, 255}));
    CHECK(f.history.undo(f.document)); CHECK(data(*f.surface) == painted);
    const auto redo = f.history.redoDepth();
    {
        BasicPixelBrushStroke cancelled(f.document, f.id, settings());
        CHECK(cancelled.begin(point())); cancelled.cancel();
    }
    CHECK(data(*f.surface) == painted); CHECK(f.history.redoDepth() == redo);

    // Scalar painting never changes the shared channel grid or surface.
    const auto id = f.surface->id();
    BasicPixelBrushStroke edge(f.document, f.id, settings());
    CHECK(edge.begin(point(0, 0)));
    CHECK(edge.end(point(0, 0), f.history) == RasterEditCommitResult::Committed);
    CHECK(std::get<RasterLayer>(f.document.layer(f.id)->payload).surface == f.surface);
    CHECK(f.surface->id() == id); CHECK(f.surface->extent() == (Extent2u {16, 16}));
}

void selectionAndSampledValues()
{
    Fixture f;
    CHECK(f.document.setSelection(SelectionMask::filled({16, 16}, 128)));
    const auto selection = f.document.selection();
    CHECK(paint(f, .5) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface) == (Rgba8 {64, 64, 64, 255}));
    CHECK(f.document.selection() == selection);

    // Deferred repair applies the soft selection once, before encoding.
    CHECK(f.history.undo(f.document));
    BasicPixelBrushStroke repair(f.document, f.id, settings(.5), BrushCompositeMode::Paint,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, nullptr, {},
        [](Vec2d) { return PremultipliedColor {1, 1, 1, 1}; }, "Scalar repair", true);
    CHECK(repair.begin(point())); CHECK(repair.finishInput(point()));
    CHECK(pixel(*f.surface).red == 0);
    CHECK(repair.reapplySampledColorSelectionResolved(
        [](Vec2d) { return PremultipliedColor {1, 1, 1, 1}; }));
    CHECK(repair.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface) == (Rgba8 {64, 64, 64, 255}));

    Fixture donor(255);
    BasicPixelBrushStroke zero(donor.document, donor.id, settings(), BrushCompositeMode::Paint,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, nullptr, {},
        [](Vec2d) { return PremultipliedColor {0, 0, 0, 1}; });
    CHECK(zero.begin(point())); CHECK(zero.end(point(), donor.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*donor.surface) == (Rgba8 {0, 0, 0, 255}));
    BasicPixelBrushStroke invalid(donor.document, donor.id, settings(), BrushCompositeMode::Paint,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, nullptr, {},
        [](Vec2d) { return PremultipliedColor {1, 1, 1, 0}; });
    CHECK(invalid.begin(point())); CHECK(invalid.end(point(), donor.history) == RasterEditCommitResult::NoChanges);
    CHECK(pixel(*donor.surface).red == 0);

    BasicPixelBrushStroke blur(donor.document, donor.id, settings(), BrushCompositeMode::FilterColor,
        std::make_unique<BasicPixelBrushEngine>(), {}, {}, nullptr, {},
        [](Vec2d) { return PremultipliedColor {.5F, .5F, .5F, 1}; });
    CHECK(blur.begin(point())); CHECK(blur.end(point(), donor.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*donor.surface) == (Rgba8 {128, 128, 128, 255}));
}

void fillsAndNumericTolerance()
{
    Fixture f;
    FillOperation fill(f.document, f.id, {.color = {255, 255, 255, 255}, .opacity = .5});
    finish(fill); CHECK(fill.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface) == (Rgba8 {128, 128, 128, 255}));
    CHECK(f.history.undo(f.document)); CHECK(pixel(*f.surface).red == 0);
    CHECK(f.history.redo(f.document));
    CHECK(f.document.setSelection(SelectionMask::filled({16, 16}, 128)));
    FillOperation erase(f.document, f.id, {.eraseSelection = true});
    finish(erase); CHECK(erase.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*f.surface) == (Rgba8 {64, 64, 64, 255}));
    CHECK(f.history.undo(f.document)); CHECK(pixel(*f.surface).red == 128);

    Fixture bucket;
    std::vector<std::byte> row(16);
    for (int i = 0; i < 16; ++i) row[std::size_t(i)] = std::byte(i < 4 ? 0 : i < 8 ? 8 : 12);
    std::vector<RasterPatch> rows;
    for (int y = 0; y < 16; ++y) rows.push_back({{0, y, 16, 1}, row, 16});
    (void)bucket.surface->replaceNativeBatch(rows);
    FillOperation contiguous(bucket.document, bucket.id,
        {.mode = FillMode::Contiguous, .color = {231, 231, 231, 255}, .tolerance = 8, .seed = {.5, .5}});
    finish(contiguous); CHECK(contiguous.commit(bucket.history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*bucket.surface, 0, 0).red == 231);
    CHECK(pixel(*bucket.surface, 7, 7).red == 231);
    CHECK(pixel(*bucket.surface, 8, 7).red == 12);
}

void colourPaintingRemainsLinearLight()
{
    Document document(CanvasSpec {.extent = {16, 16}});
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u {16, 16}, Rgba8 {0, 0, 0, 255});
    auto layer = Layer::raster("Colour", surface); const auto id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    History history;
    BasicPixelBrushStroke stroke(document, id, settings(.5));
    CHECK(stroke.begin(point())); CHECK(stroke.end(point(), history) == RasterEditCommitResult::Committed);
    const auto* raster = std::get_if<RasterLayer>(&document.layer(id)->payload);
    CHECK(raster && pixel(*raster->surface) == (Rgba8 {188, 188, 188, 255}));
}
}

int main()
{
    brushNumericAndHistory(); selectionAndSampledValues(); fillsAndNumericTolerance();
    colourPaintingRemainsLinearLight();
    if (failures) std::cerr << failures << " scalar brush checks failed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
