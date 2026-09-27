#pragma once
#include "imageeditor/core/Geometry.hpp"
#include <QColor>
#include <array>
#include <span>

class QApplication;
class QSettings;

namespace imageeditor::ui {

void applyEditorTheme(QApplication& application);
enum class ThemePreset { Dark, Light, Custom, Midnight, Graphite, Forest, Lagoon,
    Aubergine, Rose, Ember, Coffee, Olive, Arctic };
struct ThemePresetInfo {
    ThemePreset preset;
    const char* key; // Stable settings identity, independent of menu order/translation.
    const char* label;
};
[[nodiscard]] std::span<const ThemePresetInfo> themePresets();
enum class ThemeColor : std::size_t {
    Background, Surface, Control, Text, SecondaryText, Accent, Selection, LayerSelection,
    SelectedText, Border, Canvas, CheckerLight, CheckerDark, Thumbnail,
    DocumentTabActive, DocumentTabInactive, Count
};
using ThemeColors = std::array<QColor, static_cast<std::size_t>(ThemeColor::Count)>;
[[nodiscard]] ThemeColors darkThemeColors();
[[nodiscard]] ThemeColors lightThemeColors();
struct ThemeSettings {
    ThemePreset preset {ThemePreset::Dark};
    ThemeColors custom {darkThemeColors()};
    friend bool operator==(const ThemeSettings&, const ThemeSettings&) = default;
};
[[nodiscard]] ThemeColors resolvedThemeColors(const ThemeSettings&);
[[nodiscard]] const ThemeSettings& currentThemeSettings();
[[nodiscard]] QColor themeColor(ThemeColor);
[[nodiscard]] const char* themeColorKey(ThemeColor);
[[nodiscard]] const char* themeColorLabel(ThemeColor);
[[nodiscard]] ThemeSettings loadThemeSettings(QSettings&);
[[nodiscard]] bool saveThemeSettings(QSettings&, const ThemeSettings&);
// Runtime preview only: no settings IO, font/style recreation, or document edits.
void applyEditorTheme(QApplication&, const ThemeSettings&);
// Semantic shade adaptation for shared chrome; original Dark tones are exact.
[[nodiscard]] QColor themeTone(const char* darkColor);
// Runtime accent authority is the application Highlight palette token.
[[nodiscard]] core::Rgba8 editorAccent();

} // namespace imageeditor::ui
