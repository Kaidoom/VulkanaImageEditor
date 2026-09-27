#include "imageeditor/core/ColorSelection.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/SelectionCommands.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

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

// A non-contiguous authoritative source catches full-image readbacks, writes,
// and resampling during immutable comparison-field refinements.
class ObservedSurface final : public RasterSurface {
public:
    using Pixel = std::function<Rgba8(int, int)>;
    ObservedSurface(Extent2u extent, Pixel pixel)
        : extent_(extent), pixel_(std::move(pixel)), id_(nextId_++) {}
    SurfaceId id() const noexcept override { return id_; }
    Extent2u extent() const noexcept override { return extent_; }
    Revision revision() const noexcept override { return revision_; }
    DirtySet dirtySince(Revision revision) const override
    { return {.revision = revision_, .fullRefresh = revision != revision_, .regions = {}}; }
    void copyRgba8(RectI area, std::span<std::byte> output, std::size_t stride) const override
    {
        CHECK(area.x >= 0 && area.y >= 0 && area.right() <= int(extent_.width)
            && area.bottom() <= int(extent_.height));
        const auto count = std::size_t(area.width) * std::size_t(area.height);
        maximumRead = std::max(maximumRead, count); texelsRead += count; ++readCalls;
        for (int y = 0; y < area.height; ++y) for (int x = 0; x < area.width; ++x) {
            const auto color = pixel_(area.x + x, area.y + y);
            const auto offset = std::size_t(y) * stride + std::size_t(x) * 4;
            output[offset] = std::byte(color.red); output[offset + 1] = std::byte(color.green);
            output[offset + 2] = std::byte(color.blue); output[offset + 3] = std::byte(color.alpha);
        }
    }
    DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override
    { ++writes; throw std::runtime_error("Selection must not write source pixels"); }
    DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override
    { ++writes; throw std::runtime_error("Selection must not swap source pixels"); }
    void changed() { ++revision_; }
    mutable std::size_t maximumRead {0}, texelsRead {0}, readCalls {0};
    std::size_t writes {0};
private:
    inline static SurfaceId nextId_ {9000000};
    Extent2u extent_;
    Pixel pixel_;
    SurfaceId id_;
    Revision revision_ {1};
};

LayerId add(Document& document, Layer layer)
{
    const auto id = layer.id;
    CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
    return id;
}
std::shared_ptr<ObservedSurface> solid(Extent2u extent, Rgba8 color)
{ return std::make_shared<ObservedSurface>(extent, [color](int, int) { return color; }); }
std::shared_ptr<const ColorSelectionField> finish(ColorSelectionReference& reference, std::size_t budget = 31)
{
    while (!reference.field()) {
        const auto before = reference.sampledPixels();
        (void)reference.step(budget);
        CHECK(reference.sampledPixels() - before <= std::max(std::size_t(1), budget));
    }
    CHECK(reference.step(budget));
    return reference.field();
}
ColorSelectionResult build(const ColorSelectionField& field, int fuzziness = 0,
    SelectionState base = {}, SelectionOperation operation = SelectionOperation::Replace)
{
    const std::atomic_bool cancelled {false};
    return buildColorSelection(field, fuzziness, std::move(base), operation, cancelled);
}
std::shared_ptr<const ColorSelectionField> sample(Document& document, std::optional<LayerId> layer,
    Vec2d seed, ColorSampleSource source = ColorSampleSource::ActiveLayer)
{
    ColorSelectionReference reference(document, layer, source, seed);
    return finish(reference);
}
std::uint8_t at(const SelectionState& mask, int x, int y = 0)
{ CHECK(mask != nullptr); return mask ? mask->coverageAtDocumentPixel(x, y) : 0; }

void disconnectedAndReadOnly()
{
    EditorSession session;
    auto document = std::make_unique<Document>(CanvasSpec {{7, 3}});
    auto surface = std::make_shared<ObservedSurface>(Extent2u {7, 3}, [](int x, int y) {
        return (x == 0 && y == 0) || (x == 6 && y == 2) || (x == 3 && y == 1)
            ? Rgba8 {177, 35, 82, 255} : Rgba8 {0, 190, 20, 255};
    });
    const auto id = add(*document, Layer::raster("Disconnected", surface));
    session.replaceDocument(std::move(document));
    session.document()->markSaved();
    const auto colors = session.colors();
    const auto revision = session.document()->revision();
    const auto content = session.document()->contentState();
    ColorSelectionReference reference(*session.document(), id, ColorSampleSource::ActiveLayer, {.2, .9});
    CHECK(reference.sampledColor() == Rgba8({177, 35, 82, 255}));
    CHECK(!reference.field() && reference.sampledPixels() == 0);
    const auto field = finish(reference, 4);
    CHECK(reference.sampledPixels() == 21);
    const auto reads = surface->texelsRead;
    const auto result = build(*field);
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 7; ++x)
        CHECK(at(result.combined, x, y) == (((x == 0 && y == 0) || (x == 6 && y == 2)
            || (x == 3 && y == 1)) ? 255 : 0));
    CHECK(result.incoming == result.combined);
    CHECK(result.combined->extent() == Extent2u({7, 3}));
    CHECK(result.combined->boundaryEdges().size() == 12);
    CHECK(at(result.combined, -1) == 0 && at(result.combined, 7) == 0);
    for (int fuzziness : {25, 100, 255, 0}) CHECK(build(*field, fuzziness).combined != nullptr);
    CHECK(surface->texelsRead == reads && surface->maximumRead <= 4 && surface->writes == 0);
    CHECK(reference.sampledPixels() == 21 && reference.matches(*session.document()));
    CHECK(session.document()->revision() == revision && session.document()->contentState() == content);
    CHECK(!session.document()->isModified() && !session.document()->selection());
    CHECK(session.colors().foreground() == colors.foreground());
    CHECK(session.colors().background() == colors.background());
    CHECK(!session.history().canUndo());
}

void gradientsAndMonotonicity()
{
    Document document({{256, 3}});
    auto surface = std::make_shared<ObservedSurface>(Extent2u {256, 3}, [](int x, int y) {
        const auto value = std::uint8_t(x);
        return y == 0 ? Rgba8 {value, value, value, 255}
            : y == 1 ? Rgba8 {value, 0, 0, 255} : Rgba8 {0, value, 0, 255};
    });
    const auto id = add(document, Layer::raster("Channel gradients", surface));
    const auto field = sample(document, id, {.5, .5});
    std::array<std::uint8_t, 768> previous {};
    for (int fuzziness = 0; fuzziness <= 255; ++fuzziness) {
        const auto result = build(*field, fuzziness);
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 256; ++x) {
            const auto value = at(result.combined, x, y);
            const auto index = std::size_t(y * 256 + x);
            CHECK(value >= previous[index]);
            CHECK(value == (x <= fuzziness ? 255 : 0));
            previous[index] = value;
        }
    }
    // Gray luminance and channel differences count, despite equal/undefined hue.
    CHECK(colorSelectionDistance({20, 20, 20, 255}, {120, 120, 120, 255}) == 25500);
    CHECK(colorSelectionDistance({90, 0, 0, 255}, {0, 90, 0, 255}) == 22950);
    CHECK(build(*field, -50).combined->equivalent(*build(*field, 0).combined));
    CHECK(build(*field, 999).combined->equivalent(*build(*field, 255).combined));
}

void alphaClassesAndQuantizationCoverage()
{
    constexpr std::array colors {Rgba8 {255, 0, 0, 0}, Rgba8 {0, 255, 203, 0},
        Rgba8 {255, 0, 0, 255}, Rgba8 {255, 0, 0, 128}, Rgba8 {254, 0, 0, 128},
        Rgba8 {253, 0, 0, 128}, Rgba8 {0, 0, 0, 1}};
    Document document({{7, 1}});
    auto surface = std::make_shared<ObservedSurface>(Extent2u {7, 1}, [colors](int x, int) { return colors[std::size_t(x)]; });
    const auto id = add(document, Layer::raster("Alpha classes", surface));
    const auto transparent = sample(document, id, {.5, .5});
    for (const int fuzziness : {0, 127, 255}) {
        const auto result = build(*transparent, fuzziness);
        CHECK(at(result.combined, 0) == 255 && at(result.combined, 1) == 255);
        for (int x = 2; x < 7; ++x) CHECK(at(result.combined, x) == 0);
    }
    const auto opaque = sample(document, id, {2.5, .5});
    for (const int fuzziness : {0, 127, 255}) {
        const auto result = build(*opaque, fuzziness);
        CHECK(at(result.combined, 0) == 0 && at(result.combined, 1) == 0);
    }
    const auto partial = sample(document, id, {3.5, .5});
    const auto exact = build(*partial, 0);
    CHECK(at(exact.combined, 3) == 255); // Partial alpha is compared, not multiplied into an exact match.
    CHECK(at(exact.combined, 4) == 127); // One encoded-red byte at alpha 128 is 128/255 distance.
    // Isolate byte quantization from the steep alpha discontinuity at x=6;
    // distance-contour AA is separately checked on genuine spatial ramps.
    const ColorSelectionField byteRamp {{3, 1}, partial->sampled,
        {partial->distances[3], partial->distances[4], partial->distances[5]}};
    CHECK(at(build(byteRamp).combined, 2) == 0);
    const auto next = build(*partial, 1);
    CHECK(at(next.combined, 4) == 255 && at(build(byteRamp, 1).combined, 2) == 254);
    CHECK(colorSelectionDistance(colors[0], colors[1]) == 0);
    CHECK(colorSelectionDistance(colors[0], colors[6]) == 65535);
    CHECK(colorSelectionCoverage(65535, 255) == 0);
    CHECK(colorSelectionCoverage(0, 0) == 255);
    // The transition responds to comparison precision and never spatially blurs neighbors.
    ColorSelectionField isolated {{5, 1}, {}, {65535, 128, 65535, 128, 65535}};
    const auto coverage = build(isolated);
    CHECK(at(coverage.combined, 0) == 0 && at(coverage.combined, 1) == 127
        && at(coverage.combined, 2) == 0 && at(coverage.combined, 3) == 127);
    CHECK(coverage.combined->bounds() == RectI({1, 0, 3, 1}));
    CHECK(coverage.combined->boundaryEdges().empty());
}

void distanceContourAntialiasing()
{
    // Both an opaque sRGB ramp and a constant-hue alpha ramp have a distance
    // slope of 32 byte units/pixel. A threshold of 32.5 crosses 33/64 of the
    // pixel centered on distance 32, giving round(255 * 33/64) == 131.
    std::shared_ptr<const ColorSelectionField> opaqueField;
    for (const bool alphaRamp : {false, true}) {
        Document document({{7, 3}});
        auto surface = std::make_shared<ObservedSurface>(Extent2u {7, 3}, [alphaRamp](int x, int) {
            return alphaRamp ? Rgba8 {200, 100, 20, std::uint8_t(255 - 32 * x)}
                : Rgba8 {std::uint8_t(32 * x), std::uint8_t(32 * x), std::uint8_t(32 * x), 255};
        });
        const auto id = add(document, Layer::raster(alphaRamp ? "Alpha ramp" : "Opaque ramp", surface));
        const auto field = sample(document, id, {.5, 1.5});
        if (!alphaRamp) opaqueField = field;
        else CHECK(field->distances == opaqueField->distances);
        const auto exact = build(*field, 32).combined;
        CHECK(at(exact, 0, 1) == 255 && at(exact, 1, 1) == 131 && at(exact, 2, 1) == 0);
        CHECK(at(build(*field, 31).combined, 1, 1) == 124);
        CHECK(at(build(*field, 40).combined, 1, 1) == 195);
        CHECK(at(build(*field, 48).combined, 1, 1) == 255);
        CHECK(at(build(*field, 48).combined, 2, 1) == 4);
        CHECK(at(exact, 1, 0) == at(exact, 1, 2)); // No dependence on the canvas row edge.
        std::array<std::uint8_t, 21> previous {};
        for (int fuzziness = 0; fuzziness <= 255; ++fuzziness) {
            const auto mask = build(*field, fuzziness).combined;
            for (int y = 0; y < 3; ++y) for (int x = 0; x < 7; ++x) {
                const auto index = std::size_t(y * 7 + x), value = std::size_t(at(mask, x, y));
                CHECK(value >= previous[index]);
                if (x == 0) CHECK(value == 255); // Exact reference matches remain fully selected.
                previous[index] = std::uint8_t(value);
            }
        }
        CHECK(build(*field, 999).combined->equivalent(*build(*field, 255).combined));
        CHECK(surface->writes == 0);
    }
    // A diagonal ramp tests the two-dimensional pixel-area integral. At the
    // centered threshold, the selected area is 1/2 + 1/32 - 1/2048.
    Document diagonal({{7, 7}});
    auto surface = std::make_shared<ObservedSurface>(Extent2u {7, 7}, [](int x, int y) {
        const auto value = std::uint8_t(16 * (x + y)); return Rgba8 {value, value, value, 255};
    });
    const auto diagonalId = add(diagonal, Layer::raster("Diagonal ramp", surface));
    const auto field = sample(diagonal, diagonalId, {.5, .5});
    const auto diagonalMask = build(*field, 64).combined;
    CHECK(at(diagonalMask, 2, 2) == 135);
    CHECK(at(diagonalMask, 1, 2) == 255 && at(diagonalMask, 3, 2) == 0);
    CHECK(at(build(*field, 63).combined, 2, 2) == 120);
    CHECK(at(build(*field, 72).combined, 2, 2) == 227);
    std::array<std::uint8_t, 49> previous {};
    for (int fuzziness = 0; fuzziness <= 255; ++fuzziness) {
        const auto mask = build(*field, fuzziness).combined;
        for (int y = 0; y < 7; ++y) for (int x = 0; x < 7; ++x) {
            const auto index = std::size_t(y * 7 + x);
            CHECK(at(mask, x, y) >= previous[index]); previous[index] = at(mask, x, y);
        }
    }
    // Transparent/invalid neighbors cannot create a contour or dilute a
    // constant patch, and even maximum fuzziness cannot cross alpha classes.
    const ColorSelectionField classes {{7, 1}, {}, {0, 65535, 32 * 255, 32 * 255, 32 * 255, 65535, 0}};
    for (int fuzziness = 0; fuzziness <= 255; ++fuzziness) {
        const auto mask = build(classes, fuzziness).combined;
        CHECK(at(mask, 0) == 255 && at(mask, 6) == 255);
        CHECK(at(mask, 1) == 0 && at(mask, 5) == 0);
        CHECK(at(mask, 2) == (fuzziness < 32 ? 0 : 255));
        CHECK(at(mask, 2) == at(mask, 3) && at(mask, 3) == at(mask, 4));
    }
    const ColorSelectionField extremes {{5, 1}, {}, {0, 0, 65025, 65025, 0}};
    CHECK(build(extremes, 999).combined->equivalent(*build(extremes, 255).combined));
}

void transformedRasterAndTransparentFiltering()
{
    Document document({{6, 3}});
    auto surface = std::make_shared<ObservedSurface>(Extent2u {2, 1}, [](int x, int) {
        return x == 0 ? Rgba8 {255, 0, 0, 255} : Rgba8 {0, 255, 255, 0};
    });
    const auto id = add(document, Layer::raster("Filtered source", surface));
    CHECK(document.setLayerTransform(id, {.m00 = 2, .m02 = 1, .m12 = 1}));
    const auto first = sample(document, id, {2.5, 1.5});
    CHECK(first->sampled == Rgba8({255, 0, 0, 191}));
    const auto second = sample(document, id, {3.5, 1.5});
    CHECK(second->sampled == Rgba8({255, 0, 0, 64}));
    const auto allNontransparent = build(*first, 255);
    CHECK(allNontransparent.combined->bounds() == RectI({1, 1, 3, 1}));
    for (const auto seed : {Vec2d {.5, 1.5}, Vec2d {5.5, 1.5}, Vec2d {2.5, .5},
             Vec2d {-1, 1}, Vec2d {6, 1}, Vec2d {std::numeric_limits<double>::quiet_NaN(), 1}})
        throws<std::invalid_argument>([&] { ColorSelectionReference invalid(document, id, ColorSampleSource::ActiveLayer, seed); });
    const auto transparent = sample(document, id, {4.5, 1.5});
    const auto emptySpace = build(*transparent, 255);
    CHECK(emptySpace.combined->bounds() == RectI({4, 1, 1, 1}));
    CHECK(at(emptySpace.combined, 5, 1) == 0); // Outside layer extent is ineligible, even at maximum fuzziness.
    CHECK(document.setLayerTransform(id, {.m00 = 0, .m01 = -1, .m02 = 3, .m10 = 1, .m11 = 0, .m12 = 1}));
    const auto rotated = build(*sample(document, id, {2.5, 1.5}));
    CHECK(rotated.combined->bounds() == RectI({2, 1, 1, 1}));
    CHECK(document.setLayerTransform(id, {.m00 = -1, .m02 = 3, .m12 = 1}));
    CHECK(build(*sample(document, id, {2.5, 1.5})).combined->bounds() == RectI({2, 1, 1, 1}));
    CHECK(surface->maximumRead <= 4 && surface->writes == 0);
}

void typedSourcesAndMergedHierarchy()
{
    Document document({{8, 4}});
    const auto blue = add(document, Layer::raster("Blue bottom", solid({8, 4}, {0, 0, 255, 255})));
    TextLayer textData; textData.utf8 = "A";
    auto text = Layer::text("Prepared text", textData);
    auto textCache = std::make_shared<LayerRenderCache>();
    textCache->surface = solid({4, 2}, {255, 0, 0, 128});
    textCache->pixelsToLocal = {.m00 = .5, .m02 = 1, .m11 = .5, .m12 = 1};
    textCache->logicalExtent = {2, 1};
    textCache->contentRevision = text.textRevision;
    text.renderCache = textCache;
    text.localToDocument = {.m02 = 2};
    text.opacity = .5F;
    const auto textId = add(document, std::move(text));
    auto shape = Layer::shape("Prepared shape", {});
    auto shapeCache = std::make_shared<LayerRenderCache>(*textCache);
    shapeCache->surface = solid({2, 1}, {0, 255, 0, 255});
    shapeCache->pixelsToLocal = {};
    shapeCache->contentRevision = shape.shapeRevision;
    shape.renderCache = shapeCache;
    shape.localToDocument = {.m02 = 3, .m12 = 1};
    shape.visible = false;
    const auto shapeId = add(document, std::move(shape));
    auto hierarchy = document.tree();
    const auto outer = makeLayerId(), inner = makeLayerId();
    hierarchy.roots = {blue, outer};
    hierarchy.containers = {{.id = outer, .name = "Folder", .kind = ContainerKind::Folder,
        .children = {inner}}, {.id = inner, .name = "Group", .kind = ContainerKind::Group,
        .children = {textId, shapeId}}};
    CHECK(document.replaceStructure(document.tree(), std::move(hierarchy)));
    const auto activeText = sample(document, textId, {3.5, 1.5});
    CHECK(activeText->sampled == Rgba8({255, 0, 0, 128}));
    CHECK(build(*activeText).combined->bounds() == RectI({3, 1, 2, 1}));
    const auto activeShape = sample(document, shapeId, {3.5, 1.5});
    CHECK(activeShape->sampled == Rgba8({0, 255, 0, 255})); // Active ignores the shape's visibility.
    CHECK(build(*activeShape).combined->bounds() == RectI({3, 1, 2, 1}));
    const auto merged = sample(document, {}, {3.5, 1.5}, ColorSampleSource::MergedVisible);
    CHECK(merged->sampled == Rgba8({137, 0, 224, 255}));
    CHECK(build(*merged).combined->bounds() == RectI({3, 1, 2, 1}));
    CHECK(document.setLayerVisibility(shapeId, true));
    CHECK(sample(document, {}, {3.5, 1.5}, ColorSampleSource::MergedVisible)->sampled == Rgba8({0, 255, 0, 255}));
    auto reordered = document.tree();
    reordered.container(inner)->children = {shapeId, textId};
    CHECK(document.replaceStructure(document.tree(), std::move(reordered)));
    CHECK(sample(document, {}, {3.5, 1.5}, ColorSampleSource::MergedVisible)->sampled == Rgba8({137, 224, 0, 255}));
    const std::array hide {ItemVisibilityUpdate {outer, true, false}};
    CHECK(document.setItemVisibilities(hide));
    CHECK(sample(document, {}, {3.5, 1.5}, ColorSampleSource::MergedVisible)->sampled == Rgba8({0, 0, 255, 255}));
    CHECK(sample(document, textId, {3.5, 1.5})->sampled == Rgba8({255, 0, 0, 128}));
    CHECK(document.setLayerVisibility(blue, false));
    const auto transparent = sample(document, {}, {.5, .5}, ColorSampleSource::MergedVisible);
    CHECK(transparent->sampled == Rgba8({0, 0, 0, 0}));
    CHECK(build(*transparent).combined->bounds() == RectI({0, 0, 8, 4}));
}

void coherentReferencesAndInvalidation()
{
    Document document({{8, 2}});
    auto surface = solid({8, 2}, {33, 66, 99, 255});
    const auto id = add(document, Layer::raster("Mutable source", surface));
    ColorSelectionReference pinned(document, id, ColorSampleSource::ActiveLayer, {.5, .5});
    (void)pinned.step(3);
    CHECK(pinned.matches(document));
    CHECK(document.setSelection(SelectionMask::rectangle({8, 2}, {0, 0, 2, 1})));
    CHECK(pinned.matches(document)); // Preview/session selection revisions do not change source content.
    Document other({{8, 2}});
    CHECK(!pinned.matches(other));
    surface->changed();
    CHECK(!pinned.matches(document));
    throws<std::runtime_error>([&] { (void)pinned.step(3); });
    CHECK(!pinned.field());
    const auto complete = sample(document, id, {.5, .5});
    const auto before = build(*complete).combined;
    const auto reads = surface->texelsRead;
    surface->changed();
    CHECK(build(*complete, 0).combined->equivalent(*before));
    CHECK(surface->texelsRead == reads); // Published fields remain immutable coherent snapshots.
    for (const auto mutate : {0, 1, 2, 3}) {
        ColorSelectionReference reference(document, id, ColorSampleSource::ActiveLayer, {1.5, .5});
        if (mutate == 0) CHECK(document.setLayerOpacity(id, .5F));
        if (mutate == 1) CHECK(document.setLayerVisibility(id, false));
        if (mutate == 2) CHECK(document.setLayerTransform(id, {.m02 = 1}));
        if (mutate == 3) CHECK(document.setCanvas({{9, 2}}));
        CHECK(!reference.matches(document));
    }
    Document typed({{2, 2}});
    TextLayer textData; textData.utf8 = "A";
    auto text = Layer::text("Typed invalidation", textData);
    auto cache = std::make_shared<LayerRenderCache>(); cache->surface = solid({2, 2}, {255, 0, 0, 255});
    text.renderCache = cache;
    const auto textId = add(typed, std::move(text));
    ColorSelectionReference textReference(typed, textId, ColorSampleSource::ActiveLayer, {.5, .5});
    textData.utf8 = "B";
    CHECK(typed.setLayerText(textId, textData));
    CHECK(!textReference.matches(typed));
    throws<std::invalid_argument>([&] { ColorSelectionReference missing(typed, textId, ColorSampleSource::ActiveLayer, {.5, .5}); });
}

void combinationsRefineOriginalAndHistory()
{
    Document document({{5, 1}});
    document.markSaved();
    History history;
    const std::array<std::uint8_t, 5> bytes {0, 80, 180, 255, 60};
    const auto original = SelectionMask::fromR8({5, 1}, bytes, 5);
    CHECK(document.setSelection(original));
    const auto content = document.contentState(), revision = document.revision();
    const ColorSelectionField field {{5, 1}, {}, {0, 128, 255, 510, 65535}};
    for (const auto operation : {SelectionOperation::Replace, SelectionOperation::Add,
             SelectionOperation::Subtract, SelectionOperation::Intersect}) {
        const auto first = build(field, 0, original, operation);
        const auto broad = build(field, 2, original, operation);
        const auto repeated = build(field, 0, original, operation);
        CHECK(first.combined->equivalent(*repeated.combined));
        CHECK(original->coverageAtDocumentPixel(1, 0) == 80);
        CHECK(broad.combined != nullptr && broad.incoming != nullptr);
        for (int x = 0; x < 5; ++x) {
            const auto a = bytes[std::size_t(x)], b = at(first.incoming, x);
            const auto expected = operation == SelectionOperation::Replace ? b
                : operation == SelectionOperation::Add ? std::max(a, b)
                : operation == SelectionOperation::Subtract ? std::uint8_t(std::max(0, int(a) - b))
                : std::min(a, b);
            CHECK(at(first.combined, x) == expected);
        }
        CHECK(document.selection() == original); // Pending masks do not replace original visualization.
    }
    const auto completed = build(field, 2, original, SelectionOperation::Subtract);
    CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(completed.combined, "Select by Color")));
    CHECK(history.undoDepth() == 1 && history.undoLabel() == "Select by Color");
    CHECK(document.contentState() == content && document.revision() == revision && !document.isModified());
    CHECK(history.undo(document) && document.selection()->equivalent(*original));
    CHECK(history.redoDepth() == 1);
    CHECK(!history.execute(document, std::make_unique<SetSelectionCommand>(SelectionMask::fromR8({5, 1}, bytes, 5))));
    CHECK(history.undoDepth() == 0 && history.redoDepth() == 1);
    const std::atomic_bool cancel {true};
    const auto cancelled = buildColorSelection(field, 255, original, SelectionOperation::Add, cancel);
    CHECK(!cancelled.incoming && !cancelled.combined);
    CHECK(document.selection()->equivalent(*original) && history.redoDepth() == 1);
    CHECK(history.redo(document) && document.selection()->equivalent(*completed.combined));
    const ColorSelectionField emptyField {{5, 1}, {}, {65535, 65535, 65535, 65535, 65535}};
    const auto empty = build(emptyField, 255);
    CHECK(empty.incoming && empty.combined && empty.combined->bounds().empty());
    CHECK(history.execute(document, std::make_unique<SetSelectionCommand>(empty.combined, "Select by Color")));
    CHECK(document.selection() && document.selection()->bounds().empty());
    CHECK(history.undo(document) && document.selection()->equivalent(*completed.combined));
    CHECK(history.redo(document) && document.selection() && document.selection()->bounds().empty());
    CHECK(!document.isModified());
    for (const auto operation : {SelectionOperation::Replace, SelectionOperation::Add,
             SelectionOperation::Subtract, SelectionOperation::Intersect}) {
        const auto noBase = build(field, 0, {}, operation);
        for (int x = 0; x < 5; ++x) CHECK(at(noBase.combined, x) ==
            (operation == SelectionOperation::Subtract ? 255 - at(noBase.incoming, x) : at(noBase.incoming, x)));
    }
}

void generatedMasksClipExistingEditingPaths()
{
    for (const int editMode : {0, 1, 2}) { // Brush, Erase, Fill.
        const Extent2u extent {7, 3};
        Document document({extent});
        const Rgba8 before = editMode == 1 ? Rgba8 {33, 66, 99, 255} : Rgba8 {};
        auto target = std::make_shared<ContiguousRasterSurface>(extent, before);
        const auto targetId = add(document, Layer::raster("Editable target", target));
        auto source = std::make_shared<ObservedSurface>(extent, [](int x, int) {
            return x == 0 || x == 4 ? Rgba8 {255, 0, 0, 128}
                : x == 1 || x == 5 ? Rgba8 {254, 0, 0, 128} : Rgba8 {0, 255, 0, 0};
        });
        const auto sourceId = add(document, Layer::raster("Color source", source));
        const auto selection = build(*sample(document, sourceId, {.5, .5})).combined;
        CHECK(document.setSelection(selection));
        CHECK(at(selection, 0) == 255 && at(selection, 1) == 127 && at(selection, 2) == 0);
        History history;
        const auto edit = [&] {
            if (editMode == 2) {
                FillOperation fill(document, targetId, {.color = {200, 40, 90, 255}});
                while (fill.state() == FillState::Applying || fill.state() == FillState::Discovering)
                    (void)fill.step(17);
                CHECK(fill.state() == FillState::Ready);
                return fill.commit(history);
            }
            BrushSettings settings;
            settings.sizePixels = 32; settings.hardness = 1; settings.opacity = 1; settings.flow = 1;
            settings.pressureToSize = false; settings.pressureToFlow = false;
            settings.foreground = {200, 40, 90, 255};
            BasicPixelBrushStroke stroke(document, targetId, settings,
                editMode == 1 ? BrushCompositeMode::Erase : BrushCompositeMode::Paint);
            const NormalizedPointerSample pointer {.documentPosition = {3.5, 1.5}, .pressure = 1,
                .pointerType = PointerType::Mouse, .buttons = PointerButtonPrimary};
            CHECK(stroke.begin(pointer));
            return stroke.end(pointer, history);
        };
        CHECK(edit() == RasterEditCommitResult::Committed);
        CHECK(history.undoDepth() == 1 && document.selection() == selection);
        std::array<std::byte, 4> pixel {};
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 7; ++x) {
            target->copyRgba8({x, y, 1, 1}, pixel, 4);
            const auto coverage = at(selection, x, y);
            CHECK(std::to_integer<int>(pixel[3]) == (editMode == 1 ? 255 - coverage : coverage));
            if (coverage == 0) CHECK(pixel == std::array {std::byte(before.red), std::byte(before.green),
                std::byte(before.blue), std::byte(before.alpha)});
        }
        CHECK(source->writes == 0);
        CHECK(history.undo(document));
        const auto revision = target->revision();
        const ColorSelectionField emptyField {extent, {}, std::vector<std::uint16_t>(21, 65535)};
        CHECK(document.setSelection(build(emptyField, 255).combined));
        CHECK(edit() == RasterEditCommitResult::NoChanges);
        CHECK(target->revision() == revision && history.redoDepth() == 1 && history.undoDepth() == 0);
        CHECK(document.selection() && document.selection()->bounds().empty());
    }
}

void boundedWorkAndInvalidFields()
{
    Document document({{129, 131}});
    auto surface = solid({129, 131}, {11, 27, 83, 255});
    const auto id = add(document, Layer::raster("Bounded work", surface));
    ColorSelectionReference reference(document, id, ColorSampleSource::ActiveLayer, {.5, .5});
    CHECK(surface->texelsRead == 1); // Construction samples only the reference.
    CHECK(!reference.step(0) && reference.sampledPixels() == 1);
    const auto field = finish(reference, 97);
    CHECK(field->distances.size() == 129 * 131);
    CHECK(surface->maximumRead == 1 && surface->texelsRead == 129 * 131 + 1);
    const std::atomic_bool cancel {false};
    for (const auto& invalid : {ColorSelectionField {}, ColorSelectionField {{2, 2}, {}, {0, 0}}})
        throws<std::invalid_argument>([&] { (void)buildColorSelection(invalid, 0, {}, SelectionOperation::Replace, cancel); });
    Document tooLarge({{8001, 8000}});
    const auto hugeId = add(tooLarge, Layer::raster("Oversized", solid({8001, 8000}, {0, 0, 0, 255})));
    throws<std::length_error>([&] { ColorSelectionReference oversized(tooLarge, hugeId, ColorSampleSource::ActiveLayer, {.5, .5}); });
    // A bounded failure is preferable to allocating millions of vector edges.
    ColorSelectionField fragmented {{800, 800}, {}, std::vector<std::uint16_t>(800 * 800)};
    for (int y = 0; y < 800; ++y) for (int x = 0; x < 800; ++x)
        fragmented.distances[std::size_t(y * 800 + x)] = (x + y) % 2 ? 65535 : 0;
    throws<std::length_error>([&] { (void)build(fragmented); });
    CHECK(!document.selection());
}
}

int main()
{
    try {
        disconnectedAndReadOnly();
        gradientsAndMonotonicity();
        alphaClassesAndQuantizationCoverage();
        distanceContourAntialiasing();
        transformedRasterAndTransparentFiltering();
        typedSourcesAndMergedHierarchy();
        coherentReferencesAndInvalidation();
        combinationsRefineOriginalAndHistory();
        generatedMasksClipExistingEditingPaths();
        boundedWorkAndInvalidFields();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n'; ++failures;
    }
    if (!failures) std::cout << "Select by Color core tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
