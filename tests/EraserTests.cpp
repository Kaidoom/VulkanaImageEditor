#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
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

NormalizedPointerSample sample(double x, double y, double pressure,
    std::uint64_t timestampMicroseconds)
{
    return {
        .documentPosition = {x, y},
        .timestampMicroseconds = timestampMicroseconds,
        .pressure = pressure,
        .tiltX = 0.0,
        .tiltY = 0.0,
        .rotationDegrees = 0.0,
        .barrelRotationDegrees = 0.0,
        .pointerType = PointerType::Pen,
        .buttons = PointerButtonPrimary,
        .modifiers = PointerModifierNone,
    };
}

struct Fixture {
    Document document;
    std::shared_ptr<ContiguousRasterSurface> surface;
    LayerId layerId {0};

    Fixture(Extent2u canvasExtent, Rgba8 fill,
        Extent2u surfaceExtent = {})
        : document(CanvasSpec {.extent = canvasExtent})
        , surface(std::make_shared<ContiguousRasterSurface>(
              surfaceExtent.empty() ? canvasExtent : surfaceExtent, fill))
    {
        auto layer = Layer::raster("Eraser target", surface);
        layerId = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
};

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

Rgba8 pixelAt(const RasterSurface& surface, std::int32_t x, std::int32_t y)
{
    std::array<std::byte, 4> bytes {};
    surface.copyRgba8({x, y, 1, 1}, bytes, 4);
    return {
        std::to_integer<std::uint8_t>(bytes[0]),
        std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]),
        std::to_integer<std::uint8_t>(bytes[3]),
    };
}

struct StrokeResult {
    RasterEditCommitResult commit {RasterEditCommitResult::TargetUnavailable};
    BrushStrokeStats stats;
    DirtySet lastDirty;
};

StrokeResult erase(Fixture& fixture, const BrushSettings& settings,
    std::span<const NormalizedPointerSample> samples, History& history)
{
    BasicPixelBrushStroke stroke(fixture.document, fixture.layerId, settings,
        BrushCompositeMode::Erase);
    CHECK(stroke.valid());
    CHECK(!samples.empty());
    CHECK(stroke.compositeMode() == BrushCompositeMode::Erase);
    CHECK(stroke.begin(samples.front()));
    for (std::size_t index = 1; index + 1 < samples.size(); ++index) {
        CHECK(stroke.append(samples[index]));
    }
    const auto commit = stroke.end(samples.back(), history);
    return {commit, stroke.stats(), stroke.lastDirtySet()};
}

BrushSettings hardEraser(double size = 16.0)
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = size;
    settings.hardness = 1.0;
    settings.opacity = 1.0;
    settings.flow = 1.0;
    settings.spacingPercent = 10.0;
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    return settings;
}

void destinationOutIsAlphaOnlyAndHistoryIsExact()
{
    Fixture fixture({32, 32}, {23, 91, 177, 128});
    auto settings = hardEraser(12.0);
    settings.opacity = 0.5;
    // Foreground, including its alpha, has no meaning for destination-out.
    settings.foreground = {255, 0, 200, 0};
    History history;
    const auto original = pixels(*fixture.surface);
    const auto revisionBefore = fixture.surface->revision();
    const std::array samples {sample(16.0, 16.0, 1.0, 1000)};
    const auto result = erase(fixture, settings, samples, history);

    CHECK(result.commit == RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);
    CHECK(history.undoLabel() == std::string_view("Eraser stroke"));
    CHECK(history.memoryUsed() > 0);
    const auto center = pixelAt(*fixture.surface, 16, 16);
    CHECK(center.red == 23);
    CHECK(center.green == 91);
    CHECK(center.blue == 177);
    CHECK(center.alpha == 64);
    CHECK(pixelAt(*fixture.surface, 0, 0)
        == (Rgba8 {23, 91, 177, 128}));
    CHECK(fixture.surface->revision() == revisionBefore + 1);
    const auto dirty = fixture.surface->dirtySince(revisionBefore);
    CHECK(!dirty.fullRefresh);
    CHECK(!dirty.regions.empty());
    CHECK(result.stats.surfaceWriteBatches == 1);
    CHECK(result.stats.uploadedRegionBytes < 32U * 32U * 4U);

    const auto erased = pixels(*fixture.surface);
    const auto retainedMemory = history.memoryUsed();
    CHECK(history.undo(fixture.document));
    CHECK(pixels(*fixture.surface) == original);
    CHECK(history.memoryUsed() == retainedMemory);
    CHECK(history.redo(fixture.document));
    CHECK(pixels(*fixture.surface) == erased);
    CHECK(history.memoryUsed() == retainedMemory);

    Fixture fullyErased({24, 24}, {213, 17, 149, 255});
    History fullHistory;
    const std::array fullDot {sample(12.0, 12.0, 1.0, 0)};
    CHECK(erase(fullyErased, hardEraser(10.0), fullDot, fullHistory).commit
        == RasterEditCommitResult::Committed);
    CHECK(pixelAt(*fullyErased.surface, 12, 12)
        == (Rgba8 {213, 17, 149, 0}));
}

void opacityCapsOneStrokeAndSeparateStrokesCompound()
{
    Fixture fixture({48, 48}, {70, 110, 150, 255});
    auto settings = hardEraser(80.0);
    settings.opacity = 0.5;
    History history;
    const std::array repeated {
        sample(8.0, 24.0, 1.0, 0),
        sample(40.0, 24.0, 1.0, 10000),
        sample(8.0, 24.0, 1.0, 20000),
        sample(40.0, 24.0, 1.0, 30000),
    };
    CHECK(erase(fixture, settings, repeated, history).commit
        == RasterEditCommitResult::Committed);
    CHECK(pixelAt(*fixture.surface, 24, 24).alpha == 128);
    CHECK(history.undoDepth() == 1);

    CHECK(erase(fixture, settings, repeated, history).commit
        == RasterEditCommitResult::Committed);
    CHECK(pixelAt(*fixture.surface, 24, 24).alpha == 64);
    CHECK(history.undoDepth() == 2);
}

void transparentAndZeroStrengthEditsAreTrueNoOps()
{
    const std::array dot {sample(16.0, 16.0, 1.0, 0)};
    {
        Fixture fixture({32, 32}, {91, 33, 201, 0});
        auto settings = hardEraser();
        settings.foreground = {0, 0, 0, 0};
        History history;
        const auto revision = fixture.surface->revision();
        const auto result = erase(fixture, settings, dot, history);
        CHECK(result.commit == RasterEditCommitResult::NoChanges);
        CHECK(fixture.surface->revision() == revision);
        CHECK(!history.canUndo());
        CHECK(history.memoryUsed() == 0);
        CHECK(result.stats.surfaceWriteBatches == 0);
        CHECK(result.stats.uploadedRegionBytes == 0);
        CHECK(pixelAt(*fixture.surface, 16, 16)
            == (Rgba8 {91, 33, 201, 0}));
    }
    for (const bool zeroOpacity : {false, true}) {
        Fixture fixture({32, 32}, {50, 60, 70, 255});
        auto settings = hardEraser();
        settings.opacity = zeroOpacity ? 0.0 : 1.0;
        settings.flow = zeroOpacity ? 1.0 : 0.0;
        History history;
        const auto revision = fixture.surface->revision();
        const auto result = erase(fixture, settings, dot, history);
        CHECK(result.commit == RasterEditCommitResult::NoChanges);
        CHECK(fixture.surface->revision() == revision);
        CHECK(!history.canUndo());
    }
}

std::vector<std::byte> renderEventRateStroke(bool dense, Rgba8 foreground)
{
    Fixture fixture({256, 96}, {44, 79, 130, 220});
    auto settings = hardEraser(30.0);
    settings.flow = 0.55;
    settings.opacity = 0.8;
    settings.spacingPercent = 8.0;
    settings.pressureToFlow = true;
    settings.foreground = foreground;
    std::vector<NormalizedPointerSample> samples;
    if (dense) {
        for (int index = 0; index <= 40; ++index) {
            const auto amount = static_cast<double>(index) / 40.0;
            samples.push_back(sample(12.0 + 232.0 * amount, 48.0,
                0.2 + 0.8 * amount,
                static_cast<std::uint64_t>(std::llround(200000.0 * amount))));
        }
    } else {
        samples = {sample(12.0, 48.0, 0.2, 0),
            sample(244.0, 48.0, 1.0, 200000)};
    }
    History history;
    CHECK(erase(fixture, settings, samples, history).commit
        == RasterEditCommitResult::Committed);
    return pixels(*fixture.surface);
}

void resamplingPressureAndColorRemainSharedAndDeterministic()
{
    const auto sparse = renderEventRateStroke(false, {255, 0, 0, 0});
    const auto dense = renderEventRateStroke(true, {0, 255, 255, 255});
    CHECK(sparse == dense);

    // A built-in bitmap tip and document-anchored grain take the same path.
    Fixture first({160, 96}, {120, 90, 40, 255});
    Fixture replay({160, 96}, {120, 90, 40, 255});
    auto dryInk = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    dryInk.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    dryInk.foreground = {1, 2, 3, 0};
    const std::array path {sample(12, 76, 0.2, 0),
        sample(78, 16, 0.65, 40000), sample(148, 70, 1.0, 80000)};
    History firstHistory;
    History replayHistory;
    CHECK(erase(first, dryInk, path, firstHistory).commit
        == RasterEditCommitResult::Committed);
    CHECK(erase(replay, dryInk, path, replayHistory).commit
        == RasterEditCommitResult::Committed);
    const auto firstPixels = pixels(*first.surface);
    CHECK(firstPixels == pixels(*replay.surface));
    CHECK(std::any_of(firstPixels.begin(), firstPixels.end(),
        [](std::byte value) { return std::to_integer<std::uint8_t>(value) < 40; }));
}

void canvasTransformCancellationAndMissingTargetsStaySafe()
{
    {
        Fixture fixture({16, 16}, {10, 20, 30, 255}, {32, 32});
        auto settings = hardEraser(10.0);
        History history;
        const std::array path {sample(14, 14, 1.0, 0),
            sample(18, 18, 1.0, 10000)};
        CHECK(erase(fixture, settings, path, history).commit
            == RasterEditCommitResult::Committed);
        CHECK(pixelAt(*fixture.surface, 14, 14).alpha < 255);
        CHECK(pixelAt(*fixture.surface, 20, 20).alpha == 255);
    }
    {
        Fixture fixture({32, 32}, {31, 61, 91, 255}, {16, 16});
        auto* layer = fixture.document.layer(fixture.layerId);
        CHECK(layer != nullptr);
        if (layer) {
            layer->localToDocument.m02 = 8.0;
            layer->localToDocument.m12 = 5.0;
        }
        History history;
        const std::array dot {sample(10, 7, 1.0, 0)};
        CHECK(erase(fixture, hardEraser(6.0), dot, history).commit
            == RasterEditCommitResult::Committed);
        CHECK(pixelAt(*fixture.surface, 2, 2).alpha == 0);
        CHECK(pixelAt(*fixture.surface, 10, 7).alpha == 255);
    }
    {
        Fixture fixture({64, 64}, {101, 77, 53, 211});
        const auto original = pixels(*fixture.surface);
        History history;
        BasicPixelBrushStroke stroke(fixture.document, fixture.layerId,
            hardEraser(18.0), BrushCompositeMode::Erase);
        CHECK(stroke.begin(sample(12, 12, 1.0, 0)));
        CHECK(stroke.append(sample(52, 52, 1.0, 50000)));
        stroke.cancel();
        CHECK(pixels(*fixture.surface) == original);
        CHECK(!history.canUndo());
    }
    {
        Fixture fixture({64, 64}, {15, 25, 35, 255});
        const auto original = pixels(*fixture.surface);
        BasicPixelBrushStroke stroke(fixture.document, fixture.layerId,
            hardEraser(18.0), BrushCompositeMode::Erase);
        CHECK(stroke.begin(sample(12, 12, 1.0, 0)));
        const auto removed = fixture.document.takeLayer(fixture.layerId);
        CHECK(removed.has_value());
        CHECK(!stroke.append(sample(52, 52, 1.0, 50000)));
        CHECK(pixels(*fixture.surface) == original);
    }
}

QImage imageFromSurface(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    const auto raw = pixels(surface);
    return QImage(reinterpret_cast<const uchar*>(raw.data()),
        static_cast<int>(extent.width), static_cast<int>(extent.height),
        static_cast<qsizetype>(extent.width) * 4,
        QImage::Format_RGBA8888).copy();
}

void writeContactSheet(const QString& path)
{
    struct ReviewCase {
        QString name;
        BrushSettings settings;
        std::vector<NormalizedPointerSample> samples;
    };
    auto hard = hardEraser(44.0);
    auto soft = proceduralBrushPreset(ProceduralBrushPreset::SoftRound);
    soft.sizePixels = 64.0;
    soft.opacity = 0.75;
    soft.pressureToSize = false;
    soft.pressureToFlow = false;
    auto dry = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    dry.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    const std::array cases {
        ReviewCase {QStringLiteral("Hard Round"), hard,
            {sample(20, 64, 1.0, 0), sample(236, 64, 1.0, 100000)}},
        ReviewCase {QStringLiteral("Soft 75%"), soft,
            {sample(20, 64, 1.0, 0), sample(236, 64, 1.0, 100000)}},
        ReviewCase {QStringLiteral("Dry Ink / Direction"), dry,
            {sample(18, 98, 0.2, 0), sample(92, 28, 0.6, 40000),
                sample(168, 100, 0.8, 80000), sample(238, 32, 1.0, 120000)}},
    };

    constexpr int cellWidth = 300;
    constexpr int cellHeight = 190;
    QImage sheet(cellWidth * static_cast<int>(cases.size()), cellHeight,
        QImage::Format_RGBA8888);
    sheet.fill(QColor(QStringLiteral("#11141B")));
    QPainter painter(&sheet);
    painter.setPen(QColor(QStringLiteral("#E9ECF5")));
    for (std::size_t index = 0; index < cases.size(); ++index) {
        Fixture fixture({256, 128}, {77, 119, 224, 255});
        History history;
        CHECK(erase(fixture, cases[index].settings,
            cases[index].samples, history).commit
            == RasterEditCommitResult::Committed);
        const QRect cell(static_cast<int>(index) * cellWidth, 0,
            cellWidth, cellHeight);
        painter.drawText(cell.adjusted(16, 10, -16, -150),
            Qt::AlignLeft | Qt::AlignVCenter, cases[index].name);
        const QRect imageRect = cell.adjusted(16, 46, -16, -16);
        constexpr int checker = 12;
        for (int y = imageRect.top(); y < imageRect.bottom(); y += checker) {
            for (int x = imageRect.left(); x < imageRect.right(); x += checker) {
                painter.fillRect(QRect(x, y, checker, checker),
                    ((x / checker + y / checker) % 2) == 0
                        ? QColor(QStringLiteral("#343946"))
                        : QColor(QStringLiteral("#4A5060")));
            }
        }
        painter.drawImage(imageRect, imageFromSurface(*fixture.surface));
    }
    painter.end();
    QDir().mkpath(QFileInfo(path).absolutePath());
    CHECK(sheet.save(path, "PNG"));
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    destinationOutIsAlphaOnlyAndHistoryIsExact();
    opacityCapsOneStrokeAndSeparateStrokesCompound();
    transparentAndZeroStrengthEditsAreTrueNoOps();
    resamplingPressureAndColorRemainSharedAndDeterministic();
    canvasTransformCancellationAndMissingTargetsStaySafe();

    for (int index = 1; index + 1 < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--contact-sheet")) {
            writeContactSheet(QString::fromLocal8Bit(argv[index + 1]));
        }
    }
    if (failures != 0) {
        std::cerr << failures << " Eraser V1 assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Eraser V1 tests passed\n";
    return EXIT_SUCCESS;
}
