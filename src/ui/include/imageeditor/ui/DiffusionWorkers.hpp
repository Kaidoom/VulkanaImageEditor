#pragma once
#include <QString>

namespace imageeditor::ui {
// Availability means a native executable is present. Protocol/backend validation
// still happens in WorkerClient when it starts; discovery never runs inference.
struct DiffusionWorkerLocation {
  QString executable;
  bool available{};
};
DiffusionWorkerLocation diffusionWorkerLocation(const QString &backend);
QString installedDiffusionWorkerVersion(const QString& backend);
QString discoverDiffusionWorker(const QString &backend);
// Detect the native worker in a package, its bin directory, or one enclosing
// extraction directory. Never launch candidates or choose between packages.
QString diffusionWorkerInFolder(const QString &backend, const QString &folder,
                                bool *ambiguous = nullptr);
// Process-local override: never writes the saved worker path or backend.
// An empty path clears it; a nonempty path accepts an executable or package folder.
bool setSessionCudaWorker(const QString &path, QString *error = nullptr);
QString sessionCudaWorker();
bool setDiffusionWorkerPath(const QString &backend, const QString &path);
} // namespace imageeditor::ui
