#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include "imageeditor/ui/ColorDialog.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QScrollBar>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

namespace {
using namespace imageeditor::ui;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

QColor at(const ThemeColors& colors, ThemeColor role)
{
    return colors.at(static_cast<std::size_t>(role));
}
void set(ThemeSettings& settings, ThemeColor role, QColor color)
{
    settings.custom.at(static_cast<std::size_t>(role)) = color;
}
double luminance(QColor color)
{
    const auto linear = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF())
        + 0.0722 * linear(color.blueF());
}
double contrast(QColor foreground, QColor background)
{
    const auto first = luminance(foreground), second = luminance(background);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

void defaultsPreserveTheApprovedDarkTheme(QApplication& application)
{
    const ThemeSettings defaults;
    const ThemeColors approved {QColor("#202020"), QColor("#141414"), QColor("#272727"),
        QColor("#E8EAF0"), QColor("#9299AA"), QColor("#FF8D54"), QColor("#623621"),
        QColor("#E37E4B"), QColor("#FFFFFF"), QColor("#494949"), QColor("#393C42"), QColor("#888A90"),
        QColor("#777A80"), QColor("#B6BDCC"), QColor("#272727"), QColor("#141414")};
    CHECK(defaults.preset == ThemePreset::Dark);
    CHECK(darkThemeColors() == approved);
    CHECK(defaults.custom == darkThemeColors());
    CHECK(resolvedThemeColors(defaults) == darkThemeColors());
    applyEditorTheme(application, defaults);
    CHECK(currentThemeSettings() == defaults);
    const auto palette = application.palette();
    CHECK(palette.color(QPalette::Window) == QColor("#202020"));
    CHECK(palette.color(QPalette::Text) == QColor("#E8EAF0"));
    CHECK(palette.color(QPalette::Highlight) == QColor("#FF8D54"));
    CHECK(palette.color(QPalette::HighlightedText) == QColor("#FFFFFF"));
    CHECK(editorAccent() == imageeditor::core::Rgba8({255, 141, 84, 255}));
    CHECK(themeColor(ThemeColor::Canvas) == QColor("#393C42"));
    CHECK(themeColor(ThemeColor::Selection) == QColor("#623621"));
    CHECK(themeColor(ThemeColor::LayerSelection) == QColor("#E37E4B"));
    CHECK(themeColor(ThemeColor::LayerSelection) != themeColor(ThemeColor::Selection));
    CHECK(themeColor(ThemeColor::Thumbnail) == QColor("#B6BDCC"));
    CHECK(application.styleSheet().contains(QStringLiteral("QToolButton")));
    CHECK(application.styleSheet().contains(QStringLiteral("border-radius: 7px")));
    CHECK(application.styleSheet().contains(QStringLiteral("#FF8D54"), Qt::CaseInsensitive));
    // The new preset must retain the saved custom palette's shade adaptation.
    // Using the new colors as the legacy anchors would leave old blue chrome.
    for (const auto& [legacy, expected] : std::vector<std::pair<const char*, const char*>> {
             {"#151820", "#202020"}, {"#101219", "#1A1A1A"},
             {"#171A22", "#141414"}, {"#222630", "#272727"},
             {"#242936", "#2A2A2A"}, {"#6577F3", "#FF8D54"},
             {"#7C8BFA", "#FFA067"}, {"#8D9AF8", "#FFAD74"},
             {"#34405E", "#623621"}, {"#3A4665", "#683C27"},
             {"#343A49", "#494949"}, {"#0E1013", "#393C42"}})
        CHECK(themeTone(legacy) == QColor(expected));
    const auto darkSheet = application.styleSheet();
    ThemeSettings matchingCustom;
    matchingCustom.preset = ThemePreset::Custom;
    matchingCustom.custom = approved;
    applyEditorTheme(application, matchingCustom);
    CHECK(application.palette() == palette);
    CHECK(application.styleSheet() == darkSheet);
    applyEditorTheme(application, defaults);
    std::set<QString> keys;
    for (std::size_t i = 0; i < static_cast<std::size_t>(ThemeColor::Count); ++i) {
        const auto role = static_cast<ThemeColor>(i);
        CHECK(themeColor(role).isValid());
        CHECK(themeColor(role).alpha() == 255);
        CHECK(!QString::fromLatin1(themeColorLabel(role)).isEmpty());
        CHECK(keys.insert(QString::fromLatin1(themeColorKey(role))).second);
    }
}

void lightThemeHasReadableSharedPaletteRoles(QApplication& application)
{
    ThemeSettings light;
    light.preset = ThemePreset::Light;
    const auto* style = application.style();
    const auto font = application.font();
    applyEditorTheme(application, light);
    CHECK(application.style() == style);
    CHECK(application.font() == font);
    CHECK(currentThemeSettings() == light);
    CHECK(resolvedThemeColors(light) == lightThemeColors());
    const auto colors = lightThemeColors();
    CHECK(at(colors, ThemeColor::LayerSelection) == at(colors, ThemeColor::Selection));
    CHECK(at(colors, ThemeColor::Thumbnail) == QColor("#596273"));
    CHECK(luminance(at(colors, ThemeColor::Background)) > 0.6);
    for (const auto background : {ThemeColor::Background, ThemeColor::Surface, ThemeColor::Control}) {
        CHECK(contrast(at(colors, ThemeColor::Text), at(colors, background)) >= 7.0);
        CHECK(contrast(at(colors, ThemeColor::SecondaryText), at(colors, background)) >= 4.5);
    }
    CHECK(contrast(at(colors, ThemeColor::SelectedText), at(colors, ThemeColor::Selection)) >= 7.0);
    const auto palette = application.palette();
    CHECK(palette.color(QPalette::Window) == at(colors, ThemeColor::Background));
    CHECK(palette.color(QPalette::Text) == at(colors, ThemeColor::Text));
    CHECK(palette.color(QPalette::ButtonText) == at(colors, ThemeColor::Text));
    CHECK(palette.color(QPalette::Highlight) == at(colors, ThemeColor::Accent));
    CHECK(contrast(palette.color(QPalette::ToolTipText), palette.color(QPalette::ToolTipBase)) >= 4.5);
    CHECK(themeTone("#151820") != QColor("#151820"));
    CHECK(application.styleSheet().contains(at(colors, ThemeColor::Background).name(), Qt::CaseInsensitive));
}

void darkVariationsAreDistinctReadableAndPersistByStableKey()
{
    QTemporaryDir directory;
    QSettings store(directory.filePath("presets.ini"),QSettings::IniFormat);
    const auto catalog=themePresets();
    CHECK(catalog.size()==13);
    CHECK(catalog[0].preset==ThemePreset::Dark);
    CHECK(catalog[1].preset==ThemePreset::Light);
    CHECK(catalog.back().preset==ThemePreset::Custom);
    std::set<QString> keys;
    std::set<QRgb> accents, backgrounds;
    int variations=0;
    for(const auto& info:catalog) {
        CHECK(keys.insert(QString::fromLatin1(info.key)).second);
        ThemeSettings settings;
        settings.preset=info.preset;
        settings.custom[std::size_t(ThemeColor::Accent)]=QColor("#123456");
        CHECK(saveThemeSettings(store,settings));
        CHECK(store.value("preferences/theme/preset").toString()==QString::fromLatin1(info.key));
        CHECK(loadThemeSettings(store)==settings);
        if(info.preset==ThemePreset::Dark || info.preset==ThemePreset::Light || info.preset==ThemePreset::Custom)continue;
        ++variations;
        const auto colors=resolvedThemeColors(settings);
        applyEditorTheme(*qApp,settings);
        CHECK(currentThemeSettings()==settings);
        CHECK(qApp->palette().color(QPalette::Highlight)==at(colors,ThemeColor::Accent));
        CHECK(themeColor(ThemeColor::DocumentTabActive)==at(colors,ThemeColor::DocumentTabActive));
        CHECK(accents.insert(at(colors,ThemeColor::Accent).rgb()).second);
        CHECK(backgrounds.insert(at(colors,ThemeColor::Background).rgb()).second);
        for(const auto& c:colors) CHECK(c.isValid() && c.alpha()==255);
        CHECK(luminance(at(colors,ThemeColor::Background))<.1);
        for(const auto role:{ThemeColor::Background,ThemeColor::Surface,ThemeColor::Control}) {
            CHECK(contrast(at(colors,ThemeColor::Text),at(colors,role))>=7);
            CHECK(contrast(at(colors,ThemeColor::SecondaryText),at(colors,role))>=4.5);
        }
        CHECK(contrast(at(colors,ThemeColor::SelectedText),at(colors,ThemeColor::Selection))>=4.5);
        settings.custom=colors;settings.preset=ThemePreset::Custom;
        CHECK(saveThemeSettings(store,settings));CHECK(loadThemeSettings(store)==settings);
    }
    CHECK(variations==10);
    store.setValue("preferences/theme/preset","future-preset");
    CHECK(loadThemeSettings(store).preset==ThemePreset::Dark);
}

void unselectedFileRowsDoNotUseAccentOrSelectionColors(QApplication& application)
{
    for (const auto preset : {ThemePreset::Dark, ThemePreset::Light}) {
        ThemeSettings settings;
        settings.preset = preset;
        applyEditorTheme(application, settings);
        const auto neutralPalette = application.palette();
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            const auto row = neutralPalette.color(group, QPalette::AlternateBase);
            CHECK(row != themeColor(ThemeColor::Accent));
            CHECK(row != themeColor(ThemeColor::Selection));
            CHECK(row != themeColor(ThemeColor::LayerSelection));
        }

        settings.custom = resolvedThemeColors(settings);
        settings.preset = ThemePreset::Custom;
        // File dialog detail views use Base and AlternateBase for unselected
        // rows. Editing any selection token must only affect selected states.
        for (const auto& [role, color] : std::vector<std::pair<ThemeColor, QColor>> {
                 {ThemeColor::Accent, QColor("#ED2C89")},
                 {ThemeColor::Selection, QColor("#427CCF")},
                 {ThemeColor::LayerSelection, QColor("#8DC321")}}) {
            set(settings, role, color);
            applyEditorTheme(application, settings);
            const auto palette = application.palette();
            for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                CHECK(palette.color(group, QPalette::Base) == neutralPalette.color(group, QPalette::Base));
                CHECK(palette.color(group, QPalette::AlternateBase)
                    == neutralPalette.color(group, QPalette::AlternateBase));
                CHECK(palette.color(group, QPalette::Highlight) == at(settings.custom, ThemeColor::Accent));
                CHECK(palette.color(group, QPalette::HighlightedText)
                    == at(settings.custom, ThemeColor::SelectedText));
            }
        }
    }
    applyEditorTheme(application, ThemeSettings());
}

void customTokensNormalizeAndSwitchWithoutLosingTheCustomPalette(QApplication& application)
{
    ThemeSettings custom;
    custom.preset = ThemePreset::Custom;
    set(custom, ThemeColor::Accent, QColor("#F06435"));
    set(custom, ThemeColor::Background, QColor("#28201D"));
    set(custom, ThemeColor::Canvas, QColor("#43352C"));
    set(custom, ThemeColor::Selection, QColor("#694433"));
    set(custom, ThemeColor::CheckerLight, QColor("#C5B8AC"));
    applyEditorTheme(application, custom);
    CHECK(currentThemeSettings() == custom);
    CHECK(themeColor(ThemeColor::Accent) == QColor("#F06435"));
    CHECK(editorAccent() == imageeditor::core::Rgba8({240, 100, 53, 255}));
    CHECK(themeTone("#6577F3") == QColor("#F06435"));
    CHECK(themeColor(ThemeColor::Canvas) == QColor("#43352C"));
    CHECK(application.styleSheet().contains(QStringLiteral("#f06435"), Qt::CaseInsensitive));
    const auto savedColors = custom.custom;
    for (const auto preset : {ThemePreset::Dark, ThemePreset::Light, ThemePreset::Custom}) {
        custom.preset = preset;
        applyEditorTheme(application, custom);
        CHECK(currentThemeSettings().custom == savedColors);
    }
    CHECK(themeColor(ThemeColor::Accent) == QColor("#F06435"));
    set(custom, ThemeColor::Text, QColor());
    set(custom, ThemeColor::Canvas, QColor(18, 37, 56, 7));
    const auto safe = resolvedThemeColors(custom);
    CHECK(at(safe, ThemeColor::Text) == at(darkThemeColors(), ThemeColor::Text));
    CHECK(at(safe, ThemeColor::Canvas) == QColor(18, 37, 56, 255));
    for (const auto& color : safe) CHECK(color.isValid() && color.alpha() == 255);
    // Pure resolution must not mutate the caller's editable custom data.
    CHECK(!at(custom.custom, ThemeColor::Text).isValid());
    CHECK(at(custom.custom, ThemeColor::Canvas).alpha() == 7);
}

void thumbnailColorIsIndependentOfTheAccentAndLoadsOlderSettings(QApplication& application)
{
    ThemeSettings custom;
    custom.preset = ThemePreset::Custom;
    const auto defaultThumbnail = at(darkThemeColors(), ThemeColor::Thumbnail);
    CHECK(defaultThumbnail.isValid() && defaultThumbnail.alpha() == 255);
    CHECK(at(lightThemeColors(), ThemeColor::Thumbnail).isValid());
    CHECK(QString::fromLatin1(themeColorKey(ThemeColor::Thumbnail)) == QStringLiteral("thumbnail"));
    set(custom, ThemeColor::Accent, QColor("#2DCDA5"));
    applyEditorTheme(application, custom);
    CHECK(themeColor(ThemeColor::Thumbnail) == defaultThumbnail);
    set(custom, ThemeColor::Thumbnail, QColor("#C565B4"));
    applyEditorTheme(application, custom);
    CHECK(themeColor(ThemeColor::Thumbnail) == QColor("#C565B4"));
    CHECK(themeColor(ThemeColor::Accent) == QColor("#2DCDA5"));
    CHECK(application.palette().color(QPalette::Highlight) == QColor("#2DCDA5"));

    QTemporaryDir directory;
    CHECK(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("legacy-theme.ini")), QSettings::IniFormat);
    CHECK(saveThemeSettings(store, custom));
    CHECK(loadThemeSettings(store) == custom);
    // A version-1 palette saved before this role existed keeps its chosen
    // accent and obtains the thumbnail default without rewriting settings.
    const auto thumbnailKey = QStringLiteral("preferences/theme/custom/thumbnail");
    store.remove(thumbnailKey);
    const auto keysBefore = store.allKeys();
    const auto legacy = loadThemeSettings(store);
    CHECK(legacy.preset == ThemePreset::Custom);
    CHECK(at(legacy.custom, ThemeColor::Thumbnail) == defaultThumbnail);
    CHECK(at(legacy.custom, ThemeColor::Accent) == QColor("#2DCDA5"));
    CHECK(store.allKeys() == keysBefore);
    store.setValue(thumbnailKey, QStringLiteral("invalid color"));
    CHECK(at(loadThemeSettings(store).custom, ThemeColor::Thumbnail) == defaultThumbnail);
    applyEditorTheme(application, ThemeSettings());
}

void layerSelectionPersistsIndependentlyAndInheritsOlderSelectionWithoutWriting(QApplication& application)
{
    ThemeSettings custom;
    custom.preset = ThemePreset::Custom;
    set(custom, ThemeColor::Selection, QColor("#8A6240"));
    set(custom, ThemeColor::LayerSelection, QColor("#367D86"));
    set(custom, ThemeColor::Accent, QColor("#EB762E"));
    CHECK(QString::fromLatin1(themeColorKey(ThemeColor::LayerSelection)) == QStringLiteral("layerSelection"));
    CHECK(QString::fromLatin1(themeColorLabel(ThemeColor::LayerSelection)) == QStringLiteral("Layer selection"));
    applyEditorTheme(application, custom);
    CHECK(themeColor(ThemeColor::LayerSelection) == QColor("#367D86"));
    CHECK(themeColor(ThemeColor::Selection) == QColor("#8A6240"));
    CHECK(application.palette().color(QPalette::Highlight) == QColor("#EB762E"));
    CHECK(application.palette().color(QPalette::AlternateBase) != QColor("#8A6240"));

    QTemporaryDir directory;
    CHECK(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("selection-colors.ini")), QSettings::IniFormat);
    CHECK(saveThemeSettings(store, custom));
    CHECK(loadThemeSettings(store) == custom);
    const auto key = QStringLiteral("preferences/theme/custom/layerSelection");
    CHECK(store.value(key).toString().compare(QStringLiteral("#367D86"), Qt::CaseInsensitive) == 0);
    store.remove(key);
    store.sync();
    const auto keysBefore = store.allKeys();
    const auto readSettingsBytes = [&] {
        QFile file(store.fileName());
        CHECK(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    const auto bytesBefore = readSettingsBytes();
    auto inherited = loadThemeSettings(store);
    CHECK(at(inherited.custom, ThemeColor::LayerSelection) == QColor("#8A6240"));
    CHECK(at(inherited.custom, ThemeColor::Selection) == QColor("#8A6240"));
    CHECK(at(inherited.custom, ThemeColor::Accent) == QColor("#EB762E"));
    CHECK(store.allKeys() == keysBefore);
    store.sync();
    CHECK(readSettingsBytes() == bytesBefore);

    // Once the caller applies a setting, the inherited value becomes its own
    // persisted token rather than following later general selection changes.
    set(inherited, ThemeColor::Selection, QColor("#645CD8"));
    CHECK(saveThemeSettings(store, inherited));
    QSettings reopened(store.fileName(), QSettings::IniFormat);
    CHECK(loadThemeSettings(reopened) == inherited);
    CHECK(at(loadThemeSettings(reopened).custom, ThemeColor::LayerSelection) == QColor("#8A6240"));
    CHECK(at(loadThemeSettings(reopened).custom, ThemeColor::Selection) == QColor("#645CD8"));
    applyEditorTheme(application, ThemeSettings());
}

void documentTabColorsAreIndependentAndMigrateWithoutWriting(QApplication& application)
{
    QTemporaryDir directory;
    QSettings store(directory.filePath("tabs-theme.ini"), QSettings::IniFormat);
    ThemeSettings custom; custom.preset = ThemePreset::Custom;
    set(custom, ThemeColor::Surface, QColor("#213244"));
    set(custom, ThemeColor::Control, QColor("#344556"));
    set(custom, ThemeColor::DocumentTabActive, QColor("#358156"));
    set(custom, ThemeColor::DocumentTabInactive, QColor("#524275"));
    CHECK(saveThemeSettings(store, custom));
    CHECK(loadThemeSettings(store) == custom);
    applyEditorTheme(application, custom);
    CHECK(themeColor(ThemeColor::DocumentTabActive) == QColor("#358156"));
    CHECK(themeColor(ThemeColor::DocumentTabInactive) == QColor("#524275"));
    CHECK(themeColor(ThemeColor::Surface) == QColor("#213244"));
    store.remove("preferences/theme/custom/documentTabActive");
    store.remove("preferences/theme/custom/documentTabInactive");
    const auto keysBefore = store.allKeys();
    const auto migrated = loadThemeSettings(store);
    CHECK(at(migrated.custom, ThemeColor::DocumentTabActive) == QColor("#344556"));
    CHECK(at(migrated.custom, ThemeColor::DocumentTabInactive) == QColor("#213244"));
    CHECK(store.allKeys() == keysBefore);
    applyEditorTheme(application, ThemeSettings());
}

void settingsRoundTripWithoutTouchingLayoutOrOtherPreferences()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    QSettings store(directory.filePath(QStringLiteral("preferences.ini")), QSettings::IniFormat);
    store.setValue(QStringLiteral("layout/panelSizes"), QByteArray("existing-layout"));
    store.setValue(QStringLiteral("recentFiles/entries"), QStringList {QStringLiteral("/tmp/art.vulkana")});
    const auto before = store.allKeys();
    CHECK(loadThemeSettings(store) == ThemeSettings());
    CHECK(store.allKeys() == before); // Reading defaults must not create settings.
    ThemeSettings custom;
    custom.preset = ThemePreset::Custom;
    set(custom, ThemeColor::Accent, QColor("#ED7735"));
    set(custom, ThemeColor::Thumbnail, QColor("#5FADC7"));
    set(custom, ThemeColor::Canvas, QColor("#30251F"));
    CHECK(saveThemeSettings(store, custom));
    CHECK(loadThemeSettings(store) == custom);
    {
        QSettings reopened(store.fileName(), QSettings::IniFormat);
        CHECK(loadThemeSettings(reopened) == custom);
        CHECK(reopened.value(QStringLiteral("layout/panelSizes")).toByteArray() == QByteArray("existing-layout"));
        CHECK(reopened.value(QStringLiteral("recentFiles/entries")).toStringList()
            == QStringList {QStringLiteral("/tmp/art.vulkana")});
    }
    // Switching built-ins persists the custom palette rather than replacing it.
    custom.preset = ThemePreset::Light;
    CHECK(saveThemeSettings(store, custom));
    CHECK(loadThemeSettings(store) == custom);
    CHECK(loadThemeSettings(store).custom == custom.custom);
    store.setValue(QStringLiteral("preferences/theme/version"), 9000);
    CHECK(loadThemeSettings(store) == ThemeSettings());
    store.setValue(QStringLiteral("preferences/theme/version"), QStringLiteral("malformed"));
    CHECK(loadThemeSettings(store) == ThemeSettings());
    store.setValue(QStringLiteral("preferences/theme/version"), 1);
    store.setValue(QStringLiteral("preferences/theme/preset"), QStringLiteral("unknown"));
    CHECK(loadThemeSettings(store).preset == ThemePreset::Dark);
    store.setValue(QStringLiteral("preferences/theme/preset"), QStringLiteral("custom"));
    store.setValue(QStringLiteral("preferences/theme/custom/accent"), QStringLiteral("not a color"));
    store.setValue(QStringLiteral("preferences/theme/custom/canvas"), QStringLiteral("#02123456"));
    auto loaded = loadThemeSettings(store);
    CHECK(at(loaded.custom, ThemeColor::Accent) == at(darkThemeColors(), ThemeColor::Accent));
    CHECK(at(loaded.custom, ThemeColor::Canvas) == QColor("#123456"));
    set(loaded, ThemeColor::Text, QColor());
    set(loaded, ThemeColor::Control, QColor(5, 10, 15, 10));
    CHECK(saveThemeSettings(store, loaded));
    const auto normalized = loadThemeSettings(store);
    CHECK(at(normalized.custom, ThemeColor::Text) == at(darkThemeColors(), ThemeColor::Text));
    CHECK(at(normalized.custom, ThemeColor::Control) == QColor(5, 10, 15));
    CHECK(store.value(QStringLiteral("layout/panelSizes")).toByteArray() == QByteArray("existing-layout"));
}

QPushButton* colorButton(PreferencesDialog& dialog, ThemeColor role)
{
    return dialog.findChild<QPushButton*>(QStringLiteral("ThemeColor_")
        + QString::fromLatin1(themeColorKey(role)));
}

void choosePreset(QComboBox& combo, ThemePreset preset)
{
    const auto index = combo.findData(int(preset));
    CHECK(index >= 0);
    if (index >= 0) combo.setCurrentIndex(index);
}

void pickColor(PreferencesDialog& dialog, ThemeColor role, QColor picked)
{
    auto* button = colorButton(dialog, role);
    CHECK(button && button->isEnabled());
    if (!button || !button->isEnabled()) return;
    bool handled = false;
    // Supports either an asynchronous owned chooser or a synchronous getColor
    // loop, without a desktop dialog automation dependency.
    QTimer::singleShot(0, &dialog, [&] {
        imageeditor::ui::ColorDialog* chooser = dialog.findChild<imageeditor::ui::ColorDialog*>();
        if (!chooser) {
            for (auto* window : QApplication::topLevelWidgets()) {
                chooser = qobject_cast<imageeditor::ui::ColorDialog*>(window);
                if (chooser && chooser->isVisible()) break;
                chooser = nullptr;
            }
        }
        CHECK(chooser);
        if (!chooser) return;
        CHECK(chooser->testOption(imageeditor::ui::ColorDialog::DontUseNativeDialog));
        CHECK(!chooser->testOption(imageeditor::ui::ColorDialog::ShowAlphaChannel));
        chooser->setCurrentColor(picked);
        chooser->accept();
        handled = true;
    });
    button->click();
    QTest::qWait(10);
    CHECK(handled);
}

void dialogTabsControlsPreviewAndApplyCancel(QApplication& application)
{
    ThemeSettings initial;
    set(initial, ThemeColor::Accent, QColor("#E36432"));
    set(initial, ThemeColor::Canvas, QColor("#3A2822"));
    PreferencesDialog dialog({initial});
    std::vector<PreferencesState> previews, applied;
    dialog.onPreview = [&](const PreferencesState& value) {
        previews.push_back(value);
        applyEditorTheme(application, value.theme);
    };
    dialog.onApply = [&](const PreferencesState& value) { applied.push_back(value); return true; };
    CHECK(dialog.objectName() == QStringLiteral("PreferencesDialog"));
    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("PreferencesTabs"));
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesApply"));
    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesCancel"));
    auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesOk"));
    CHECK(tabs && presets && apply && cancel && ok);
    if (!tabs || !presets || !apply || !cancel || !ok) return;
    CHECK(tabs->count() == 3);
    CHECK(tabs->tabText(0) == QStringLiteral("General"));
    CHECK(tabs->tabText(1) == QStringLiteral("Themes"));
    CHECK(tabs->tabText(2) == QStringLiteral("Shortcuts"));
    CHECK(tabs->widget(2)->findChildren<QComboBox*>().empty());
    CHECK(tabs->widget(2)->findChild<QPushButton*>("ShortcutChange"));
    CHECK(!tabs->widget(0)->findChild<QComboBox*>("ThemePresetCombo"));
    CHECK(tabs->widget(1)->findChild<QComboBox*>("ThemePresetCombo")==presets);
    CHECK(presets->count() == 13);
    CHECK(dialog.draft().theme == initial);
    for (std::size_t i = 0; i < static_cast<std::size_t>(ThemeColor::Count); ++i) {
        const auto* button = colorButton(dialog, static_cast<ThemeColor>(i));
        CHECK(button && !button->isEnabled());
    }
    dialog.show();
    tabs->setCurrentIndex(1);
    QTest::qWait(10);
    auto* themeScroll=dialog.findChild<QScrollArea*>("PreferencesThemeScroll");
    CHECK(themeScroll && themeScroll->viewport()->height()>=280);
    auto* scrollbar = dialog.findChild<QScrollBar*>(QStringLiteral("PreferencesScrollBar"));
    CHECK(scrollbar && scrollbar->isVisible());
    if (scrollbar) {
        CHECK(scrollbar->orientation() == Qt::Vertical);
        CHECK(scrollbar->width() <= 14);
        CHECK(scrollbar->maximum() > scrollbar->minimum());
        QStyleOptionSlider option;
        option.initFrom(scrollbar);
        option.orientation = scrollbar->orientation();
        option.minimum = scrollbar->minimum();
        option.maximum = scrollbar->maximum();
        option.pageStep = scrollbar->pageStep();
        option.singleStep = scrollbar->singleStep();
        option.sliderPosition = scrollbar->sliderPosition();
        option.sliderValue = scrollbar->value();
        for (const auto control : {QStyle::SC_ScrollBarAddLine, QStyle::SC_ScrollBarSubLine})
            CHECK(scrollbar->style()->subControlRect(QStyle::CC_ScrollBar, &option, control, scrollbar).isEmpty());
        const auto page = scrollbar->style()->subControlRect(
            QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarAddPage, scrollbar);
        CHECK(!page.isEmpty());
        const auto before = scrollbar->value();
        if (!page.isEmpty()) QTest::mouseClick(scrollbar, Qt::LeftButton, Qt::NoModifier, page.center());
        CHECK(scrollbar->value() > before);
        scrollbar->setValue(scrollbar->minimum());
    }
    const auto cardSize = dialog.size();
    choosePreset(*presets, ThemePreset::Custom);
    CHECK(dialog.draft().theme.preset == ThemePreset::Custom);
    CHECK(dialog.draft().theme.custom == initial.custom);
    CHECK(!previews.empty() && previews.back() == dialog.draft());
    CHECK(applied.empty());
    for (std::size_t i = 0; i < static_cast<std::size_t>(ThemeColor::Count); ++i) {
        const auto* button = colorButton(dialog, static_cast<ThemeColor>(i));
        CHECK(button && button->isEnabled());
    }
    pickColor(dialog, ThemeColor::Accent, QColor("#EC8D37"));
    CHECK(at(dialog.draft().theme.custom, ThemeColor::Accent) == QColor("#EC8D37"));
    CHECK(!previews.empty() && previews.back() == dialog.draft());
    CHECK(themeColor(ThemeColor::Accent) == QColor("#EC8D37"));
    CHECK(applied.empty());
    pickColor(dialog, ThemeColor::Thumbnail, QColor("#6BAF86"));
    CHECK(at(dialog.draft().theme.custom, ThemeColor::Thumbnail) == QColor("#6BAF86"));
    CHECK(themeColor(ThemeColor::Thumbnail) == QColor("#6BAF86"));
    CHECK(themeColor(ThemeColor::Accent) == QColor("#EC8D37"));
    CHECK(applied.empty());
    const auto selectionBefore = at(dialog.draft().theme.custom, ThemeColor::Selection);
    pickColor(dialog, ThemeColor::LayerSelection, QColor("#2F8580"));
    CHECK(at(dialog.draft().theme.custom, ThemeColor::LayerSelection) == QColor("#2F8580"));
    CHECK(themeColor(ThemeColor::LayerSelection) == QColor("#2F8580"));
    CHECK(at(dialog.draft().theme.custom, ThemeColor::Selection) == selectionBefore);
    CHECK(themeColor(ThemeColor::Selection) == selectionBefore);
    CHECK(themeColor(ThemeColor::Accent) == QColor("#EC8D37"));
    CHECK(applied.empty());
    const auto reviewPath = qEnvironmentVariable("IMAGEEDITOR_PREFERENCES_REVIEW");
    if (!reviewPath.isEmpty()) CHECK(dialog.grab().save(reviewPath));
    const auto customized = dialog.draft();
    choosePreset(*presets, ThemePreset::Light);
    CHECK(dialog.draft().theme.custom == customized.theme.custom);
    CHECK(!colorButton(dialog, ThemeColor::Accent)->isEnabled());
    choosePreset(*presets, ThemePreset::Custom);
    CHECK(dialog.draft() == customized);
    CHECK(dialog.size() == cardSize); // Enabling options never collapses the card.
    apply->click();
    CHECK(applied.size() == 1 && applied.back() == customized);
    CHECK(dialog.isVisible());
    choosePreset(*presets, ThemePreset::Light);
    CHECK(previews.back().theme.preset == ThemePreset::Light);
    cancel->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK(!dialog.isVisible());
    CHECK(applied.size() == 1);
    CHECK(!previews.empty() && previews.back() == customized);
    CHECK(currentThemeSettings() == customized.theme);
}

void dialogOkAppliesAndCancelWithoutApplyRestoresTheEntryTheme(QApplication& application)
{
    ThemeSettings initial;
    applyEditorTheme(application, initial);
    {
        PreferencesDialog dialog({initial});
        int applications = 0;
        dialog.onPreview = [&](const PreferencesState& value) { applyEditorTheme(application, value.theme); };
        dialog.onApply = [&](const PreferencesState&) { ++applications; return true; };
        auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
        auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesCancel"));
        CHECK(presets && cancel);
        if (presets && cancel) {
            dialog.show();
            choosePreset(*presets, ThemePreset::Light);
            CHECK(currentThemeSettings().preset == ThemePreset::Light);
            cancel->click();
            CHECK(currentThemeSettings() == initial);
            CHECK(applications == 0);
        }
    }
    {
        PreferencesDialog dialog({initial});
        int applications = 0;
        PreferencesState saved;
        dialog.onPreview = [&](const PreferencesState& value) { applyEditorTheme(application, value.theme); };
        dialog.onApply = [&](const PreferencesState& value) { saved = value; ++applications; return true; };
        auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
        auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesOk"));
        CHECK(presets && ok);
        if (presets && ok) {
            dialog.show();
            choosePreset(*presets, ThemePreset::Light);
            ok->click();
            CHECK(applications == 1);
            CHECK(saved.theme.preset == ThemePreset::Light);
            CHECK(dialog.result() == QDialog::Accepted);
            CHECK(currentThemeSettings() == saved.theme);
        }
    }
}

void failedApplyDoesNotMoveTheRollbackBaseline(QApplication& application)
{
    const ThemeSettings initial;
    applyEditorTheme(application, initial);
    PreferencesDialog dialog({initial});
    int attempts = 0;
    dialog.onPreview = [&](const PreferencesState& value) { applyEditorTheme(application, value.theme); };
    dialog.onApply = [&](const PreferencesState&) { ++attempts; return false; };
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesApply"));
    auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesOk"));
    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesCancel"));
    auto* error = dialog.findChild<QLabel*>(QStringLiteral("ErrorLabel"));
    CHECK(presets && apply && ok && cancel && error);
    if (!presets || !apply || !ok || !cancel || !error) return;
    dialog.show();
    choosePreset(*presets, ThemePreset::Light);
    apply->click();
    CHECK(attempts == 1);
    CHECK(dialog.isVisible());
    CHECK(error->isVisible() && !error->text().isEmpty());
    CHECK(dialog.draft().theme.preset == ThemePreset::Light);
    CHECK(currentThemeSettings().preset == ThemePreset::Light);
    ok->click();
    CHECK(attempts == 2 && dialog.isVisible());
    cancel->click();
    CHECK(currentThemeSettings() == initial);
    CHECK(dialog.result() == QDialog::Rejected);
}

void explicitlyCustomizingAPresetCopiesItButSimplyChoosingCustomDoesNot()
{
    ThemeSettings initial;
    set(initial, ThemeColor::Accent, QColor("#E36432"));
    PreferencesDialog dialog({initial});
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
    QPushButton* customize = nullptr;
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Customize preset")) customize = button;
    }
    CHECK(presets && customize);
    if (!presets || !customize) return;
    choosePreset(*presets, ThemePreset::Light);
    choosePreset(*presets, ThemePreset::Custom);
    CHECK(dialog.draft().theme.custom == initial.custom);
    CHECK(!customize->isEnabled());
    choosePreset(*presets, ThemePreset::Light);
    CHECK(customize->isEnabled());
    customize->click();
    CHECK(dialog.draft().theme.preset == ThemePreset::Custom);
    CHECK(dialog.draft().theme.custom == lightThemeColors());
    CHECK(!customize->isEnabled());
    for(const auto& info:themePresets()) {
        if(info.preset==ThemePreset::Custom)continue;
        choosePreset(*presets,info.preset);
        CHECK(dialog.draft().theme.preset==info.preset);
        const auto expected=resolvedThemeColors(dialog.draft().theme);
        customize->click();
        CHECK(dialog.draft().theme.preset==ThemePreset::Custom);
        CHECK(dialog.draft().theme.custom==expected);
    }
}

void unchangedCancelDoesNotPublishARollback()
{
    PreferencesDialog dialog({});
    int previews=0;
    dialog.onPreview=[&](const PreferencesState&){++previews;};
    dialog.show();dialog.reject();CHECK(previews==0);
    dialog.show();
    auto* choice=dialog.findChild<QComboBox*>("ThemePresetCombo");
    choosePreset(*choice,ThemePreset::Midnight);CHECK(previews==1);
    dialog.findChild<QPushButton*>("PreferencesApply")->click();
    dialog.reject();CHECK(previews==1); // Apply establishes a new no-op baseline.
}

void measurementReadoutSharesPreviewApplyAndRollbackWithoutDependingOnTheme()
{
    const PreferencesState initial;
    CHECK(!initial.advancedMeasurementReadout);
    PreferencesDialog dialog(initial);
    auto* checkbox = dialog.findChild<QCheckBox*>(QStringLiteral("AdvancedMeasurementReadout"));
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesApply"));
    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesCancel"));
    CHECK(checkbox && presets && apply && cancel);
    if (!checkbox || !presets || !apply || !cancel) return;
    CHECK(!checkbox->isChecked() && checkbox->isEnabled());
    CHECK(checkbox->text() == QStringLiteral("Advanced measurement readout"));
    CHECK(checkbox->toolTip().contains(QStringLiteral("ΔX")));
    CHECK(checkbox->toolTip().contains(QStringLiteral("ΔY")));
    PreferencesState displayed = initial, saved = initial;
    int previews = 0, applications = 0;
    dialog.onPreview = [&](const PreferencesState& value) { displayed = value; ++previews; };
    dialog.onApply = [&](const PreferencesState& value) { saved = value; ++applications; return true; };
    dialog.show();
    const auto cardSize = dialog.size();
    checkbox->click();
    CHECK(previews == 1 && displayed.advancedMeasurementReadout);
    CHECK(dialog.draft().advancedMeasurementReadout);
    CHECK(displayed.theme == initial.theme && applications == 0);
    for (const auto preset : {ThemePreset::Light, ThemePreset::Custom, ThemePreset::Dark}) {
        choosePreset(*presets, preset);
        CHECK(checkbox->isChecked() && checkbox->isEnabled());
        CHECK(dialog.draft().advancedMeasurementReadout);
    }
    CHECK(dialog.size() == cardSize);
    apply->click();
    CHECK(applications == 1 && saved.advancedMeasurementReadout && dialog.isVisible());
    const auto applied = saved;
    checkbox->click();
    choosePreset(*presets, ThemePreset::Light);
    CHECK(!displayed.advancedMeasurementReadout);
    CHECK(displayed.theme.preset == ThemePreset::Light);
    cancel->click();
    CHECK(displayed == applied && saved == applied);
    CHECK(applications == 1 && dialog.result() == QDialog::Rejected);

    // Initial true must also be restored by Cancel without an Apply.
    PreferencesDialog reopened(applied);
    auto* reopenedCheckbox = reopened.findChild<QCheckBox*>(QStringLiteral("AdvancedMeasurementReadout"));
    CHECK(reopenedCheckbox && reopenedCheckbox->isChecked());
    if (!reopenedCheckbox) return;
    reopened.onPreview = [&](const PreferencesState& value) { displayed = value; };
    reopenedCheckbox->click();
    CHECK(!displayed.advancedMeasurementReadout);
    reopened.reject();
    CHECK(displayed == applied);
}

void toolHintPositionPreviewsAppliesAndRollsBack()
{
    PreferencesState initial;
    PreferencesDialog dialog(initial);
    auto* choice=dialog.findChild<QComboBox*>("ToolHintPosition");
    auto* apply=dialog.findChild<QPushButton*>("PreferencesApply");
    CHECK(choice && apply); if(!choice || !apply)return;
    CHECK(initial.toolHintPosition==1);
    CHECK(choice->count()==3 && choice->currentIndex()==1);
    CHECK(choice->itemText(0)=="Left" && choice->itemText(1)=="Center" && choice->itemText(2)=="Disabled");
    auto displayed=initial, saved=initial;
    dialog.onPreview=[&](const PreferencesState& state){displayed=state;};
    dialog.onApply=[&](const PreferencesState& state){saved=state;return true;};
    choice->setCurrentIndex(0); CHECK(displayed.toolHintPosition==0);
    choice->setCurrentIndex(1); CHECK(displayed.toolHintPosition==1);
    apply->click(); CHECK(saved.toolHintPosition==1);
    choice->setCurrentIndex(2); CHECK(displayed.toolHintPosition==2);
    dialog.reject(); CHECK(displayed==saved);
    PreferencesDialog reopened(saved);
    CHECK(reopened.findChild<QComboBox*>("ToolHintPosition")->currentIndex()==1);
}

void measurementReadoutFailedApplyKeepsTheLastSuccessfulSnapshot()
{
    const PreferencesState initial;
    PreferencesDialog dialog(initial);
    auto* checkbox = dialog.findChild<QCheckBox*>(QStringLiteral("AdvancedMeasurementReadout"));
    auto* presets = dialog.findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
    auto* apply = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesApply"));
    auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("PreferencesOk"));
    auto* error = dialog.findChild<QLabel*>(QStringLiteral("ErrorLabel"));
    CHECK(checkbox && presets && apply && ok && error);
    if (!checkbox || !presets || !apply || !ok || !error) return;
    PreferencesState displayed = initial, saved = initial;
    bool canSave = false;
    dialog.onPreview = [&](const PreferencesState& value) { displayed = value; };
    dialog.onApply = [&](const PreferencesState& value) {
        if (canSave) saved = value;
        return canSave;
    };
    dialog.show();
    checkbox->click();
    apply->click();
    CHECK(displayed.advancedMeasurementReadout && !saved.advancedMeasurementReadout);
    CHECK(dialog.isVisible() && error->isVisible());
    canSave = true;
    apply->click();
    const auto applied = saved;
    CHECK(applied.advancedMeasurementReadout && !error->isVisible());
    canSave = false;
    checkbox->click();
    choosePreset(*presets, ThemePreset::Light);
    ok->click();
    CHECK(dialog.isVisible() && error->isVisible());
    CHECK(!displayed.advancedMeasurementReadout && displayed.theme.preset == ThemePreset::Light);
    dialog.reject();
    CHECK(displayed == applied && saved == applied);
    CHECK(dialog.result() == QDialog::Rejected);
}

void propertiesScrollbarsReusePreferencesStyling(QApplication& application)
{
    const auto originalTheme=currentThemeSettings();
    PropertiesPanel panel;
    panel.setActiveTool(imageeditor::core::ToolId::Brush);
    panel.resize(340,180);
    panel.show();
    PreferencesDialog preferences({});
    preferences.show();
    preferences.findChild<QTabWidget*>("PreferencesTabs")->setCurrentIndex(1);
    auto* stack=panel.findChild<QStackedWidget*>();
    auto* page=stack?qobject_cast<QScrollArea*>(stack->currentWidget()):nullptr;
    auto* reference=preferences.findChild<QScrollBar*>(QStringLiteral("PreferencesScrollBar"));
    CHECK(page && reference);
    if(!page||!reference)return;
    // Opt-in is on the common page shell, not a Brush-only style override.
    for(auto* candidate:panel.findChildren<QScrollArea*>(QStringLiteral("PropertiesPageScroll")))
        CHECK(candidate->verticalScrollBar()->property("editorScrollBar").toBool());
    CHECK(reference->property("editorScrollBar").toBool());

    ThemeSettings dark,light,custom;
    light.preset=ThemePreset::Light;
    custom.preset=ThemePreset::Custom;
    set(custom,ThemeColor::Accent,QColor("#C252DB"));
    for(const auto& theme:{dark,light,custom}) {
        applyEditorTheme(application,theme);
        QCoreApplication::processEvents();
        auto* scrollbar=page->verticalScrollBar();
        CHECK(scrollbar->isVisible());
        CHECK(scrollbar->width()==reference->width());
        CHECK(scrollbar->width()==12);
        CHECK(scrollbar->maximum()>scrollbar->minimum());
        CHECK(page->horizontalScrollBarPolicy()==Qt::ScrollBarAlwaysOff);
        QStyleOptionSlider option;
        option.initFrom(scrollbar);
        option.orientation=Qt::Vertical;
        option.minimum=scrollbar->minimum();option.maximum=scrollbar->maximum();
        option.pageStep=scrollbar->pageStep();option.singleStep=scrollbar->singleStep();
        option.sliderPosition=scrollbar->sliderPosition();option.sliderValue=scrollbar->value();
        for(const auto control:{QStyle::SC_ScrollBarAddLine,QStyle::SC_ScrollBarSubLine})
            CHECK(scrollbar->style()->subControlRect(QStyle::CC_ScrollBar,&option,control,scrollbar).isEmpty());
        const auto track=scrollbar->style()->subControlRect(QStyle::CC_ScrollBar,&option,QStyle::SC_ScrollBarAddPage,scrollbar);
        CHECK(!track.isEmpty());
        const auto viewportGeometry=page->viewport()->geometry();
        const auto before=scrollbar->value();
        if(!track.isEmpty())QTest::mouseClick(scrollbar,Qt::LeftButton,Qt::NoModifier,track.center());
        CHECK(scrollbar->value()>before);
        CHECK(page->viewport()->geometry()==viewportGeometry);
        scrollbar->setValue(scrollbar->maximum());
        CHECK(page->widget()->y()<0);
        scrollbar->setValue(scrollbar->minimum());
    }
    preferences.reject();panel.close();
    applyEditorTheme(application,originalTheme);
}

void allScrollAreasUseSharedModernStyling(QApplication& application)
{
    const auto originalTheme=currentThemeSettings();
    AdjustmentsPanel adjustments;
    const auto target=imageeditor::core::Layer::raster("Scrollbar fixture",
        std::make_shared<imageeditor::core::ContiguousRasterSurface>(imageeditor::core::Extent2u{1,1}));
    adjustments.setTarget(&target,false);
    adjustments.resize(340,220);
    adjustments.show();
    QScrollArea futurePanel;
    auto* content=new QWidget;
    content->setFixedSize(600,900);
    futurePanel.setWidget(content);
    futurePanel.resize(260,180);
    futurePanel.show();
    auto* tabs=adjustments.findChild<QTabWidget*>(QStringLiteral("AdjustmentTabs"));
    CHECK(tabs);
    ThemeSettings light;light.preset=ThemePreset::Light;
    for(const auto& theme:{ThemeSettings{},light}) {
        applyEditorTheme(application,theme);
        const auto checkBar=[](QScrollBar* bar) {
            CHECK(bar);
            if(!bar)return;
            CHECK(bar->isVisible());
            CHECK(bar->maximum()>bar->minimum());
            CHECK(bar->orientation()==Qt::Vertical?bar->width()==12:bar->height()==12);
            QStyleOptionSlider option;
            option.initFrom(bar);option.orientation=bar->orientation();
            option.minimum=bar->minimum();option.maximum=bar->maximum();
            option.pageStep=bar->pageStep();option.singleStep=bar->singleStep();
            option.sliderPosition=bar->sliderPosition();option.sliderValue=bar->value();
            for(const auto control:{QStyle::SC_ScrollBarAddLine,QStyle::SC_ScrollBarSubLine})
                CHECK(bar->style()->subControlRect(QStyle::CC_ScrollBar,&option,control,bar).isEmpty());
            const auto track=bar->style()->subControlRect(QStyle::CC_ScrollBar,&option,QStyle::SC_ScrollBarAddPage,bar);
            CHECK(!track.isEmpty());
            if(!track.isEmpty())QTest::mouseClick(bar,Qt::LeftButton,Qt::NoModifier,track.center());
            CHECK(bar->value()>bar->minimum());
            bar->setValue(bar->minimum());
        };
        if(tabs)for(int i=0;i<tabs->count();++i) {
            tabs->setCurrentIndex(i);
            auto* scroll=tabs->currentWidget()->findChild<QScrollArea*>();
            CHECK(scroll);
            if(!scroll)continue;
            // Force overflow independently of adjustment availability/current
            // page while keeping the real panel shell and its default style.
            scroll->widget()->setMinimumHeight(750);
            QCoreApplication::processEvents();
            checkBar(scroll->verticalScrollBar());
        }
        QCoreApplication::processEvents();
        // A completely new panel needs no editorScrollBar property to opt in.
        CHECK(!futurePanel.verticalScrollBar()->property("editorScrollBar").isValid());
        checkBar(futurePanel.verticalScrollBar());checkBar(futurePanel.horizontalScrollBar());
    }
    adjustments.close();futurePanel.close();
    applyEditorTheme(application,originalTheme);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    applyEditorTheme(application);
    defaultsPreserveTheApprovedDarkTheme(application);
    lightThemeHasReadableSharedPaletteRoles(application);
    darkVariationsAreDistinctReadableAndPersistByStableKey();
    unselectedFileRowsDoNotUseAccentOrSelectionColors(application);
    customTokensNormalizeAndSwitchWithoutLosingTheCustomPalette(application);
    thumbnailColorIsIndependentOfTheAccentAndLoadsOlderSettings(application);
    layerSelectionPersistsIndependentlyAndInheritsOlderSelectionWithoutWriting(application);
    documentTabColorsAreIndependentAndMigrateWithoutWriting(application);
    settingsRoundTripWithoutTouchingLayoutOrOtherPreferences();
    dialogTabsControlsPreviewAndApplyCancel(application);
    dialogOkAppliesAndCancelWithoutApplyRestoresTheEntryTheme(application);
    failedApplyDoesNotMoveTheRollbackBaseline(application);
    explicitlyCustomizingAPresetCopiesItButSimplyChoosingCustomDoesNot();
    unchangedCancelDoesNotPublishARollback();
    measurementReadoutSharesPreviewApplyAndRollbackWithoutDependingOnTheme();
    toolHintPositionPreviewsAppliesAndRollsBack();
    measurementReadoutFailedApplyKeepsTheLastSuccessfulSnapshot();
    propertiesScrollbarsReusePreferencesStyling(application);
    allScrollAreasUseSharedModernStyling(application);
    applyEditorTheme(application, ThemeSettings());
    if (failures) {
        std::cerr << failures << " theme/preferences assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All theme/preferences tests passed\n";
    return EXIT_SUCCESS;
}
