#pragma once

#include "imageeditor/ui/QtRasterImageLoader.hpp"

class QMimeData;

namespace imageeditor::ui {
// Called only in response to Paste. Pixels take precedence over path metadata;
// one paste consumes one image. Local files only, never URL fetching or HTML.
[[nodiscard]] RasterLayerLoadResult loadClipboardImage(const QMimeData& mime);
} // namespace imageeditor::ui
