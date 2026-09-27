#pragma once

#include <QProcessEnvironment>
#include <QStringList>

namespace imageeditor::platform {
// Observe Qt's override contract, not a second platform-selection algorithm.
// Call before QApplication consumes its options. Never changes the environment.
struct PlatformStartup {
    QString sessionType;
    QString environmentOverride;
    QStringList explicitArguments;
    bool displayPresent = false;
    bool waylandDisplayPresent = false;
    bool waylandSocketPresent = false;
    [[nodiscard]] QString selectionSource() const;
};
[[nodiscard]] PlatformStartup inspectPlatformStartup(const QStringList& arguments,
    const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment());
// Only explicit command-line platform options survive update/restart. Qt reads
// explicit environment choices itself; automatic selection is never saved.
void retainPlatformArguments(const PlatformStartup& startup);
[[nodiscard]] QStringList platformRestartArguments();
} // namespace imageeditor::platform
