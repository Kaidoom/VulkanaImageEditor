#pragma once

#include "imageeditor/core/Document.hpp"

#include <QString>

#include <memory>
#include <optional>

class QImage;
class QByteArray;

namespace imageeditor::ui {

enum class RasterImportMode { Color, Data };

struct ImageLoadResult {
    std::unique_ptr<core::Document> document;
    QString error;

    [[nodiscard]] explicit operator bool() const noexcept { return document != nullptr; }
};

struct RasterLayerLoadResult {
    std::optional<core::Layer> layer;
    core::Extent2u extent;
    QString error;

    [[nodiscard]] explicit operator bool() const noexcept { return layer.has_value(); }
};

// Data mode preserves numeric channel values and the stored pixel grid. It
// bypasses colour conversion and rejects precision/premultiplication loss.
[[nodiscard]] RasterLayerLoadResult loadRasterLayer(const QString& filePath, RasterImportMode = RasterImportMode::Color);
[[nodiscard]] RasterLayerLoadResult loadRasterLayerFromData(const QByteArray& bytes, const QString& name, RasterImportMode = RasterImportMode::Color);
[[nodiscard]] RasterLayerLoadResult rasterLayerFromImage(QImage image, const QString& name, RasterImportMode = RasterImportMode::Color);
[[nodiscard]] ImageLoadResult loadRasterDocument(const QString& filePath, RasterImportMode = RasterImportMode::Color);

} // namespace imageeditor::ui
