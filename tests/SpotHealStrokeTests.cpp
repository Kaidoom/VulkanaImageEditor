#include "imageeditor/core/SpotHealStroke.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include <iostream>
#include <cmath>

using namespace imageeditor::core;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto e = surface.extent(); std::vector<std::byte> pixels(std::size_t(e.width) * e.height * 4);
    surface.copyRgba8({0, 0, int(e.width), int(e.height)}, pixels, std::size_t(e.width) * 4); return pixels;
}
std::shared_ptr<ContiguousRasterSurface> fixture(Rgba8 defect, bool alpha = false)
{
    std::vector<std::byte> pixels(64 * 64 * 4);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const auto color = x >= 29 && x <= 34 && y >= 29 && y <= 34 ? defect : Rgba8{110, 145, 170, 255};
        const auto i = std::size_t(y * 64 + x) * 4;
        pixels[i] = std::byte(color.red); pixels[i + 1] = std::byte(color.green); pixels[i + 2] = std::byte(color.blue);
        pixels[i + 3] = std::byte(alpha ? 77 + (x + y) % 170 : color.alpha);
    }
    return std::make_shared<ContiguousRasterSurface>(Extent2u{64, 64}, std::move(pixels));
}
LayerId add(Document& doc, std::shared_ptr<RasterSurface> surface, AffineTransform transform = {})
{
    auto layer = Layer::raster("Repair fixture", std::move(surface)); layer.localToDocument = transform;
    const auto id = layer.id; CHECK(doc.insertLayer(doc.layers().size(), std::move(layer))); return id;
}
CloneReference reference(Document& doc, LayerId target, CloneSampleSource source = CloneSampleSource::SourceLayer)
{
    std::string message;
    auto pinned = CloneReference::capture(doc, target, target, source, message,
        CloneReference::defaultSnapshotLimit, false, {}, {}, true);
    if (!pinned) throw std::runtime_error(message);
    return std::move(*pinned);
}
BrushSettings settings()
{
    auto brush = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    brush.sizePixels = 18; brush.hardness = .25; brush.opacity = .65;
    brush.flow = 1; brush.pressureToSize = brush.pressureToFlow = false;
    brush.foreground = {1, 2, 3, 4}; return brush;
}
NormalizedPointerSample sample(Vec2d p, std::uint64_t time = 0)
{
    return {.documentPosition = p, .timestampMicroseconds = time, .pressure = 1,
        .pointerType = PointerType::Mouse, .buttons = PointerButtonPrimary};
}
SpotHealSolved solve(SpotHealStroke& stroke, Vec2d p)
{
    CHECK(stroke.begin(sample(p))); const auto work = stroke.finishInput(sample(p, 1000)); CHECK(work);
    SpotHealOptions options; options.captureDiagnostics = true;
    auto result = SpotHealStroke::solve(work, options);
    if (!result.repair) std::cerr << result.repair.diagnostics.message << '\n';
    CHECK(result.repair); return result;
}

void masksAndHistory()
{
    std::vector<PremultipliedColor> firstCandidate;
    for (const auto defect : {Rgba8{255, 0, 220, 255}, Rgba8{0, 245, 10, 255}}) {
        Document doc({{64, 64}}); auto source = fixture(defect, true); const auto id = add(doc, source); History history;
        CHECK(doc.setSelection(SelectionMask::rectangle({64, 64}, {0, 0, 32, 64}, 128)));
        const auto before = bytes(*source);
        const auto selection = doc.selection(); const auto revision = source->revision();
        SpotHealStroke stroke(doc, id, settings(), {}, reference(doc, id));
        auto result = solve(stroke, {32, 32});
        CHECK(source->revision() == revision && bytes(*source) == before);
        CHECK(stroke.previewMask()->coverageAtDocumentPixel(34, 32) == 255);
        CHECK(result.repair.diagnostics.unknownPixels > 200);
        if (firstCandidate.empty()) firstCandidate = result.repair.pixels;
        else CHECK(firstCandidate == result.repair.pixels); // Hidden damage RGB is not evidence.
        CHECK(stroke.publish(result, history) == RasterEditCommitResult::Committed);
        const auto after = bytes(*source); CHECK(after != before && doc.selection() == selection);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const auto i = std::size_t(y * 64 + x) * 4; CHECK(after[i + 3] == before[i + 3]);
            if (x >= 32 || !stroke.previewMask()->coverageAtDocumentPixel(x, y))
                CHECK(std::equal(before.begin() + std::ptrdiff_t(i), before.begin() + std::ptrdiff_t(i + 4), after.begin() + std::ptrdiff_t(i)));
        }
        CHECK(history.undo(doc)); CHECK(bytes(*source) == before);
        CHECK(history.redo(doc)); CHECK(bytes(*source) == after); CHECK(history.undo(doc));
        const auto redo = history.redoDepth();
        SpotHealStroke cancelled(doc, id, settings(), {}, reference(doc, id));
        CHECK(cancelled.begin(sample({32, 32}))); cancelled.cancel();
        CHECK(bytes(*source) == before && history.redoDepth() == redo);
        CHECK(doc.setSelection(SelectionMask::filled({64, 64}, 0)));
        SpotHealStroke empty(doc, id, settings(), {}, reference(doc, id));
        CHECK(empty.begin(sample({32, 32}))); CHECK(!empty.finishInput(sample({32, 32})));
        CHECK(empty.commitNoop(history) == RasterEditCommitResult::NoChanges); CHECK(history.redoDepth() == redo);
    }
}

void transformedAndStale()
{
    const std::array transforms{AffineTransform{}, AffineTransform{1.2, -.3, 22, .2, .8, 12},
        AffineTransform{-1, .2, 90, .15, 1.3, 10}};
    for (const auto mapping : transforms) {
        Document doc({{128, 128}}); auto source = fixture({250, 10, 25, 255}); const auto id = add(doc, source, mapping); History history;
        auto brush = settings(); brush.sizePixels = 26; brush.opacity = 1;
        auto adjustments = std::make_shared<AdjustmentStack>(); adjustments->items[0].enabled = true;
        adjustments->items[0].parameters = ExposureParameters{1.5}; CHECK(doc.setLayerAdjustments(id, adjustments));
        const auto before = bytes(*source);
        SpotHealStroke stroke(doc, id, brush, {}, reference(doc, id)); auto result = solve(stroke, mapping.map({32, 32}));
        CHECK(stroke.publish(result, history) == RasterEditCommitResult::Committed);
        CHECK(doc.layer(id)->localToDocument == mapping && doc.layer(id)->adjustments == adjustments);
        CHECK(history.undo(doc)); CHECK(bytes(*source) == before);
        SpotHealStroke stale(doc, id, brush, {}, reference(doc, id)); auto pending = solve(stale, mapping.map({32, 32}));
        auto other = fixture({2, 3, 4, 255}); add(doc, other);
        CHECK(!stale.targetMatches()); CHECK(stale.publish(pending, history) == RasterEditCommitResult::TargetUnavailable);
        CHECK(bytes(*source) == before);
        SpotHealStroke stopped(doc, id, brush, {}, reference(doc, id)); CHECK(stopped.begin(sample(mapping.map({32, 32}))));
        auto input = stopped.finishInput(sample(mapping.map({32, 32})));
        SpotHealOptions cancelled; cancelled.cancelled = [] { return true; };
        CHECK(SpotHealStroke::solve(input, cancelled).repair.status == SpotHealStatus::Cancelled);
        stopped.cancel(); CHECK(bytes(*source) == before);
    }
}

void renderedRetouch()
{
    for (const bool opaque : {true, false}) {
        Document doc({{64, 64}}); auto lower = fixture({250, 10, 20, std::uint8_t(opaque ? 255 : 120)});
        add(doc, lower); auto target = std::make_shared<ContiguousRasterSurface>(Extent2u{64, 64}); const auto id = add(doc, target);
        CloneSettings clone; clone.source = CloneSampleSource::CurrentAndBelow;
        auto brush = settings(); brush.opacity = 1;
        SpotHealStroke stroke(doc, id, brush, clone, reference(doc, id, clone.source));
        CHECK(stroke.begin(sample({32, 32}))); auto input = stroke.finishInput(sample({32, 32})); CHECK(input);
        auto result = SpotHealStroke::solve(input); History history;
        if (opaque) { CHECK(result.repair); CHECK(stroke.publish(result, history) == RasterEditCommitResult::Committed);
            CHECK(history.undoDepth() == 1); CHECK(history.undo(doc)); }
        else { CHECK(result.repair.status == SpotHealStatus::InvalidInput); CHECK(history.undoDepth() == 0); }
        const auto empty = bytes(*target);
        CHECK(std::all_of(empty.begin(), empty.end(), [](std::byte b) { return b == std::byte{}; }));
        CHECK(doc.setLayerOpacity(id, .5F)); CHECK(!spotHealTargetDiagnostic(doc, id, clone.source).empty());
    }
}

void cropAndNoop()
{
    Document doc({{64, 64}}); auto source = fixture({110, 145, 170, 255}); const auto id = add(doc, source); History history;
    LayerCrop crop{8, 8, 48, 48}; crop.corners = {16, 5, 12, 6}; CHECK(doc.setLayerCrop(id, crop));
    auto pinned = reference(doc, id);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        if (layerCropCoverage(crop, {x + .5, y + .5}, {1, 0}, {0, 1}) == 0) CHECK(pinned.rawTexel(x, y)[3] == 0);
    auto brush = settings(); brush.sizePixels = 16; brush.opacity = 1;
    const auto original = bytes(*source); const auto revision = source->revision();
    SpotHealStroke stroke(doc, id, brush, {}, std::move(pinned));
    CHECK(stroke.begin(sample({17, 17}))); CHECK(stroke.append(sample({28, 18}, 10000)));
    CHECK(stroke.previewMask()->coverageAtDocumentPixel(13, 13) != 0); // Not clipped to chamfer writes.
    auto input = stroke.finishInput(sample({28, 18}, 20000)); CHECK(input);
    auto result = SpotHealStroke::solve(input); CHECK(result.repair);
    CHECK(stroke.stats().surfaceWriteBatches == 0 && source->revision() == revision);
    CHECK(stroke.publish(result, history) == RasterEditCommitResult::NoChanges);
    CHECK(bytes(*source) == original && source->revision() == revision && history.undoDepth() == 0);
    CHECK(doc.layer(id)->crop == crop);

    auto marked = fixture({250, 20, 200, 255}); const auto markedId = add(doc, marked);
    CHECK(doc.setLayerCrop(markedId, crop)); const auto before = bytes(*marked);
    SpotHealStroke edited(doc, markedId, brush, {}, reference(doc, markedId)); auto changed = solve(edited, {32, 32});
    CHECK(edited.publish(changed, history) == RasterEditCommitResult::Committed);
    const auto after = bytes(*marked);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        if (layerCropCoverage(crop, {x + .5, y + .5}, {1, 0}, {0, 1}) == 0 || !edited.previewMask()->coverageAtDocumentPixel(x, y)) {
            const auto i = std::size_t(y * 64 + x) * 4;
            CHECK(std::equal(before.begin() + std::ptrdiff_t(i), before.begin() + std::ptrdiff_t(i + 4), after.begin() + std::ptrdiff_t(i)));
        }
}

void filteredTypedReferenceAndCorruption()
{
    std::vector<PremultipliedColor> candidate;
    for (const auto defect : {Rgba8{255, 0, 170, 255}, Rgba8{0, 255, 10, 255}}) {
        Document doc({{128, 128}});
        const auto background = add(doc, std::make_shared<ContiguousRasterSurface>(Extent2u{128, 128}, Rgba8{70, 95, 115, 255}));
        auto raster = fixture(defect); TextLayer text; text.utf8 = "Editable fixture";
        auto layer = Layer::text("Typed reference", text); const auto textId = layer.id;
        auto cache = std::make_shared<LayerRenderCache>(); cache->surface = raster; cache->logicalExtent = {64, 64};
        layer.renderCache = cache; layer.localToDocument = {-1, .2, 95, .15, 1.1, 16}; layer.opacity = .65F;
        layer.blendMode = BlendMode::Screen; layer.crop = LayerCrop{2, 2, 60, 60}; layer.crop->corners = {3, 8, 5, 4};
        auto filters = std::make_shared<SpatialFilterStack>(); filters->items[0].enabled = true;
        filters->items[0].parameters = GaussianBlurParameters{3, 3}; layer.filters = filters;
        CHECK(doc.insertLayer(doc.layers().size(), layer));
        const auto hidden = add(doc, std::make_shared<ContiguousRasterSurface>(Extent2u{128, 128}, Rgba8{255, 20, 0, 255}));
        auto empty = std::make_shared<ContiguousRasterSurface>(Extent2u{128, 128}); const auto target = add(doc, empty);
        const auto folder = makeLayerId(); LayerTree tree{{background, textId, folder, target},
            {{folder, "Hidden context", ContainerKind::Folder, ColorLabel::None, {hidden}, false}}};
        CHECK(doc.replaceStructure(doc.tree(), std::move(tree)));
        CloneSettings clone; clone.source = CloneSampleSource::CurrentAndBelow;
        auto referenceInput = reference(doc, target, clone.source);
        CHECK(referenceInput.sourceCount() == 3);
        auto ready = referenceInput.prepared();
        CHECK(ready.readSupport().x >= 4 && ready.readSupport().y >= 4);
        PinnedDocumentSampler expected(doc, target, ColorSampleSource::MergedVisible);
        for (const Vec2d p : {Vec2d{20.5, 20.5}, layer.localToDocument.map({32.5, 32.5}), Vec2d{110.5, 90.5}})
            CHECK(encodeColor(ready.sample(p)) == encodeColor(expected.sampleLinear(p)));
        bool budgetRejected = false;
        try { (void)referenceInput.prepared({}, referenceInput.snapshotBytes() + 8); }
        catch (const std::exception&) { budgetRejected = true; }
        CHECK(budgetRejected);
        auto brush = settings(); brush.sizePixels = 24; brush.opacity = 1;
        SpotHealStroke stroke(doc, target, brush, clone, std::move(referenceInput));
        auto repaired = solve(stroke, layer.localToDocument.map({32, 32}));
        std::size_t markedCount = 0;
        for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x)
            markedCount += stroke.previewMask()->coverageAtDocumentPixel(x, y) != 0;
        CHECK(repaired.repair.diagnostics.unknownPixels > markedCount); // Filtering support is unknown, never donors.
        if (candidate.empty()) candidate = repaired.repair.pixels; else CHECK(candidate == repaired.repair.pixels);
        // A disposable viewport cache replacement does not stale the captured
        // authoritative source. Typing or changing geometry would change doc revision.
        auto replacement = std::make_shared<LayerRenderCache>(*cache);
        replacement->surface = std::make_shared<ContiguousRasterSurface>(Extent2u{64, 64}, bytes(*raster));
        doc.layer(textId)->renderCache = replacement;
        CHECK(stroke.targetMatches()); History history;
        CHECK(stroke.publish(repaired, history) == RasterEditCommitResult::Committed);
        CHECK(std::get<TextLayer>(doc.layer(textId)->payload).utf8 == text.utf8);
        CHECK(bytes(*raster) == bytes(*replacement->surface));
        CHECK(history.undo(doc));
        const auto emptyPixels = bytes(*empty); CHECK(std::all_of(emptyPixels.begin(), emptyPixels.end(), [](std::byte b) { return b == std::byte{}; }));
    }
}

double referenceDecode(std::byte channel)
{
    const double encoded = double(std::to_integer<unsigned>(channel)) / 255.0;
    return encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
}
std::byte referenceEncode(double linear)
{
    linear = std::clamp(linear, 0.0, 1.0);
    const double encoded = linear <= .0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - .055;
    return std::byte(std::clamp(std::lround(encoded * 255.0), 0L, 255L));
}

void finalQuantization()
{
    std::size_t oldDoubleRoundingDifferences = 0;
    for (const bool direct : {true, false}) for (const bool flipped : {false, true})
    for (const double opacity : {.23, .65, 1.0}) {
        constexpr Extent2u canvas{56, 52};
        Document doc({canvas});
        if (!direct) add(doc, std::make_shared<ContiguousRasterSurface>(canvas, Rgba8{90, 130, 170, 255}));
        std::vector<std::byte> before(64 * 64 * 4);
        for (std::size_t i = 0; i < 64 * 64; ++i) {
            before[i * 4] = std::byte((i * 37) % 256);
            before[i * 4 + 1] = std::byte((i * 71 + 5) % 256);
            before[i * 4 + 2] = std::byte((i * 13 + 201) % 256);
            before[i * 4 + 3] = std::byte((i * 29) % 256);
        }
        const AffineTransform mapping = flipped ? AffineTransform{-1, 0, 60, 0, 1, -3} : AffineTransform{};
        auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u{64, 64}, before);
        const auto id = add(doc, surface, mapping);
        LayerCrop crop{4, 5, 53, 54}; crop.corners = {3, 4, 5, 6}; CHECK(doc.setLayerCrop(id, crop));
        std::vector<std::uint8_t> selection(std::size_t(canvas.width) * canvas.height);
        for (std::size_t i = 0; i < selection.size(); ++i) selection[i] = std::uint8_t((i * 113) % 256);
        CHECK(doc.setSelection(SelectionMask::fromR8(canvas, selection, canvas.width)));
        const auto pinnedSelection = doc.selection();
        auto brush = settings(); brush.sizePixels = 256; brush.hardness = 1; brush.opacity = opacity; brush.flow = .37;
        CloneSettings clone; clone.source = direct ? CloneSampleSource::SourceLayer : CloneSampleSource::CurrentAndBelow;
        SpotHealStroke stroke(doc, id, brush, clone, reference(doc, id, clone.source));
        CHECK(stroke.begin(sample({28, 26})));
        SpotHealSolved solved; solved.input = stroke.finishInput(sample({28, 26}, 1000)); CHECK(solved.input);
        // Isolate publication from the solver: its floating-point contract is
        // exercised across all R8 coverages/colors/alphas, not a constant repair.
        solved.gridBounds = direct ? RectI{0, 0, 64, 64} : RectI{0, 0, 56, 52};
        solved.repair.status = SpotHealStatus::Complete;
        solved.repair.pixels.resize(std::size_t(solved.gridBounds.width * solved.gridBounds.height));
        for (std::size_t i = 0; i < solved.repair.pixels.size(); ++i) {
            const float a = .11F + float((i * 17) % 87) / 100.0F;
            solved.repair.pixels[i] = {a * (float((i * 19) % 997) / 997.0F),
                a * (float((i * 41 + 31) % 991) / 991.0F), a * (float((i * 29 + 83) % 983) / 983.0F), a};
        }
        auto expected = before;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            const auto p = mapping.map({x + .5, y + .5});
            if (!cropAllowsTexel(crop, x, y) || p.x < 0 || p.y < 0 || p.x >= canvas.width || p.y >= canvas.height) continue;
            const unsigned mask = pinnedSelection->coverageAtDocumentPixel(int(std::floor(p.x)), int(std::floor(p.y)));
            const auto offset = std::size_t(y * 64 + x) * 4;
            if (!mask || (direct && before[offset + 3] == std::byte{})) continue;
            const auto index = direct ? std::size_t(y * 64 + x) : std::size_t(int(p.y) * 56 + int(p.x));
            const auto candidate = solved.repair.pixels[index];
            const double weight = opacity * (double(float(.37)) * double(mask) / 255.0);
            const double oldAlpha = double(std::to_integer<unsigned>(before[offset + 3])) / 255.0;
            const double alpha = direct ? oldAlpha : weight + oldAlpha * (1 - weight);
            for (std::size_t c = 0; c < 3; ++c) {
                const double from = referenceDecode(before[offset + c]);
                // Rendered retouch exposes unassociated float RGB at alpha=1;
                // direct repair retains the solver's premultiplied float RGBA.
                const double to = direct ? double(candidate[c]) / double(candidate[3]) : double(candidate[c] / candidate[3]);
                const double value = direct ? from + (to - from) * weight
                    : (to * weight + from * oldAlpha * (1 - weight)) / alpha;
                expected[offset + c] = referenceEncode(value);
                if (direct) {
                    const double firstEncoded = referenceDecode(referenceEncode(from + (to - from) * opacity * double(float(.37))));
                    const auto twice = referenceEncode(from + (firstEncoded - from) * double(mask) / 255.0);
                    oldDoubleRoundingDifferences += twice != expected[offset + c];
                }
            }
            if (!direct) expected[offset + 3] = std::byte(std::clamp(std::lround(alpha * 255.0), 0L, 255L));
        }
        CHECK(stroke.stats().emittedDabs == 1 && bytes(*surface) == before);
        History history;
        CHECK(stroke.publish(solved, history) == RasterEditCommitResult::Committed);
        CHECK(bytes(*surface) == expected); // Exact bytes; no +/-1 allowance.
        CHECK(doc.selection() == pinnedSelection && history.undoDepth() == 1);
        CHECK(history.undo(doc) && bytes(*surface) == before);
        CHECK(history.redo(doc) && bytes(*surface) == expected);
        CHECK(history.undo(doc) && bytes(*surface) == before);
        SpotHealStroke cancelled(doc, id, brush, clone, reference(doc, id, clone.source));
        CHECK(cancelled.begin(sample({28, 26})));
        solved.input = cancelled.finishInput(sample({28, 26}, 1000)); CHECK(solved.input);
        int checks = 0;
        CHECK(cancelled.publish(solved, history, [&] { return ++checks >= 2; }) == RasterEditCommitResult::TargetUnavailable);
        CHECK(bytes(*surface) == before && history.redoDepth() == 1 && history.undoDepth() == 0);
    }
    CHECK(oldDoubleRoundingDifferences > 100); // This fixture catches the old path.
}

void selectionResolvedAdmission()
{
    for (const bool selected : {false, true}) {
        Document doc({{8, 8}}); auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u{12, 12}, Rgba8{1, 2, 3, 77});
        const auto id = add(doc, surface, AffineTransform{1, 0, -2, 0, 1, -2});
        LayerCrop crop{3, 3, 7, 7}; CHECK(doc.setLayerCrop(id, crop));
        if (selected) CHECK(doc.setSelection(SelectionMask::rectangle({8, 8}, {2, 0, 6, 8}, 128)));
        const auto before = bytes(*surface); auto candidate = before;
        std::fill(candidate.begin(), candidate.end(), std::byte{201});
        RasterEditTransaction edit(doc, id, "Final repair");
        const RasterPatch patch{{0, 0, 12, 12}, candidate, 12 * 4};
        CHECK(!edit.writeSelectionResolvedRgba8Batch(std::span<const RasterPatch>(&patch, 1)).empty());
        const auto after = bytes(*surface);
        for (int y = 0; y < 12; ++y) for (int x = 0; x < 12; ++x) {
            const bool admitted = x >= 2 && y >= 2 && x < 10 && y < 10 && cropAllowsTexel(crop, x, y)
                && (!selected || x >= 4);
            const auto offset = std::size_t(y * 12 + x) * 4;
            CHECK(std::equal(after.begin() + std::ptrdiff_t(offset), after.begin() + std::ptrdiff_t(offset + 4),
                (admitted ? candidate : before).begin() + std::ptrdiff_t(offset)));
        }
        edit.cancel(); CHECK(bytes(*surface) == before);
    }
}
}
int main()
{
    try { masksAndHistory(); transformedAndStale(); renderedRetouch(); cropAndNoop(); filteredTypedReferenceAndCorruption();
        finalQuantization(); selectionResolvedAdmission(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    if (!failures) std::cout << "Spot Heal stroke tests passed\n";
    return failures ? 1 : 0;
}
