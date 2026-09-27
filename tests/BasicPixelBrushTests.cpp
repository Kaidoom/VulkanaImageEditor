#include "imageeditor/core/BasicPixelBrushEngine.hpp"
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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using namespace imageeditor::core;

struct StrokeCase {
    std::string name;
    Extent2u extent;
    Rgba8 background;
    BrushSettings settings;
    std::vector<NormalizedPointerSample> samples;
};

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

std::vector<StrokeCase> goldenCases()
{
    std::vector<StrokeCase> result;

    auto hard = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    hard.sizePixels = 24.0;
    hard.foreground = {39, 86, 230, 255};
    hard.spacingPercent = 12.0;
    result.push_back({"horizontal-hard", {256, 128}, {0, 0, 0, 0}, hard,
        {sample(18.0, 64.0, 1.0, 0), sample(238.0, 64.0, 1.0, 180000)}});

    auto soft = proceduralBrushPreset(ProceduralBrushPreset::SoftRound);
    soft.sizePixels = 42.0;
    soft.opacity = 0.72;
    soft.flow = 0.28;
    soft.foreground = {255, 112, 38, 230};
    soft.spacingPercent = 9.0;
    result.push_back({"vertical-soft-semitransparent", {128, 256}, {22, 42, 84, 128}, soft,
        {sample(64.25, 18.5, 1.0, 0), sample(64.25, 238.5, 1.0, 400000)}});

    auto diagonal = hard;
    diagonal.sizePixels = 17.5;
    diagonal.hardness = 0.78;
    diagonal.opacity = 0.83;
    diagonal.foreground = {35, 225, 167, 255};
    diagonal.spacingPercent = 15.0;
    result.push_back({"diagonal-subpixel", {256, 256}, {0, 0, 0, 0}, diagonal,
        {sample(15.25, 20.75, 1.0, 1000), sample(240.5, 232.125, 1.0, 151000)}});

    auto pressure = proceduralBrushPreset(ProceduralBrushPreset::PressureRound);
    pressure.sizePixels = 52.0;
    pressure.hardness = 0.86;
    pressure.opacity = 0.9;
    pressure.flow = 0.75;
    pressure.foreground = {232, 55, 112, 255};
    pressure.spacingPercent = 8.0;
    result.push_back({"pressure-ramp", {320, 128}, {0, 0, 0, 0}, pressure,
        {sample(18, 64, 0.05, 0), sample(80, 64, 0.25, 30000),
            sample(150, 64, 0.52, 65000), sample(230, 64, 0.78, 100000),
            sample(302, 64, 1.0, 135000)}});

    auto abrupt = pressure;
    abrupt.sizePixels = 44.0;
    abrupt.foreground = {245, 206, 55, 255};
    result.push_back({"abrupt-pressure", {300, 160}, {18, 20, 26, 255}, abrupt,
        {sample(20, 80, 0.12, 0), sample(85, 80, 0.12, 35000),
            sample(86, 80, 1.0, 36000), sample(150, 40, 1.0, 70000),
            sample(151, 40, 0.18, 71000), sample(220, 120, 0.18, 110000),
            sample(280, 80, 0.9, 145000)}});

    auto circle = hard;
    circle.sizePixels = 13.0;
    circle.hardness = 0.62;
    circle.foreground = {132, 93, 247, 255};
    circle.spacingPercent = 11.0;
    std::vector<NormalizedPointerSample> circleSamples;
    constexpr double pi = 3.14159265358979323846;
    for (int index = 0; index <= 64; ++index) {
        const auto angle = static_cast<double>(index) * 2.0 * pi / 64.0;
        circleSamples.push_back(sample(128.0 + std::cos(angle) * 82.0,
            128.0 + std::sin(angle) * 82.0, 1.0,
            static_cast<std::uint64_t>(index) * 5000U));
    }
    result.push_back({"circle-tight-curve", {256, 256}, {0, 0, 0, 0}, circle,
        std::move(circleSamples)});

    auto wave = hard;
    wave.sizePixels = 9.0;
    wave.hardness = 0.95;
    wave.foreground = {67, 203, 246, 255};
    wave.spacingPercent = 18.0;
    std::vector<NormalizedPointerSample> slowWave;
    for (int index = 0; index <= 100; ++index) {
        const auto x = 12.0 + static_cast<double>(index) * 2.96;
        const auto y = 80.0 + std::sin(static_cast<double>(index) * 0.22) * 38.0;
        slowWave.push_back(sample(x, y, 1.0,
            static_cast<std::uint64_t>(index) * 12000U));
    }
    result.push_back({"slow-wave", {320, 160}, {0, 0, 0, 0}, wave,
        std::move(slowWave)});

    auto fast = wave;
    fast.sizePixels = 21.0;
    fast.foreground = {113, 232, 98, 255};
    fast.spacingPercent = 10.0;
    result.push_back({"fast-flick", {320, 160}, {0, 0, 0, 0}, fast,
        {sample(8, 145, 1.0, 0), sample(88, 108, 1.0, 4000),
            sample(170, 70, 1.0, 8000), sample(245, 34, 1.0, 12000),
            sample(330, -4, 1.0, 16000)}});

    auto overlap = soft;
    overlap.sizePixels = 34.0;
    overlap.hardness = 0.7;
    overlap.opacity = 0.45;
    overlap.flow = 0.18;
    overlap.foreground = {255, 70, 70, 255};
    std::vector<NormalizedPointerSample> overlapSamples;
    for (int pass = 0; pass < 5; ++pass) {
        const bool forward = (pass % 2) == 0;
        overlapSamples.push_back(sample(forward ? 30 : 226, 64, 1.0,
            static_cast<std::uint64_t>(pass) * 70000U));
        overlapSamples.push_back(sample(forward ? 226 : 30, 64, 1.0,
            static_cast<std::uint64_t>(pass) * 70000U + 60000U));
    }
    result.push_back({"repeated-overlap-opacity-cap", {256, 128}, {0, 0, 0, 0}, overlap,
        std::move(overlapSamples)});

    auto edge = hard;
    edge.sizePixels = 36.0;
    edge.foreground = {255, 255, 255, 255};
    result.push_back({"canvas-edge-clipping", {192, 128}, {14, 16, 22, 255}, edge,
        {sample(-30, -10, 1.0, 0), sample(40, 22, 1.0, 30000),
            sample(205, 120, 1.0, 90000)}});

    return result;
}

struct RenderedCase {
    QImage image;
    std::vector<std::byte> raw;
    BrushStrokeStats stats;
};

RenderedCase renderCase(const StrokeCase& testCase)
{
    Document document(CanvasSpec {.extent = testCase.extent});
    auto surface = std::make_shared<ContiguousRasterSurface>(
        testCase.extent, testCase.background);
    auto layer = Layer::raster(testCase.name, surface);
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    History history;
    BasicPixelBrushStroke stroke(document, layerId, testCase.settings);
    CHECK(stroke.valid());
    CHECK(!testCase.samples.empty());
    CHECK(stroke.begin(testCase.samples.front()));
    for (std::size_t index = 1; index + 1 < testCase.samples.size(); ++index) {
        CHECK(stroke.append(testCase.samples[index]));
    }
    const auto commit = stroke.end(testCase.samples.back(), history);
    CHECK(commit == RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);

    std::vector<std::byte> raw(static_cast<std::size_t>(testCase.extent.width)
        * static_cast<std::size_t>(testCase.extent.height) * 4U);
    surface->copyRgba8({0, 0, static_cast<std::int32_t>(testCase.extent.width),
                           static_cast<std::int32_t>(testCase.extent.height)},
        raw, static_cast<std::size_t>(testCase.extent.width) * 4U);
    QImage image(reinterpret_cast<const uchar*>(raw.data()),
        static_cast<int>(testCase.extent.width),
        static_cast<int>(testCase.extent.height),
        static_cast<qsizetype>(testCase.extent.width) * 4,
        QImage::Format_RGBA8888);
    return {image.copy(), std::move(raw), stroke.stats()};
}

std::uint64_t fnv1a(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : bytes) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void compareOrUpdateGoldens(const std::vector<StrokeCase>& cases, bool update)
{
    const QString goldenDirectory = QStringLiteral(IMAGEEDITOR_SOURCE_DIR)
        + QStringLiteral("/tests/assets/brush-goldens");
    QDir().mkpath(goldenDirectory);
    QDir().mkpath(QStringLiteral("brush-test-artifacts"));
    for (const auto& testCase : cases) {
        const auto rendered = renderCase(testCase);
        const auto fileName = QString::fromStdString(testCase.name) + QStringLiteral(".png");
        const auto goldenPath = goldenDirectory + QLatin1Char('/') + fileName;
        if (update) {
            CHECK(rendered.image.save(goldenPath, "PNG"));
            std::cout << testCase.name << " 0x" << std::hex
                      << fnv1a(rendered.raw) << std::dec << '\n';
            continue;
        }

        const QImage golden(goldenPath);
        if (golden.isNull() || golden.size() != rendered.image.size()) {
            std::cerr << "Missing or invalid golden: " << goldenPath.toStdString() << '\n';
            ++failures;
            continue;
        }
        const auto canonicalGolden = golden.convertToFormat(QImage::Format_RGBA8888);
        std::uint64_t absoluteError = 0;
        std::uint8_t maximumError = 0;
        std::size_t changedPixels = 0;
        QImage difference(canonicalGolden.size(), QImage::Format_RGBA8888);
        difference.fill(Qt::black);
        for (int y = 0; y < canonicalGolden.height(); ++y) {
            const auto* expected = canonicalGolden.constScanLine(y);
            const auto* actual = rendered.image.constScanLine(y);
            auto* diff = difference.scanLine(y);
            for (int x = 0; x < canonicalGolden.width(); ++x) {
                bool pixelChanged = false;
                for (int channel = 0; channel < 4; ++channel) {
                    const auto error = static_cast<std::uint8_t>(std::abs(
                        static_cast<int>(expected[x * 4 + channel])
                        - static_cast<int>(actual[x * 4 + channel])));
                    absoluteError += error;
                    maximumError = std::max(maximumError, error);
                    diff[x * 4 + channel] = channel == 3 ? 255 : error;
                    pixelChanged = pixelChanged || error != 0;
                }
                changedPixels += pixelChanged ? 1U : 0U;
            }
        }
        if (changedPixels != 0) {
            const auto actualPath = QStringLiteral("brush-test-artifacts/")
                + QString::fromStdString(testCase.name) + QStringLiteral(".actual.png");
            const auto diffPath = QStringLiteral("brush-test-artifacts/")
                + QString::fromStdString(testCase.name) + QStringLiteral(".diff.png");
            (void)rendered.image.save(actualPath, "PNG");
            (void)difference.save(diffPath, "PNG");
            const auto channelCount = static_cast<double>(canonicalGolden.width())
                * canonicalGolden.height() * 4.0;
            std::cerr << "Golden mismatch " << testCase.name
                      << ": changedPixels=" << changedPixels
                      << " maxError=" << static_cast<int>(maximumError)
                      << " MAD=" << static_cast<double>(absoluteError) / channelCount
                      << " hash=0x" << std::hex << fnv1a(rendered.raw) << std::dec << '\n';
            ++failures;
        }
    }
}

void writeContactSheet(const std::vector<StrokeCase>& cases, const QString& path)
{
    constexpr int columns = 3;
    constexpr int cellWidth = 360;
    constexpr int cellHeight = 260;
    const auto rows = static_cast<int>((cases.size() + columns - 1U) / columns);
    QImage sheet(columns * cellWidth, rows * cellHeight, QImage::Format_RGBA8888);
    sheet.fill(QColor(QStringLiteral("#11141B")));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QColor(QStringLiteral("#E9ECF5")));
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto column = static_cast<int>(index % columns);
        const auto row = static_cast<int>(index / columns);
        const QRect cell(column * cellWidth, row * cellHeight, cellWidth, cellHeight);
        painter.fillRect(cell.adjusted(8, 8, -8, -8), QColor(QStringLiteral("#191D27")));
        painter.drawText(cell.adjusted(18, 14, -18, -220),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromStdString(cases[index].name));
        const auto rendered = renderCase(cases[index]);
        const QRect imageArea = cell.adjusted(18, 46, -18, -18);
        const auto scaled = rendered.image.scaled(imageArea.size(),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QPoint topLeft(imageArea.center().x() - scaled.width() / 2,
            imageArea.center().y() - scaled.height() / 2);
        // A checker under transparent cases keeps edge quality reviewable.
        constexpr int checker = 12;
        for (int y = imageArea.top(); y < imageArea.bottom(); y += checker) {
            for (int x = imageArea.left(); x < imageArea.right(); x += checker) {
                const bool alternate = ((x - imageArea.left()) / checker
                    + (y - imageArea.top()) / checker) % 2 != 0;
                painter.fillRect(QRect(x, y, checker, checker),
                    alternate ? QColor("#3A3F4B") : QColor("#505664"));
            }
        }
        painter.drawImage(topLeft, scaled);
    }
    painter.end();
    QDir().mkpath(QFileInfo(path).absolutePath());
    CHECK(sheet.save(path, "PNG"));
}

class DabCollector final : public BrushDabSink {
public:
    void emitDab(const BrushDab& dab) override { dabs.push_back(dab); }
    std::vector<BrushDab> dabs;
};

class SquareTestTip final : public IBrushTip {
public:
    BrushTipBounds prepareDab(const BrushDab& dab, double,
        double documentPixelFootprint) noexcept override
    {
        center_ = dab.documentCenter;
        radius_ = dab.diameterPixels * 0.5;
        const auto boundsRadius = radius_ + documentPixelFootprint;
        return {center_.x - boundsRadius, center_.y - boundsRadius,
            center_.x + boundsRadius, center_.y + boundsRadius};
    }

    double coverage(Vec2d point) const noexcept override
    {
        return std::abs(point.x - center_.x) <= radius_
                && std::abs(point.y - center_.y) <= radius_
            ? 1.0 : 0.0;
    }

private:
    Vec2d center_;
    double radius_ {0.0};
};

class LeftHalfDocumentGrain final : public IBrushGrain {
public:
    void prepareDab(const BrushDab& dab, double) noexcept override
    {
        center_ = dab.documentCenter;
    }

    double modulation(Vec2d point) const noexcept override
    {
        return point.x < center_.x ? 1.0 : 0.0;
    }

private:
    Vec2d center_;
};

void engineResamplesIndependentlyAndPreservesDynamics()
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::PressureRound);
    settings.sizePixels = 20.0;
    settings.spacingPercent = 10.0;
    settings.smoothing = BrushSmoothingMode::None;
    NormalizedPointerSample first = sample(0, 0, 0.2, 1000);
    first.tiltX = -0.8;
    first.tiltY = 0.3;
    first.rotationDegrees = 15.0;
    first.barrelRotationDegrees = 20.0;
    NormalizedPointerSample last = sample(40, 0, 1.0, 41000);
    last.tiltX = 0.8;
    last.tiltY = -0.5;
    last.rotationDegrees = 95.0;
    last.barrelRotationDegrees = 140.0;

    BasicPixelBrushEngine sparse;
    DabCollector sparseDabs;
    CHECK(sparse.beginStroke(settings, first, sparseDabs));
    CHECK(sparse.endStroke(last, sparseDabs));
    CHECK(!sparseDabs.dabs.empty());
    CHECK(sparseDabs.dabs.front().sourceSample.tiltX == first.tiltX);
    CHECK(sparseDabs.dabs.back().sourceSample.barrelRotationDegrees
        == last.barrelRotationDegrees);
    CHECK(std::any_of(sparseDabs.dabs.begin(), sparseDabs.dabs.end(),
        [](const BrushDab& dab) {
            return dab.sourceSample.tiltX > -0.7 && dab.sourceSample.tiltX < 0.7
                && dab.sourceSample.rotationDegrees > 15.0
                && dab.sourceSample.rotationDegrees < 95.0;
        }));

    BasicPixelBrushEngine dense;
    DabCollector denseDabs;
    CHECK(dense.beginStroke(settings, first, denseDabs));
    for (int index = 1; index < 20; ++index) {
        const auto amount = static_cast<double>(index) / 20.0;
        auto intermediate = sample(40.0 * amount, 0.0,
            0.2 + 0.8 * amount, 1000U + static_cast<std::uint64_t>(40000.0 * amount));
        intermediate.tiltX = -0.8 + 1.6 * amount;
        intermediate.tiltY = 0.3 - 0.8 * amount;
        intermediate.rotationDegrees = 15.0 + 80.0 * amount;
        intermediate.barrelRotationDegrees = 20.0 + 120.0 * amount;
        CHECK(dense.appendSample(intermediate, denseDabs));
    }
    CHECK(dense.endStroke(last, denseDabs));
    CHECK(denseDabs.dabs.size() == sparseDabs.dabs.size());
    for (std::size_t index = 0; index < sparseDabs.dabs.size(); ++index) {
        CHECK(std::abs(sparseDabs.dabs[index].documentCenter.x
            - denseDabs.dabs[index].documentCenter.x) < 1.0e-8);
        CHECK(std::abs(sparseDabs.dabs[index].diameterPixels
            - denseDabs.dabs[index].diameterPixels) < 1.0e-8);
    }
}

void tipGrainAndPresetDataAreIndependent()
{
    const auto presets = builtinBrushPresets();
    CHECK(presets.size() == 23); // five unchanged originals plus eighteen generated presets
    for (const auto& preset : presets) {
        CHECK(!preset.id.empty());
        CHECK(!preset.displayName.empty());
        CHECK(!preset.settings.tip.assetId.empty());
        CHECK(!preset.settings.grain.assetId.empty());
        CHECK(makeBuiltinBrushTip(preset.settings.tip.assetId) != nullptr);
        CHECK(makeBuiltinBrushGrain(preset.settings.grain.assetId) != nullptr);
        CHECK(findBuiltinBrushPreset(preset.id) == &preset);
    }
    CHECK(presets[3].settings.tip.assetId
        == BrushAssetIds::ProceduralEllipseTip);
    CHECK(presets[4].settings.tip.assetId
        == BrushAssetIds::DryInkMaskTip);
    CHECK(presets[4].settings.grain.assetId
        == BrushAssetIds::DryInkPaperGrain);
    const auto cacheStats = builtinBrushAssetResolver().cacheStats();
    CHECK(cacheStats.grayscaleMaskCount == 17); // two originals + fifteen shared generated masks
    CHECK(cacheStats.retainedBytes > 0);
    CHECK(cacheStats.retainedBytes < 16U * 1024U * 1024U);
    BrushTipDescriptor cachedTipDescriptor;
    cachedTipDescriptor.assetId = BrushAssetIds::DryInkMaskTip;
    const auto firstCachedTip = builtinBrushAssetResolver().createTip(
        cachedTipDescriptor);
    const auto secondCachedTip = builtinBrushAssetResolver().createTip(
        cachedTipDescriptor);
    const auto* firstBitmap = dynamic_cast<const BitmapMaskTip*>(
        firstCachedTip.get());
    const auto* secondBitmap = dynamic_cast<const BitmapMaskTip*>(
        secondCachedTip.get());
    CHECK(firstBitmap != nullptr);
    CHECK(secondBitmap != nullptr);
    CHECK(firstBitmap && secondBitmap
        && firstBitmap->mask() == secondBitmap->mask());
    CHECK(builtinBrushAssetResolver().cacheStats().grayscaleMaskCount
        == cacheStats.grayscaleMaskCount);
    CHECK(builtinBrushAssetResolver().cacheStats().retainedBytes
        == cacheStats.retainedBytes);
    CHECK(findBuiltinBrushPreset("missing.preset") == nullptr);
    CHECK(makeBuiltinBrushTip("missing.tip") == nullptr);
    CHECK(makeBuiltinBrushGrain("missing.grain") == nullptr);

    const Extent2u extent {64, 64};
    Document document(CanvasSpec {.extent = extent});
    auto surface = std::make_shared<ContiguousRasterSurface>(
        extent, Rgba8 {0, 0, 0, 0});
    auto layer = Layer::raster("Tip seam", surface);
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = 20.0;
    settings.foreground = {230, 80, 40, 255};
    History history;
    BasicPixelBrushStroke stroke(document, layerId, settings,
        std::make_unique<BasicPixelBrushEngine>(),
        std::make_unique<SquareTestTip>(),
        std::make_unique<LeftHalfDocumentGrain>());
    CHECK(stroke.valid());
    const auto dot = sample(32.0, 32.0, 1.0, 1000);
    CHECK(stroke.begin(dot));
    CHECK(stroke.end(dot, history) == RasterEditCommitResult::Committed);

    std::array<std::byte, 4> left {};
    std::array<std::byte, 4> right {};
    surface->copyRgba8({27, 32, 1, 1}, left, 4);
    surface->copyRgba8({37, 32, 1, 1}, right, 4);
    CHECK(std::to_integer<std::uint8_t>(left[3]) > 0);
    CHECK(std::to_integer<std::uint8_t>(right[3]) == 0);
}

void differentEventRatesRenderIdentically()
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::PressureRound);
    settings.sizePixels = 30;
    settings.spacingPercent = 8;
    settings.foreground = {25, 180, 230, 255};
    StrokeCase sparse {"event-rate-sparse", {256, 96}, {0, 0, 0, 0}, settings,
        {sample(12, 48, 0.2, 0), sample(244, 48, 1.0, 200000)}};
    StrokeCase dense = sparse;
    dense.name = "event-rate-dense";
    dense.samples.clear();
    for (int index = 0; index <= 40; ++index) {
        const auto amount = static_cast<double>(index) / 40.0;
        dense.samples.push_back(sample(12.0 + 232.0 * amount, 48.0,
            0.2 + 0.8 * amount,
            static_cast<std::uint64_t>(std::llround(200000.0 * amount))));
    }
    const auto sparseRender = renderCase(sparse);
    const auto denseRender = renderCase(dense);
    CHECK(sparseRender.raw == denseRender.raw);
}

void fixedSeedReplayIsDeterministic()
{
    auto replay = goldenCases()[5];
    replay.name = "fixed-seed-replay";
    replay.settings.deterministicSeed = 0xC0FFEEU;
    const auto first = renderCase(replay);
    const auto second = renderCase(replay);
    CHECK(first.raw == second.raw);
    CHECK(fnv1a(first.raw) == fnv1a(second.raw));
}

void cancelAndUndoRedoArePixelExact()
{
    const Extent2u extent {128, 128};
    Document document(CanvasSpec {.extent = extent});
    auto surface = std::make_shared<ContiguousRasterSurface>(
        extent, Rgba8 {12, 22, 32, 128});
    auto layer = Layer::raster("Pixels", surface);
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = 25;
    History history;

    std::vector<std::byte> original(128U * 128U * 4U);
    surface->copyRgba8({0, 0, 128, 128}, original, 128U * 4U);
    {
        BasicPixelBrushStroke stroke(document, layerId, settings);
        CHECK(stroke.begin(sample(10, 10, 1, 0)));
        CHECK(stroke.append(sample(118, 118, 1, 30000)));
        stroke.cancel();
    }
    std::vector<std::byte> afterCancel(original.size());
    surface->copyRgba8({0, 0, 128, 128}, afterCancel, 128U * 4U);
    CHECK(afterCancel == original);
    CHECK(!history.canUndo());

    BasicPixelBrushStroke committed(document, layerId, settings);
    CHECK(committed.begin(sample(10, 100, 1, 0)));
    CHECK(committed.end(sample(118, 20, 1, 30000), history)
        == RasterEditCommitResult::Committed);
    std::vector<std::byte> painted(original.size());
    surface->copyRgba8({0, 0, 128, 128}, painted, 128U * 4U);
    CHECK(painted != original);
    CHECK(history.undo(document));
    std::vector<std::byte> undone(original.size());
    surface->copyRgba8({0, 0, 128, 128}, undone, 128U * 4U);
    CHECK(undone == original);
    CHECK(history.redo(document));
    std::vector<std::byte> redone(original.size());
    surface->copyRgba8({0, 0, 128, 128}, redone, 128U * 4U);
    CHECK(redone == painted);
}

void documentClipAndSparseUploadStats()
{
    const auto testCase = goldenCases().back();
    const auto rendered = renderCase(testCase);
    CHECK(rendered.stats.surfaceWriteBatches <= rendered.stats.inputSamples);
    CHECK(rendered.stats.retainedStrokeTiles > 0);
    const auto fullLayerBytes = static_cast<std::uint64_t>(testCase.extent.width)
        * testCase.extent.height * 4U;
    CHECK(rendered.stats.uploadedRegionBytes < fullLayerBytes
        * std::max<std::uint64_t>(1, rendered.stats.emittedDabs));
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const auto cases = goldenCases();
    const bool update = qEnvironmentVariableIntValue("IMAGEEDITOR_UPDATE_BRUSH_GOLDENS") == 1;
    QString contactSheetPath;
    for (int index = 1; index + 1 < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--contact-sheet")) {
            contactSheetPath = QString::fromLocal8Bit(argv[index + 1]);
        }
    }

    engineResamplesIndependentlyAndPreservesDynamics();
    tipGrainAndPresetDataAreIndependent();
    differentEventRatesRenderIdentically();
    fixedSeedReplayIsDeterministic();
    cancelAndUndoRedoArePixelExact();
    documentClipAndSparseUploadStats();
    compareOrUpdateGoldens(cases, update);
    if (!contactSheetPath.isEmpty()) {
        writeContactSheet(cases, contactSheetPath);
    }

    if (failures != 0) {
        std::cerr << failures << " basic pixel brush assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Basic Pixel Brush tests passed\n";
    return EXIT_SUCCESS;
}
