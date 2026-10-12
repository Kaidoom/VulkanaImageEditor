#include "imageeditor/platform/AtomicFile.hpp"
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace imageeditor::platform {
bool syncFile(QFileDevice& file)
{
    if (!file.flush() || file.handle() < 0) return false;
#ifdef Q_OS_WIN
    return ::_commit(file.handle()) == 0;
#else
    return ::fsync(file.handle()) == 0;
#endif
}

bool replaceFileAtomically(const QString& staged, const QString& destination, QString& error)
{
    error.clear();
    if (QFileInfo(staged).absolutePath() != QFileInfo(destination).absolutePath()) {
        error = QStringLiteral("Replacement must be staged beside its destination.");
        return false;
    }
#ifdef Q_OS_WIN
    const auto nativePath = [](const QString& path) {
        auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        if (!native.startsWith(QStringLiteral("\\\\?\\")))
            native = native.startsWith(QStringLiteral("\\\\"))
                ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
        return native.toStdWString();
    };
    // Same-volume rename; never permit the non-atomic cross-volume copy fallback.
    if (MoveFileExW(nativePath(staged).c_str(), nativePath(destination).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    error = QStringLiteral("Cannot replace project (Windows error %1).").arg(GetLastError());
#else
    if (::rename(QFile::encodeName(staged).constData(), QFile::encodeName(destination).constData()) == 0)
        return true;
    error = QStringLiteral("Cannot replace project: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
#endif
    return false;
}
}
