#include "imageeditor/ui/Theme.hpp"
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
namespace {
constexpr std::array keys {"background", "surface", "control", "text", "secondaryText", "accent",
    "selection", "layerSelection", "selectedText", "border", "canvas", "checkerLight", "checkerDark", "thumbnail",
    "documentTabActive", "documentTabInactive"};
constexpr std::array labels {"Primary background", "Panel / toolbar background", "Controls",
    "Primary text", "Secondary text", "Accent", "Selection highlight", "Layer selection", "Selected text",
    "Borders", "Canvas surround", "Checkerboard light", "Checkerboard dark", "Default thumbnail color",
    "Active document tab", "Inactive document tab"};
static_assert(keys.size() == static_cast<std::size_t>(ThemeColor::Count));
static_assert(labels.size() == keys.size());
constexpr std::array presets {
    ThemePresetInfo{ThemePreset::Dark, "dark", "Dark (Default)"},
    ThemePresetInfo{ThemePreset::Light, "light", "Light"},
    ThemePresetInfo{ThemePreset::Midnight, "midnight", "Midnight · Blue"},
    ThemePresetInfo{ThemePreset::Graphite, "graphite", "Graphite · Silver"},
    ThemePresetInfo{ThemePreset::Forest, "forest", "Forest · Green"},
    ThemePresetInfo{ThemePreset::Lagoon, "lagoon", "Lagoon · Teal"},
    ThemePresetInfo{ThemePreset::Aubergine, "aubergine", "Aubergine · Violet"},
    ThemePresetInfo{ThemePreset::Rose, "rose", "Rose · Pink"},
    ThemePresetInfo{ThemePreset::Ember, "ember", "Ember · Coral"},
    ThemePresetInfo{ThemePreset::Coffee, "coffee", "Coffee · Sand"},
    ThemePresetInfo{ThemePreset::Olive, "olive", "Olive · Gold"},
    ThemePresetInfo{ThemePreset::Arctic, "arctic", "Arctic · Ice"},
    ThemePresetInfo{ThemePreset::Custom, "custom", "Custom"}
};
ThemeColors darkVariation(const char* background, const char* surface, const char* control,
    const char* text, const char* secondary, const char* accent, const char* border, const char* canvas)
{
    const QColor base(surface), highlight(accent);
    const auto tint = [&](double amount) {
        return QColor(int(std::lround(base.red()+(highlight.red()-base.red())*amount)),
            int(std::lround(base.green()+(highlight.green()-base.green())*amount)),
            int(std::lround(base.blue()+(highlight.blue()-base.blue())*amount)));
    };
    // Coordinated selection/tab surfaces without changing checkerboard contrast
    // or making the canvas depend on a later custom accent edit.
    return {QColor(background), base, QColor(control), QColor(text), QColor(secondary), highlight,
        tint(.22), highlight, QColor("#FFFFFF"), QColor(border), QColor(canvas),
        QColor("#888A90"), QColor("#777A80"), QColor(secondary), tint(.12), base};
}
}
std::span<const ThemePresetInfo> themePresets() { return presets; }
const char* themeColorKey(ThemeColor role) { return keys.at(static_cast<std::size_t>(role)); }
const char* themeColorLabel(ThemeColor role) { return labels.at(static_cast<std::size_t>(role)); }
ThemeColors darkThemeColors()
{
    // User's saved Custom palette, promoted to Dark (2026-09-13).
    return {QColor("#202020"), QColor("#141414"), QColor("#272727"), QColor("#E8EAF0"),
        QColor("#9299AA"), QColor("#FF8D54"), QColor("#623621"), QColor("#E37E4B"), QColor("#FFFFFF"),
        QColor("#494949"), QColor("#393C42"), QColor("#888A90"), QColor("#777A80"), QColor("#B6BDCC"),
        QColor("#272727"), QColor("#141414")};
}
ThemeColors lightThemeColors()
{
    return {QColor("#E9EBEF"), QColor("#F6F7F9"), QColor("#FFFFFF"), QColor("#232832"),
        QColor("#596273"), QColor("#5264C8"), QColor("#D8DFF5"), QColor("#D8DFF5"), QColor("#182449"),
        QColor("#BEC4D0"), QColor("#B4B8C0"), QColor("#E3E5E9"), QColor("#CBCFD6"), QColor("#596273"),
        QColor("#FFFFFF"), QColor("#E9EBEF")};
}
ThemeColors resolvedThemeColors(const ThemeSettings& settings)
{
    switch (settings.preset) {
    case ThemePreset::Dark: return darkThemeColors();
    case ThemePreset::Light: return lightThemeColors();
    case ThemePreset::Midnight: return darkVariation("#141B2B", "#0E1422", "#232E44", "#E5ECFA", "#A6B4CF", "#79A8FF", "#415372", "#202A3D");
    case ThemePreset::Graphite: return darkVariation("#171717", "#101010", "#292929", "#EEEEEE", "#AEAEAE", "#CFD4DD", "#4C4C4C", "#343434");
    case ThemePreset::Forest: return darkVariation("#18231D", "#111B16", "#29372E", "#E8F0E7", "#ACBDAF", "#84C98B", "#465C4C", "#2C3A31");
    case ThemePreset::Lagoon: return darkVariation("#102729", "#0B1D20", "#203A3D", "#E0F1F0", "#9FBFBE", "#52D3BD", "#3C6366", "#233D40");
    case ThemePreset::Aubergine: return darkVariation("#231A2C", "#18121F", "#332740", "#EFE6F8", "#B9A9CD", "#C29CFF", "#5A446D", "#352A41");
    case ThemePreset::Rose: return darkVariation("#291B24", "#1E121A", "#3D2A36", "#F8E8F1", "#C7ADB9", "#F29BC1", "#664756", "#403039");
    case ThemePreset::Ember: return darkVariation("#2A1919", "#1D1112", "#40292A", "#FBE9E5", "#CCAEA8", "#FF9585", "#694743", "#402C2B");
    case ThemePreset::Coffee: return darkVariation("#29231F", "#1D1815", "#3B332B", "#F0E8DD", "#C4B5A2", "#D3B58D", "#615343", "#413930");
    case ThemePreset::Olive: return darkVariation("#25261B", "#191C12", "#363A27", "#EFF0DF", "#B9BFA1", "#D9CE74", "#585D40", "#393D2D");
    case ThemePreset::Arctic: return darkVariation("#242F3A", "#1B2530", "#344352", "#ECF3F9", "#B3C5D4", "#85CFED", "#546A7B", "#3B4B58");
    case ThemePreset::Custom: break;
    default: return darkThemeColors();
    }
    auto result = settings.custom;
    const auto fallback = darkThemeColors();
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!result[i].isValid()) result[i] = fallback[i];
        result[i].setAlpha(255); // UI surfaces are opaque; this never changes artwork alpha.
    }
    return result;
}
ThemeSettings loadThemeSettings(QSettings& settings)
{
    ThemeSettings result;
    settings.beginGroup(QStringLiteral("preferences/theme"));
    if (settings.value(QStringLiteral("version"), 1).toInt() == 1) {
        const auto preset = settings.value(QStringLiteral("preset"), QStringLiteral("dark")).toString();
        for (const auto& info : themePresets())
            if (preset == QString::fromLatin1(info.key)) { result.preset = info.preset; break; }
        for (std::size_t i = 0; i < result.custom.size(); ++i) {
            const QColor color(settings.value(QStringLiteral("custom/") + QString::fromLatin1(keys[i])).toString());
            if (color.isValid()) { result.custom[i] = color; result.custom[i].setAlpha(255); }
        }
        // Preserve the user's previous row appearance when introducing the
        // independent role. Loading never rewrites settings; Apply persists it.
        if (!settings.contains(QStringLiteral("custom/layerSelection"))
            && settings.contains(QStringLiteral("custom/selection")))
            result.custom[static_cast<std::size_t>(ThemeColor::LayerSelection)]
                = result.custom[static_cast<std::size_t>(ThemeColor::Selection)];
        // Additive roles inherit the existing custom palette until explicitly
        // edited. Loading older settings must not rewrite or reset them.
        if (!settings.contains(QStringLiteral("custom/documentTabActive")))
            result.custom[static_cast<std::size_t>(ThemeColor::DocumentTabActive)]
                = result.custom[static_cast<std::size_t>(ThemeColor::Control)];
        if (!settings.contains(QStringLiteral("custom/documentTabInactive")))
            result.custom[static_cast<std::size_t>(ThemeColor::DocumentTabInactive)]
                = result.custom[static_cast<std::size_t>(ThemeColor::Surface)];
    }
    settings.endGroup();
    return result;
}
bool saveThemeSettings(QSettings& settings, const ThemeSettings& value)
{
    settings.beginGroup(QStringLiteral("preferences/theme"));
    settings.setValue(QStringLiteral("version"), 1);
    const char* key = "dark";
    for (const auto& info : themePresets())
        if (info.preset == value.preset) { key = info.key; break; }
    settings.setValue(QStringLiteral("preset"), QString::fromLatin1(key));
    ThemeSettings custom = value; custom.preset = ThemePreset::Custom;
    const auto colors = resolvedThemeColors(custom);
    for (std::size_t i = 0; i < colors.size(); ++i)
        settings.setValue(QStringLiteral("custom/") + QString::fromLatin1(keys[i]), colors[i].name(QColor::HexRgb));
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}
}
