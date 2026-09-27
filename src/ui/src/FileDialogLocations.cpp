#include "imageeditor/ui/FileDialogLocations.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace imageeditor::ui {
QString existingFileDialogDirectory(
    const QString& remembered, const QString& pictures, const QString& application)
{
    for (const auto& path : {remembered, pictures, application})
        if (!path.isEmpty() && QFileInfo(path).isDir())
            return QFileInfo(path).absoluteFilePath();
    return application;
}
namespace {
QString resolve(const QString& directory)
{
    return existingFileDialogDirectory(directory,
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QCoreApplication::applicationDirPath());
}
}
QString openDocumentDirectory()
{
    const auto document = QSettings().value("files/lastOpenedDocument").toString();
    return resolve(document.isEmpty() ? QString() : QFileInfo(document).absolutePath());
}
QString lastExportDirectory()
{
    QSettings settings;
    auto directory = settings.value("files/lastExportDirectory").toString();
    if (directory.isEmpty()) {
        // Preserve the location saved by older releases.
        const auto previous = settings.value("export/v1/destination").toString();
        if (!previous.isEmpty())
            directory = QFileInfo(previous).absolutePath();
    }
    return resolve(directory);
}
void rememberOpenedDocument(const QString& path)
{
    if (path.isEmpty()) return;
    QSettings settings;
    settings.setValue("files/lastOpenedDocument", QFileInfo(path).absoluteFilePath());
    settings.sync();
}
void rememberExportDirectory(const QString& path)
{
    if (path.isEmpty()) return;
    QSettings settings;
    settings.setValue("files/lastExportDirectory", QFileInfo(path).absolutePath());
    settings.sync();
}
}
