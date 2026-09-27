#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/SelectedPixelTransform.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <sys/resource.h>
using namespace imageeditor::core;
namespace
{
int failures = 0;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::cerr << __LINE__ << ": " #x "\n";                                                           \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (false)
std::vector<std::byte> bytes(const RasterSurface &s)
{
    const auto e = s.extent();
    std::vector<std::byte> result(std::size_t(e.width) * e.height * 4);
    s.copyRgba8({0, 0, int(e.width), int(e.height)}, result, std::size_t(e.width) * 4);
    return result;
}
struct Fixture {
    Document doc{{{48, 40}, 96}};
    History history;
    std::shared_ptr<ContiguousRasterSurface> source =
        std::make_shared<ContiguousRasterSurface>(Extent2u{16, 12}, Rgba8{});
    LayerId id;
    std::vector<std::byte> original;
    Fixture()
    {
        original.resize(16 * 12 * 4);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 16; ++x) {
                const auto p = (y * 16 + x) * 4;
                original[p] = std::byte((x * 37 + y * 7) % 256);
                original[p + 1] = std::byte((x * 13 + y * 31) % 256);
                original[p + 2] = std::byte((x * 19 + y * 23) % 256);
                original[p + 3] = std::byte((x + y) % 5 == 0 ? 0 : (x + y) % 3 == 0 ? 128 : 255);
            }
        RasterPatch patch{{0, 0, 16, 12}, original, 64};
        (void)source->replaceRgba8Batch(std::span(&patch, 1));
        auto layer = Layer::raster("Original", source);
        id = layer.id;
        CHECK(doc.insertLayer(0, std::move(layer)));
        doc.setSelection(SelectionMask::rectangle({48, 40}, {2, 2, 6, 5}));
    }
    const RasterSurface &surface() const { return *std::get<RasterLayer>(doc.layer(id)->payload).surface; }
    std::array<std::byte, 4> pixel(int x, int y) const
    {
        const auto o = doc.layer(id)->rasterOrigin;
        std::array<std::byte, 4> b{};
        surface().copyRgba8({x - int(o.x), y - int(o.y), 1, 1}, b, 4);
        return b;
    }
};
void identityAndCoverage()
{
    Fixture f;
    auto mask = SelectionMask::rectangle({48, 40}, {2, 2, 6, 5}, 93);
    f.doc.setSelection(mask);
    const auto state = f.doc.contentState();
    {
        SelectedPixelTransformSession s(f.doc, f.id);
        CHECK(s.preview({}));
        CHECK(bytes(f.surface()) == f.original);
        CHECK(s.commit(f.history) == TransformCommitResult::NoChange);
    }
    CHECK(!f.history.canUndo());
    CHECK(f.doc.contentState() == state);
    {
        SelectedPixelTransformSession s(f.doc, f.id);
        CHECK(s.preview({1, 0, 9, 0, 1, 1}));
        CHECK(s.completeAction());
        CHECK(s.preview({}));
        CHECK(bytes(f.surface()) == f.original);
        CHECK(s.completeAction());
        CHECK(s.commit(f.history) == TransformCommitResult::NoChange);
    }
    CHECK(f.doc.selection()->equivalent(*mask));
    CHECK(bytes(f.surface()) == f.original);
    CHECK(!f.history.canUndo());
    {
        SelectedPixelTransformSession s(f.doc, f.id);
        CHECK(s.preview({1, 0, 18, 0, 1, 0}));
        CHECK(s.completeAction());
        // Selection coverage is independent from source alpha, including holes.
        CHECK(f.doc.selection()->coverageAtDocumentPixel(20, 2) == 93);
        CHECK(f.pixel(2, 2)[3] == std::byte(162)); // full source alpha, fractional removal once
        s.cancel();
    }
    CHECK(bytes(f.surface()) == f.original);
    CHECK(f.doc.contentState() == state);
}
void integerMoveAndHistory()
{
    Fixture f;
    const auto selected = f.doc.selection();
    SelectedPixelTransformSession s(f.doc, f.id);
    CHECK(s.preview({1, 0, 8, 0, 1, 0}));
    CHECK(s.completeAction());
    CHECK(f.pixel(2, 2)[3] == std::byte{});
    CHECK(f.pixel(10, 2)[3] != std::byte{});
    const auto first = bytes(f.surface());
    const auto firstOrigin = f.doc.layer(f.id)->rasterOrigin;
    CHECK(s.preview({1, 0, -9, 0, 1, -7}));
    CHECK(s.completeAction());
    CHECK(f.doc.layer(f.id)->rasterOrigin.x < 0);
    CHECK(f.doc.layer(f.id)->localToDocument == ProjectiveTransform{});
    const auto second = bytes(f.surface());
    const auto secondOrigin = f.doc.layer(f.id)->rasterOrigin;
    CHECK(s.undo());
    CHECK(bytes(f.surface()) == first);
    CHECK(f.doc.layer(f.id)->rasterOrigin == firstOrigin);
    CHECK(s.undo());
    CHECK(bytes(f.surface()) == f.original);
    CHECK(f.doc.selection()->equivalent(*selected));
    CHECK(s.redo());
    CHECK(bytes(f.surface()) == first);
    CHECK(s.redo());
    CHECK(bytes(f.surface()) == second);
    CHECK(s.commit(f.history) == TransformCommitResult::Committed);
    CHECK(f.history.undoDepth() == 2);
    CHECK(f.history.undo(f.doc));
    CHECK(bytes(f.surface()) == first);
    CHECK(f.history.undo(f.doc));
    CHECK(bytes(f.surface()) == f.original);
    CHECK(f.history.redo(f.doc));
    CHECK(f.history.redo(f.doc));
    CHECK(bytes(f.surface()) == second);
    CHECK(f.doc.layer(f.id)->rasterOrigin == secondOrigin);
    CHECK(bytes(*f.source) == f.original);
}
void immutableFragmentAndNoOpRedo()
{
    Fixture f;
    CHECK(f.history.execute(f.doc, std::make_unique<SetSelectionCommand>(
                                       SelectionMask::rectangle({48, 40}, {1, 1, 3, 3}), "Select")));
    CHECK(f.history.undo(f.doc));
    const auto state = f.doc.contentState();
    CHECK(f.history.canRedo());
    {
        SelectedPixelTransformSession s(f.doc, f.id);
        CHECK(s.preview({.2, 0, 3, 0, .2, 2}));
        CHECK(s.completeAction());
        CHECK(s.preview({1, 0, 0, 0, 1, 0}));
        CHECK(s.commit(f.history) == TransformCommitResult::NoChange);
    }
    CHECK(f.history.canRedo());
    CHECK(state == f.doc.contentState());
    CHECK(bytes(f.surface()) == f.original);
    Fixture g;
    {
        SelectedPixelTransformSession s(f.doc, f.id);
        CHECK(s.preview({.17, 0, 2, 0, .24, 3}));
        CHECK(s.completeAction());
        CHECK(s.preview({1, 0, 20.25, 0, 1, 3.5}));
        CHECK(s.completeAction());
        CHECK(s.commit(f.history) == TransformCommitResult::Committed);
    }
    {
        SelectedPixelTransformSession s(g.doc, g.id);
        CHECK(s.preview({1, 0, 20.25, 0, 1, 3.5}));
        CHECK(s.commit(g.history) == TransformCommitResult::Committed);
    }
    CHECK(bytes(f.surface()) == bytes(g.surface()));
    CHECK(f.doc.layer(f.id)->rasterOrigin == g.doc.layer(g.id)->rasterOrigin);
}
void transformedTargetAndIsolation()
{
    Fixture f;
    auto *layer = f.doc.layer(f.id);
    layer->localToDocument = {0, -1, 20, 2, .3, 4};
    const auto external = layer->localToDocument;
    layer->crop = LayerCrop{{1, 1, 13, 10}};
    const auto crop = layer->crop;
    f.doc.setSelection(SelectionMask::rectangle({48, 40}, {10, 8, 8, 15}, 170));
    auto other = Layer::raster(
        "Untouched", std::make_shared<ContiguousRasterSurface>(Extent2u{4, 3}, Rgba8{11, 22, 33, 44}));
    const auto otherId = other.id;
    CHECK(f.doc.insertLayer(0, std::move(other)));
    const auto otherBytes = bytes(*std::get<RasterLayer>(f.doc.layer(otherId)->payload).surface);
    {
        SelectedPixelTransformSession s(f.doc, f.id);
        CHECK(s.preview({1, 0, -20, 0, 1, 11}));
        CHECK(s.completeAction());
        CHECK(f.doc.layer(f.id)->localToDocument == external);
        CHECK(f.doc.layer(f.id)->crop == crop);
        CHECK(s.commit(f.history) == TransformCommitResult::Committed);
    }
    CHECK(bytes(*std::get<RasterLayer>(f.doc.layer(otherId)->payload).surface) == otherBytes);
    CHECK(f.history.undo(f.doc));
    CHECK(bytes(f.surface()) == f.original);
    CHECK(f.doc.layer(f.id)->localToDocument == external);
}
void projectiveAndFailure()
{
    Fixture f;
    SelectedPixelTransformSession s(f.doc, f.id);
    const auto h = rectangleToQuad({2, 2, 6, 5}, {{{1, 3}, {8, 2}, {8, 7}, {2, 7}}});
    CHECK(h);
    CHECK(s.preview(*h));
    CHECK(s.completeAction());
    const auto previous = bytes(f.surface());
    CHECK(!s.preview({1, 0, 0, 0, 1, 0, -.2, 0, 1}));
    CHECK(bytes(f.surface()) == previous);
    CHECK(s.preview(*h));
    CHECK(bytes(f.surface()) == previous);
    CHECK(!s.completeAction());
    s.cancel();
    CHECK(bytes(f.surface()) == f.original);
    bool rejected = false;
    try {
        SelectedPixelTransformSession tiny(f.doc, f.id, 64);
    } catch (const std::exception &) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(bytes(f.surface()) == f.original);
}
void regionalCopyOnWrite()
{
    Fixture f;
    auto a = std::make_shared<RegionalRasterSurface>(f.source, Vec2d{});
    auto state = a->state();
    state.bounds = {-5, -3, 30, 20};
    a->restore(state);
    auto b = std::make_shared<RegionalRasterSurface>(a, a->origin());
    const auto bBefore = bytes(*b);
    std::array<std::byte, 4> value{std::byte{3}, std::byte{9}, std::byte{19}, std::byte{128}};
    RasterPatch patch{{8, 7, 1, 1}, value, 4};
    const auto revision = a->revision();
    (void)a->replaceRgba8Batch(std::span(&patch, 1));
    CHECK(bytes(*b) == bBefore);
    CHECK(bytes(*f.source) == f.original);
    CHECK(!a->dirtySince(revision).fullRefresh);
    a->restore(state);
    CHECK(bytes(*a) == bBefore);
}
void nativeBytesAndSubsequentEdits()
{
    Fixture f;
    {
        SelectedPixelTransformSession edit(f.doc, f.id);
        CHECK(edit.preview({1, 0, -12, 0, 1, -9}));
        CHECK(edit.commit(f.history) == TransformCommitResult::Committed);
    }
    // Opaque and partial-alpha RGBA8 values survive exact-grid placement once.
    for (int y = 2; y < 7; ++y)
        for (int x = 2; x < 8; ++x) {
            const auto i = std::size_t(y * 16 + x) * 4;
            if (f.original[i + 3] != std::byte{})
                CHECK(std::equal(f.original.begin() + std::ptrdiff_t(i),
                                 f.original.begin() + std::ptrdiff_t(i + 4), f.pixel(x - 12, y - 9).begin()));
        }
    const auto transformed = bytes(f.surface());
    CHECK(f.history.execute(f.doc, std::make_unique<SetSelectionCommand>(SelectionState{}, "Deselect")));
    const auto origin = f.doc.layer(f.id)->rasterOrigin;
    const auto surface = std::get<RasterLayer>(f.doc.layer(f.id)->payload).surface;
    const std::array<std::byte, 4> value{std::byte{11}, std::byte{72}, std::byte{83}, std::byte{91}};
    // Exercise the same ordinary regional surface writes used by painting.
    auto before = surface->revision();
    RasterPatch patch{{int(-10 - origin.x), int(-7 - origin.y), 1, 1}, value, 4};
    RasterEditTransaction paint(f.doc, f.id, "Paint after pixel transform");
    (void)paint.writeRgba8Batch(std::span(&patch, 1));
    CHECK(paint.commit(f.history) == RasterEditCommitResult::Committed);
    CHECK(!surface->dirtySince(before).fullRefresh);
    CHECK(f.pixel(-10, -7) == value);
    CHECK(bytes(*f.source) == f.original);
    // The source is immutable even when a later ordinary edit changes the COW surface.
    CHECK(bytes(f.surface()) != transformed);
    CHECK(f.history.undo(f.doc));
    CHECK(bytes(f.surface()) == transformed);
    CHECK(f.history.undo(f.doc));
    CHECK(f.history.undo(f.doc));
    CHECK(bytes(f.surface()) == f.original);
    CHECK(f.history.redo(f.doc));
    CHECK(f.history.redo(f.doc));
    CHECK(f.history.redo(f.doc));
    CHECK(f.pixel(-10, -7) == value);
}
void staleTargetsDoNotRetarget()
{
    Fixture f;
    SelectedPixelTransformSession edit(f.doc, f.id);
    CHECK(f.doc.takeLayer(f.id).has_value());
    auto replacement = Layer::raster(
        "Replacement", std::make_shared<ContiguousRasterSurface>(Extent2u{16, 12}, Rgba8{1, 2, 3, 4}));
    replacement.id = f.id;
    CHECK(f.doc.insertLayer(0, std::move(replacement)));
    const auto untouched = bytes(f.surface());
    CHECK(!edit.preview({1, 0, 3, 0, 1, 4}));
    CHECK(edit.commit(f.history) == TransformCommitResult::TargetUnavailable);
    edit.cancel();
    CHECK(bytes(f.surface()) == untouched);
    Fixture g;
    SelectedPixelTransformSession next(g.doc, g.id);
    g.doc.layer(g.id)->rasterOrigin = {1, 0};
    CHECK(!next.preview({1, 0, 4, 0, 1, 1}));
    next.cancel();
    CHECK(g.doc.layer(g.id)->rasterOrigin == Vec2d({1, 0}));
}
void distantPreviewsNeverResurrectPixels()
{
    Fixture wandered, direct;
    const ProjectiveTransform final{1, 0, -200, 0, 1, 200};
    SelectedPixelTransformSession edit(wandered.doc, wandered.id);
    CHECK(edit.preview({1, 0, -180, 0, 1, 0}));
    CHECK(edit.preview({1, 0, 180, 0, 1, 0}));
    CHECK(edit.preview(final));
    CHECK(wandered.pixel(-178, 2)[3] == std::byte{});
    CHECK(edit.commit(wandered.history) == TransformCommitResult::Committed);
    SelectedPixelTransformSession once(direct.doc, direct.id);
    CHECK(once.preview(final));
    CHECK(once.commit(direct.history) == TransformCommitResult::Committed);
    CHECK(bytes(wandered.surface()) == bytes(direct.surface()));
    CHECK(wandered.history.undo(wandered.doc));
    CHECK(bytes(wandered.surface()) == wandered.original);
    CHECK(wandered.history.redo(wandered.doc));
    CHECK(bytes(wandered.surface()) == bytes(direct.surface()));
}
void irregularCoverageAndOverlap()
{
    Fixture f;
    std::vector<std::uint8_t> coverage(48 * 40);
    // Feathered triangular lasso, a hole, and a disconnected soft island.
    for (int y = 1; y < 10; ++y)
        for (int x = 1; x <= y && x < 11; ++x)
            coverage[std::size_t(y) * 48 + std::size_t(x)] = (x == y ? 93 : 255);
    for (int y = 4; y < 6; ++y)
        for (int x = 2; x < 4; ++x)
            coverage[std::size_t(y) * 48 + std::size_t(x)] = 0;
    coverage[2 * 48 + 13] = 171;
    const auto selection = SelectionMask::fromR8({48, 40}, coverage, 48);
    f.doc.setSelection(selection);
    f.doc.setLastSelection(selection);
    SelectedPixelTransformSession edit(f.doc, f.id);
    CHECK(edit.preview({1, 0, 1, 0, 1, 0}));
    CHECK(edit.completeAction());
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            const auto originalIndex = std::size_t(y * 16 + x) * 4;
            if (!coverage[std::size_t(y) * 48 + std::size_t(x)] &&
                (x == 0 || !coverage[std::size_t(y) * 48 + std::size_t(x - 1)]))
                CHECK(std::equal(f.original.begin() + std::ptrdiff_t(originalIndex),
                                 f.original.begin() + std::ptrdiff_t(originalIndex + 4),
                                 f.pixel(x, y).begin()));
        }
    CHECK(f.doc.selection()->equivalent(*selection->translated(1, 0)));
    CHECK(f.doc.lastSelection()->equivalent(*f.doc.selection()));
    CHECK(edit.preview({-1, 0, 34, 0, 1, 0}));
    CHECK(edit.completeAction());
    CHECK(edit.preview({}));
    CHECK(edit.completeAction());
    CHECK(bytes(f.surface()) == f.original);
    CHECK(edit.commit(f.history) == TransformCommitResult::NoChange);
    CHECK(f.doc.lastSelection() == selection);
}
void parallelPreviewIsByteIdentical()
{
    const auto produce = [](bool serial) {
        Document doc({{1024, 1024}, 96});
        History history;
        std::vector<std::byte> pixels(1024 * 1024 * 4);
        for (std::size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = std::byte((i * 73 + (i / 4111) * 17) & 255);
        auto layer = Layer::raster(
            "Parallel", std::make_shared<ContiguousRasterSurface>(Extent2u{1024, 1024}, std::move(pixels)));
        const auto id = layer.id;
        CHECK(doc.insertLayer(0, std::move(layer)));
        doc.setSelection(SelectionMask::rectangle({1024, 1024}, {23, 13, 768, 768}, 117));
        SelectedPixelTransformSession edit(doc, id);
        const auto run = [&](unsigned, unsigned) { CHECK(edit.preview({.92, .12, 3.3, -.08, .87, 7.25})); };
        if (serial)
            CHECK(boundedParallel(1, run));
        else
            run(0, 1);
        CHECK(edit.commit(history) == TransformCommitResult::Committed);
        return bytes(*std::get<RasterLayer>(doc.layer(id)->payload).surface);
    };
    CHECK(produce(true) == produce(false));
}
} // namespace
int main(int argc, char **argv)
{
    if (argc > 1) {
        const bool textured = std::string_view(argv[1]) != "--benchmark";
        const bool distort = std::string_view(argv[1]) == "--benchmark-distort";
        for (const auto e : std::array{Extent2u{3840, 2160}, Extent2u{5120, 2880}})
            for (const auto region : std::array{RectI{128, 128, 256, 256}, RectI{128, 128, 1024, 1024},
                                                RectI{0, 0, int(e.width), int(e.height)}}) {
                Document document({e, 96});
                History history;
                std::shared_ptr<RasterSurface> source;
                if (textured) {
                    std::vector<std::byte> pixels(std::size_t(e.width) * e.height * 4);
                    for (std::uint32_t y = 0; y < e.height; ++y)
                        for (std::uint32_t x = 0; x < e.width; ++x) {
                            const auto p = (std::size_t(y) * e.width + x) * 4;
                            pixels[p] = std::byte((x * 17 + y * 7) & 255);
                            pixels[p + 1] = std::byte((x * 3 + y * 19) & 255);
                            pixels[p + 2] = std::byte((x * 11 + y * 5) & 255);
                            pixels[p + 3] = std::byte(64 + ((x + y * 3) % 192));
                        }
                    source = std::make_shared<ContiguousRasterSurface>(e, std::move(pixels));
                } else {
                    source = std::make_shared<ContiguousRasterSurface>(e, Rgba8{51, 179, 233, 191});
                }
                auto layer = Layer::raster("Benchmark", std::move(source));
                const auto id = layer.id;
                CHECK(document.insertLayer(0, std::move(layer)));
                document.setSelection(SelectionMask::rectangle(e, region, 173));
                using Clock = std::chrono::steady_clock;
                const auto start = Clock::now();
                SelectedPixelTransformSession edit(document, id);
                const auto captured = Clock::now();
                const RectD frame{double(region.x), double(region.y), double(region.width), double(region.height)};
                const auto projective = rectangleToQuad(frame, {{{frame.x, frame.y},
                    {frame.right() - frame.width * .08, frame.y + frame.height * .04},
                    {frame.right(), frame.bottom()}, {frame.x + frame.width * .04, frame.bottom()}}});
                CHECK(projective);
                const auto mapping = [&](int step) {
                    const ProjectiveTransform translation{1, 0, 40.25 + step, 0, 1, 12.5 + step};
                    return distort ? composeTransform(translation, *projective) : translation;
                };
                CHECK(edit.preview(mapping(0)));
                const auto cold = Clock::now();
                for (int i = 1; i <= 8; ++i)
                    CHECK(edit.preview(mapping(i)));
                const auto warm = Clock::now();
                CHECK(edit.completeAction());
                const auto checkpoint = Clock::now();
                CHECK(edit.commit(history) == TransformCommitResult::Committed);
                const auto applied = Clock::now();
                CHECK(history.undo(document));
                const auto undone = Clock::now();
                const auto ms = [](auto a, auto b) {
                    return std::chrono::duration<double, std::milli>(b - a).count();
                };
                rusage memory{};
                getrusage(RUSAGE_SELF, &memory);
                std::cout << (textured ? "textured " : "uniform ") << (distort ? "projective " : "translation ")
                          << e.width << 'x' << e.height << " ROI=" << region.width << 'x' << region.height
                          << " capture_ms=" << ms(start, captured)
                          << " first_preview_ms=" << ms(captured, cold)
                          << " warm_preview_ms=" << ms(cold, warm) / 8
                          << " checkpoint_ms=" << ms(warm, checkpoint)
                          << " apply_ms=" << ms(checkpoint, applied) << " undo_ms=" << ms(applied, undone)
                          << " fragment_bytes=" << edit.temporaryBytes()
                          << " history_bytes=" << history.memoryUsed()
                          << " evaluated_pixels=" << edit.evaluatedPixels()
                          << " peak_rss_kib=" << memory.ru_maxrss << '\n';
            }
        return failures ? 1 : 0;
    }
    identityAndCoverage();
    integerMoveAndHistory();
    immutableFragmentAndNoOpRedo();
    transformedTargetAndIsolation();
    projectiveAndFailure();
    regionalCopyOnWrite();
    nativeBytesAndSubsequentEdits();
    staleTargetsDoNotRetarget();
    distantPreviewsNeverResurrectPixels();
    irregularCoverageAndOverlap();
    parallelPreviewIsByteIdentical();
    std::cout << "Selected-pixel transforms: " << failures << " failures\n";
    return failures ? 1 : 0;
}
