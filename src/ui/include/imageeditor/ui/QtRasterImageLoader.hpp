#pragma once

#include "imageeditor/core/Document.hpp"

#include <QString>

#include <memory>
#include <optional>

class QImage;
class QByteArray;

namespace imageeditor::ui {

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

[[nodiscard]] RasterLayerLoadResult loadRasterLayer(const QString& filePath);
[[nodiscard]] RasterLayerLoadResult loadRasterLayerFromData(const QByteArray& bytes, const QString& name);
[[nodiscard]] RasterLayerLoadResult rasterLayerFromImage(QImage image, const QString& name);
[[nodiscard]] ImageLoadResult loadRasterDocument(const QString& filePath);

} // namespace imageeditor::ui
