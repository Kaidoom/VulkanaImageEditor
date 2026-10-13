#include "imageeditor/project/ProjectPreview.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QSaveFile>
#include <QFileInfo>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool writePreallocatedPreview(const QString& path, const QImage& image)
{
    // Desktop thumbnailers supply an empty, already-open temporary file. The
    // Fedora SELinux thumb_t domain can fill it, but cannot rename over a file
    // created by Dolphin. The caller publishes the completed thumbnail; this
    // scratch output does not need our own atomic replacement.
    // Do not follow symlinks, block on FIFOs or truncate an unexpected existing
    // file. Check the opened descriptor, not just the earlier path lookup.
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size != 0 || info.st_nlink != 1) {
        ::close(fd);
        return false;
    }
    QFile output;
    if (!output.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    return image.save(&output, "PNG") && output.flush();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    QCoreApplication::setApplicationName("vulkana-thumbnailer");
    QCommandLineParser parser;
    parser.setApplicationDescription("Extract the saved preview of a Vulkana project without opening it.");
    parser.addHelpOption();
    parser.addOption({"size","Maximum thumbnail side in pixels.","pixels","256"});
    parser.addPositionalArgument("input","Input .vulkana project.");
    parser.addPositionalArgument("output","Output PNG thumbnail.");
    parser.process(app);
    const auto paths = parser.positionalArguments();
    bool valid = false;
    const auto size = parser.value("size").toInt(&valid);
    if (paths.size() != 2 || !valid || size < 1 || size > 4096) return 2;
    const QFileInfo source(paths[0]), destination(paths[1]);
    if (source.absoluteFilePath() == destination.absoluteFilePath()
        || (!destination.canonicalFilePath().isEmpty()
            && source.canonicalFilePath() == destination.canonicalFilePath())) return 2;
    const auto image = imageeditor::project::readPreview(paths[0],size);
    if (image.isNull()) return 1;
    if (destination.isSymLink()) return 1;
    if (destination.exists() && destination.size() == 0)
        return writePreallocatedPreview(paths[1], image) ? 0 : 1;
    // Standalone use with a new output path (or a nonempty prior preview) still
    // replaces atomically. Never fall back to truncating an arbitrary file.
    QSaveFile output(paths[1]);
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output,"PNG") || !output.commit()) return 1;
    return 0;
}
