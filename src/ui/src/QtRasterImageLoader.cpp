#include "imageeditor/ui/QtRasterImageLoader.hpp"

#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/ui/RasterLimits.hpp"

#include <QColorSpace>
#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace imageeditor::ui {
namespace {
RasterLayerLoadResult readLayer(QImageReader& reader, const QString& name)
{
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        return {.layer = std::nullopt, .extent = {}, .error = reader.errorString()};
    }
    const auto advertisedSize = reader.size();
    if (advertisedSize.isValid()
        && !rasterExtentWithinLimits(static_cast<std::uint32_t>(advertisedSize.width()),
            static_cast<std::uint32_t>(advertisedSize.height()))) {
        return {.layer = std::nullopt, .extent = {},
            .error = QStringLiteral("Image exceeds the V1 limit of %1 pixels per side and 40 megapixels.")
                .arg(kMaximumRasterDimension)};
    }

    QImage image = reader.read();
    if (image.isNull()) {
        return {.layer = std::nullopt, .extent = {}, .error = reader.errorString()};
    }
    return rasterLayerFromImage(std::move(image), name);
}
}

RasterLayerLoadResult loadRasterLayer(const QString& filePath)
{
    QImageReader reader(filePath);
    return readLayer(reader, QFileInfo(filePath).completeBaseName());
}

RasterLayerLoadResult loadRasterLayerFromData(const QByteArray& bytes, const QString& name)
{
    QBuffer buffer;
    buffer.setData(bytes); // implicitly shared; no second encoded-image copy
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return readLayer(reader, name);
}

RasterLayerLoadResult rasterLayerFromImage(QImage image, const QString& name)
{
    if (image.isNull()) return {.layer = {}, .extent = {}, .error = QStringLiteral("The image is empty.")};
    if (!rasterExtentWithinLimits(static_cast<std::uint32_t>(image.width()),
            static_cast<std::uint32_t>(image.height()))) {
        return {.layer = std::nullopt, .extent = {},
            .error = QStringLiteral("Image exceeds the V1 limit of %1 pixels per side and 40 megapixels.")
                .arg(kMaximumRasterDimension)};
    }
    try {
        if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb) {
            image = image.convertedToColorSpace(QColorSpace::SRgb);
        }
        image = image.convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) throw std::bad_alloc();

        const auto width = static_cast<std::uint32_t>(image.width());
        const auto height = static_cast<std::uint32_t>(image.height());
        const auto rowBytes = static_cast<std::size_t>(width) * 4U;
        std::vector<std::byte> pixels(rowBytes * static_cast<std::size_t>(height));
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * rowBytes,
                image.constScanLine(static_cast<int>(row)), rowBytes);
        }

        auto surface = std::make_shared<core::ContiguousRasterSurface>(
            core::Extent2u {width, height}, std::move(pixels));
        const auto layerName = name.toUtf8().toStdString();
        return {
            .layer = core::Layer::raster(layerName, std::move(surface)),
            .extent = {width, height},
            .error = {},
        };
    } catch (const std::bad_alloc&) {
        return {.layer = std::nullopt, .extent = {},
            .error = QStringLiteral("There is not enough memory to load this image.")};
    } catch (const std::exception& exception) {
        return {.layer = std::nullopt, .extent = {},
            .error = QString::fromUtf8(exception.what())};
    }
}

ImageLoadResult loadRasterDocument(const QString& filePath)
{
    auto loaded = loadRasterLayer(filePath);
    if (!loaded) {
        return {.document = nullptr, .error = std::move(loaded.error)};
    }
    try {
        auto document = std::make_unique<core::Document>(
            core::CanvasSpec {.extent = loaded.extent, .dotsPerInch = 96.0});
        if (!document->insertLayer(0, std::move(*loaded.layer))) {
            return {.document = nullptr,
                .error = QStringLiteral("The image layer could not be added to a document.")};
        }
        return {.document = std::move(document), .error = {}};
    } catch (const std::bad_alloc&) {
        return {.document = nullptr,
            .error = QStringLiteral("There is not enough memory to open this image.")};
    } catch (const std::exception& exception) {
        return {.document = nullptr, .error = QString::fromUtf8(exception.what())};
    }
}

} // namespace imageeditor::ui
