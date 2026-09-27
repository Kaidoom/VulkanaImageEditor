#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/EditorSession.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) { std::cerr << "FAIL " << line << ": " << expression << '\n'; ++failures; }
}
#define CHECK(...) check(bool((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
template<class Exception, class Function> void throws(Function&& function)
{
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}
SmartReferenceImage image(Extent2u extent, Rgba8 color)
{
    const auto count = std::size_t(extent.width) * extent.height;
    return {extent, std::vector<Rgba8>(count, color), std::vector<std::uint8_t>(count, 1)};
}
void set(SmartReferenceImage& reference, int x, int y, Rgba8 color)
{ reference.pixels[std::size_t(y) * reference.extent.width + std::size_t(x)] = color; }
int at(const SelectionState& mask, int x, int y = 0)
{ CHECK(mask != nullptr); return mask ? mask->coverageAtDocumentPixel(x, y) : -1; }
SmartSelectionResult wand(const SmartReferenceImage& reference, Vec2d seed, int tolerance = 0,
    SelectionState original = {}, SelectionOperation operation = SelectionOperation::Replace)
{
    const std::atomic_bool cancelled {false};
    return buildMagicWand(reference, seed, tolerance, std::move(original), operation, cancelled);
}
ColorSelectionField field(const SmartReferenceImage& reference, Vec2d seed)
{
    const auto index = std::size_t(seed.y) * reference.extent.width + std::size_t(seed.x);
    ColorSelectionField result {reference.extent, reference.pixels[index], {}};
    for (std::size_t i = 0; i < reference.pixels.size(); ++i)
        result.distances.push_back(reference.valid[i]
            ? colorSelectionDistance(result.sampled, reference.pixels[i]) : 65535);
    return result;
}

void connectedAndDisconnected()
{
    constexpr Rgba8 subject {170, 20, 60, 255}, background {0, 170, 220, 255};
    auto reference = image({11, 7}, background);
    // A hole, a one-pixel arm, contact with the canvas edge, a nearby diagonal
    // match and a distant matching island all exercise four-connectivity.
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x)
        if (x == 0 || x == 4 || y == 0 || y == 4) set(reference, x, y, subject);
    for (int x = 5; x < 8; ++x) set(reference, x, 2, subject);
    set(reference, 8, 3, subject);
    set(reference, 10, 6, subject);
    const auto result = wand(reference, {.4, .8});
    CHECK(result.incoming == result.combined);
    CHECK(result.combined->extent() == reference.extent);
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 11; ++x) {
        const bool ring = x < 5 && y < 5 && (x == 0 || x == 4 || y == 0 || y == 4);
        const bool arm = y == 2 && x >= 5 && x < 8;
        CHECK(at(result.incoming, x, y) == (ring || arm ? 255 : 0));
    }
    CHECK(result.stats.workspaceBytes > 0);
    CHECK(result.stats.evaluatedPixels >= reference.pixels.size());
    const std::atomic_bool cancelled {false};
    const auto global = buildColorSelection(field(reference, {.4, .8}), 0, {}, SelectionOperation::Replace, cancelled);
    CHECK(at(global.combined, 8, 3) == 255 && at(global.combined, 10, 6) == 255);
    CHECK(at(result.combined, 8, 3) == 0 && at(result.combined, 10, 6) == 0);
}

void alphaAndInvalidPixels()
{
    auto reference = image({8, 1}, {255, 40, 0, 0});
    reference.pixels = {{255, 40, 0, 0}, {0, 240, 250, 0}, {0, 0, 0, 1},
        {255, 0, 0, 128}, {254, 0, 0, 128}, {0, 0, 0, 0}, {0, 220, 0, 0}, {0, 0, 0, 0}};
    for (int tolerance : {0, 127, 255}) {
        const auto transparent = wand(reference, {.5, .5}, tolerance);
        CHECK(at(transparent.combined, 0) == 255 && at(transparent.combined, 1) == 255);
        for (int x = 2; x < 8; ++x) CHECK(at(transparent.combined, x) == 0);
        const auto opaque = wand(reference, {3.5, .5}, tolerance);
        CHECK(at(opaque.combined, 0) == 0 && at(opaque.combined, 1) == 0);
        CHECK(at(opaque.combined, 5) == 0 && at(opaque.combined, 7) == 0);
    }
    // Fractional premultiplied differences agree with Select by Color.
    const auto partial = wand(reference, {3.5, .5});
    const auto comparisons = field(reference, {3.5, .5});
    CHECK(at(partial.combined, 3) == 255);
    CHECK(at(partial.combined, 4) == colorSelectionPixelCoverage(comparisons, 4, 0));
    auto cropped = image({9, 3}, {25, 30, 35, 255});
    for (int y = 0; y < 3; ++y) cropped.valid[std::size_t(y) * 9 + 4] = 0;
    const auto left = wand(cropped, {.5, 1.5}, 255);
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 9; ++x)
        CHECK(at(left.combined, x, y) == (x < 4 ? 255 : 0));
    throws<std::invalid_argument>([&] { (void)wand(cropped, {4.5, 1.5}); });
}

void antialiasedContourAndFringe()
{
    auto reference = image({8, 3}, {0, 0, 0, 255});
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 8; ++x) {
        const auto value = std::uint8_t(32 * x);
        set(reference, x, y, {value, value, value, 255});
    }
    const auto comparisons = field(reference, {.5, 1.5});
    const auto edge = wand(reference, {.5, 1.5}, 32);
    CHECK(at(edge.combined, 1, 1) == 131);
    CHECK(at(edge.combined, 0, 1) == 255 && at(edge.combined, 2, 1) == 0);
    // At tolerance 48, pixel 2 is outside the center-connected region but its
    // reconstructed threshold contour covers a fraction of that pixel.
    const auto fringe = wand(reference, {.5, 1.5}, 48);
    CHECK(at(fringe.combined, 2, 1) > 0 && at(fringe.combined, 2, 1) < 128);
    CHECK(at(fringe.combined, 2, 1) == colorSelectionPixelCoverage(comparisons, 10, 48));
    CHECK(at(fringe.combined, 3, 1) == 0);
    const auto equal = wand(reference, {.99, 1.01}, 48);
    CHECK(equal.combined->equivalent(*fringe.combined));
    CHECK(wand(reference, {.5, 1.5}, -80).combined->equivalent(*wand(reference, {.5, 1.5}, 0).combined));
    CHECK(wand(reference, {.5, 1.5}, 900).combined->equivalent(*wand(reference, {.5, 1.5}, 255).combined));
}

void baselineCombinationsAndHistory()
{
    auto reference = image({16, 4}, {0, 0, 0, 255});
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 16; ++x)
        set(reference, x, y, {std::uint8_t(x * 16), 0, 0, 255});
    const auto original = SelectionMask::rectangle(reference.extent, {3, 0, 8, 4});
    for (const auto operation : {SelectionOperation::Replace, SelectionOperation::Add,
             SelectionOperation::Subtract, SelectionOperation::Intersect}) {
        const auto low = wand(reference, {.5, 1.5}, 32, original, operation);
        (void)wand(reference, {.5, 1.5}, 192, original, operation);
        const auto lowAgain = wand(reference, {.5, 1.5}, 32, original, operation);
        CHECK(low.combined->equivalent(*lowAgain.combined));
        CHECK(low.combined->equivalent(*combineSelection(original, low.incoming, operation)));
    }
    EditorSession session;
    session.replaceDocument(std::make_unique<Document>(CanvasSpec {reference.extent}));
    auto& document = *session.document();
    document.markSaved();
    const auto revision = document.revision(), content = document.contentState();
    const auto first = wand(reference, {.5, 1.5}, 32);
    const auto second = wand(reference, {.5, 1.5}, 96);
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(first.combined, "Magic Wand")));
    CHECK(session.execute(std::make_unique<SetSelectionCommand>(second.combined, "Magic Wand")));
    CHECK(session.history().undoDepth() == 2);
    CHECK(session.undo());
    CHECK(document.selection()->equivalent(*first.combined));
    CHECK(!session.execute(std::make_unique<SetSelectionCommand>(first.combined, "Magic Wand")));
    CHECK(session.history().canRedo());
    CHECK(session.redo());
    CHECK(document.selection()->equivalent(*second.combined));
    CHECK(document.revision() == revision && document.contentState() == content && !document.isModified());
}

void validationAndCancellation()
{
    const auto reference = image({32, 16}, {10, 20, 30, 255});
    const std::atomic_bool cancelled {true};
    const auto result = buildMagicWand(reference, {.5, .5}, 255, {}, SelectionOperation::Replace, cancelled);
    CHECK(!result.incoming && !result.combined && !result.hints.foreground && !result.hints.background);
    for (const auto point : {Vec2d {-1, 0}, Vec2d {32, 0}, Vec2d {0, 16},
             Vec2d {std::numeric_limits<double>::quiet_NaN(), 0},
             Vec2d {0, std::numeric_limits<double>::infinity()}})
        throws<std::invalid_argument>([&] { (void)wand(reference, point); });
    auto malformed = reference;
    malformed.valid.pop_back();
    throws<std::invalid_argument>([&] { (void)wand(malformed, {.5, .5}); });
    malformed = reference;
    malformed.pixels.pop_back();
    throws<std::invalid_argument>([&] { (void)wand(malformed, {.5, .5}); });
    throws<std::invalid_argument>([&] { (void)wand({}, {.5, .5}); });
}
}
int main()
{
    connectedAndDisconnected();
    alphaAndInvalidPixels();
    antialiasedContourAndFringe();
    baselineCombinationsAndHistory();
    validationAndCancellation();
    if (failures) std::cerr << failures << " Magic Wand check(s) failed\n";
    else std::cout << "Magic Wand connectivity, alpha, coverage, refinement and history checks passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
