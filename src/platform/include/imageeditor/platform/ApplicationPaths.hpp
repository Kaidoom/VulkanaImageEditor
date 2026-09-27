#pragma once

#include <QString>
#include <QStringList>

namespace imageeditor::platform {
// Host adapters only: no Qt path types enter canonical document data.
QString installedDataPath(const QString& relativePath);
QString shaderPath(const QString& name);
QString defaultUiConfigPath();
QString userDataDirectory();
QString userCacheDirectory();
QString userPresetDirectory();
// Run once at startup before loading user presets. Copy-only, no overwrites;
// legacy data stays untouched. New QSettings uses the stable vulkanaEditor ID.
QStringList migrateLegacyUserPresets();
// Explicit directories make migration testable without touching real user data.
QStringList migrateLegacyUserPresets(const QString& legacyDirectory,
    const QString& destinationDirectory);
}
