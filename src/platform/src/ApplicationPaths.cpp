#include "imageeditor/platform/ApplicationPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace imageeditor::platform {
QString installedDataPath(const QString& relativePath)
{
    // Relative-to-executable first supports DESTDIR verification and /opt
    // prefixes. Never consult cwd, user data, or environment for packaged data.
    const auto sibling = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../share/vulkana-editor/") + relativePath);
#ifdef VULKANA_RELOCATABLE_BUNDLE
    // Missing bundle assets must fail visibly, never be supplied by a host RPM.
    return QDir::cleanPath(sibling);
#else
    if (QFileInfo::exists(sibling)) return QDir::cleanPath(sibling);
    return QDir(QStringLiteral(VULKANA_INSTALLED_DATA_DIR)).filePath(relativePath);
#endif
}

QString shaderPath(const QString& name)
{
#ifdef VULKANA_DEVELOPMENT_SHADER_DIR
    const auto development = QDir(QStringLiteral(VULKANA_DEVELOPMENT_SHADER_DIR)).filePath(name);
    if (QFileInfo::exists(development)) return development;
#endif
    return installedDataPath(QStringLiteral("shaders/") + name);
}

QString defaultUiConfigPath()
{
#ifdef VULKANA_DEVELOPMENT_UI_CONFIG
    if (QFileInfo::exists(QStringLiteral(VULKANA_DEVELOPMENT_UI_CONFIG)))
        return QStringLiteral(VULKANA_DEVELOPMENT_UI_CONFIG);
#endif
    return installedDataPath(QStringLiteral("ui-layout.ini"));
}

QString userDataDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("vulkanaEditor"));
}

QString objectSelectionBundlePath()
{
#ifdef VULKANA_DEVELOPMENT_OBJECT_SELECTION
    return QStringLiteral(VULKANA_DEVELOPMENT_OBJECT_SELECTION);
#else
    return installedDataPath(QStringLiteral("object-selection/mobilesam-v1"));
#endif
}

QString userCacheDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
        .filePath(QStringLiteral("vulkanaEditor"));
}

QString userPresetDirectory()
{
    return QDir(userDataDirectory()).filePath(QStringLiteral("brush-presets-v2"));
}

QStringList migrateLegacyUserPresets()
{
    // QStandardPaths' former AppDataLocation included both organization and
    // application names. Only custom presets move; old files remain untouched.
    const auto legacy = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("ImageEditor/ImageEditor/brush-presets-v2"));
    return migrateLegacyUserPresets(legacy, userPresetDirectory());
}

QStringList migrateLegacyUserPresets(const QString& legacyDirectory,
    const QString& destinationDirectory)
{
    const QDir legacy(legacyDirectory);
    QDir destination(destinationDirectory);
    const auto marker = destination.filePath(QStringLiteral(".legacy-migration-v1"));
    if (!legacy.exists() || QFileInfo::exists(marker)) return {};
    if (!QDir().mkpath(destinationDirectory))
        return {QStringLiteral("Cannot migrate custom brushes to %1; originals remain in %2")
                    .arg(destinationDirectory, legacyDirectory)};
    QLockFile lock(destination.filePath(QStringLiteral(".migration.lock")));
    if (!lock.tryLock(0))
        return {QStringLiteral("Custom-brush migration is busy; restart to retry. Originals are unchanged.")};
    if (QFileInfo::exists(marker)) return {};
    QStringList errors;
    for (const auto& info : legacy.entryInfoList({QStringLiteral("*.iebrush")}, QDir::Files, QDir::Name)) {
        const auto target = destination.filePath(info.fileName());
        if (QFileInfo::exists(target)) continue; // Never overwrite a newer user copy.
        QFile input(info.absoluteFilePath());
        QSaveFile output(target);
        if (info.isSymLink() || info.size() > 1024 * 1024 || !input.open(QIODevice::ReadOnly)
            || !output.open(QIODevice::WriteOnly)) {
            errors.append(QStringLiteral("Custom brush not migrated: %1; original retained").arg(info.absoluteFilePath()));
            continue;
        }
        const auto data = input.readAll();
        if (input.error() != QFileDevice::NoError || output.write(data) != data.size() || !output.commit())
            errors.append(QStringLiteral("Failed to copy custom brush %1; original retained").arg(info.absoluteFilePath()));
    }
    if (errors.isEmpty()) {
        QSaveFile complete(marker);
        if (!complete.open(QIODevice::WriteOnly) || complete.write("copy-only migration v1\n") < 0 || !complete.commit())
            errors.append(QStringLiteral("Unable to mark custom-brush migration complete; it will retry safely."));
    }
    return errors;
}
}
