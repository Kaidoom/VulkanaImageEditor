#include "imageeditor/ui/ClipboardImage.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/RasterLimits.hpp"

#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeData>
#include <QPixmap>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <variant>

namespace {
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using imageeditor::core::Extent2u;
using imageeditor::core::RasterLayer;
using imageeditor::core::Rgba8;
using imageeditor::ui::loadClipboardImage;
using imageeditor::ui::RasterLayerLoadResult;

QImage referenceImage()
{
    QImage image(3, 2, QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    image.setPixelColor(1, 0, QColor(128, 64, 32, 128));
    image.setPixelColor(2, 0, QColor(0, 255, 0, 255));
    image.setPixelColor(0, 1, QColor(0, 0, 255, 255));
    image.setPixelColor(1, 1, QColor(19, 37, 81, 0));
    image.setPixelColor(2, 1, QColor(255, 255, 0, 64));
    return image;
}

Rgba8 pixel(const RasterLayerLoadResult& loaded, int x, int y)
{
    std::array<std::byte, 4> bytes {};
    if (!loaded || !std::holds_alternative<RasterLayer>(loaded.layer->payload)) return {};
    const auto& surface = std::get<RasterLayer>(loaded.layer->payload).surface;
    CHECK(surface);
    if (!surface) return {};
    surface->copyRgba8({x, y, 1, 1}, bytes, 4);
    return {std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3])};
}

void checkPixels(const RasterLayerLoadResult& loaded, const QImage& expected)
{
    CHECK(loaded);
    if (!loaded) {
        std::cerr << "Clipboard load: " << loaded.error.toStdString() << '\n';
        return;
    }
    CHECK(loaded.error.isEmpty());
    CHECK(loaded.extent == Extent2u({static_cast<std::uint32_t>(expected.width()),
                                    static_cast<std::uint32_t>(expected.height())}));
    if (loaded.extent.width != static_cast<unsigned>(expected.width())
        || loaded.extent.height != static_cast<unsigned>(expected.height())) return;
    const auto straight = expected.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < straight.height(); ++y) {
        for (int x = 0; x < straight.width(); ++x) {
            const auto* rgba = straight.constScanLine(y) + x * 4;
            CHECK(pixel(loaded, x, y) == Rgba8({rgba[0], rgba[1], rgba[2], rgba[3]}));
        }
    }
}

QByteArray encoded(const QImage& image, const char* format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    CHECK(buffer.open(QIODevice::WriteOnly));
    QImageWriter writer(&buffer, format);
    writer.setQuality(100);
    CHECK(writer.write(image));
    return bytes;
}

void imageObjectsAndSharedStraightRgbaConversion()
{
    const auto reference = referenceImage();
    QMimeData imageMime;
    imageMime.setImageData(reference);
    auto loaded = loadClipboardImage(imageMime);
    checkPixels(loaded, reference);
    CHECK(loaded && loaded.layer->name == "Clipboard image");
    CHECK(pixel(loaded, 0, 0) == Rgba8({255, 0, 0, 255}));
    CHECK(pixel(loaded, 2, 0) == Rgba8({0, 255, 0, 255}));
    CHECK(pixel(loaded, 0, 1) == Rgba8({0, 0, 255, 255}));
    CHECK(pixel(loaded, 2, 1) == Rgba8({255, 255, 0, 64}));

    const auto premultiplied = reference.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QMimeData premultipliedMime;
    premultipliedMime.setImageData(premultiplied);
    checkPixels(loadClipboardImage(premultipliedMime), premultiplied);
    const auto shared = imageeditor::ui::rasterLayerFromImage(premultiplied, QStringLiteral("Converted"));
    checkPixels(shared, premultiplied);
    CHECK(shared && shared.layer->name == "Converted");
    CHECK(pixel(shared, 1, 0).alpha == 128);
    CHECK(pixel(shared, 1, 0).red >= 127); // Not the premultiplied stored channel (64).

    const auto pixmap = QPixmap::fromImage(reference);
    QMimeData pixmapMime;
    pixmapMime.setImageData(pixmap);
    checkPixels(loadClipboardImage(pixmapMime), pixmap.toImage());

    QImage padded(3, 2, QImage::Format_RGB888);
    padded.fill(QColor(12, 34, 56));
    padded.setPixelColor(2, 1, QColor(98, 76, 54));
    CHECK(padded.bytesPerLine() > padded.width() * 3);
    checkPixels(imageeditor::ui::rasterLayerFromImage(padded, QStringLiteral("Padded")), padded);
    CHECK(!imageeditor::ui::rasterLayerFromImage({}, QStringLiteral("Empty")));
    QImage tooWide(static_cast<int>(imageeditor::ui::kMaximumRasterDimension) + 1, 1,
        QImage::Format_RGBA8888);
    CHECK(!tooWide.isNull());
    CHECK(!imageeditor::ui::rasterLayerFromImage(tooWide, QStringLiteral("Too wide")));
}

void encodedRasterFormatsAndMetadataPrecedence()
{
    QMimeData alternatives;
    alternatives.setData(QStringLiteral("image/x-unsupported-test-format"), QByteArrayLiteral("not encoded image data"));
    alternatives.setData(QStringLiteral("image/png"), encoded(referenceImage(), "png"));
    checkPixels(loadClipboardImage(alternatives), referenceImage());
    struct Format { const char* codec; const char* mime; };
    for (const auto format : {Format {"png", "image/png"}, Format {"jpeg", "image/jpeg"},
                              Format {"webp", "image/webp"}}) {
        // These are the app's supported raster formats. A missing plugin is
        // reported as a test failure instead of silently reducing coverage.
        CHECK(QImageReader::supportedImageFormats().contains(format.codec));
        CHECK(QImageWriter::supportedImageFormats().contains(format.codec));
        const auto bytes = encoded(referenceImage(), format.codec);
        QMimeData mime;
        mime.setData(QString::fromLatin1(format.mime), bytes);
        mime.setUrls({QUrl(QStringLiteral("https://example.invalid/not-an-image.png"))});
        const auto decoded = QImage::fromData(bytes, format.codec);
        CHECK(!decoded.isNull());
        if (!decoded.isNull()) checkPixels(loadClipboardImage(mime), decoded);
    }
}

void localFileRepresentationsAndDeterministicUrlLists()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    if (!directory.isValid()) return;
    const auto path = directory.filePath(QStringLiteral("画面 # colors.png"));
    const auto reference = referenceImage();
    CHECK(reference.save(path, "PNG"));
    const auto url = QUrl::fromLocalFile(path);
    for (const auto& text : {path, QStringLiteral("  ") + path + QStringLiteral("  "),
                            QStringLiteral("\"") + path + QStringLiteral("\""),
                            QStringLiteral("'") + path + QStringLiteral("'"),
                            url.toString(QUrl::FullyEncoded), QDir::current().relativeFilePath(path)}) {
        QMimeData mime;
        mime.setText(text);
        auto loaded = loadClipboardImage(mime);
        checkPixels(loaded, reference);
        CHECK(loaded && loaded.layer->name == QStringLiteral("画面 # colors").toStdString());
    }

    const auto notImage = directory.filePath(QStringLiteral("plain.png"));
    QFile file(notImage);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write("This is not image data.") > 0);
    file.close();
    QImage different(5, 4, QImage::Format_RGBA8888);
    different.fill(Qt::white);
    const auto secondPath = directory.filePath(QStringLiteral("second.png"));
    CHECK(different.save(secondPath, "PNG"));
    QMimeData urlMime;
    urlMime.setUrls({QUrl(QStringLiteral("https://example.invalid/image.png")),
        QUrl::fromLocalFile(notImage), QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.png"))),
        url, QUrl::fromLocalFile(secondPath)});
    checkPixels(loadClipboardImage(urlMime), reference);
    QUrl localhost = url;
    localhost.setHost(QStringLiteral("localhost"));
    QMimeData localHostMime;
    localHostMime.setText(localhost.toString(QUrl::FullyEncoded));
    checkPixels(loadClipboardImage(localHostMime), reference);

    QMimeData pixelMime;
    pixelMime.setUrls({QUrl::fromLocalFile(secondPath)});
    pixelMime.setImageData(reference);
    checkPixels(loadClipboardImage(pixelMime), reference); // Pixel payload beats file-manager metadata.
    QMimeData encodedMime;
    encodedMime.setData(QStringLiteral("image/png"), encoded(reference, "PNG"));
    encodedMime.setImageData(different);
    encodedMime.setUrls({QUrl::fromLocalFile(secondPath)});
    checkPixels(loadClipboardImage(encodedMime), reference);

    for (const auto& text : {QString(), QStringLiteral("not an image"), directory.path(), notImage,
                            directory.filePath(QStringLiteral("missing.png")),
                            QStringLiteral("https://example.invalid/image.png"),
                            QStringLiteral("http://example.invalid/image.png"),
                            QStringLiteral("data:image/png;base64,AAAA"),
                            QStringLiteral("file://remote.example") + path,
                            url.toString(QUrl::FullyEncoded) + QStringLiteral("?query=1"),
                            url.toString(QUrl::FullyEncoded) + QStringLiteral("#fragment"),
                            path + QStringLiteral("\n") + path}) {
        QMimeData mime;
        mime.setText(text);
        const auto loaded = loadClipboardImage(mime);
        CHECK(!loaded && !loaded.error.isEmpty());
    }
    QMimeData remoteUrls;
    remoteUrls.setUrls({QUrl(QStringLiteral("https://example.invalid/image.png")),
        QUrl(QStringLiteral("file://remote.example") + url.path())});
    CHECK(!loadClipboardImage(remoteUrls));
    QMimeData empty;
    CHECK(!loadClipboardImage(empty));
}

class ImageDataProbe final : public QMimeData {
public:
    mutable int imageDataRequests {0};
protected:
    QVariant retrieveData(const QString& mimeType, QMetaType type) const override
    {
        if (mimeType == QStringLiteral("application/x-qt-image")) ++imageDataRequests;
        return QMimeData::retrieveData(mimeType, type);
    }
};

void writeBigEndian(QByteArray& bytes, int offset, std::uint32_t value)
{
    for (int index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<char>((value >> (24 - 8 * index)) & 0xffU);
}

QByteArray oversizedPng(std::uint32_t width, std::uint32_t height)
{
    auto bytes = encoded(referenceImage(), "PNG");
    CHECK(bytes.size() > 33 && bytes.mid(12, 4) == QByteArrayLiteral("IHDR"));
    if (bytes.size() <= 33) return {};
    writeBigEndian(bytes, 16, width);
    writeBigEndian(bytes, 20, height);
    // A valid IHDR CRC allows the reader to report dimensions without
    // allocating a huge image. The original tiny IDAT is never decoded.
    std::uint32_t crc = 0xffffffffU;
    for (int index = 12; index < 29; ++index) {
        crc ^= static_cast<unsigned char>(bytes[index]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    writeBigEndian(bytes, 29, crc ^ 0xffffffffU);
    return bytes;
}

void advertisedOversizeAndMalformedDataCannotFallBackToImplicitDecode()
{
    for (const auto dimensions : {Extent2u {imageeditor::ui::kMaximumRasterDimension + 1, 1},
                                   Extent2u {8000, 6000}}) {
        ImageDataProbe mime;
        mime.setData(QStringLiteral("image/png"), oversizedPng(dimensions.width, dimensions.height));
        mime.setImageData(referenceImage());
        const auto loaded = loadClipboardImage(mime);
        CHECK(!loaded);
        CHECK(loaded.error.contains(QStringLiteral("exceeds")));
        CHECK(mime.imageDataRequests == 0);
    }
    ImageDataProbe malformed;
    malformed.setData(QStringLiteral("image/png"), QByteArrayLiteral("not a PNG"));
    malformed.setImageData(referenceImage());
    CHECK(!loadClipboardImage(malformed));
    CHECK(malformed.imageDataRequests == 0);
}
} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    imageObjectsAndSharedStraightRgbaConversion();
    encodedRasterFormatsAndMetadataPrecedence();
    localFileRepresentationsAndDeterministicUrlLists();
    advertisedOversizeAndMalformedDataCannotFallBackToImplicitDecode();
    if (failures) {
        std::cerr << failures << " clipboard-image assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All clipboard-image tests passed\n";
    return EXIT_SUCCESS;
}
