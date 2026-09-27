#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BrushLineConstraint.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
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

bool near(double first, double second)
{
    return std::abs(first - second) < 1.0e-9;
}

bool near(Vec2d first, Vec2d second)
{
    return near(first.x, second.x) && near(first.y, second.y);
}

NormalizedPointerSample sample(double x, double y, std::uint64_t timestamp = 0,
    bool shift = true)
{
    return {
        .documentPosition = {x, y},
        .timestampMicroseconds = timestamp,
        .pressure = 1.0,
        .pointerType = PointerType::Pen,
        .buttons = PointerButtonPrimary,
        .modifiers = shift ? PointerModifierShift : PointerModifierNone,
    };
}

BrushSettings settings()
{
    auto result = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    result.sizePixels = 6.0;
    result.hardness = 1.0;
    result.opacity = 0.8;
    result.flow = 0.7;
    result.spacingPercent = 25.0;
    result.pressureToSize = false;
    result.pressureToFlow = false;
    result.smoothing = BrushSmoothingMode::None;
    result.foreground = {219, 47, 91, 255};
    return result;
}

class DabCollector final : public BrushDabSink {
public:
    void emitDab(const BrushDab& dab) override { dabs.push_back(dab); }
    std::vector<BrushDab> dabs;
};

std::vector<BrushDab> collect(const BrushSettings& brushSettings,
    std::span<const NormalizedPointerSample> samples)
{
    CHECK(!samples.empty());
    if (samples.empty()) return {};
    BasicPixelBrushEngine engine;
    DabCollector collector;
    CHECK(engine.beginStroke(brushSettings, samples.front(), collector));
    for (std::size_t index = 1; index + 1 < samples.size(); ++index) {
        CHECK(engine.appendSample(samples[index], collector));
    }
    CHECK(engine.endStroke(samples.back(), collector));
    CHECK(!engine.active());
    CHECK(!engine.constrainedPosition());
    return collector.dabs;
}

void axesStayLockedAndSignedProjectionAllowsRetracing()
{
    struct AxisCase { Vec2d first, axis; };
    const auto diagonal = std::sqrt(0.5);
    const std::array cases {
        AxisCase {{8, 1}, {1, 0}}, AxisCase {{1, 8}, {0, 1}},
        AxisCase {{8, 7}, {diagonal, diagonal}},
        AxisCase {{-8, 7}, {-diagonal, diagonal}},
        AxisCase {{-8, -1}, {-1, 0}}, AxisCase {{1, -8}, {0, -1}},
        AxisCase {{-8, -7}, {-diagonal, -diagonal}},
        AxisCase {{8, -7}, {diagonal, -diagonal}},
    };
    const Vec2d origin {20.25, 40.75};
    for (const auto& test : cases) {
        BrushLineConstraint constraint;
        constraint.reset(origin, true);
        const auto first = constraint.resolve(origin + test.first, true);
        const auto expectedFirst = origin + test.axis
            * (test.first.x * test.axis.x + test.first.y * test.axis.y);
        CHECK(near(first, expectedFirst));
        const Vec2d perpendicular {-test.axis.y, test.axis.x};
        for (const auto along : {20.0, 45.0, 10.0, 0.0, -15.0}) {
            for (const auto wobble : {-90.0, 0.0, 120.0}) {
                const auto actual = constraint.resolve(
                    origin + test.axis * along + perpendicular * wobble, true);
                CHECK(near(actual, origin + test.axis * along));
                const auto delta = actual - origin;
                CHECK(near(delta.x * perpendicular.x + delta.y * perpendicular.y, 0.0));
                CHECK(constraint.constrainedPosition().has_value());
                CHECK(near(*constraint.constrainedPosition(), actual));
            }
        }
    }
}

void noisyBeginningsAndModifierTransitionsUseTheResolvedAnchor()
{
    BrushLineConstraint constraint;
    const Vec2d origin {10, 20};
    constraint.reset(origin, true);
    CHECK(near(constraint.resolve({10, 20}, true), origin));
    CHECK(near(constraint.resolve({9.2, 21.1}, true), origin));
    CHECK(near(constraint.resolve({10, 21.99}, true), origin));
    CHECK(near(constraint.resolve({12, 20}, true), Vec2d {12, 20}));
    CHECK(near(constraint.resolve({30, 37}, true), Vec2d {30, 20}));

    CHECK(near(constraint.resolve({31, 37}, false), Vec2d {31, 37}));
    CHECK(!constraint.constrainedPosition());
    CHECK(near(constraint.resolve({32, 50}, true), Vec2d {31, 50}));
    CHECK(near(constraint.resolve({80, 65}, true), Vec2d {31, 65}));
    CHECK(near(constraint.resolve({80, 65}, false), Vec2d {80, 65}));
    CHECK(near(constraint.resolve({86, 70}, true), Vec2d {85.5, 70.5}));

    constraint.reset();
    CHECK(!constraint.constrainedPosition());
    for (const auto point : {Vec2d {3.25, -17}, Vec2d {-12, 90.5}, Vec2d {0, 0}}) {
        CHECK(constraint.resolve(point, false) == point);
    }
}

void engineRetainsDynamicsAndUsesOneResolvedCursorPath()
{
    auto brushSettings = settings();
    brushSettings.pressureToSize = true;
    brushSettings.pressureToFlow = true;
    std::array constrained {
        sample(20, 20, 0), sample(30, 22, 10000),
        sample(50, 80, 30000), sample(70, -30, 50000),
    };
    for (std::size_t index = 0; index < constrained.size(); ++index) {
        auto& point = constrained[index];
        point.pressure = 0.2 + static_cast<double>(index) * 0.2;
        point.tiltX = -0.6 + static_cast<double>(index) * 0.3;
        point.tiltY = 0.5 - static_cast<double>(index) * 0.2;
        point.rotationDegrees = static_cast<double>(index) * 30.0;
        point.barrelRotationDegrees = -static_cast<double>(index) * 20.0;
    }
    auto reference = constrained;
    for (auto& point : reference) {
        point.documentPosition.y = 20;
        point.modifiers = PointerModifierNone;
    }
    const auto actual = collect(brushSettings, constrained);
    const auto expected = collect(brushSettings, reference);
    CHECK(actual.size() == expected.size());
    for (std::size_t index = 0; index < actual.size() && index < expected.size(); ++index) {
        const auto& dab = actual[index];
        const auto& wanted = expected[index];
        CHECK(near(dab.documentCenter, wanted.documentCenter));
        CHECK(near(dab.sourceSample.pressure, wanted.sourceSample.pressure));
        CHECK(near(dab.sourceSample.tiltX, wanted.sourceSample.tiltX));
        CHECK(near(dab.sourceSample.tiltY, wanted.sourceSample.tiltY));
        CHECK(near(dab.sourceSample.rotationDegrees, wanted.sourceSample.rotationDegrees));
        CHECK(near(dab.sourceSample.barrelRotationDegrees, wanted.sourceSample.barrelRotationDegrees));
        CHECK(dab.sourceSample.pointerType == PointerType::Pen);
        CHECK(near(dab.diameterPixels, wanted.diameterPixels));
        CHECK(near(dab.flow, wanted.flow));
    }

    BasicPixelBrushEngine engine;
    DabCollector collector;
    CHECK(!engine.constrainedPosition());
    CHECK(!engine.appendSample(constrained.front(), collector));
    CHECK(!engine.endStroke(constrained.front(), collector));
    CHECK(engine.beginStroke(settings(), sample(20, 20), collector));
    CHECK(near(*engine.constrainedPosition(), Vec2d {20, 20}));
    CHECK(engine.appendSample(sample(35, 22, 10000), collector));
    CHECK(near(*engine.constrainedPosition(), Vec2d {35, 20}));
    CHECK(near(collector.dabs.back().documentCenter, *engine.constrainedPosition()));
    CHECK(engine.appendSample(sample(40, 30, 20000, false), collector));
    CHECK(!engine.constrainedPosition());
    CHECK(engine.appendSample(sample(41, 45, 30000), collector));
    CHECK(near(*engine.constrainedPosition(), Vec2d {40, 45}));
    engine.cancelStroke();
    CHECK(!engine.active());
    CHECK(!engine.constrainedPosition());
    CHECK(engine.beginStroke(settings(), sample(4, 8, 0, false), collector));
    CHECK(!engine.constrainedPosition());
    CHECK(engine.endStroke(sample(10, 11, 10000, false), collector));
    CHECK(near(collector.dabs.back().documentCenter, Vec2d {10, 11}));
}

void smoothingAndEventRatesKeepDabsOnTheConstrainedLine()
{
    auto weighted = settings();
    weighted.smoothing = BrushSmoothingMode::Weighted;
    const std::array path {sample(10, 10), sample(60, 58, 1000),
        sample(80, 20, 8000), sample(90, 70, 16000)};
    const auto smoothed = collect(weighted, path);
    CHECK(smoothed.size() > 10);
    for (const auto& dab : smoothed) CHECK(near(dab.documentCenter.x, dab.documentCenter.y));
    CHECK(near(smoothed.back().documentCenter, Vec2d {80, 80}));

    const std::array sparse {sample(10, 10), sample(90, 26, 80000)};
    std::vector<NormalizedPointerSample> dense;
    for (int index = 0; index <= 16; ++index) {
        dense.push_back(sample(10 + index * 5, 10 + index,
            static_cast<std::uint64_t>(index) * 5000U));
    }
    const auto sparseDabs = collect(settings(), sparse);
    const auto denseDabs = collect(settings(), dense);
    CHECK(sparseDabs.size() == denseDabs.size());
    for (std::size_t index = 0; index < sparseDabs.size() && index < denseDabs.size(); ++index) {
        CHECK(near(sparseDabs[index].documentCenter, denseDabs[index].documentCenter));
        CHECK(near(sparseDabs[index].documentCenter.y, 10.0));
    }
}

void releasingShiftAtAStationaryMouseUpNeverAddsAHook()
{
    for (const auto smoothing : {BrushSmoothingMode::None, BrushSmoothingMode::Weighted}) {
        for (const auto endTime : {10000U, 20000U}) {
            auto brushSettings = settings();
            brushSettings.smoothing = smoothing;
            BasicPixelBrushEngine engine;
            DabCollector collector;
            CHECK(engine.beginStroke(brushSettings, sample(20, 20), collector));
            CHECK(engine.appendSample(sample(50, 23, 10000), collector));
            CHECK(engine.endStroke(sample(50, 23, endTime, false), collector));
            for (const auto& dab : collector.dabs) CHECK(near(dab.documentCenter.y, 20.0));
            CHECK(near(collector.dabs.back().documentCenter, Vec2d {50, 20}));
        }
    }
    const std::array stationary {sample(12, 15), sample(12, 15, 10000, false)};
    const auto dabs = collect(settings(), stationary);
    CHECK(dabs.size() == 1);
    CHECK(near(dabs.front().documentCenter, Vec2d {12, 15}));
}

std::vector<std::byte> pixels(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(static_cast<std::size_t>(extent.width)
        * static_cast<std::size_t>(extent.height) * 4U);
    surface.copyRgba8({0, 0, static_cast<std::int32_t>(extent.width),
                          static_cast<std::int32_t>(extent.height)},
        result, static_cast<std::size_t>(extent.width) * 4U);
    return result;
}

struct Fixture {
    Document document {CanvasSpec {.extent = {128, 128}}};
    std::shared_ptr<ContiguousRasterSurface> surface =
        std::make_shared<ContiguousRasterSurface>(Extent2u {64, 64}, Rgba8 {25, 80, 140, 230});
    LayerId layerId {0};
    AffineTransform transform {.m00 = 0, .m01 = -1.25, .m02 = 110,
        .m10 = 0.75, .m11 = 0, .m12 = 20};
    SelectionState selection = SelectionMask::rectangle({128, 128}, {40, 35, 42, 18});

    Fixture()
    {
        auto layer = Layer::raster("Constrained brush target", surface);
        layerId = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
        CHECK(document.setLayerTransform(layerId, transform));
        CHECK(document.setSelection(selection));
    }
};

void paintAndEraseShareSelectionTransformCancelAndOneHistoryCommit()
{
    for (const auto mode : {BrushCompositeMode::Paint, BrushCompositeMode::Erase}) {
        Fixture fixture;
        Fixture reference;
        History history;
        History referenceHistory;
        const auto original = pixels(*fixture.surface);
        const std::array path {sample(32, 42), sample(48, 43, 10000),
            sample(70, 68, 20000), sample(95, 74, 30000)};
        BasicPixelBrushStroke stroke(fixture.document, fixture.layerId, settings(), mode);
        BasicPixelBrushStroke referenceStroke(reference.document, reference.layerId, settings(), mode);
        CHECK(stroke.valid());
        CHECK(stroke.begin(path.front()));
        auto straight = path.front();
        straight.modifiers = PointerModifierNone;
        CHECK(referenceStroke.begin(straight));
        for (std::size_t index = 1; index + 1 < path.size(); ++index) {
            CHECK(stroke.append(path[index]));
            CHECK(near(*stroke.constrainedPosition(), Vec2d {path[index].documentPosition.x, 42}));
            straight = path[index];
            straight.documentPosition.y = 42;
            straight.modifiers = PointerModifierNone;
            CHECK(referenceStroke.append(straight));
        }
        CHECK(history.undoDepth() == 0);
        CHECK(stroke.end(path.back(), history) == RasterEditCommitResult::Committed);
        straight = path.back();
        straight.documentPosition.y = 42;
        straight.modifiers = PointerModifierNone;
        CHECK(referenceStroke.end(straight, referenceHistory) == RasterEditCommitResult::Committed);
        const auto committed = pixels(*fixture.surface);
        CHECK(committed != original);
        CHECK(committed == pixels(*reference.surface));
        CHECK(history.undoDepth() == 1);
        CHECK(history.undoLabel() == (mode == BrushCompositeMode::Paint
            ? std::string_view("Brush stroke") : std::string_view("Eraser stroke")));
        CHECK(!stroke.constrainedPosition());

        std::size_t changed = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto offset = static_cast<std::size_t>(y * 64 + x) * 4U;
                bool differs = false;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    differs = differs || committed[offset + channel] != original[offset + channel];
                }
                if (!differs) continue;
                ++changed;
                const auto center = fixture.transform.map({x + 0.5, y + 0.5});
                CHECK(fixture.selection->coverageAtDocumentPixel(
                    static_cast<std::int32_t>(std::floor(center.x)),
                    static_cast<std::int32_t>(std::floor(center.y))) != 0);
                CHECK(std::abs(center.y - 42.0) < 5.0);
                if (mode == BrushCompositeMode::Erase) {
                    CHECK(committed[offset] == original[offset]);
                    CHECK(committed[offset + 1] == original[offset + 1]);
                    CHECK(committed[offset + 2] == original[offset + 2]);
                    CHECK(committed[offset + 3] < original[offset + 3]);
                }
            }
        }
        CHECK(changed > 0);

        BasicPixelBrushStroke canceled(fixture.document, fixture.layerId, settings(), mode);
        CHECK(canceled.begin(sample(45, 49)));
        CHECK(canceled.append(sample(75, 50, 10000)));
        CHECK(pixels(*fixture.surface) != committed);
        canceled.cancel();
        CHECK(!canceled.active());
        CHECK(!canceled.constrainedPosition());
        CHECK(pixels(*fixture.surface) == committed);
        CHECK(history.undoDepth() == 1);
        CHECK(history.undo(fixture.document));
        CHECK(pixels(*fixture.surface) == original);
        CHECK(history.redo(fixture.document));
        CHECK(pixels(*fixture.surface) == committed);
        CHECK(fixture.document.selection() == fixture.selection);
        CHECK(fixture.document.layer(fixture.layerId)->localToDocument == fixture.transform);
    }
}

} // namespace

int main()
{
    axesStayLockedAndSignedProjectionAllowsRetracing();
    noisyBeginningsAndModifierTransitionsUseTheResolvedAnchor();
    engineRetainsDynamicsAndUsesOneResolvedCursorPath();
    smoothingAndEventRatesKeepDabsOnTheConstrainedLine();
    releasingShiftAtAStationaryMouseUpNeverAddsAHook();
    paintAndEraseShareSelectionTransformCancelAndOneHistoryCommit();
    if (failures != 0) {
        std::cerr << failures << " brush constraint assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All brush constraint tests passed\n";
    return EXIT_SUCCESS;
}
