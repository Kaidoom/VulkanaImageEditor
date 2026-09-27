#include "imageeditor/ui/ClipboardImage.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QUrl>
#include <QVariant>

#include <exception>

namespace imageeditor::ui {
namespace {
QString localPath(QString text)
{
    text = text.trimmed();
    if (text.size() > 32768 || text.contains(QChar::Null) || text.contains('\n') || text.contains('\r')) return {};
    if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"')
        || (text.front() == '\'' && text.back() == '\''))) text = text.mid(1, text.size() - 2);
    const QUrl url(text, QUrl::StrictMode);
    if (!url.scheme().isEmpty()) {
        if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host() != QStringLiteral("localhost"))
            || url.hasQuery() || url.hasFragment()) return {};
        text = url.path(QUrl::FullyDecoded);
    }
    if (text.contains(QChar::Null)) return {};
    if (text.startsWith(QStringLiteral("~/"))) text = QDir::home().filePath(text.mid(2));
    // Only regular files: don't block on a device, directory or named pipe.
    const QFileInfo file(text);
    return file.isFile() && file.isReadable() ? file.absoluteFilePath() : QString {};
}
}

RasterLayerLoadResult loadClipboardImage(const QMimeData& mime)
{
    try {
        const auto name = QStringLiteral("Clipboard image");
        // Prefer encoded raster MIME to Qt's implicit imageData decoding, so
        // the shared reader checks advertised dimensions BEFORE allocation.
        QString encodedError;
        int encodedRepresentations = 0;
        qsizetype encodedBytes = 0;
        for (const auto& format : mime.formats()) {
            if (!format.startsWith(QStringLiteral("image/"))) continue;
            // Some applications advertise an unavailable codec before PNG.
            // Try alternatives without unbounded format/transfer work.
            if (++encodedRepresentations > 8) break;
            const auto bytes = mime.data(format);
            if (bytes.size() > 256 * 1024 * 1024 - encodedBytes)
                return {.layer = {}, .extent = {}, .error = QStringLiteral("Clipboard image data is too large.")};
            encodedBytes += bytes.size();
            auto loaded = loadRasterLayerFromData(bytes, name);
            if (loaded) return loaded;
            if (encodedError.isEmpty()) encodedError = loaded.error;
        }
        // Don't bypass malformed/oversized encoded data with Qt's implicit
        // imageData decoder. Each attempted representation used our bounds.
        if (encodedRepresentations)
            return {.layer = {}, .extent = {}, .error = encodedError.isEmpty()
                ? QStringLiteral("Clipboard image could not be decoded.") : encodedError};
        if (mime.hasImage()) {
            const auto image = qvariant_cast<QImage>(mime.imageData());
            if (!image.isNull()) return rasterLayerFromImage(image, name);
        }
        // File managers offer text/uri-list. The first decodable local image
        // wins; all web/remote URLs are ignored, without filesystem probing.
        if (mime.hasUrls()) {
            for (const auto& url : mime.urls()) {
                const auto path = localPath(url.toString(QUrl::FullyEncoded));
                if (path.isEmpty()) continue;
                auto loaded = loadRasterLayer(path);
                if (loaded) return loaded;
            }
        } else if (mime.hasText()) {
            const auto path = localPath(mime.text());
            if (!path.isEmpty()) return loadRasterLayer(path);
        }
        return {.layer = {}, .extent = {},
            .error = QStringLiteral("Clipboard has no image or readable local image file.")};
    } catch (const std::bad_alloc&) {
        return {.layer = {}, .extent = {}, .error = QStringLiteral("Not enough memory to paste this image.")};
    } catch (const std::exception& error) {
        return {.layer = {}, .extent = {}, .error = QString::fromUtf8(error.what())};
    }
}
} // namespace imageeditor::ui
