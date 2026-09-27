#include "imageeditor/ui/UiLayoutConfig.hpp"

#include <QSettings>
#include <QStringList>
#include <QWidget>

#include <algorithm>

namespace imageeditor::ui {
namespace {

constexpr int kSmallestUsefulPanelHeight = 80;

int readInteger(QSettings& settings, const QString& key, int fallback,
    QStringList* diagnostics)
{
    if (!settings.contains(key)) {
        return fallback;
    }
    bool valid = false;
    const int value = settings.value(key).toInt(&valid);
    if (valid) {
        return value;
    }
    if (diagnostics) {
        diagnostics->push_back(QStringLiteral("%1 is not an integer; using %2")
                .arg(key)
                .arg(fallback));
    }
    return fallback;
}

int readHeight(QSettings& settings, const QString& prefix,
    const QString& key, const QString& legacyKey, int fallback,
    QStringList* diagnostics)
{
    const auto preferred = prefix + key;
    if (settings.contains(preferred)) {
        return readInteger(settings, preferred, fallback, diagnostics);
    }
    return readInteger(
        settings, prefix + legacyKey, fallback, diagnostics);
}

PanelHeightRange readRange(QSettings& settings,
    const QString& panelName, PanelHeightRange fallback,
    QStringList* diagnostics)
{
    const auto prefix = QStringLiteral("%1/").arg(panelName);
    const int requestedMinimum = readHeight(settings, prefix,
        QStringLiteral("minimumHeight"),
        QStringLiteral("minimumDockHeight"), fallback.minimum, diagnostics);
    const int requestedMaximum = readHeight(settings, prefix,
        QStringLiteral("maximumHeight"),
        QStringLiteral("maximumDockHeight"), fallback.maximum, diagnostics);

    const int largestMinimum = QWIDGETSIZE_MAX - 1;
    const int minimum = std::clamp(
        requestedMinimum, kSmallestUsefulPanelHeight, largestMinimum);
    const int maximum = requestedMaximum <= 0
        ? QWIDGETSIZE_MAX
        : std::clamp(requestedMaximum,
              minimum + 1, QWIDGETSIZE_MAX);

    if (diagnostics && minimum != requestedMinimum) {
        diagnostics->push_back(
            QStringLiteral("%1minimumHeight normalized from %2 to %3")
                .arg(prefix)
                .arg(requestedMinimum)
                .arg(minimum));
    }
    if (diagnostics && requestedMaximum > 0 && maximum != requestedMaximum) {
        diagnostics->push_back(
            QStringLiteral("%1maximumHeight normalized from %2 to %3")
                .arg(prefix)
                .arg(requestedMaximum)
                .arg(maximum));
    }
    return {minimum, maximum};
}

} // namespace

UiLayoutConfig UiLayoutConfig::loadFromIni(
    const QString& filePath, QStringList* diagnostics)
{
    UiLayoutConfig result;
    QSettings settings(filePath, QSettings::IniFormat);
    result.color = readRange(
        settings, QStringLiteral("ColorPanel"), result.color, diagnostics);
    result.layers = readRange(
        settings, QStringLiteral("LayersPanel"), result.layers, diagnostics);
    result.properties = readRange(
        settings, QStringLiteral("PropertiesPanel"), result.properties,
        diagnostics);
    result.adjustments = readRange(
        settings, QStringLiteral("AdjustmentsPanel"), result.adjustments, diagnostics);
    if (diagnostics && settings.status() != QSettings::NoError) {
        diagnostics->push_back(
            QStringLiteral("Qt reported an error while reading %1")
                .arg(filePath));
    }
    return result;
}

} // namespace imageeditor::ui
