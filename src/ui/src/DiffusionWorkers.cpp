#include "imageeditor/ui/DiffusionWorkers.hpp"
#include "imageeditor/ui/UpdateService.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace imageeditor::ui {
namespace {
constexpr auto sessionCudaProperty = "vulkanaSessionCudaWorker";
bool executable(const QString &path) {
  const QFileInfo file(path);
  return file.isFile() && file.isExecutable()
#ifdef Q_OS_WIN
         && file.suffix().compare("exe", Qt::CaseInsensitive) == 0
#endif
      ;
}
QString find(const QString &name) {
  const auto filename = name
#ifdef Q_OS_WIN
      + ".exe"
#endif
      ;
  const auto adjacent = QDir(QCoreApplication::applicationDirPath()).filePath(filename);
  if (executable(adjacent)) return adjacent;
  return QStandardPaths::findExecutable(filename);
}
} // namespace

DiffusionWorkerLocation diffusionWorkerLocation(const QString &backend) {
  if (backend != "vulkan" && backend != "cuda") return {};
  auto path = backend == "cuda" ? sessionCudaWorker() : QString{};
  if (path.isEmpty()) path = qEnvironmentVariable(qPrintable("VULKANA_DIFFUSION_WORKER_" + backend.toUpper()));
  if (path.isEmpty() && backend == "cuda") path = QSettings().value("diffusion/workers/" + backend).toString();
  // CUDA is an explicit, optional installation. Do not pick up an unrelated
  // worker from PATH or from a previous editor installation.
  if (path.isEmpty() && backend == "vulkan") path = find("vulkana-diffusion-worker-vulkan");
  return {path, !path.isEmpty() && executable(path)};
}

QString installedDiffusionWorkerVersion(const QString& backend) {
  const auto location = diffusionWorkerLocation(backend);
  if (!location.available) return {};
  auto root = QFileInfo(location.executable).absoluteDir();
  if (!root.cdUp()) return {};
  QFile file(root.filePath("worker.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) return {};
  const auto meta = QJsonDocument::fromJson(file.readAll()).object();
  const auto version = meta["version"].toString();
  const auto relative = QString("bin/vulkana-diffusion-worker-") + backend
#ifdef Q_OS_WIN
      + ".exe"
#endif
      ;
  if (meta["schemaVersion"].toInt() != 1 || meta["protocol"].toInt() != 1 || meta["backend"] != backend
      || meta["executable"] != relative || !UpdateService::isNewer(version, version).has_value()
      || QFileInfo(root.filePath(relative)).canonicalFilePath() != QFileInfo(location.executable).canonicalFilePath()) return {};
  return version;
}

QString discoverDiffusionWorker(const QString &backend) {
  const auto session = sessionCudaWorker();
  if (!session.isEmpty() && (backend == "auto" || backend == "cuda"))
    return executable(session) ? session : QString{};
  const auto override = qEnvironmentVariable("VULKANA_DIFFUSION_WORKER");
  if (!override.isEmpty()) return executable(override) ? override : QString{};
  for (const auto &candidate : backend == "auto" ? QStringList{"cuda", "vulkan"} : QStringList{backend}) {
    const auto location = diffusionWorkerLocation(candidate);
    if (location.available) return location.executable;
    // An explicit missing override must not silently launch a different binary.
    if (backend != "auto" && !location.executable.isEmpty()) return {};
  }
  // Generic names remain supported for command-line/development integrations.
  return find("vulkana-diffusion-worker");
}

QString diffusionWorkerInFolder(const QString &backend, const QString &folder,
                                bool *ambiguous) {
  if (ambiguous) *ambiguous = false;
  if (backend != "cuda" || !QFileInfo(folder).isDir()) return {};
  const auto filename = QString("vulkana-diffusion-worker-") + backend
#ifdef Q_OS_WIN
      + ".exe"
#endif
      ;
  QStringList candidates, canonicalPaths;
  const auto inspect = [&](const QDir &directory) {
    for (const auto &relative : {filename, "bin/" + filename}) {
      const auto path = directory.absoluteFilePath(relative);
      if (!executable(path)) continue;
      const auto canonical = QFileInfo(path).canonicalFilePath();
      if (canonicalPaths.contains(canonical)) continue;
      canonicalPaths.append(canonical);
      candidates.append(path);
    }
  };
  const QDir root(folder);
  inspect(root);
  if (candidates.isEmpty()) {
    // Extraction tools may introduce a wrapper directory. Limit discovery to
    // one level rather than recursively searching unrelated user folders.
    for (const auto &child : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
      inspect(QDir(child.absoluteFilePath()));
      if (candidates.size() > 1) break;
    }
  }
  if (ambiguous) *ambiguous = candidates.size() > 1;
  return candidates.size() == 1 ? candidates.front() : QString{};
}

bool setDiffusionWorkerPath(const QString &backend, const QString &path) {
  if (backend != "cuda" || !executable(path)) return false;
  QSettings().setValue("diffusion/workers/" + backend, QFileInfo(path).absoluteFilePath());
  return true;
}

QString sessionCudaWorker() {
  const auto* app = QCoreApplication::instance();
  return app ? app->property(sessionCudaProperty).toString() : QString{};
}

bool setSessionCudaWorker(const QString &path, QString *error) {
  if (error) error->clear();
  auto* app = QCoreApplication::instance();
  if (!app) return false;
  QString resolved;
  bool ambiguous = false;
  if (!path.isEmpty()) {
    resolved = QFileInfo(path).isDir() ? diffusionWorkerInFolder("cuda", path, &ambiguous) : path;
    if (resolved.isEmpty() || !executable(resolved)) {
      if (error) *error = ambiguous
          ? QStringLiteral("Several CUDA workers were found. Choose one package folder or its bin folder.")
          : QStringLiteral("CUDA worker not found or not executable. Choose an extracted worker folder, its bin folder, or the worker executable.");
      return false;
    }
    resolved = QFileInfo(resolved).absoluteFilePath();
  }
  app->setProperty(sessionCudaProperty, resolved);
  return true;
}
} // namespace imageeditor::ui
