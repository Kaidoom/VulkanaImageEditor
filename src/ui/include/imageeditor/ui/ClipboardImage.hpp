#pragma once

#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include <QByteArray>

class QMimeData;

namespace imageeditor::ui {
// Lightweight availability check: inspect offered formats and local file
// headers, without decoding or transferring an image payload.
[[nodiscard]] bool clipboardOffersImage(const QMimeData& mime);
struct ClipboardGifSource {
    QString path;
    QByteArray data;
    QString error;
    explicit operator bool() const { return !path.isEmpty() || !data.isEmpty() || !error.isEmpty(); }
};
// Preserve animation for From clipboard. Encoded GIFs/local GIF files take
// precedence over static thumbnail representations; remote URLs are never read.
[[nodiscard]] ClipboardGifSource clipboardGifSource(const QMimeData& mime);
// Called only for an explicit Paste/From clipboard action. Pixels take
// precedence over paths; local files only, never URL fetching or HTML.
[[nodiscard]] RasterLayerLoadResult loadClipboardImage(const QMimeData& mime);
} // namespace imageeditor::ui
