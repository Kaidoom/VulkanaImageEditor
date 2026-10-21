#pragma once

#include "imageeditor/ui/QtRasterImageLoader.hpp"

class QMimeData;

namespace imageeditor::ui {
// Lightweight availability check: inspect offered formats and local file
// headers, without decoding or transferring an image payload.
[[nodiscard]] bool clipboardOffersImage(const QMimeData& mime);
// Called only for an explicit Paste/From clipboard action. Pixels take
// precedence over paths; local files only, never URL fetching or HTML.
[[nodiscard]] RasterLayerLoadResult loadClipboardImage(const QMimeData& mime);
} // namespace imageeditor::ui
