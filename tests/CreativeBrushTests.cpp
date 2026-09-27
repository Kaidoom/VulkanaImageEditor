#include "imageeditor/core/CreativeBrushes.hpp"
#include "imageeditor/core/BasicPixelBrushEngine.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/ui/BrushAssetLibrary.hpp"
#include "imageeditor/ui/BrushPresetStore.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <iostream>
#include <set>

namespace c = imageeditor::core;
namespace ui = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n'; } } while(false)
c::NormalizedPointerSample point(double x, double y, std::uint64_t time = 0)
{
    return {.documentPosition = {x, y}, .timestampMicroseconds = time,
        .pressure = 1, .pointerType = c::PointerType::Mouse, .buttons = c::PointerButtonPrimary};
}
QImage pixels(const c::ContiguousRasterSurface& surface)
{
    const auto e = surface.extent();
    QImage image(int(e.width), int(e.height), QImage::Format_RGBA8888);
    surface.copyRgba8({0,0,int(e.width),int(e.height)},
        std::span(reinterpret_cast<std::byte*>(image.bits()), std::size_t(image.sizeInBytes())), std::size_t(image.bytesPerLine()));
    return image;
}
QImage paint(c::BrushSettings settings, bool curved = true, int samples = 60, bool erase = false,
    c::Rgba8 background = {0,0,0,0})
{
    constexpr c::Extent2u extent {320, 128};
    c::Document document(c::CanvasSpec{.extent = extent});
    const c::Rgba8 initial = erase ? c::Rgba8{65, 150, 225, 255} : background;
    auto surface = std::make_shared<c::ContiguousRasterSurface>(extent, initial);
    const auto before = pixels(*surface);
    auto layer = c::Layer::raster("Brush review", surface); const auto id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    c::History history;
    c::BasicPixelBrushStroke stroke(document, id, settings,
        erase ? c::BrushCompositeMode::Erase : c::BrushCompositeMode::Paint,
        std::make_unique<c::BasicPixelBrushEngine>());
    CHECK(stroke.valid());
    CHECK(stroke.begin(point(48.5, 64.5)));
    for (int i = 1; i <= samples; ++i) {
        const auto t = double(i) / samples;
        CHECK(stroke.append(point(48.5 + 223 * t, 64.5 + (curved ? 23 * std::sin(t * 6.28318530718) : 0), std::uint64_t(i) * 1000)));
    }
    CHECK(stroke.end(point(271.5, 64.5, std::uint64_t(samples) * 1000), history) == c::RasterEditCommitResult::Committed);
    const auto image = pixels(*surface);
    CHECK(history.undoDepth() == 1);
    CHECK(history.undo(document));
    CHECK(pixels(*surface) == before);
    CHECK(history.redo(document)); CHECK(pixels(*surface) == image);
    c::BasicPixelBrushStroke cancelled(document, id, settings,
        erase ? c::BrushCompositeMode::Erase : c::BrushCompositeMode::Paint,
        std::make_unique<c::BasicPixelBrushEngine>());
    CHECK(cancelled.begin(point(120, 30)));
    CHECK(cancelled.append(point(180, 40, 1000)));
    cancelled.cancel();
    CHECK(pixels(*surface) == image && history.undoDepth() == 1);
    return image;
}
QImage stamp(const c::BrushPresetRecord& preset)
{
    auto tip = c::makeBuiltinBrushTip(preset.settings.tip.assetId);
    CHECK(tip != nullptr);
    QImage image(100,100,QImage::Format_RGBA8888); image.fill(Qt::transparent);
    if (!tip) return image;
    c::BrushDab dab;
    dab.documentCenter = {50.5,50.5}; dab.diameterPixels = preset.displayName == "Precision Ink" ? 3 : 78;
    dab.tipAspectRatio = preset.settings.tip.aspectRatio;
    dab.tipAngleDegrees = preset.settings.tip.angleDegrees;
    dab.deterministicSeed = preset.settings.deterministicSeed;
    const auto bounds = tip->prepareDab(dab, preset.settings.hardness, 1);
    CHECK(!bounds.empty());
    double total = 0;
    for(int y = 0; y < 100; ++y) for(int x = 0; x < 100; ++x) {
        const auto value = tip->coverage({x + .5,y + .5});
        CHECK(std::isfinite(value) && value >= 0 && value <= 1);
        total += value;
        image.setPixelColor(x,y,QColor(225,230,238,int(std::lround(value * 255))));
    }
    CHECK(total > 2);
    CHECK(tip->coverage({bounds.left - 1,bounds.top - 1}) == 0);
    if (preset.displayName == "Precision Ink") {
        CHECK(total == 5); // a genuinely aliased 3 px round cross, not a fuzzy round dab
        return image.copy(44,44,13,13).scaled(100,100,Qt::IgnoreAspectRatio,Qt::FastTransformation);
    }
    return image;
}
void verifyAndSheet(const QString& sheetPath)
{
    const auto presets = c::creativeBrushPresets();
    CHECK(presets.size() == 18);
    QElapsedTimer timer; timer.start();
    const auto cache = c::creativeBrushCacheStats();
    std::cout << "Generated masks: " << cache.grayscaleMaskCount << ", bytes " << cache.retainedBytes
              << ", cold generation " << timer.elapsed() << " ms\n";
    CHECK(cache.grayscaleMaskCount == 15); CHECK(cache.retainedBytes < 16 * 1024 * 1024);
    QImage sheet(1230, 6 * 244 + 56, QImage::Format_RGB32); sheet.fill(QColor("#17191d"));
    QPainter painter(&sheet);
    painter.setPen(QColor("#e9e9ed"));
    painter.drawText(QRect(20,8,1190,36), Qt::AlignVCenter, "Vulkana · original generated brush set | left: tip (Precision ×8), right: default-size mouse stroke");
    std::set<std::string> ids;
    timer.restart();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const auto& preset = presets[i];
        CHECK(ids.insert(preset.id).second);
        const auto encoded = c::serializeBrushPreset(preset);
        CHECK(c::deserializeBrushPreset(encoded) == preset);
        const auto thumbnail = stamp(preset);
        auto settings = preset.settings; settings.foreground = {30, 45, 65, 255};
        const auto image = paint(settings);
        CHECK(paint(settings) == image);
        CHECK(paint(settings, false, 2) == paint(settings, false, 128));
        const auto erased = paint(settings, false, 20, true);
        CHECK(!erased.isNull());
        const auto x = int(i % 3) * 410, y = 56 + int(i / 3) * 244;
        painter.setPen(QColor("#e9e9ed"));
        painter.drawText(QRect(x+12,y,390,28), Qt::AlignVCenter,
            QString::fromStdString(preset.displayName) + QStringLiteral(" · %1 px").arg(settings.sizePixels));
        painter.drawImage(QRect(x+6,y+33,82,82), thumbnail);
        // Composite through the actual linear-light stroke path. QPainter must
        // not introduce its different RGB-space alpha blend into the review.
        painter.drawImage(QPoint(x+88,y+32),paint(settings, true, 60, false, {242,238,230,255}).copy(0,16,320,96));
        settings.foreground = {235,227,211,255};
        painter.drawImage(QPoint(x+88,y+128),paint(settings, true, 60, false, {23,25,29,255}).copy(0,16,320,96));
        painter.setPen(QColor("#666b74"));
        painter.drawText(QRect(x+8,y+124,76,70), Qt::AlignCenter | Qt::TextWordWrap,
            preset.displayName == "Precision Ink" ? "hard pixels" : preset.settings.tip.rotationMode == c::BrushTipRotationMode::Fixed ? "fixed\nangle" : "follows\nstroke");
    }
    painter.end();
    std::cout << "Preset strokes/replay/history checks: " << timer.elapsed() << " ms\n";
    if (!sheetPath.isEmpty()) CHECK(sheet.save(sheetPath));
    CHECK(c::creativeBrushCacheStats().retainedBytes == cache.retainedBytes);
    for (auto name : {"confetti", "smoke", "spark", "grunge", "sponge"}) {
        auto settings = c::findBuiltinBrushPreset(std::string("builtin.preset.") + name + ".v1")->settings;
        const auto original = paint(settings);
        ++settings.deterministicSeed;
        CHECK(paint(settings) != original);
    }
    QTemporaryDir directory;
    ui::BrushPresetStore store(directory.path());
    auto copy = store.saveCopy("Seeded confetti", presets[13].settings, c::builtinBrushPresets());
    CHECK(copy.preset.has_value());
    ui::BrushPresetStore reopened(directory.path());
    const auto loaded = reopened.loadOnce(c::builtinBrushPresets());
    CHECK(loaded.size() == 1 && loaded.front().settings == presets[13].settings);
    // Simulate a user's pre-upgrade preset acquiring the same display name as
    // a new factory recipe: loading the new catalog must not hide their work.
    QTemporaryDir upgradeDirectory;
    ui::BrushPresetStore oldStore(upgradeDirectory.path());
    CHECK(oldStore.saveCopy("Chalk", presets[13].settings, c::builtinBrushPresets().first(5)).preset);
    ui::BrushPresetStore upgraded(upgradeDirectory.path());
    CHECK(upgraded.loadOnce(c::builtinBrushPresets()).size() == 1);
    const auto library = ui::BrushAssetLibrary::createPackaged();
    CHECK(library->diagnostics().empty());
    for (const auto& preset : presets) CHECK(library->prepareSettings(preset.settings));
    const auto components = library->components(c::BrushAssetType::Tip);
    CHECK(components.size() == 26);
    for (std::size_t i = components.size() - 5; i < components.size(); ++i) CHECK(components[i].packaged);
}

void anisotropicCoverage()
{
    // Independent rectangular stripe fields: exact on/off reference under a
    // one-pixel box footprint, not another implementation of the mip sampler.
    for (bool compressedX : {false, true}) {
        const unsigned width = 256, height = compressedX ? 32 : 256;
        std::vector<std::uint8_t> bytes(std::size_t(width) * height);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
            bytes[std::size_t(y) * width + x] = ((compressedX ? y / 4 : x / 16) % 2 == 0) ? 255 : 0;
        const auto mask = std::make_shared<c::GrayscaleMaskAsset>("test.stripes", 1, width, height, bytes);
        c::BitmapMaskTip legacy(mask);
        c::BitmapMaskTip filtered(mask, c::BitmapMaskTip::Filtering::Anisotropic);
        for (double angle : {0., 90., 33.}) {
            c::BrushDab dab; dab.documentCenter = {32,32};
            dab.diameterPixels = compressedX ? 8 : 64;
            dab.tipAspectRatio = compressedX ? 1 : .125;
            dab.tipAngleDegrees = angle;
            (void)legacy.prepareDab(dab, .5, 1);
            const auto bounds = filtered.prepareDab(dab, .5, 1);
            const auto world = [angle](double x, double y) {
                const auto radians = angle * std::acos(-1) / 180;
                return c::Vec2d{32 + x * std::cos(radians) - y * std::sin(radians),
                    32 + x * std::sin(radians) + y * std::cos(radians)};
            };
            const auto white = compressedX ? world(0,.5) : world(2.5,0);
            const auto black = compressedX ? world(0,1.5) : world(6.5,0);
            // The old square mip averages the stripes to half coverage. In the
            // 256x32 case its one-row mip also interpolates with transparent
            // border: .5 * (1 - .5 / 8) = .46875 at this point.
            CHECK(std::abs(legacy.coverage(white) - (compressedX ? .46875 : .5)) < .001);
            CHECK(filtered.coverage(white) > .999);
            CHECK(filtered.coverage(black) < .001);
            CHECK(filtered.coverage({bounds.left-2,bounds.top-2}) == 0);
        }
        c::BrushDab tiny; tiny.diameterPixels = .25; tiny.tipAspectRatio = .02;
        (void)filtered.prepareDab(tiny, .7, 8);
        CHECK(std::isfinite(filtered.coverage({0,0})));
        tiny.diameterPixels = 0;
        CHECK(filtered.prepareDab(tiny, .7, 1).empty());
        CHECK(filtered.coverage({0,0}) == 0);
    }
}

void stampSpacingAndDefaults()
{
    CHECK(c::findBuiltinBrushPreset("builtin.preset.precision-ink.v1")->settings.sizePixels == 3);
    CHECK(c::findBuiltinBrushPreset("builtin.preset.thin-chisel.v1")->settings.sizePixels == 7);
    CHECK(c::findBuiltinBrushPreset("builtin.preset.dry-marker.v1")->settings.sizePixels == 35);
    class Sink final : public c::BrushDabSink {
    public:
        std::vector<c::BrushDab> dabs;
        void emitDab(const c::BrushDab& dab) override { dabs.push_back(dab); }
    } sink;
    auto settings = c::findBuiltinBrushPreset("builtin.preset.star.v1")->settings;
    c::BasicPixelBrushEngine engine;
    CHECK(engine.beginStroke(settings, point(0,0), sink));
    CHECK(engine.endStroke(point(85,0,1000), sink));
    CHECK(sink.dabs.size() == 3); // 0, 40, 80: no overlapping extra release stamp at 85
    for (std::size_t i = 0; i < sink.dabs.size(); ++i) {
        CHECK(sink.dabs[i].documentCenter.x == double(i) * 40);
        CHECK(sink.dabs[i].sequenceIndex == i);
    }
    sink.dabs.clear();
    CHECK(engine.beginStroke(settings, point(8,8), sink));
    CHECK(engine.endStroke(point(8,8), sink));
    CHECK(sink.dabs.size() == 1 && sink.dabs.front().sequenceIndex == 0);
    auto cells = c::makeBuiltinBrushTip("builtin.tip.generated.cells.v1");
    c::BrushDab dab; dab.documentCenter = {50,50}; dab.diameterPixels = 100;
    (void)cells->prepareDab(dab, 1, 1);
    CHECK(cells->coverage({50,50}) == 0); // honeycomb holes remain empty
}

void materialProfiles()
{
    for (const auto* name : {"hatch-pen", "acrylic", "bristles", "oils", "chalk", "charcoal"}) {
        const auto settings = c::findBuiltinBrushPreset(std::string("builtin.preset.") + name + ".v1")->settings;
        const auto rendered = paint(settings, false);
        int peak = 0, solid = 0, covered = 0;
        for (int y = 16; y < 112; ++y) {
            const auto alpha = rendered.pixelColor(160,y).alpha();
            peak = std::max(peak, alpha); solid += alpha >= 240; covered += alpha > 0;
        }
        std::cout << name << " profile peak=" << peak << " solid=" << solid << " covered=" << covered << " :";
        std::cout << '\n';
        if (std::string_view(name) == "acrylic") {
            CHECK(settings.spacingPercent == 3);
            CHECK(peak == 255 && solid >= 20);
        }
        if (std::string_view(name) == "hatch-pen") {
            CHECK(settings.tip.rotationMode == c::BrushTipRotationMode::FollowStrokeDirection);
            // Every column of the interior stroke retains five separate tracks,
            // instead of intermittent diagonal stamps or a filled ribbon.
            for (int x = 80; x < 240; ++x) {
                int runs = 0; bool previous = false;
                for (int y = 0; y < rendered.height(); ++y) {
                    const bool ink = rendered.pixelColor(x,y).alpha() >= 128;
                    runs += ink && !previous; previous = ink;
                }
                CHECK(runs == 5);
            }
        }
    }
    for (double size : {21., 42., 84.}) {
        auto settings = c::findBuiltinBrushPreset("builtin.preset.bristles.v1")->settings;
        settings.sizePixels = size;
        settings.foreground = {240,25,35,255};
        const auto image = paint(settings, false);
        int peaks = 0, deepGaps = 0;
        for (int y = 1; y + 1 < image.height(); ++y) {
            const auto alpha = image.pixelColor(160,y).alpha();
            if (alpha > 180 && alpha > image.pixelColor(160,y-1).alpha()
                && alpha >= image.pixelColor(160,y+1).alpha()) ++peaks;
            if (y > 64 - int(size * .4) && y < 64 + int(size * .4) && alpha < 80) ++deepGaps;
        }
        std::cout << "Bristles " << size << " px: peaks=" << peaks << ", clear-gap pixels=" << deepGaps << '\n';
        CHECK(peaks >= 7 && deepGaps >= 2);
        // The brush changes coverage, never desaturates the stored straight RGB.
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x,y);
            if (pixel.alpha() > 0) CHECK(pixel.red() == 240 && pixel.green() == 25 && pixel.blue() == 35);
        }
        const auto onWhite = paint(settings, false, 60, false, {255,255,255,255});
        for (int y = 0; y < image.height(); ++y) {
            // Stored alpha is quantized: 255 also represents coverage just
            // below 1. Bound the independent linear-light reference using the
            // half-byte interval, rather than assuming every 255 is exactly 1.
            const auto a = image.pixelColor(160,y).alpha();
            const auto low = std::max(0., (a - .5) / 255);
            const auto high = std::min(1., (a + .5) / 255);
            const auto srgb = [](double v) { return v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v,1/2.4) - .055; };
            const auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v+.055)/1.055,2.4); };
            for (int channel = 0; channel < 3; ++channel) {
                const int source[] {240,25,35};
                const auto actual = onWhite.constScanLine(y)[160 * 4 + channel];
                const auto value = linear(source[channel] / 255.);
                const auto minimum = std::lround(255 * srgb(value * high + 1 - high));
                const auto maximum = std::lround(255 * srgb(value * low + 1 - low));
                CHECK(actual >= minimum && actual <= maximum);
            }
        }
    }
}

void refinementSheet(const QString& path)
{
    if (path.isEmpty()) return;
    QImage sheet(1010, 950, QImage::Format_RGB32); sheet.fill(Qt::white);
    QPainter painter(&sheet); painter.setPen(QColor("#202028"));
    painter.drawText(QRect(12,6,980,28), Qt::AlignVCenter, "Native pixels · full-pressure red paint on white · 21 px / 42 px / 84 px");
    int row = 0;
    for (const auto* name : {"hatch-pen", "acrylic", "bristles", "oils", "chalk", "charcoal"}) {
        const auto* preset = c::findBuiltinBrushPreset(std::string("builtin.preset.") + name + ".v1");
        painter.drawText(QRect(12,36+row*152,980,24), Qt::AlignVCenter, QString::fromStdString(preset->displayName));
        int col = 0;
        for (double size : {21.,42.,84.}) {
            auto settings = preset->settings;
            settings.sizePixels = size; settings.foreground = {240,25,35,255};
            painter.drawImage(QPoint(10+col*335,58+row*152), paint(settings, false, 60, false, {255,255,255,255}));
            ++col;
        }
        ++row;
    }
    painter.end(); CHECK(sheet.save(path));
}

void seedControlsAndShelfOrdering()
{
    const auto library = ui::BrushAssetLibrary::createPackaged();
    ui::PropertiesPanel panel(library);
    panel.setActiveTool(c::ToolId::Brush);
    const auto builtins = c::builtinBrushPresets();
    std::vector<c::BrushPresetRecord> catalog(builtins.begin(), builtins.end());
    for (const auto& item : library->components(c::BrushAssetType::Tip)) if (item.packaged) {
        c::BrushSettings settings;
        settings.tip.assetId = item.id;
        catalog.push_back({"builtin.preset.asset." + item.id, item.displayName.toStdString(), settings});
    }
    catalog.push_back({"user.example", "Chalk", builtins[5].settings});
    panel.setBrushPresets(catalog);
    panel.resize(600, 1000); panel.show();
    auto* grid = panel.findChild<QListWidget*>("BrushPresetGrid");
    auto* seed = panel.findChild<QLineEdit*>("BrushVariationSeed");
    auto* reseed = panel.findChild<QPushButton*>("BrushReseedButton");
    CHECK(grid && seed && reseed);
    if (!grid || !seed || !reseed) return;
    CHECK(grid->count() == 29);
    CHECK(grid->item(23)->data(Qt::UserRole).toString() == "user.example");
    CHECK(grid->item(23)->text() == "Chalk (User)");
    for (int i = 24; i < 29; ++i)
        CHECK(grid->item(i)->data(Qt::UserRole).toString().startsWith("builtin.preset.asset."));
    c::BrushSettings published;
    int changes = 0;
    panel.onBrushSettingsChanged = [&](const c::BrushSettings& settings) { published = settings; ++changes; };
    for (int i = 0; i < grid->count(); ++i)
        if (grid->item(i)->data(Qt::UserRole).toString() == "builtin.preset.confetti.v1") grid->setCurrentRow(i);
    CHECK(changes == 1);
    const auto originalSeed = published.deterministicSeed;
    const auto originalColor = published.foreground;
    seed->setFocus(); seed->setText("18446744073709551615");
    QTest::keyClick(seed, Qt::Key_Return);
    CHECK(changes == 2 && published.deterministicSeed == UINT64_MAX);
    CHECK(panel.brushPresetModified());
    seed->setText("18446744073709551616"); // overflow restores, never clamps or rounds
    QTest::keyClick(seed, Qt::Key_Return);
    CHECK(changes == 2 && seed->text() == "18446744073709551615");
    seed->setText("42"); QTest::keyClick(seed, Qt::Key_Escape);
    CHECK(changes == 2 && seed->text() == "18446744073709551615");
    reseed->click();
    CHECK(changes == 3 && seed->text().toULongLong() == published.deterministicSeed);
    CHECK(published.foreground == originalColor);
    CHECK(panel.resetBrushPreset());
    CHECK(published.deterministicSeed == originalSeed && !panel.brushPresetModified());
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    verifyAndSheet(app.arguments().size() >= 2 ? app.arguments()[1] : QString{});
    stampSpacingAndDefaults();
    seedControlsAndShelfOrdering();
    materialProfiles();
    anisotropicCoverage();
    refinementSheet(app.arguments().size() >= 3 ? app.arguments()[2] : QString{});
    return failures ? 1 : 0;
}
