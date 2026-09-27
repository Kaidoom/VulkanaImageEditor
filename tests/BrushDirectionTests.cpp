#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BrushTip.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
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

constexpr double kPi = 3.14159265358979323846;

double circularDistance(double first, double second) noexcept
{
    return std::abs(std::remainder(first - second, 360.0));
}

NormalizedPointerSample sample(double x, double y,
    std::uint64_t timestampMicroseconds = 0)
{
    return {
        .documentPosition = {x, y},
        .timestampMicroseconds = timestampMicroseconds,
        .pressure = 1.0,
        .pointerType = PointerType::Pen,
        .buttons = PointerButtonPrimary,
    };
}

class DabCollector final : public BrushDabSink {
public:
    void emitDab(const BrushDab& dab) override { dabs.push_back(dab); }
    std::vector<BrushDab> dabs;
};

BrushSettings directionSettings(double tipAngle = 0.0)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::InkPen);
    settings.smoothing = BrushSmoothingMode::None;
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    settings.sizePixels = 10.0;
    settings.spacingPercent = 10.0;
    settings.tip.angleDegrees = tipAngle;
    settings.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    return settings;
}

std::vector<BrushDab> collect(const BrushSettings& settings,
    std::span<const NormalizedPointerSample> samples)
{
    CHECK(!samples.empty());
    if (samples.empty()) {
        return {};
    }
    BasicPixelBrushEngine engine;
    DabCollector collector;
    CHECK(engine.beginStroke(settings, samples.front(), collector));
    for (std::size_t index = 1; index + 1 < samples.size(); ++index) {
        CHECK(engine.appendSample(samples[index], collector));
    }
    CHECK(engine.endStroke(samples.back(), collector));
    return collector.dabs;
}

void straightDirectionsAndOffsetsAreResolved()
{
    struct DirectionCase {
        Vec2d end;
        double expected;
    };
    const std::array cases {
        DirectionCase {{20.0, 0.0}, 0.0},
        DirectionCase {{0.0, 20.0}, 90.0},
        DirectionCase {{20.0, 20.0}, 45.0},
        DirectionCase {{-20.0, 0.0}, 180.0},
        DirectionCase {{0.0, -20.0}, -90.0},
        DirectionCase {{-20.0, -20.0}, -135.0},
    };
    for (const auto offset : {0.0, 90.0, -35.0}) {
        for (const auto& test : cases) {
            const std::array path {sample(0.0, 0.0, 0),
                sample(test.end.x, test.end.y, 100000)};
            const auto dabs = collect(directionSettings(offset), path);
            CHECK(dabs.size() > 1);
            CHECK(!dabs.front().strokeDirectionDegrees.has_value());
            CHECK(circularDistance(dabs.front().tipAngleDegrees, offset) < 1.0e-9);
            for (std::size_t index = 1; index < dabs.size(); ++index) {
                CHECK(dabs[index].strokeDirectionDegrees.has_value());
                CHECK(circularDistance(
                    *dabs[index].strokeDirectionDegrees, test.expected) < 1.0e-8);
                CHECK(circularDistance(dabs[index].tipAngleDegrees,
                    test.expected + offset) < 1.0e-8);
            }
        }
    }
}

void stationaryNoiseAndCancelKeepStableSemantics()
{
    const auto settings = directionSettings(-27.0);
    const std::array dot {sample(12.25, 9.75, 0)};
    const auto dotDabs = collect(settings, dot);
    CHECK(dotDabs.size() == 1);
    CHECK(!dotDabs.front().strokeDirectionDegrees.has_value());
    CHECK(circularDistance(dotDabs.front().tipAngleDegrees, -27.0) < 1.0e-9);

    const std::array noisy {sample(0.0, 0.0, 0), sample(5.0, 0.0, 10000),
        sample(5.18, 0.10, 20000), sample(4.84, -0.08, 30000),
        sample(5.12, 0.06, 40000)};
    const auto noisyDabs = collect(settings, noisy);
    CHECK(noisyDabs.size() > 2);
    CHECK(noisyDabs.back().strokeDirectionDegrees.has_value());
    CHECK(circularDistance(*noisyDabs.back().strokeDirectionDegrees, 0.0)
        < 1.0e-9);

    BasicPixelBrushEngine engine;
    DabCollector firstStroke;
    CHECK(engine.beginStroke(settings, sample(0.0, 0.0), firstStroke));
    CHECK(engine.appendSample(sample(20.0, 0.0, 10000), firstStroke));
    engine.cancelStroke();
    CHECK(!engine.active());
    DabCollector afterCancel;
    CHECK(engine.beginStroke(settings, sample(4.0, 4.0), afterCancel));
    CHECK(engine.endStroke(sample(4.0, 4.0), afterCancel));
    CHECK(afterCancel.dabs.size() == 1);
    CHECK(!afterCancel.dabs.front().strokeDirectionDegrees.has_value());
    CHECK(circularDistance(afterCancel.dabs.front().tipAngleDegrees, -27.0)
        < 1.0e-9);
}

void angleWrapUsesTheShortestArc()
{
    const auto vectorAt = [](double degrees) {
        const auto radians = degrees * kPi / 180.0;
        return Vec2d {20.0 * std::cos(radians), 20.0 * std::sin(radians)};
    };
    const auto firstVector = vectorAt(179.0);
    const auto secondVector = vectorAt(-179.0);
    const std::array path {sample(0.0, 0.0, 0),
        sample(firstVector.x, firstVector.y, 10000),
        sample(firstVector.x + secondVector.x,
            firstVector.y + secondVector.y, 20000)};
    const auto dabs = collect(directionSettings(), path);
    CHECK(dabs.size() > 10);
    for (std::size_t index = 1; index < dabs.size(); ++index) {
        CHECK(dabs[index].strokeDirectionDegrees.has_value());
        CHECK(std::abs(dabs[index].tipAngleDegrees) > 170.0);
        if (index > 1) {
            CHECK(circularDistance(dabs[index].tipAngleDegrees,
                dabs[index - 1].tipAngleDegrees) < 20.0);
        }
    }

    const auto nearZeroA = vectorAt(-2.0);
    const auto nearZeroB = vectorAt(2.0);
    const std::array zeroPath {sample(0.0, 0.0, 0),
        sample(nearZeroA.x, nearZeroA.y, 10000),
        sample(nearZeroA.x + nearZeroB.x,
            nearZeroA.y + nearZeroB.y, 20000)};
    const auto zeroDabs = collect(directionSettings(), zeroPath);
    CHECK(std::all_of(zeroDabs.begin() + 1, zeroDabs.end(),
        [](const BrushDab& dab) {
            return std::abs(dab.tipAngleDegrees) < 10.0;
        }));
}

void equivalentDeliveryAndSpeedProduceIdenticalDabs()
{
    const auto settings = directionSettings(13.0);
    const std::array sparse {sample(10.0, 90.0, 0),
        sample(90.0, 20.0, 100000), sample(170.0, 100.0, 200000)};
    std::vector<NormalizedPointerSample> dense;
    for (std::size_t segment = 0; segment + 1 < sparse.size(); ++segment) {
        const auto firstStep = segment == 0 ? 0 : 1;
        for (int step = firstStep; step <= 20; ++step) {
            const auto amount = static_cast<double>(step) / 20.0;
            dense.push_back(sample(
                sparse[segment].documentPosition.x
                    + (sparse[segment + 1].documentPosition.x
                        - sparse[segment].documentPosition.x) * amount,
                sparse[segment].documentPosition.y
                    + (sparse[segment + 1].documentPosition.y
                        - sparse[segment].documentPosition.y) * amount,
                sparse[segment].timestampMicroseconds
                    + static_cast<std::uint64_t>(std::llround(
                        static_cast<double>(sparse[segment + 1].timestampMicroseconds
                            - sparse[segment].timestampMicroseconds) * amount))));
        }
    }
    const auto sparseDabs = collect(settings, sparse);
    const auto denseDabs = collect(settings, dense);
    CHECK(sparseDabs.size() == denseDabs.size());
    for (std::size_t index = 0;
         index < std::min(sparseDabs.size(), denseDabs.size()); ++index) {
        CHECK(std::hypot(sparseDabs[index].documentCenter.x
                - denseDabs[index].documentCenter.x,
            sparseDabs[index].documentCenter.y
                - denseDabs[index].documentCenter.y) < 1.0e-8);
        CHECK(sparseDabs[index].strokeDirectionDegrees.has_value()
            == denseDabs[index].strokeDirectionDegrees.has_value());
        if (sparseDabs[index].strokeDirectionDegrees) {
            CHECK(circularDistance(*sparseDabs[index].strokeDirectionDegrees,
                *denseDabs[index].strokeDirectionDegrees) < 1.0e-8);
        }
        CHECK(circularDistance(sparseDabs[index].tipAngleDegrees,
            denseDabs[index].tipAngleDegrees) < 1.0e-8);
    }

    auto slow = sparse;
    slow[1].timestampMicroseconds = 5'000'000;
    slow[2].timestampMicroseconds = 10'000'000;
    const auto slowDabs = collect(settings, slow);
    CHECK(slowDabs.size() == sparseDabs.size());
    for (std::size_t index = 0;
         index < std::min(slowDabs.size(), sparseDabs.size()); ++index) {
        CHECK(circularDistance(slowDabs[index].tipAngleDegrees,
            sparseDabs[index].tipAngleDegrees) < 1.0e-8);
    }
}

void weightedSmoothingStillUsesResolvedPathGeometry()
{
    auto settings = directionSettings(11.0);
    settings.smoothing = BrushSmoothingMode::Weighted;
    settings.smoothingTimeMilliseconds = 10.0;
    const std::array path {sample(0.0, 0.0, 0),
        sample(80.0, 0.0, 10000), sample(80.0, 80.0, 20000)};
    const auto dabs = collect(settings, path);
    CHECK(dabs.size() > 20);
    for (std::size_t index = 2; index + 1 < dabs.size(); ++index) {
        const auto deltaX = dabs[index].documentCenter.x
            - dabs[index - 1].documentCenter.x;
        const auto deltaY = dabs[index].documentCenter.y
            - dabs[index - 1].documentCenter.y;
        const auto resolvedPathTangent = std::atan2(deltaY, deltaX)
            * 180.0 / kPi;
        CHECK(circularDistance(dabs[index].tipAngleDegrees,
            resolvedPathTangent + 11.0) < 6.0);
    }

    const std::array sparseLine {sample(0.0, 0.0, 0),
        sample(100.0, 0.0, 100000)};
    std::vector<NormalizedPointerSample> denseLine;
    for (int index = 0; index <= 20; ++index) {
        denseLine.push_back(sample(index * 5.0, 0.0,
            static_cast<std::uint64_t>(index) * 5000U));
    }
    for (const auto& delivery : {collect(settings, sparseLine),
             collect(settings, denseLine)}) {
        CHECK(delivery.size() > 2);
        CHECK(std::all_of(delivery.begin() + 1, delivery.end(),
            [](const BrushDab& dab) {
                return dab.strokeDirectionDegrees.has_value()
                    && circularDistance(*dab.strokeDirectionDegrees, 0.0)
                        < 1.0e-9;
            }));
    }
}

void curvesTurnsAndZoomMappingFollowResolvedGeometry()
{
    std::vector<NormalizedPointerSample> circle;
    for (int index = 0; index <= 96; ++index) {
        const auto angle = static_cast<double>(index) / 96.0 * 2.0 * kPi;
        circle.push_back(sample(80.0 + std::cos(angle) * 45.0,
            80.0 + std::sin(angle) * 45.0,
            static_cast<std::uint64_t>(index) * 1000U));
    }
    const auto circleDabs = collect(directionSettings(), circle);
    CHECK(circleDabs.size() > 200);
    for (std::size_t index = 2; index + 1 < circleDabs.size(); ++index) {
        const auto deltaX = circleDabs[index].documentCenter.x
            - circleDabs[index - 1].documentCenter.x;
        const auto deltaY = circleDabs[index].documentCenter.y
            - circleDabs[index - 1].documentCenter.y;
        const auto tangent = std::atan2(deltaY, deltaX) * 180.0 / kPi;
        CHECK(circularDistance(circleDabs[index].tipAngleDegrees, tangent) < 8.0);
    }

    std::vector<NormalizedPointerSample> sCurve;
    for (int index = 0; index <= 80; ++index) {
        const auto amount = static_cast<double>(index) / 80.0;
        sCurve.push_back(sample(20.0 + 160.0 * amount,
            80.0 + 34.0 * std::sin((amount * 2.0 - 0.5) * kPi),
            static_cast<std::uint64_t>(index) * 1000U));
    }
    const auto sDabs = collect(directionSettings(90.0), sCurve);
    CHECK(sDabs.size() > 150);
    for (std::size_t index = 2; index + 1 < sDabs.size(); ++index) {
        const auto deltaX = sDabs[index].documentCenter.x
            - sDabs[index - 1].documentCenter.x;
        const auto deltaY = sDabs[index].documentCenter.y
            - sDabs[index - 1].documentCenter.y;
        const auto tangent = std::atan2(deltaY, deltaX) * 180.0 / kPi;
        CHECK(circularDistance(sDabs[index].tipAngleDegrees,
            tangent + 90.0) < 8.0);
    }

    const std::array tightTurn {sample(10.0, 40.0, 0),
        sample(80.0, 40.0, 10000), sample(80.0, 100.0, 20000),
        sample(25.0, 100.0, 30000)};
    const auto turnDabs = collect(directionSettings(), tightTurn);
    const auto vertical = std::find_if(turnDabs.begin(), turnDabs.end(),
        [](const BrushDab& dab) {
            return dab.documentCenter.y >= 42.0
                && circularDistance(dab.tipAngleDegrees, 90.0) < 1.0;
        });
    CHECK(vertical != turnDabs.end());

    const Extent2d documentExtent {200.0, 140.0};
    const Extent2d viewportExtent {900.0, 600.0};
    std::optional<std::vector<BrushDab>> reference;
    for (const auto zoom : {0.25, 1.0, 4.0}) {
        ViewportState viewport;
        viewport.setZoom(zoom);
        viewport.setPan({21.5, -12.75});
        auto mapped = tightTurn;
        for (auto& pointer : mapped) {
            const auto viewportPoint = viewport.documentToViewport(
                pointer.documentPosition, documentExtent, viewportExtent);
            pointer.documentPosition = viewport.viewportToDocument(
                viewportPoint, documentExtent, viewportExtent);
        }
        auto dabs = collect(directionSettings(-18.0), mapped);
        if (!reference) {
            reference = std::move(dabs);
            continue;
        }
        CHECK(dabs.size() == reference->size());
        for (std::size_t index = 0;
             index < std::min(dabs.size(), reference->size()); ++index) {
            CHECK(circularDistance(dabs[index].tipAngleDegrees,
                (*reference)[index].tipAngleDegrees) < 1.0e-7);
        }
    }
}

void proceduralAndBitmapTipsShareDirectionalForwardAxis()
{
    ProceduralEllipseTip ellipse;
    BrushDab dab;
    dab.documentCenter = {50.0, 50.0};
    dab.diameterPixels = 30.0;
    dab.tipAspectRatio = 0.25;
    dab.tipAngleDegrees = 0.0;
    (void)ellipse.prepareDab(dab, 1.0, 1.0);
    CHECK(ellipse.coverage({62.0, 50.0}) > 0.9);
    CHECK(ellipse.coverage({50.0, 62.0}) < 0.1);
    dab.tipAngleDegrees = 90.0;
    (void)ellipse.prepareDab(dab, 1.0, 1.0);
    CHECK(ellipse.coverage({50.0, 62.0}) > 0.9);

    const std::array<std::uint8_t, 15> directionalMask {
        0, 0, 0, 0, 255,
        0, 0, 0, 0, 255,
        0, 0, 0, 0, 255,
    };
    auto asset = std::make_shared<GrayscaleMaskAsset>(
        "test.tip.forward-x", 1, 5, 3, directionalMask);
    BitmapMaskTip bitmap(asset);
    dab.tipAspectRatio = 1.0;
    dab.tipAngleDegrees = 0.0;
    (void)bitmap.prepareDab(dab, 1.0, 0.25);
    CHECK(bitmap.coverage({61.0, 50.0}) > bitmap.coverage({39.0, 50.0}));
    dab.tipAngleDegrees = 90.0;
    (void)bitmap.prepareDab(dab, 1.0, 0.25);
    CHECK(bitmap.coverage({50.0, 61.0}) > bitmap.coverage({50.0, 39.0}));
    dab.tipAngleDegrees = 180.0;
    (void)bitmap.prepareDab(dab, 1.0, 0.25);
    CHECK(bitmap.coverage({39.0, 50.0}) > bitmap.coverage({61.0, 50.0}));
}

struct PaintedRound {
    std::vector<std::byte> pixels;
    BrushStrokeStats stats;
};

PaintedRound paintRound(BrushTipRotationMode rotationMode)
{
    const Extent2u extent {128, 80};
    Document document(CanvasSpec {.extent = extent});
    auto surface = std::make_shared<ContiguousRasterSurface>(
        extent, Rgba8 {0, 0, 0, 0});
    auto layer = Layer::raster("Round", surface);
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.tip.rotationMode = rotationMode;
    settings.tip.angleDegrees = 37.0;
    settings.sizePixels = 13.0;
    settings.spacingPercent = 9.0;
    settings.foreground = {20, 90, 210, 220};
    History history;
    BasicPixelBrushStroke stroke(document, layerId, settings);
    CHECK(stroke.begin(sample(8.0, 56.0, 0)));
    CHECK(stroke.append(sample(64.0, 18.0, 10000)));
    CHECK(stroke.end(sample(120.0, 62.0, 20000), history)
        == RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);
    const auto stats = stroke.stats();
    std::vector<std::byte> pixels(128U * 80U * 4U);
    surface->copyRgba8({0, 0, 128, 80}, pixels, 128U * 4U);
    const auto painted = pixels;
    CHECK(history.undo(document));
    std::vector<std::byte> undone(pixels.size());
    surface->copyRgba8({0, 0, 128, 80}, undone, 128U * 4U);
    CHECK(std::all_of(undone.begin(), undone.end(),
        [](std::byte value) { return value == std::byte {0}; }));
    CHECK(history.redo(document));
    surface->copyRgba8({0, 0, 128, 80}, pixels, 128U * 4U);
    CHECK(pixels == painted);
    return {std::move(pixels), stats};
}

void rotationAddsNoRasterTransactionPath()
{
    const auto fixed = paintRound(BrushTipRotationMode::Fixed);
    const auto follow = paintRound(BrushTipRotationMode::FollowStrokeDirection);
    CHECK(fixed.pixels == follow.pixels);
    CHECK(fixed.stats.inputSamples == follow.stats.inputSamples);
    CHECK(fixed.stats.emittedDabs == follow.stats.emittedDabs);
    CHECK(fixed.stats.evaluatedPixels == follow.stats.evaluatedPixels);
    CHECK(fixed.stats.changedPixels == follow.stats.changedPixels);
    CHECK(fixed.stats.surfaceWriteBatches == follow.stats.surfaceWriteBatches);
    CHECK(fixed.stats.retainedStrokeTiles == follow.stats.retainedStrokeTiles);
}

} // namespace

int main()
{
    straightDirectionsAndOffsetsAreResolved();
    stationaryNoiseAndCancelKeepStableSemantics();
    angleWrapUsesTheShortestArc();
    equivalentDeliveryAndSpeedProduceIdenticalDabs();
    weightedSmoothingStillUsesResolvedPathGeometry();
    curvesTurnsAndZoomMappingFollowResolvedGeometry();
    proceduralAndBitmapTipsShareDirectionalForwardAxis();
    rotationAddsNoRasterTransactionPath();

    if (failures != 0) {
        std::cerr << failures << " brush-direction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All brush-direction tests passed\n";
    return EXIT_SUCCESS;
}
