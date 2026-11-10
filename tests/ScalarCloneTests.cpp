#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/ScalarRasterSurface.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace imageeditor::core;
int failures{};
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

LayerId add(Document& document, const std::shared_ptr<RasterSurface>& surface, AffineTransform transform = {})
{
    auto layer = Layer::raster("Channel", surface); layer.localToDocument = transform;
    const auto id = layer.id; CHECK(document.insertLayer(document.layers().size(), std::move(layer))); return id;
}
std::shared_ptr<ScalarRasterSurface> scalar(Extent2u extent, std::uint8_t value)
{ return std::make_shared<ScalarRasterSurface>(extent, value); }
std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto e = surface.extent();
    std::vector<std::byte> result(std::size_t(e.width) * e.height * surface.nativeBytesPerPixel());
    surface.copyNative({0, 0, int(e.width), int(e.height)}, result, std::size_t(e.width) * surface.nativeBytesPerPixel());
    return result;
}
unsigned pixel(const RasterSurface& surface, int x = 32, int y = 32)
{
    std::byte value; surface.copyNative({x, y, 1, 1}, {&value, 1}, 1); return std::to_integer<unsigned>(value);
}
void write(ScalarRasterSurface& surface, RectI region, std::uint8_t value)
{
    std::vector<std::byte> values(std::size_t(region.width) * std::size_t(region.height), std::byte(value));
    const RasterPatch patch {region, values, std::size_t(region.width)};
    (void)surface.replaceNativeBatch({&patch, 1});
}
CloneReference capture(const Document& doc, LayerId target, LayerId source, bool context = true)
{
    std::string diagnostic;
    auto result = CloneReference::capture(doc, target, source, CloneSampleSource::SourceLayer,
        diagnostic, CloneReference::defaultSnapshotLimit, context);
    if (!result) throw std::runtime_error(diagnostic);
    CHECK(result->numericValues()); return *result;
}
BrushSettings brush(double opacity = 1)
{
    auto value = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    value.sizePixels = 12; value.opacity = opacity; value.flow = 1; value.hardness = 1;
    value.pressureToFlow = value.pressureToSize = false;
    return value;
}
NormalizedPointerSample point(Vec2d p = {32.5, 32.5})
{ return {.documentPosition = p, .pressure = 1, .buttons = PointerButtonPrimary}; }

void samplingAndFrozenStorage()
{
    const Extent2u extent {256, 256};
    Document doc(CanvasSpec {extent});
    std::vector<std::byte> values(256 * 256);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = std::byte(i % 256);
    auto surface = std::make_shared<ScalarRasterSurface>(extent, values, 256);
    const auto id = add(doc, surface);
    auto reference = capture(doc, id, id);
    CHECK(reference.sourceCount() == 1);
    CHECK(reference.snapshotBytes() < values.size() * 2);
    for (int x = 0; x < 256; ++x) {
        const auto sample = reference.rawTexel(x, 0);
        CHECK(std::abs(double(sample[0]) * 255 - x) < .00003);
        CHECK(sample[0] == sample[1] && sample[1] == sample[2] && sample[3] == 1);
    }
    CHECK(reference.rawTexel(0, 0) == (PremultipliedColor {0, 0, 0, 1}));
    CHECK(reference.rawTexel(-1, 0) == PremultipliedColor{});
    write(*surface, {0, 0, 256, 256}, 255);
    CHECK(reference.rawTexel(0, 0)[0] == 0);
    CHECK(std::abs(reference.sample({128, .5})[0] * 255 - 127.5F) < .00003F);
    CHECK(reference.sampleDestination({.5, .5}) == (PremultipliedColor {0, 0, 0, 1}));

    Document transformed(CanvasSpec {{64, 64}});
    const std::array samples {std::byte{0}, std::byte{255}};
    auto source = std::make_shared<ScalarRasterSurface>(Extent2u {2, 1}, samples, 2);
    const AffineTransform transform {2, 0, 4, 0, 2, 2};
    const auto transformedId = add(transformed, source, transform);
    auto mapped = capture(transformed, transformedId, transformedId);
    CHECK(mapped.sample(transform.map({1, .5})) == (PremultipliedColor {.5F, .5F, .5F, 1}));
    CHECK(!mapped.validSample(transform.map({-.1, .5})));
    CHECK(mapped.sample(transform.map({-.1, .5})) == PremultipliedColor{});
}

void rejectionAndMemoryBudget()
{
    Document doc(CanvasSpec {{256, 256}});
    std::vector<std::byte> pattern(256 * 256);
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = std::byte(i % 256);
    auto source = std::make_shared<ScalarRasterSurface>(Extent2u {256, 256}, pattern, 256);
    const auto sourceId = add(doc, source), targetId = add(doc, scalar({256, 256}, 0));
    const auto colourId = add(doc, std::make_shared<ContiguousRasterSurface>(Extent2u {256, 256}, Rgba8{1, 2, 3, 255}));
    std::string error;
    for (auto mode : {CloneSampleSource::AllVisible, CloneSampleSource::CurrentAndBelow})
        CHECK(!CloneReference::capture(doc, targetId, sourceId, mode, error));
    CHECK(!CloneReference::capture(doc, colourId, sourceId, CloneSampleSource::SourceLayer, error));
    CHECK(!CloneReference::capture(doc, targetId, colourId, CloneSampleSource::SourceLayer, error));
    CHECK(!CloneReference::capture(doc, targetId, sourceId, CloneSampleSource::SourceLayer, error, source->memoryCost() - 1));
    CHECK(error.find("memory") != std::string::npos);
    CHECK(!CloneReference::capture(doc, targetId, sourceId, CloneSampleSource::SourceLayer,
        error, CloneReference::defaultSnapshotLimit, true, [] { return true; }));
    CHECK(error.find("cancelled") != std::string::npos);
    CloneStroke mismatch(doc, colourId, brush(), {}, capture(doc, targetId, sourceId), {});
    CHECK(!mismatch.begin(point())); CHECK(!mismatch.diagnostic().empty());
}

void stampAndHealingCoverage()
{
    Document doc(CanvasSpec {{64, 64}});
    auto original = Layer::raster("Packed texture", std::make_shared<ContiguousRasterSurface>(
        Extent2u {64, 64}, Rgba8 {2, 0, 95, 0}));
    const auto groupId = original.id; CHECK(doc.insertLayer(0, std::move(original)));
    History history;
    CHECK(history.execute(doc, convertToChannelGroup(doc, {{groupId}, groupId})));
    history.clear();
    const auto roles = doc.tree().container(groupId)->channelRoles;
    const auto target = std::get<RasterLayer>(doc.layer(roles[1])->payload).surface;
    const auto blue = std::get<RasterLayer>(doc.layer(roles[2])->payload).surface;
    const auto alpha = std::get<RasterLayer>(doc.layer(roles[3])->payload).surface;
    const auto blueBefore = bytes(*blue), alphaBefore = bytes(*alpha), before = bytes(*target);
    CHECK(doc.setSelection(SelectionMask::filled({64, 64}, 100)));
    const auto selection = doc.selection();
    // 2 * .7 * (100/255) rounds once to 1. Rounding the intermediate
    // candidate first would incorrectly leave this pixel at zero.
    CloneStroke stamp(doc, roles[1], brush(.7), {}, capture(doc, roles[1], roles[0]), {});
    CHECK(stamp.begin(point())); CHECK(stamp.end(point(), history) == RasterEditCommitResult::Committed);
    CHECK(pixel(*target) == 1); const auto stamped = bytes(*target);
    CHECK(bytes(*blue) == blueBefore && bytes(*alpha) == alphaBefore);
    CHECK(doc.selection() == selection);
    CHECK(history.undo(doc)); CHECK(bytes(*target) == before);
    CHECK(history.redo(doc)); CHECK(bytes(*target) == stamped);
    CHECK(history.undo(doc));
    CloneSettings settings; settings.mode = CloneMode::Heal; settings.adaptation = 0;
    CloneStroke heal(doc, roles[1], brush(.7), settings, capture(doc, roles[1], roles[0]), {});
    CHECK(heal.begin(point())); CHECK(heal.end(point(), history) == RasterEditCommitResult::Committed);
    CHECK(bytes(*target) == stamped);
    CHECK(history.undo(doc));
    const auto redo = history.redoDepth();
    CloneStroke cancelled(doc, roles[1], brush(), {}, capture(doc, roles[1], roles[0]), {});
    CHECK(cancelled.begin(point())); cancelled.cancel();
    CHECK(bytes(*target) == before); CHECK(history.redoDepth() == redo);
    CloneStroke interrupted(doc, roles[1], brush(), settings, capture(doc, roles[1], roles[0]), {});
    CHECK(interrupted.begin(point()));
    CHECK(interrupted.end(point(), history, [] { return true; }) == RasterEditCommitResult::TargetUnavailable);
    CHECK(bytes(*target) == before); CHECK(history.redoDepth() == redo);
}

void scalarHealingContext()
{
    Document doc(CanvasSpec {{64, 64}});
    auto source = scalar({64, 64}, 100), target = scalar({64, 64}, 0);
    const auto sourceId = add(doc, source), targetId = add(doc, target);
    write(*target, {30, 30, 5, 5}, 220);
    auto reference = capture(doc, targetId, sourceId);
    CHECK(reference.sampleDestination({10.5, 10.5}) == (PremultipliedColor {0, 0, 0, 1}));
    CHECK(std::abs(reference.sampleDestination({32.5, 32.5})[0] * 255 - 220) < .00003F);
    CloneSettings settings; settings.mode = CloneMode::Heal; settings.adaptation = 1;
    History history; const auto before = bytes(*target);
    CloneStroke heal(doc, targetId, brush(), settings, reference, {});
    CHECK(heal.begin(point())); CHECK(heal.end(point(), history) == RasterEditCommitResult::Committed);
    CHECK(heal.healingResult().diagnostics.contextSamples > 0);
    CHECK(pixel(*target) <= 1); // Valid black surroundings supply the tone context.
    CHECK(pixel(*target, 10, 10) == 0);
    CHECK(history.undo(doc)); CHECK(bytes(*target) == before);

    // Identity cloning of native numeric bytes is an exact no-op.
    CloneStroke identity(doc, targetId, brush(), {}, capture(doc, targetId, targetId), {});
    CHECK(identity.begin(point())); CHECK(identity.end(point(), history) == RasterEditCommitResult::NoChanges);
    CHECK(bytes(*target) == before); CHECK(history.redoDepth() == 1);
}
}

int main()
{
    try { samplingAndFrozenStorage(); rejectionAndMemoryBudget(); stampAndHealingCoverage(); scalarHealingContext(); }
    catch (const std::exception& e) { CHECK(false); std::cerr << e.what() << '\n'; }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
