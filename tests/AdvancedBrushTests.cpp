#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BrushTip.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/ViewportState.hpp"

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
#include <fstream>
#include <iostream>
#include <iterator>
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

struct StrokeCase {
    std::string name;
    Extent2u extent;
    Rgba8 background;
    BrushSettings settings;
    std::vector<NormalizedPointerSample> samples;
};

struct RenderedCase {
    QImage image;
    std::vector<std::byte> raw;
    BrushStrokeStats stats;
    DirtySet dirty;
};

NormalizedPointerSample sample(double x, double y, double pressure,
    std::uint64_t timestampMicroseconds, double rotationDegrees = 0.0)
{
    return {
        .documentPosition = {x, y},
        .timestampMicroseconds = timestampMicroseconds,
        .pressure = pressure,
        .tiltX = 0.0,
        .tiltY = 0.0,
        .rotationDegrees = rotationDegrees,
        .barrelRotationDegrees = rotationDegrees,
        .pointerType = PointerType::Pen,
        .buttons = PointerButtonPrimary,
        .modifiers = PointerModifierNone,
    };
}

std::vector<NormalizedPointerSample> ellipsePath(Vec2d center,
    double radiusX, double radiusY, int segments, std::uint64_t duration)
{
    constexpr double pi = 3.14159265358979323846;
    std::vector<NormalizedPointerSample> result;
    result.reserve(static_cast<std::size_t>(segments + 1));
    for (int index = 0; index <= segments; ++index) {
        const auto amount = static_cast<double>(index) / segments;
        const auto angle = amount * pi * 2.0;
        result.push_back(sample(center.x + std::cos(angle) * radiusX,
            center.y + std::sin(angle) * radiusY,
            0.2 + 0.8 * std::abs(std::sin(angle * 0.5)),
            static_cast<std::uint64_t>(std::llround(
                amount * static_cast<double>(duration)))));
    }
    return result;
}

std::vector<StrokeCase> goldenCases()
{
    std::vector<StrokeCase> result;

    auto inkFine = proceduralBrushPreset(ProceduralBrushPreset::InkPen);
    inkFine.sizePixels = 10.0;
    inkFine.pressureToSize = false;
    inkFine.smoothing = BrushSmoothingMode::None;
    inkFine.foreground = {28, 34, 48, 255};
    result.push_back({"ink-fine-angle", {320, 120}, {244, 239, 224, 255},
        inkFine, {sample(18.25, 91.5, 1.0, 0),
                     sample(86.5, 26.25, 1.0, 22000),
                     sample(164.75, 89.125, 1.0, 48000),
                     sample(302.25, 31.75, 1.0, 82000)}});

    auto inkPressure = proceduralBrushPreset(ProceduralBrushPreset::InkPen);
    inkPressure.sizePixels = 38.0;
    inkPressure.tip.angleDegrees = 28.0;
    inkPressure.smoothing = BrushSmoothingMode::None;
    inkPressure.foreground = {40, 45, 59, 255};
    result.push_back({"ink-pressure-ramp", {360, 150}, {250, 247, 236, 255},
        inkPressure, {sample(20, 76, 0.08, 0), sample(80, 62, 0.22, 18000),
                         sample(150, 88, 0.48, 38000),
                         sample(232, 48, 0.75, 61000),
                         sample(340, 82, 1.0, 90000)}});

    auto inkRotation = proceduralBrushPreset(ProceduralBrushPreset::InkPen);
    inkRotation.sizePixels = 30.0;
    inkRotation.tip.aspectRatio = 0.18;
    inkRotation.tip.angleDegrees = 0.0;
    inkRotation.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    inkRotation.pressureToSize = false;
    inkRotation.smoothing = BrushSmoothingMode::None;
    inkRotation.foreground = {23, 28, 38, 255};
    result.push_back({"ink-rotation-sweep", {340, 180}, {239, 233, 216, 255},
        inkRotation, {sample(22, 90, 1.0, 0, -70),
                         sample(95, 35, 1.0, 24000, -25),
                         sample(172, 145, 1.0, 49000, 20),
                         sample(247, 48, 1.0, 73000, 65),
                         sample(320, 92, 1.0, 98000, 110)}});

    auto inkCurve = proceduralBrushPreset(ProceduralBrushPreset::InkPen);
    inkCurve.sizePixels = 22.0;
    inkCurve.tip.angleDegrees = -55.0;
    inkCurve.smoothing = BrushSmoothingMode::None;
    inkCurve.foreground = {37, 44, 62, 255};
    result.push_back({"ink-tight-curve", {280, 240}, {246, 241, 226, 255},
        inkCurve, ellipsePath({140, 120}, 92, 74, 96, 260000)});

    auto dry = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    // Keep historical fixed-angle fixtures explicit as product defaults evolve;
    // the separate dry-ink-rotation fixture exercises following the path.
    dry.tip.rotationMode = BrushTipRotationMode::Fixed;
    dry.smoothing = BrushSmoothingMode::None;
    dry.pressureToSize = false;
    dry.pressureToFlow = false;
    dry.foreground = {35, 31, 28, 255};
    result.push_back({"dry-ink-diagonal", {360, 180}, {239, 227, 205, 255}, dry,
        {sample(15.5, 151.25, 1.0, 0), sample(112.25, 86.5, 1.0, 28000),
            sample(218.75, 124.25, 1.0, 57000),
            sample(344.5, 24.75, 1.0, 90000)}});

    auto dryPressure = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    dryPressure.tip.rotationMode = BrushTipRotationMode::Fixed;
    dryPressure.sizePixels = 66.0;
    dryPressure.tip.angleDegrees = 42.0;
    dryPressure.smoothing = BrushSmoothingMode::None;
    dryPressure.foreground = {48, 37, 29, 255};
    result.push_back({"dry-ink-pressure-flick", {380, 180}, {242, 232, 211, 255},
        dryPressure, {sample(12, 150, 0.08, 0), sample(70, 126, 0.2, 4000),
                         sample(142, 103, 0.42, 8000),
                         sample(225, 72, 0.7, 12000),
                         sample(371, 20, 1.0, 16000)}});

    auto dryOverlap = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    dryOverlap.tip.rotationMode = BrushTipRotationMode::Fixed;
    dryOverlap.sizePixels = 38.0;
    dryOverlap.tip.aspectRatio = 0.55;
    dryOverlap.tip.angleDegrees = -12.0;
    dryOverlap.opacity = 0.72;
    dryOverlap.flow = 0.34;
    dryOverlap.smoothing = BrushSmoothingMode::None;
    dryOverlap.pressureToSize = false;
    dryOverlap.pressureToFlow = false;
    dryOverlap.foreground = {45, 38, 34, 255};
    result.push_back({"dry-ink-overlap", {320, 160}, {235, 225, 207, 255},
        dryOverlap, {sample(24, 48, 1.0, 0), sample(296, 112, 1.0, 60000),
                        sample(296, 48, 1.0, 70000),
                        sample(24, 112, 1.0, 130000),
                        sample(24, 80, 1.0, 140000),
                        sample(296, 80, 1.0, 200000)}});

    auto dryRotated = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    dryRotated.sizePixels = 34.0;
    dryRotated.tip.aspectRatio = 0.22;
    dryRotated.tip.angleDegrees = 0.0;
    dryRotated.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    dryRotated.smoothing = BrushSmoothingMode::None;
    dryRotated.pressureToSize = false;
    dryRotated.foreground = {31, 28, 27, 255};
    result.push_back({"dry-ink-rotation", {340, 180}, {244, 235, 216, 255},
        dryRotated, {sample(20, 96, 0.45, 0, 350),
                        sample(88, 36, 0.6, 25000, 10),
                        sample(172, 142, 0.78, 52000, 55),
                        sample(250, 45, 0.9, 76000, 105),
                        sample(322, 98, 1.0, 100000, 160)}});

    auto edge = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    edge.tip.rotationMode = BrushTipRotationMode::Fixed;
    edge.sizePixels = 72.0;
    edge.tip.angleDegrees = 73.0;
    edge.smoothing = BrushSmoothingMode::None;
    edge.pressureToSize = false;
    edge.pressureToFlow = false;
    edge.foreground = {28, 32, 38, 255};
    result.push_back({"dry-ink-edge-clipping", {260, 150}, {238, 231, 216, 255},
        edge, {sample(-38, -22, 1.0, 0), sample(45, 24, 1.0, 25000),
                  sample(220, 125, 1.0, 70000), sample(298, 180, 1.0, 92000)}});

    return result;
}

RenderedCase renderCase(const StrokeCase& testCase,
    AffineTransform layerTransform = {})
{
    Document document(CanvasSpec {.extent = testCase.extent});
    auto surface = std::make_shared<ContiguousRasterSurface>(
        testCase.extent, testCase.background);
    auto layer = Layer::raster(testCase.name, surface);
    layer.localToDocument = layerTransform;
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    History history;
    const auto initialRevision = surface->revision();
    BasicPixelBrushStroke stroke(document, layerId, testCase.settings);
    CHECK(stroke.valid());
    CHECK(!testCase.samples.empty());
    CHECK(stroke.begin(testCase.samples.front()));
    for (std::size_t index = 1; index + 1 < testCase.samples.size(); ++index) {
        CHECK(stroke.append(testCase.samples[index]));
    }
    CHECK(stroke.end(testCase.samples.back(), history)
        == RasterEditCommitResult::Committed);
    CHECK(history.undoDepth() == 1);

    std::vector<std::byte> raw(static_cast<std::size_t>(testCase.extent.width)
        * testCase.extent.height * 4U);
    surface->copyRgba8({0, 0, static_cast<std::int32_t>(testCase.extent.width),
                           static_cast<std::int32_t>(testCase.extent.height)},
        raw, static_cast<std::size_t>(testCase.extent.width) * 4U);
    QImage image(reinterpret_cast<const uchar*>(raw.data()),
        static_cast<int>(testCase.extent.width),
        static_cast<int>(testCase.extent.height),
        static_cast<qsizetype>(testCase.extent.width) * 4,
        QImage::Format_RGBA8888);
    return {image.copy(), std::move(raw), stroke.stats(),
        surface->dirtySince(initialRevision)};
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
        + QStringLiteral("/tests/assets/advanced-brush-goldens");
    QDir().mkpath(goldenDirectory);
    QDir().mkpath(QStringLiteral("advanced-brush-test-artifacts"));
    for (const auto& testCase : cases) {
        const auto rendered = renderCase(testCase);
        CHECK(!rendered.dirty.fullRefresh);
        CHECK(!rendered.dirty.regions.empty());
        const auto fullLayerBytes = static_cast<std::uint64_t>(testCase.extent.width)
            * testCase.extent.height * 4U;
        CHECK(rendered.stats.uploadedRegionBytes
            < fullLayerBytes * std::max<std::uint64_t>(1, rendered.stats.emittedDabs));
        const auto fileName = QString::fromStdString(testCase.name)
            + QStringLiteral(".png");
        const auto goldenPath = goldenDirectory + QLatin1Char('/') + fileName;
        if (update) {
            CHECK(rendered.image.save(goldenPath, "PNG"));
            std::cout << testCase.name << " 0x" << std::hex
                      << fnv1a(rendered.raw) << std::dec << '\n';
            continue;
        }
        const QImage golden(goldenPath);
        if (golden.isNull() || golden.size() != rendered.image.size()) {
            std::cerr << "Missing or invalid golden: "
                      << goldenPath.toStdString() << '\n';
            ++failures;
            continue;
        }
        const auto expected = golden.convertToFormat(QImage::Format_RGBA8888);
        if (expected != rendered.image) {
            const auto actualPath = QStringLiteral("advanced-brush-test-artifacts/")
                + QString::fromStdString(testCase.name)
                + QStringLiteral(".actual.png");
            (void)rendered.image.save(actualPath, "PNG");
            std::cerr << "Golden mismatch " << testCase.name << " hash=0x"
                      << std::hex << fnv1a(rendered.raw) << std::dec << '\n';
            ++failures;
        }
    }
}

void writeContactSheet(const std::vector<StrokeCase>& cases, const QString& path)
{
    constexpr int columns = 3;
    constexpr int cellWidth = 420;
    constexpr int cellHeight = 270;
    const auto rows = static_cast<int>((cases.size() + columns - 1U) / columns);
    QImage sheet(columns * cellWidth, rows * cellHeight,
        QImage::Format_RGBA8888);
    sheet.fill(QColor(QStringLiteral("#11141B")));
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QColor(QStringLiteral("#E9ECF5")));
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto column = static_cast<int>(index % columns);
        const auto row = static_cast<int>(index / columns);
        const QRect cell(column * cellWidth, row * cellHeight,
            cellWidth, cellHeight);
        painter.fillRect(cell.adjusted(8, 8, -8, -8),
            QColor(QStringLiteral("#191D27")));
        painter.drawText(cell.adjusted(18, 14, -18, -225),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromStdString(cases[index].name));
        const auto rendered = renderCase(cases[index]);
        const QRect imageArea = cell.adjusted(18, 46, -18, -18);
        const auto scaled = rendered.image.scaled(imageArea.size(),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QPoint topLeft(imageArea.center().x() - scaled.width() / 2,
            imageArea.center().y() - scaled.height() / 2);
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

void ellipseTipGeometryIsStable()
{
    ProceduralEllipseTip tip;
    BrushDab dab;
    dab.documentCenter = {50.0, 50.0};
    dab.diameterPixels = 40.0;
    dab.tipAspectRatio = 0.25;
    dab.tipAngleDegrees = 0.0;
    const auto horizontal = tip.prepareDab(dab, 1.0, 1.0);
    CHECK(std::abs(horizontal.left - 29.0) < 1.0e-9);
    CHECK(std::abs(horizontal.top - 44.0) < 1.0e-9);
    CHECK(tip.coverage({50.0, 50.0}) > 0.99);
    CHECK(tip.coverage({65.0, 50.0}) > 0.99);
    CHECK(tip.coverage({50.0, 58.0}) < 0.01);

    dab.tipAngleDegrees = 90.0;
    const auto vertical = tip.prepareDab(dab, 1.0, 1.0);
    CHECK(std::abs(vertical.left - 44.0) < 1.0e-9);
    CHECK(std::abs(vertical.top - 29.0) < 1.0e-9);
    CHECK(tip.coverage({50.0, 65.0}) > 0.99);
    CHECK(tip.coverage({58.0, 50.0}) < 0.01);

    dab.tipAngleDegrees = 45.0;
    const auto diagonal = tip.prepareDab(dab, 0.5, 0.25);
    CHECK(std::abs((diagonal.right - diagonal.left)
            - (2.0 * (std::hypot(20.0 / std::sqrt(2.0),
                          5.0 / std::sqrt(2.0))
                + 0.25)))
        < 1.0e-8);
    CHECK(tip.coverage({50.0, 50.0}) > 0.99);

    // Vertical scale is a tip-wide contract. Procedural Round remains a true
    // circle at 100%, but at lower values it uses the same oriented local
    // geometry as every other tip rather than silently ignoring the control.
    ProceduralRoundTip roundTip;
    dab.tipAspectRatio = 0.5;
    dab.tipAngleDegrees = 0.0;
    const auto scaledRound = roundTip.prepareDab(dab, 1.0, 1.0);
    CHECK(std::abs(scaledRound.left - 29.0) < 1.0e-9);
    CHECK(std::abs(scaledRound.top - 39.0) < 1.0e-9);
    CHECK(roundTip.coverage({65.0, 50.0}) > 0.99);
    CHECK(roundTip.coverage({50.0, 63.0}) < 0.01);
    dab.tipAngleDegrees = 90.0;
    const auto rotatedRound = roundTip.prepareDab(dab, 1.0, 1.0);
    CHECK(std::abs(rotatedRound.left - 39.0) < 1.0e-9);
    CHECK(std::abs(rotatedRound.top - 29.0) < 1.0e-9);
    CHECK(roundTip.coverage({50.0, 65.0}) > 0.99);
    CHECK(roundTip.coverage({63.0, 50.0}) < 0.01);

    auto roundSettings = proceduralBrushPreset(
        ProceduralBrushPreset::HardRound);
    roundSettings.sizePixels = 27.5;
    roundSettings.hardness = 0.63;
    roundSettings.opacity = 0.82;
    roundSettings.foreground = {90, 140, 230, 255};
    StrokeCase roundCase {"round-parity", {220, 130}, {0, 0, 0, 0},
        roundSettings, {sample(12.25, 25.5, 1.0, 0),
                           sample(207.75, 104.25, 1.0, 80000)}};
    auto ellipseCase = roundCase;
    ellipseCase.name = "ellipse-parity";
    ellipseCase.settings.tip.assetId = BrushAssetIds::ProceduralEllipseTip;
    ellipseCase.settings.tip.aspectRatio = 1.0;
    ellipseCase.settings.tip.angleDegrees = 0.0;
    CHECK(renderCase(roundCase).raw == renderCase(ellipseCase).raw);
}

void bitmapTipFiltersAndRotatesAsymmetricMasks()
{
    const std::array<std::uint8_t, 15> pixels {
        255, 0, 0, 0, 0,
        0, 64, 128, 192, 0,
        0, 0, 0, 0, 32,
    };
    auto asset = std::make_shared<GrayscaleMaskAsset>(
        "test.asymmetric", 7, 5, 3, pixels);
    CHECK(asset->mipLevelCount() == 4);
    CHECK(asset->retainedBytes() >= pixels.size() * sizeof(float));
    BitmapMaskTip tip(asset);
    BrushDab dab;
    dab.documentCenter = {80.0, 60.0};
    dab.diameterPixels = 50.0;
    dab.tipAspectRatio = 0.6;
    dab.tipAngleDegrees = 0.0;
    const auto bounds = tip.prepareDab(dab, 0.5, 0.25);
    CHECK(bounds.left < 55.0 && bounds.right > 105.0);
    const auto texelPoint = [](Vec2d center, double angleDegrees,
                                double u, double v) {
        constexpr double pi = 3.14159265358979323846;
        const auto localX = (u - 0.5) * 50.0;
        const auto localY = (v - 0.5) * 30.0;
        const auto radians = angleDegrees * pi / 180.0;
        return Vec2d {center.x + std::cos(radians) * localX
                    - std::sin(radians) * localY,
            center.y + std::sin(radians) * localX
                + std::cos(radians) * localY};
    };
    const auto bright = texelPoint(dab.documentCenter, 0.0, 0.1, 1.0 / 6.0);
    const auto dark = texelPoint(dab.documentCenter, 0.0, 0.9, 1.0 / 6.0);
    CHECK(tip.coverage(bright) > 0.95);
    CHECK(tip.coverage(dark) < 0.01);
    CHECK(tip.coverage({20.0, 20.0}) == 0.0);

    dab.tipAngleDegrees = 90.0;
    (void)tip.prepareDab(dab, 0.5, 0.25);
    const auto rotatedBright = texelPoint(
        dab.documentCenter, 90.0, 0.1, 1.0 / 6.0);
    CHECK(tip.coverage(rotatedBright) > 0.95);
    CHECK(tip.coverage(bright) < 0.95);

    dab.diameterPixels = 1.0;
    (void)tip.prepareDab(dab, 0.5, 1.0);
    const auto minified = tip.coverage(dab.documentCenter);
    CHECK(std::isfinite(minified));
    CHECK(minified >= 0.0 && minified <= 1.0);
}

void bitmapCoverageKeepsExactBlackEmpty()
{
    // Near-black is real coverage, not black. High hardness intentionally
    // amplifies it; mask assets must encode empty areas as exactly zero.
    const std::array<std::uint8_t, 4> pixels {0, 1, 3, 255};
    auto mask = std::make_shared<GrayscaleMaskAsset>("test.black", 1, 4, 1, pixels);
    BitmapMaskTip tip(mask);
    BrushDab dab;
    dab.documentCenter = {2.0, 0.5};
    dab.diameterPixels = 4;
    dab.tipAspectRatio = 0.25;
    for (double hardness : {0.0, 0.5, 1.0}) {
        (void)tip.prepareDab(dab, hardness, 1.0);
        CHECK(tip.coverage({0.5, 0.5}) == 0.0);
        CHECK(tip.coverage({3.5, 0.5}) == 1.0);
        const double exponent = std::exp2((0.5 - hardness) * 2.0);
        CHECK(std::abs(tip.coverage({1.5, 0.5}) - std::pow(1.0 / 255.0, exponent)) < 1e-7);
        CHECK(std::abs(tip.coverage({2.5, 0.5}) - std::pow(3.0 / 255.0, exponent)) < 1e-7);
    }
    const std::array<std::uint8_t, 64> empty {};
    BitmapMaskTip emptyTip(std::make_shared<GrayscaleMaskAsset>("test.empty", 1, 8, 8, empty));
    for (double diameter : {1.0, 3.0, 8.0, 32.0}) {
        dab.diameterPixels = diameter;
        dab.tipAspectRatio = 1;
        for (double hardness : {0.0, 0.5, 1.0}) {
            (void)emptyTip.prepareDab(dab, hardness, 1.0);
            CHECK(emptyTip.coverage(dab.documentCenter) == 0.0);
        }
    }
}

void preparedMaskSamplingExactlyMatchesTheReference()
{
    struct Level { std::uint32_t width, height; std::vector<float> pixels; };
    for (const auto extent : {Extent2u{1, 1}, Extent2u{5, 3}, Extent2u{16, 8}, Extent2u{7, 1}}) {
        std::vector<std::uint8_t> bytes(std::size_t(extent.width) * extent.height);
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::uint8_t((i * 71 + 33) % 256);
        GrayscaleMaskAsset mask("sampling-reference", 1, extent.width, extent.height, bytes);
        std::vector<Level> levels {{extent.width, extent.height, {}}};
        for (const auto byte : bytes) levels[0].pixels.push_back(float(byte) / 255.0F);
        while (levels.back().width > 1 || levels.back().height > 1) {
            const auto& previous = levels.back();
            Level next {std::max(1U, (previous.width + 1) / 2), std::max(1U, (previous.height + 1) / 2), {}};
            for (std::uint32_t y = 0; y < next.height; ++y) for (std::uint32_t x = 0; x < next.width; ++x) {
                double sum = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) for (std::uint32_t dx = 0; dx < 2; ++dx)
                    sum += previous.pixels[std::size_t(std::min(previous.height - 1, y * 2 + dy)) * previous.width
                        + std::min(previous.width - 1, x * 2 + dx)];
                next.pixels.push_back(float(sum * .25));
            }
            levels.push_back(std::move(next));
        }
        // Deliberately retain the pre-optimization algorithm: separate wrapped
        // fetches, two mip samples even at integer LOD, unchanged arithmetic.
        const auto sampleLevel = [&levels](std::size_t index, double u, double v, bool repeat) {
            const auto& level = levels[index];
            const auto x = u * level.width - .5, y = v * level.height - .5;
            const auto x0 = std::int64_t(std::floor(x)), y0 = std::int64_t(std::floor(y));
            const auto tx = x - double(x0), ty = y - double(y0);
            const auto fetch = [&level, repeat](std::int64_t column, std::int64_t row) {
                const auto width = std::int64_t(level.width), height = std::int64_t(level.height);
                if (repeat) { column = ((column % width) + width) % width; row = ((row % height) + height) % height; }
                else if (column < 0 || row < 0 || column >= width || row >= height) return 0.;
                return double(level.pixels[std::size_t(row) * level.width + std::size_t(column)]);
            };
            const auto top = fetch(x0, y0) + (fetch(x0 + 1, y0) - fetch(x0, y0)) * tx;
            const auto bottom = fetch(x0, y0 + 1) + (fetch(x0 + 1, y0 + 1) - fetch(x0, y0 + 1)) * tx;
            return top + (bottom - top) * ty;
        };
        for (const bool repeat : {false, true}) for (const double lod : {-1., 0., .125, .5, 1., 1.75, 2., 4., 100.}) {
            const auto prepared = mask.prepareSampler(lod, repeat);
            const auto clamped = std::clamp(lod, 0., double(levels.size() - 1));
            const auto first = std::size_t(std::floor(clamped)), second = std::min(first + 1, levels.size() - 1);
            for (int y = -12; y <= 20; ++y) for (int x = -12; x <= 20; ++x) {
                const auto u = x / 8.0, v = y / 8.0;
                const auto a = sampleLevel(first, u, v, repeat), b = sampleLevel(second, u, v, repeat);
                const auto expected = std::clamp(a + (b - a) * (clamped - double(first)), 0., 1.);
                CHECK(prepared.sample(u, v) == expected);
                CHECK(mask.sample(u, v, lod, repeat) == expected);
            }
        }
    }
    CHECK(GrayscaleMaskAsset::Sampler{}.sample(0, 0) == 0);
}

void rotationInterpolationUsesTheShortestArc()
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::InkPen);
    settings.smoothing = BrushSmoothingMode::None;
    settings.pressureToSize = false;
    settings.tip.angleDegrees = 0.0;
    settings.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    settings.sizePixels = 10.0;
    settings.spacingPercent = 10.0;
    BasicPixelBrushEngine engine;
    DabCollector collector;
    const auto first = sample(0.0, 0.0, 1.0, 0);
    constexpr double pi = 3.14159265358979323846;
    const auto nearPositiveSeam = Vec2d {
        std::cos(179.0 * pi / 180.0) * 20.0,
        std::sin(179.0 * pi / 180.0) * 20.0};
    const auto nearNegativeSeam = Vec2d {
        nearPositiveSeam.x + std::cos(-179.0 * pi / 180.0) * 20.0,
        nearPositiveSeam.y + std::sin(-179.0 * pi / 180.0) * 20.0};
    CHECK(engine.beginStroke(settings, first, collector));
    CHECK(engine.appendSample(sample(nearPositiveSeam.x,
        nearPositiveSeam.y, 1.0, 20000), collector));
    CHECK(engine.endStroke(sample(nearNegativeSeam.x,
        nearNegativeSeam.y, 1.0, 40000), collector));
    CHECK(collector.dabs.size() > 4);
    CHECK(std::all_of(collector.dabs.begin() + 1, collector.dabs.end(),
        [](const BrushDab& dab) {
            return dab.strokeDirectionDegrees.has_value()
                && std::abs(dab.tipAngleDegrees) >= 170.0;
        }));
    for (std::size_t index = 2; index < collector.dabs.size(); ++index) {
        CHECK(std::abs(std::remainder(collector.dabs[index].tipAngleDegrees
                - collector.dabs[index - 1].tipAngleDegrees, 360.0))
            < 20.0);
    }
}

void grainIsDocumentAnchoredAndSeeded()
{
    const std::array<std::uint8_t, 16> pixels {
        0, 32, 64, 96,
        128, 160, 192, 224,
        255, 224, 192, 160,
        128, 96, 64, 32,
    };
    auto asset = std::make_shared<GrayscaleMaskAsset>(
        "test.grain", 3, 4, 4, pixels);
    DocumentAnchoredMaskGrain grain(asset);
    BrushDab dab;
    dab.documentCenter = {10.0, 10.0};
    dab.grainScalePixels = 40.0;
    dab.grainAngleDegrees = 0.0;
    dab.grainStrength = 1.0;
    dab.deterministicSeed = 91;
    grain.prepareDab(dab, 1.0);
    const auto first = grain.modulation({-3.25, 7.75});
    const auto repeated = grain.modulation({36.75, 7.75});
    CHECK(std::abs(first - repeated) < 1.0e-7);

    dab.documentCenter = {900.0, -400.0};
    grain.prepareDab(dab, 1.0);
    CHECK(std::abs(first - grain.modulation({-3.25, 7.75})) < 1.0e-7);

    DocumentAnchoredMaskGrain replay(asset);
    replay.prepareDab(dab, 1.0);
    CHECK(std::abs(first - replay.modulation({-3.25, 7.75})) < 1.0e-7);
    dab.deterministicSeed = 92;
    replay.prepareDab(dab, 1.0);
    CHECK(std::abs(first - replay.modulation({-3.25, 7.75})) > 1.0e-5);
}

void presetsRoundTripAsVersionedData()
{
    for (const auto& preset : builtinBrushPresets()) {
        const auto encoded = serializeBrushPreset(preset);
        CHECK(!encoded.empty());
        std::string error;
        const auto decoded = deserializeBrushPreset(encoded, &error);
        CHECK(decoded.has_value());
        CHECK(error.empty());
        CHECK(decoded && *decoded == preset);
        CHECK(decoded && serializeBrushPreset(*decoded) == encoded);
    }
    std::string error;
    CHECK(!deserializeBrushPreset("ImageEditorBrushPreset 9", &error));
    CHECK(!deserializeBrushPreset("ImageEditorBrushPreset 1", &error));
    CHECK(!error.empty());
    const auto valid = serializeBrushPreset(builtinBrushPresets().back());
    CHECK(valid.starts_with("ImageEditorBrushPreset 2\n"));
    CHECK(!deserializeBrushPreset(valid + "trailing data\n", &error));

    auto noRotation = valid;
    const auto rotationStart = noRotation.find("tip.rotation ");
    const auto rotationEnd = noRotation.find('\n', rotationStart);
    CHECK(rotationStart != std::string::npos);
    if (rotationStart != std::string::npos && rotationEnd != std::string::npos) {
        noRotation.erase(rotationStart, rotationEnd - rotationStart + 1U);
        const auto decoded = deserializeBrushPreset(noRotation, &error);
        CHECK(decoded.has_value());
        CHECK(decoded && decoded->settings.tip.rotationMode
            == BrushTipRotationMode::Fixed);
    }

    auto unknownRotation = valid;
    const auto unknownToken = unknownRotation.find("tip.rotation follow-stroke");
    CHECK(unknownToken != std::string::npos);
    if (unknownToken != std::string::npos) {
        unknownRotation.replace(unknownToken,
            std::string("tip.rotation follow-stroke").size(),
            "tip.rotation random");
        CHECK(!deserializeBrushPreset(unknownRotation, &error));
    }

    const auto& builtins = builtinBrushPresets();
    CHECK(BrushSettings {}.tip.rotationMode == BrushTipRotationMode::FollowStrokeDirection);
    for (const auto& preset : builtins.first(5)) { // legacy defaults are unchanged; stamps deliberately use Fixed
        CHECK(preset.settings.tip.rotationMode == BrushTipRotationMode::FollowStrokeDirection);
        auto fixed = preset;
        fixed.settings.tip.rotationMode = BrushTipRotationMode::Fixed;
        const auto decoded = deserializeBrushPreset(serializeBrushPreset(fixed), &error);
        CHECK(decoded && *decoded == fixed);
    }

    auto invalidIdentity = builtinBrushPresets().front();
    invalidIdentity.displayName = "\t";
    CHECK(serializeBrushPreset(invalidIdentity).empty());
    invalidIdentity = builtinBrushPresets().front();
    invalidIdentity.id = std::string(193, 'a');
    CHECK(serializeBrushPreset(invalidIdentity).empty());
    invalidIdentity = builtinBrushPresets().front();
    invalidIdentity.settings.tip.assetId = "asset id with spaces";
    CHECK(serializeBrushPreset(invalidIdentity).empty());

    auto malformedName = valid;
    const auto nameStart = malformedName.find("name ");
    const auto nameEnd = malformedName.find('\n', nameStart);
    CHECK(nameStart != std::string::npos);
    CHECK(nameEnd != std::string::npos);
    if (nameStart != std::string::npos && nameEnd != std::string::npos) {
        malformedName.replace(nameStart, nameEnd - nameStart,
            std::string("name \"\t\""));
        CHECK(!deserializeBrushPreset(malformedName, &error));
    }

    const std::array expectedAssets {
        std::pair {"hard-round.iebrush", "builtin.preset.hard-round.v1"},
        std::pair {"soft-round.iebrush", "builtin.preset.soft-round.v1"},
        std::pair {"pressure-round.iebrush",
            "builtin.preset.pressure-round.v1"},
        std::pair {"ink-pen.iebrush", "builtin.preset.ink-pen.v1"},
        std::pair {"dry-ink.iebrush", "builtin.preset.dry-ink.v1"},
    };
    for (const auto& [fileName, presetId] : expectedAssets) {
        const auto path = std::string(IMAGEEDITOR_SOURCE_DIR)
            + "/assets/brush-presets/" + fileName;
        std::ifstream file(path, std::ios::binary);
        CHECK(file.good());
        const std::string contents {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        const auto assetPreset = deserializeBrushPreset(contents, &error);
        const auto* builtin = findBuiltinBrushPreset(presetId);
        CHECK(assetPreset.has_value());
        CHECK(builtin != nullptr);
        CHECK(assetPreset && builtin && *assetPreset == *builtin);
    }
}

void eventRateAndZoomDoNotChangeDocumentOutput()
{
    for (const auto preset : {ProceduralBrushPreset::InkPen,
             ProceduralBrushPreset::DryInk}) {
        auto settings = proceduralBrushPreset(preset);
        settings.smoothing = BrushSmoothingMode::None;
        StrokeCase sparse {"rate-sparse", {300, 140}, {0, 0, 0, 0}, settings,
            {sample(20, 110, 0.2, 0), sample(150, 30, 0.6, 100000),
                sample(280, 100, 1.0, 200000)}};
        auto dense = sparse;
        dense.name = "rate-dense";
        dense.samples.clear();
        for (int segment = 0; segment < 2; ++segment) {
            const auto from = sparse.samples[static_cast<std::size_t>(segment)];
            const auto to = sparse.samples[static_cast<std::size_t>(segment + 1)];
            const auto firstStep = segment == 0 ? 0 : 1;
            for (int step = firstStep; step <= 20; ++step) {
                const auto amount = static_cast<double>(step) / 20.0;
                dense.samples.push_back(sample(
                    from.documentPosition.x
                        + (to.documentPosition.x - from.documentPosition.x) * amount,
                    from.documentPosition.y
                        + (to.documentPosition.y - from.documentPosition.y) * amount,
                    from.pressure + (to.pressure - from.pressure) * amount,
                    from.timestampMicroseconds
                        + static_cast<std::uint64_t>(std::llround(
                            static_cast<double>(to.timestampMicroseconds
                                - from.timestampMicroseconds)
                            * amount))));
            }
        }
        CHECK(renderCase(sparse).raw == renderCase(dense).raw);

        const Extent2d documentExtent {300.0, 140.0};
        const Extent2d viewportExtent {900.0, 600.0};
        std::vector<std::vector<std::byte>> zoomOutputs;
        for (const auto zoom : {0.25, 1.0, 4.0}) {
            ViewportState viewport;
            viewport.setZoom(zoom);
            viewport.setPan({37.5, -19.25});
            auto zoomCase = sparse;
            zoomCase.name = "zoom-parity";
            for (auto& pointer : zoomCase.samples) {
                const auto logical = viewport.documentToViewport(
                    pointer.documentPosition, documentExtent, viewportExtent);
                pointer.documentPosition = viewport.viewportToDocument(
                    logical, documentExtent, viewportExtent);
            }
            zoomOutputs.push_back(renderCase(zoomCase).raw);
        }
        CHECK(zoomOutputs[0] == zoomOutputs[1]);
        CHECK(zoomOutputs[1] == zoomOutputs[2]);
    }
}

void transformedLayersKeepGrainInDocumentSpace()
{
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    settings.tip.rotationMode = BrushTipRotationMode::FollowStrokeDirection;
    settings.smoothing = BrushSmoothingMode::None;
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    StrokeCase testCase {"transformed-grain-anchor", {220, 160},
        {0, 0, 0, 0}, settings,
        {sample(45, 118, 1.0, 0), sample(172, 42, 1.0, 80000)}};
    const auto identity = renderCase(testCase);
    constexpr std::int32_t offsetX = 17;
    constexpr std::int32_t offsetY = 9;
    const AffineTransform translated {
        .m00 = 1.0,
        .m01 = 0.0,
        .m02 = offsetX,
        .m10 = 0.0,
        .m11 = 1.0,
        .m12 = offsetY,
    };
    const auto movedLayer = renderCase(testCase, translated);
    const auto rowBytes = static_cast<std::size_t>(testCase.extent.width) * 4U;
    bool allDocumentPixelsMatch = true;
    for (std::int32_t y = 0;
         allDocumentPixelsMatch
         && y < static_cast<std::int32_t>(testCase.extent.height) - offsetY;
         ++y) {
        for (std::int32_t x = 0;
             x < static_cast<std::int32_t>(testCase.extent.width) - offsetX; ++x) {
            const auto identityOffset
                = static_cast<std::size_t>(y + offsetY) * rowBytes
                + static_cast<std::size_t>(x + offsetX) * 4U;
            const auto translatedOffset = static_cast<std::size_t>(y) * rowBytes
                + static_cast<std::size_t>(x) * 4U;
            if (!std::equal(movedLayer.raw.begin()
                    + static_cast<std::ptrdiff_t>(translatedOffset),
                movedLayer.raw.begin()
                    + static_cast<std::ptrdiff_t>(translatedOffset + 4U),
                identity.raw.begin()
                    + static_cast<std::ptrdiff_t>(identityOffset))) {
                allDocumentPixelsMatch = false;
                break;
            }
        }
    }
    CHECK(allDocumentPixelsMatch);
}

void dryInkCancelUndoRedoAndReplayAreExact()
{
    const Extent2u extent {192, 144};
    Document document(CanvasSpec {.extent = extent});
    const Rgba8 background {236, 228, 212, 255};
    auto surface = std::make_shared<ContiguousRasterSurface>(extent, background);
    auto layer = Layer::raster("Dry ink", surface);
    const auto layerId = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::DryInk);
    settings.smoothing = BrushSmoothingMode::None;
    History history;
    std::vector<std::byte> original(192U * 144U * 4U);
    surface->copyRgba8({0, 0, 192, 144}, original, 192U * 4U);

    {
        BasicPixelBrushStroke cancelled(document, layerId, settings);
        CHECK(cancelled.begin(sample(8, 120, 0.3, 0)));
        CHECK(cancelled.append(sample(184, 18, 1.0, 40000)));
        cancelled.cancel();
    }
    std::vector<std::byte> afterCancel(original.size());
    surface->copyRgba8({0, 0, 192, 144}, afterCancel, 192U * 4U);
    CHECK(afterCancel == original);
    CHECK(history.undoDepth() == 0);

    BasicPixelBrushStroke stroke(document, layerId, settings);
    CHECK(stroke.begin(sample(8, 120, 0.3, 0)));
    CHECK(stroke.end(sample(184, 18, 1.0, 40000), history)
        == RasterEditCommitResult::Committed);
    std::vector<std::byte> painted(original.size());
    surface->copyRgba8({0, 0, 192, 144}, painted, 192U * 4U);
    CHECK(painted != original);
    CHECK(history.undo(document));
    std::vector<std::byte> undone(original.size());
    surface->copyRgba8({0, 0, 192, 144}, undone, 192U * 4U);
    CHECK(undone == original);
    CHECK(history.redo(document));
    std::vector<std::byte> redone(original.size());
    surface->copyRgba8({0, 0, 192, 144}, redone, 192U * 4U);
    CHECK(redone == painted);

    Document replayDocument(CanvasSpec {.extent = extent});
    auto replaySurface = std::make_shared<ContiguousRasterSurface>(extent, background);
    auto replayLayer = Layer::raster("Replay", replaySurface);
    const auto replayLayerId = replayLayer.id;
    CHECK(replayDocument.insertLayer(0, std::move(replayLayer)));
    History replayHistory;
    BasicPixelBrushStroke replay(replayDocument, replayLayerId, settings);
    CHECK(replay.begin(sample(8, 120, 0.3, 0)));
    CHECK(replay.end(sample(184, 18, 1.0, 40000), replayHistory)
        == RasterEditCommitResult::Committed);
    std::vector<std::byte> replayed(original.size());
    replaySurface->copyRgba8({0, 0, 192, 144}, replayed, 192U * 4U);
    CHECK(replayed == painted);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const auto cases = goldenCases();
    const bool update = qEnvironmentVariableIntValue(
        "IMAGEEDITOR_UPDATE_ADVANCED_BRUSH_GOLDENS") == 1;
    QString contactSheetPath;
    for (int index = 1; index + 1 < argc; ++index) {
        if (QString::fromLocal8Bit(argv[index])
            == QStringLiteral("--contact-sheet")) {
            contactSheetPath = QString::fromLocal8Bit(argv[index + 1]);
        }
    }

    ellipseTipGeometryIsStable();
    bitmapTipFiltersAndRotatesAsymmetricMasks();
    bitmapCoverageKeepsExactBlackEmpty();
    preparedMaskSamplingExactlyMatchesTheReference();
    rotationInterpolationUsesTheShortestArc();
    grainIsDocumentAnchoredAndSeeded();
    presetsRoundTripAsVersionedData();
    eventRateAndZoomDoNotChangeDocumentOutput();
    transformedLayersKeepGrainInDocumentSpace();
    dryInkCancelUndoRedoAndReplayAreExact();
    compareOrUpdateGoldens(cases, update);
    if (!contactSheetPath.isEmpty()) {
        writeContactSheet(cases, contactSheetPath);
    }

    if (failures != 0) {
        std::cerr << failures << " advanced brush assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All advanced brush direction tests passed\n";
    return EXIT_SUCCESS;
}
