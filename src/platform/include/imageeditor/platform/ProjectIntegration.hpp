#pragma once
#include <QString>
class QSettings;

namespace imageeditor::platform {
inline constexpr auto projectIntegrationChoiceKey = "integration/windows/projectFilesChoice";
// Only supported, complete installed/portable layouts are offered registration.
bool projectIntegrationAvailable(const QString& executable);
// Empty result means success. Diagnostics are intentionally technical English.
// These only change the current user's file-type registration, never UserChoice,
// machine-wide defaults, Explorer policy or document contents.
QString registerProjectFiles(QSettings& preferences, const QString& executable, const QString& version);
// Startup maintenance for previous opt-ins only. A strictly newer numeric
// release can refresh paths still owned by this registration; it never restores
// removed associations or overwrites later external changes. No-op is success.
QString refreshProjectFiles(QSettings& preferences, const QString& executable, const QString& version);
QString unregisterProjectFiles(QSettings& preferences);
}
