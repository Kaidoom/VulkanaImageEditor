#include "imageeditor/core/BrushAssetRegistry.hpp"
#include "imageeditor/core/BrushTip.hpp"
#include "imageeditor/ui/BrushAssetLibrary.hpp"
#include "imageeditor/ui/BrushComponentPicker.hpp"
#include "imageeditor/ui/BrushPresetStore.hpp"
#include "imageeditor/ui/BrushPresetGrid.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace imageeditor;

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kTaperedClaw = "builtin.tip.bitmap.tapered-claw.v1";
constexpr auto kFinePaper = "builtin.grain.paper-breakup-fine.v1";

void transparentTipsUseAlphaAndKeepTonalDetail()
{
    const auto library = ui::BrushAssetLibrary::createPackaged();
    int tips = 0;
    for (const auto& record : library->registry().assets()) {
        if (record.type != core::BrushAssetType::Tip) continue;
        ++tips;
        CHECK(record.revision == 2);
        CHECK(record.coverageChannel == core::BrushCoverageChannel::LuminanceTimesAlpha);
        const bool angled = record.id == "builtin.tip.bitmap.parallel-bristle-broad.v1"
            || record.id == "builtin.tip.bitmap.tapered-claw.v1"
            || record.id == "builtin.tip.bitmap.four-tooth-rake.v1";
        CHECK(record.defaultRotationDegrees == (angled ? 45.0 : 0.0));
        CHECK(library->component(record.id)->defaultRotationDegrees == record.defaultRotationDegrees);
        const auto image = QImage(QStringLiteral(":/imageeditor/brush/v1/")
            + QString::fromStdString(record.relativePackagedPath)).convertToFormat(QImage::Format_RGBA8888);
        CHECK(!image.isNull() && image.hasAlphaChannel());
        CHECK(library->prepareAsset(record.id, core::BrushAssetType::Tip));
        core::BrushTipDescriptor descriptor;
        descriptor.assetId = record.id;
        const auto tip = library->createTip(descriptor);
        const auto* bitmap = dynamic_cast<core::BitmapMaskTip*>(tip.get());
        CHECK(bitmap && bitmap->mask());
        if (image.isNull() || !bitmap || !bitmap->mask()) continue;
        std::size_t transparent = 0, soft = 0;
        bool exact = true;
        for (int y = 0; y < image.height(); ++y) {
            const auto* row = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const auto* p = row + x * 4;
                const auto luma = (54U*p[0] + 183U*p[1] + 19U*p[2] + 128U) >> 8U;
                const auto coverage = (luma*p[3] + 127U) / 255U;
                const auto actual = bitmap->mask()->sample((x + .5)/image.width(), (y + .5)/image.height(), 0, false);
                exact &= std::abs(actual - double(coverage)/255.0) < 1e-7;
                if (!p[3]) {
                    ++transparent;
                    exact &= actual == 0;
                }
                soft += coverage > 0 && coverage < 255;
            }
        }
        CHECK(exact && transparent > 100000 && soft > 1000);
    }
    CHECK(tips == 5);
}

void packagedRegistryIsCompleteAndWorkingDirectoryIndependent()
{
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    const auto previous = QDir::currentPath();
    CHECK(QDir::setCurrent(temporary.path()));
    const auto library = ui::BrushAssetLibrary::createPackaged();
    CHECK(QDir::setCurrent(previous));

    CHECK(library != nullptr);
    CHECK(library->diagnostics().empty());
    CHECK(library->registry().version()
        == core::BrushAssetRegistry::SupportedVersion);
    CHECK(library->registry().assets().size() == 14);

    const auto tips = library->components(core::BrushAssetType::Tip);
    const auto grains = library->components(core::BrushAssetType::Grain);
    CHECK(tips.size() == 26); // original eight plus eighteen generated tips
    CHECK(grains.size() == 11);
    CHECK(std::all_of(tips.begin(), tips.end(),
        [](const ui::BrushComponentItem& item) {
            return item.available && !item.thumbnail.isNull();
        }));
    CHECK(std::all_of(grains.begin(), grains.end(),
        [](const ui::BrushComponentItem& item) {
            return item.available && !item.thumbnail.isNull();
        }));

    const auto grain = std::find_if(grains.begin(), grains.end(),
        [](const ui::BrushComponentItem& item) { return item.packaged; });
    CHECK(grain != grains.end());
    if (grain != grains.end()) {
        // The preview is deliberately a 2x2 tile so a visible seam cannot be
        // hidden by showing just one source image.
        CHECK(grain->thumbnail.pixelColor(9, 9)
            == grain->thumbnail.pixelColor(41, 41));
    }
}

void assetsPrepareOnceAndNeverLoadThroughTheResolver()
{
    const auto library = ui::BrushAssetLibrary::createPackaged();
    const auto startup = library->runtimeStats();
    CHECK(startup.resourceReads == 14);
    CHECK(startup.imageDecodes == 14);
    CHECK(startup.mipBuilds == 0);

    QString error;
    CHECK(library->prepareAsset(kTaperedClaw,
        core::BrushAssetType::Tip, &error));
    CHECK(error.isEmpty());
    CHECK(library->prepareAsset(kFinePaper,
        core::BrushAssetType::Grain, &error));
    const auto prepared = library->runtimeStats();
    CHECK(prepared.resourceReads == startup.resourceReads);
    // Thumbnail generation and brush preparation share the same canonical
    // R8 decode; selection only promotes it into the mipmapped core cache.
    CHECK(prepared.imageDecodes == startup.imageDecodes);
    CHECK(prepared.mipBuilds == 2);
    CHECK(prepared.residentMaskCount == 2);

    CHECK(library->prepareAsset(kTaperedClaw,
        core::BrushAssetType::Tip, &error));
    CHECK(library->prepareAsset(kFinePaper,
        core::BrushAssetType::Grain, &error));
    CHECK(library->runtimeStats() == prepared);

    core::BrushTipDescriptor tipDescriptor;
    tipDescriptor.assetId = kTaperedClaw;
    core::BrushGrainDescriptor grainDescriptor;
    grainDescriptor.assetId = kFinePaper;
    auto firstTip = library->createTip(tipDescriptor);
    auto secondTip = library->createTip(tipDescriptor);
    auto grain = library->createGrain(grainDescriptor);
    CHECK(firstTip != nullptr);
    CHECK(secondTip != nullptr);
    CHECK(grain != nullptr);
    const auto* firstBitmap = dynamic_cast<core::BitmapMaskTip*>(firstTip.get());
    const auto* secondBitmap = dynamic_cast<core::BitmapMaskTip*>(secondTip.get());
    CHECK(firstBitmap != nullptr);
    CHECK(secondBitmap != nullptr);
    CHECK(firstBitmap && secondBitmap
        && firstBitmap->mask() == secondBitmap->mask());
    const auto afterResolution = library->runtimeStats();
    CHECK(afterResolution.resourceReads == prepared.resourceReads);
    CHECK(afterResolution.imageDecodes == prepared.imageDecodes);
    CHECK(afterResolution.mipBuilds == prepared.mipBuilds);

    tipDescriptor.assetId = "missing.tip.v1";
    CHECK(library->createTip(tipDescriptor) == nullptr);
    CHECK(!library->prepareAsset("missing.tip.v1",
        core::BrushAssetType::Tip, &error));
    CHECK(error.contains(QStringLiteral("Unknown tip asset")));
}

void userPresetCopiesAreAtomicImmutableAndLoadedOnlyOnce()
{
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    ui::BrushPresetStore store(temporary.path());
    const auto builtins = core::builtinBrushPresets();
    CHECK(store.loadOnce(builtins).empty());

    auto settings = core::proceduralBrushPreset(
        core::ProceduralBrushPreset::PressureRound);
    settings.tip.assetId = kTaperedClaw;
    settings.grain.assetId = kFinePaper;
    settings.grain.strength = 0.6;
    const auto saved = store.saveCopy(QStringLiteral("Studio Ink"),
        settings, builtins);
    CHECK(saved.preset.has_value());
    CHECK(saved.error.isEmpty());
    CHECK(saved.preset && saved.preset->id.starts_with("user.preset."));
    CHECK(core::findBuiltinBrushPreset(saved.preset->id) == nullptr);

    std::vector<core::BrushPresetRecord> existing {
        builtins.begin(), builtins.end()};
    if (saved.preset) {
        existing.push_back(*saved.preset);
    }
    const auto duplicate = store.saveCopy(QStringLiteral(" studio ink "),
        settings, existing);
    CHECK(!duplicate.preset);
    CHECK(duplicate.error.contains(QStringLiteral("already exists")));
    CHECK(!store.saveCopy(QStringLiteral("  "), settings, existing).preset);
    const auto staleCatalogDuplicate = store.saveCopy(
        QStringLiteral("Studio Ink"), settings, builtins);
    CHECK(!staleCatalogDuplicate.preset);
    CHECK(staleCatalogDuplicate.error.contains(
        QStringLiteral("already exists")));

    QDir directory(temporary.path());
    CHECK(directory.entryList({QStringLiteral("*.iebrush")},
        QDir::Files).size() == 1);
    CHECK(directory.entryList({QStringLiteral("*.tmp")},
        QDir::Files).empty());

    QFile corrupt(directory.filePath(QStringLiteral("corrupt.iebrush")));
    CHECK(corrupt.open(QIODevice::WriteOnly));
    CHECK(corrupt.write("not a brush preset\n") > 0);
    corrupt.close();

    ui::BrushPresetStore restarted(temporary.path());
    QStringList diagnostics;
    const auto loaded = restarted.loadOnce(builtins, &diagnostics);
    CHECK(diagnostics.size() == 1);
    CHECK(diagnostics.front().contains(QStringLiteral("corrupt.iebrush")));
    CHECK(loaded.size() == 1);
    CHECK(loaded.size() == 1 && saved.preset
        && loaded.front() == *saved.preset);
    CHECK(restarted.loadOnce(builtins).size() == 1);

    QTemporaryDir blockedRoot;
    QFile blocker(blockedRoot.filePath(QStringLiteral("not-a-directory")));
    CHECK(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    ui::BrushPresetStore blocked(blocker.fileName());
    const auto failedWrite = blocked.saveCopy(QStringLiteral("Cannot Save"),
        settings, builtins);
    CHECK(!failedWrite.preset);
    CHECK(failedWrite.error.contains(QStringLiteral("directory")));

    // Saving before an explicit load still performs the one startup scan and
    // must not duplicate the saved record when loadOnce is called later.
    QTemporaryDir saveFirstDirectory;
    ui::BrushPresetStore saveFirst(saveFirstDirectory.path());
    const auto saveFirstResult = saveFirst.saveCopy(
        QStringLiteral("Save First"), settings, builtins);
    CHECK(saveFirstResult.preset.has_value());
    const auto afterSaveLoad = saveFirst.loadOnce(builtins);
    CHECK(afterSaveLoad.size() == 1);
    CHECK(afterSaveLoad.size() == 1 && saveFirstResult.preset
        && afterSaveLoad.front() == *saveFirstResult.preset);
}

void missingAssetIdsSurvivePresetRestartAndRemainUnavailable()
{
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    auto record = core::builtinBrushPresets().front();
    record.id = "user.preset.missing-assets.v1";
    record.displayName = "Missing Asset Review";
    record.settings.tip.assetId = "missing.tip.saved.v1";
    record.settings.grain.assetId = "missing.grain.saved.v1";
    const auto encoded = core::serializeBrushPreset(record);
    CHECK(!encoded.empty());

    QFile file(QDir(temporary.path()).filePath(
        QStringLiteral("missing-assets.iebrush")));
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(encoded.data(), static_cast<qint64>(encoded.size()))
        == static_cast<qint64>(encoded.size()));
    file.close();

    ui::BrushPresetStore restarted(temporary.path());
    const auto builtins = core::builtinBrushPresets();
    QStringList diagnostics;
    const auto loaded = restarted.loadOnce(builtins, &diagnostics);
    CHECK(diagnostics.empty());
    CHECK(loaded.size() == 1);
    CHECK(loaded.size() == 1
        && loaded.front().settings.tip.assetId == "missing.tip.saved.v1");
    CHECK(loaded.size() == 1
        && loaded.front().settings.grain.assetId == "missing.grain.saved.v1");

    auto catalog = std::vector<core::BrushPresetRecord> {
        builtins.begin(), builtins.end()};
    catalog.insert(catalog.end(), loaded.begin(), loaded.end());
    ui::PropertiesPanel panel(ui::BrushAssetLibrary::createPackaged());
    panel.setActiveTool(core::ToolId::Brush);
    panel.setBrushPresets(std::move(catalog));
    auto* grid = dynamic_cast<ui::BrushPresetGrid*>(
        panel.findChild<QListWidget*>(QStringLiteral("BrushPresetGrid")));
    CHECK(grid && grid->count() == 24);
    CHECK(grid && !grid->item(23)->flags().testFlag(Qt::ItemIsEnabled));
    CHECK(grid && grid->item(23)->toolTip().contains(
        QStringLiteral("Unavailable")));
}

void propertiesRetainPresetIdentityAcrossModificationResetAndSave()
{
    const auto library = ui::BrushAssetLibrary::createPackaged();
    ui::PropertiesPanel panel(library);
    panel.setActiveTool(core::ToolId::Brush);
    CHECK(!panel.brushPresetModified());

    auto pressure = core::proceduralBrushPreset(
        core::ProceduralBrushPreset::PressureRound);
    pressure.foreground = {12, 34, 56, 78};
    panel.setBrushSettings(pressure);
    CHECK(!panel.brushPresetModified());

    core::BrushSettings published;
    int changes = 0;
    panel.onBrushSettingsChanged = [&](const core::BrushSettings& settings) {
        published = settings;
        ++changes;
    };
    auto modified = pressure;
    modified.tip.aspectRatio = 0.51;
    panel.setBrushSettings(modified);
    CHECK(panel.brushPresetModified());
    CHECK(panel.resetBrushPreset());
    CHECK(!panel.brushPresetModified());
    CHECK(published.tip.aspectRatio == 1.0);
    changes = 0;
    QString error;
    CHECK(panel.selectBrushComponent(core::BrushAssetType::Tip,
        kTaperedClaw, &error));
    CHECK(panel.selectBrushComponent(core::BrushAssetType::Grain,
        kFinePaper, &error));
    CHECK(panel.brushPresetModified());
    CHECK(published.tip.assetId == kTaperedClaw);
    CHECK(published.grain.assetId == kFinePaper);
    CHECK(changes == 2);

    auto* presetGrid = dynamic_cast<ui::BrushPresetGrid*>(
        panel.findChild<QListWidget*>(QStringLiteral("BrushPresetGrid")));
    auto* reset = panel.findChild<QPushButton*>(
        QStringLiteral("BrushPresetResetButton"));
    auto* grainGrid = panel.findChild<QListWidget*>(
        QStringLiteral("BrushGrainPickerGrid"));
    CHECK(presetGrid != nullptr && presetGrid->count() == 23);
    CHECK(reset != nullptr && reset->isEnabled());
    CHECK(panel.findChild<QListWidget*>(
        QStringLiteral("BrushTipPickerGrid")) == nullptr);
    CHECK(grainGrid != nullptr && grainGrid->count() == 11);
    CHECK(presetGrid && presetGrid->currentItem()
        && presetGrid->currentItem()->toolTip().contains(
            QStringLiteral("Modified")));

    CHECK(panel.resetBrushPreset());
    CHECK(!panel.brushPresetModified());
    CHECK(published.tip.assetId == core::BrushAssetIds::ProceduralRoundTip);
    CHECK(published.grain.assetId == core::BrushAssetIds::NoGrain);
    CHECK(published.foreground == core::Rgba8({12, 34, 56, 78}));

    QTemporaryDir temporary;
    ui::BrushPresetStore store(temporary.path());
    std::vector<core::BrushPresetRecord> catalog {
        core::builtinBrushPresets().begin(), core::builtinBrushPresets().end()};
    panel.onSaveBrushCopyRequested = [&](const QString& name,
                                            const core::BrushSettings& settings) {
        auto result = store.saveCopy(name, settings, catalog);
        if (result.preset) {
            catalog.push_back(*result.preset);
        }
        return result;
    };
    CHECK(panel.selectBrushComponent(core::BrushAssetType::Tip,
        kTaperedClaw, &error));
    const auto copy = panel.saveBrushCopy(QStringLiteral("Tapered Review"));
    CHECK(copy.preset.has_value());
    CHECK(!panel.brushPresetModified());
    CHECK(presetGrid && presetGrid->currentPresetId().starts_with(
        "user.preset."));
    const auto duplicate = panel.saveBrushCopy(QStringLiteral("tapered review"));
    CHECK(!duplicate.preset);
    CHECK(!panel.brushPresetModified());

    auto unavailable = core::builtinBrushPresets().front();
    unavailable.id = "user.preset.missing-component.v1";
    unavailable.displayName = "Missing Component";
    unavailable.settings.tip.assetId = "missing.tip.v1";
    auto presets = std::vector<core::BrushPresetRecord> {
        core::builtinBrushPresets().begin(), core::builtinBrushPresets().end()};
    presets.push_back(unavailable);
    panel.setBrushPresets(std::move(presets));
    CHECK(presetGrid && presetGrid->count() == 24);
    CHECK(presetGrid
        && !presetGrid->item(23)->flags().testFlag(Qt::ItemIsEnabled));
    CHECK(presetGrid && presetGrid->item(23)->toolTip().contains(
        QStringLiteral("Unavailable")));
}

void componentPopupSupportsKeyboardSelectionAndEscape()
{
    const auto library = ui::BrushAssetLibrary::createPackaged();
    ui::BrushComponentPicker picker(core::BrushAssetType::Grain);
    picker.setItems(library->components(core::BrushAssetType::Grain));
    picker.setCurrentAssetId(core::BrushAssetIds::NoGrain);
    picker.resize(310, 54);
    picker.show();
    QCoreApplication::processEvents();

    auto* grid = picker.findChild<QListWidget*>(
        QStringLiteral("BrushGrainPickerGrid"));
    auto* menu = picker.findChild<QMenu*>();
    CHECK(grid != nullptr);
    CHECK(menu != nullptr);
    int activations = 0;
    std::string selected;
    picker.onAssetSelected = [&](const std::string& id) {
        ++activations;
        selected = id;
    };

    picker.openPopup();
    QCoreApplication::processEvents();
    CHECK(menu && menu->isVisible());
    CHECK(grid && grid->hasFocus());
    CHECK(grid && grid->currentRow() >= 0);
    if (grid) {
        QTest::keyClick(grid, Qt::Key_Down);
        const auto expected = grid->currentItem()
            ? grid->currentItem()->data(Qt::UserRole).toString().toStdString()
            : std::string {};
        CHECK(expected != core::BrushAssetIds::NoGrain);
        QTest::keyClick(grid, Qt::Key_Return);
        QCoreApplication::processEvents();
        CHECK(selected == expected);
    }
    QCoreApplication::processEvents();
    CHECK(activations == 1);
    CHECK(!selected.empty());
    CHECK(menu && !menu->isVisible());

    picker.openPopup();
    QCoreApplication::processEvents();
    CHECK(menu && menu->isVisible());
    if (grid) {
        QTest::keyClick(grid, Qt::Key_Escape);
    }
    QCoreApplication::processEvents();
    CHECK(menu && !menu->isVisible());
    CHECK(activations == 1);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    transparentTipsUseAlphaAndKeepTonalDetail();
    packagedRegistryIsCompleteAndWorkingDirectoryIndependent();
    assetsPrepareOnceAndNeverLoadThroughTheResolver();
    userPresetCopiesAreAtomicImmutableAndLoadedOnlyOnce();
    missingAssetIdsSurvivePresetRestartAndRemainUnavailable();
    propertiesRetainPresetIdentityAcrossModificationResetAndSave();
    componentPopupSupportsKeyboardSelectionAndEscape();

    if (failures != 0) {
        std::cerr << failures << " brush asset runtime assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All brush asset runtime tests passed\n";
    return EXIT_SUCCESS;
}
