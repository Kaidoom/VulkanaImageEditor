#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>

namespace imageeditor::ui {
namespace {
    constexpr quint64 maximumPixels = 32ULL * 1024 * 1024;
    constexpr quint64 maximumWork = 128ULL * 1024 * 1024;
    constexpr qsizetype maximumEncodedBytes = 256 * 1024 * 1024;
    void require(bool condition, const char* reason)
    {
        if (!condition)
            throw std::runtime_error(reason);
    }
    struct Cancelled { };
    void checkCancel(const std::atomic_bool& cancel)
    {
        if (cancel.load())
            throw Cancelled { };
    }
    struct WebpSink {
        QByteArray bytes;
        const std::atomic_bool* cancel;
        bool failed { false };
    };
    int appendWebp(const uint8_t* data, size_t size, const WebPPicture* picture)
    {
        auto& sink = *static_cast<WebpSink*>(picture->custom_ptr);
        if (sink.cancel->load() || size > std::size_t(maximumEncodedBytes - sink.bytes.size()))
            return 0;
        try {
            sink.bytes.append(reinterpret_cast<const char*>(data), qsizetype(size));
            return 1;
        } catch (...) {
            sink.failed = true;
            return 0;
        } // No exceptions cross libwebp's C ABI.
    }
    int webpProgress(int, const WebPPicture* picture)
    {
        return !static_cast<WebpSink*>(picture->custom_ptr)->cancel->load();
    }
    QByteArray encodeWebp(const QImage& image, const ExportSettings& settings, const std::atomic_bool& cancel)
    {
        WebPConfig config;
        require(WebPConfigInit(&config), "Cannot initialize WebP encoder");
        config.lossless = settings.webpLossless;
        config.quality = settings.webpLossless ? 75.0F : float(settings.webpQuality);
        config.method = settings.webpEffort;
        config.alpha_compression = 1;
        config.alpha_filtering = 1;
        config.alpha_quality = 100;
        config.near_lossless = 100;
        config.exact = 1; // Lossless keeps canonical zero-alpha RGB; lossy RGB
                          // remains lossy even behind alpha=0.
        config.thread_level = 1;
        require(WebPValidateConfig(&config), "Invalid WebP options");
        WebPPicture picture;
        require(WebPPictureInit(&picture), "Cannot initialize WebP picture");
        struct Guard {
            WebPPicture* p;
            ~Guard() { WebPPictureFree(p); }
        } guard { &picture };
        picture.width = image.width();
        picture.height = image.height();
        picture.use_argb = 1;
        WebpSink sink { { }, &cancel };
        picture.custom_ptr = &sink;
        picture.writer = appendWebp;
        picture.progress_hook = webpProgress;
        require(WebPPictureImportRGBA(&picture, image.constBits(), int(image.bytesPerLine())),
            "Not enough memory for WebP input");
        const bool encoded = WebPEncode(&config, &picture);
        checkCancel(cancel);
        require(encoded && !sink.failed, "WebP encoding failed or exceeded the encoded-byte budget");
        WebPData bitstream { reinterpret_cast<const uint8_t*>(sink.bytes.constData()),
            std::size_t(sink.bytes.size()) };
        std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(WebPMuxCreate(&bitstream, 0), WebPMuxDelete);
        require(bool(mux), "Cannot initialize WebP metadata container");
        const QByteArray profile = image.colorSpace().iccProfile();
        WebPData icc { reinterpret_cast<const uint8_t*>(profile.constData()), std::size_t(profile.size()) };
        require(
            WebPMuxSetChunk(mux.get(), "ICCP", &icc, 1) == WEBP_MUX_OK, "Cannot attach sRGB profile to WebP");
        WebPData assembled { };
        const auto assembledStatus = WebPMuxAssemble(mux.get(), &assembled);
        struct DataGuard {
            WebPData* d;
            ~DataGuard() { WebPDataClear(d); }
        } dataGuard { &assembled };
        require(assembledStatus == WEBP_MUX_OK && assembled.size <= std::size_t(maximumEncodedBytes),
            "Cannot assemble WebP output within the byte budget");
        checkCancel(cancel);
        WebPBitstreamFeatures features { };
        require(WebPGetFeatures(assembled.bytes, assembled.size, &features) == VP8_STATUS_OK
                && features.format == (settings.webpLossless ? 2 : 1),
            "WebP encoder returned an unexpected bitstream mode");
        return QByteArray(reinterpret_cast<const char*>(assembled.bytes), qsizetype(assembled.size));
    }
} // namespace
QByteArray exportFormatName(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Png:
        return "png";
    case ExportFormat::Jpeg:
        return "jpeg";
    case ExportFormat::WebP:
        return "webp";
    case ExportFormat::Pdf:
        return "pdf";
    }
    return { };
}
QColor exportMatteColor(const ExportSettings& settings)
{
    return settings.format == ExportFormat::Jpeg ? settings.jpegMatteColor : settings.pngMatteColor;
}
QString exportPathForFormat(const QString& path, ExportFormat format)
{
    if (path.isEmpty())
        return { };
    const auto suffix = QFileInfo(path).suffix().toLower();
    if ((format == ExportFormat::Png && suffix == "png") || (format == ExportFormat::WebP && suffix == "webp")
        || (format == ExportFormat::Jpeg && (suffix == "jpg" || suffix == "jpeg")))
        return path;
    const auto extension = format == ExportFormat::Jpeg ? QStringLiteral("jpg")
                                                        : QString::fromLatin1(exportFormatName(format));
    return (suffix.isEmpty() ? path : path.left(path.size() - suffix.size() - 1)) + '.' + extension;
}
QString validateExport(const ExportSettings& s, QSize canvas)
{
    if(s.format==ExportFormat::Pdf)return {}; // Geometry/selection validated by the immutable PDF page plan.
    const auto invalidSize = [](QSize size) {
        return size.width() < 1 || size.height() < 1 || size.width() > kMaximumExportDimension
            || size.height() > kMaximumExportDimension;
    };
    if (invalidSize(s.size) || invalidSize(canvas))
        return QStringLiteral("Export and canvas dimensions must be between 1 and 32768 pixels.");
    if (s.format == ExportFormat::WebP && (s.size.width() > 16383 || s.size.height() > 16383))
        return QStringLiteral("WebP supports at most 16383 × 16383 pixels. Choose "
                              "smaller output dimensions or another format.");
    if (quint64(s.size.width()) * quint64(s.size.height()) > maximumPixels)
        return QStringLiteral("Export exceeds the 32-megapixel output budget. "
                              "Choose smaller output dimensions.");
    if (quint64(std::max(canvas.width(), s.size.width()))
            * quint64(std::max(canvas.height(), s.size.height()))
        > maximumWork)
        return QStringLiteral("Export exceeds the reconstruction work budget. Use "
                              "less extreme output dimensions.");
    if (s.pngEffort < 0 || s.pngEffort > 9 || s.jpegQuality < 1 || s.jpegQuality > 100 || s.webpQuality < 0
        || s.webpQuality > 100 || s.webpEffort < 0 || s.webpEffort > 6 || !s.pngMatteColor.isValid()
        || !s.jpegMatteColor.isValid())
        return QStringLiteral("Invalid export encoding options.");
    const auto format = exportFormatName(s.format);
    if (format.isEmpty())
        return QStringLiteral("Unsupported export format.");
    if (s.format != ExportFormat::WebP
        && (!QImageWriter::supportedImageFormats().contains(format)
            || !QImageReader::supportedImageFormats().contains(format)))
        return QStringLiteral("The %1 image codec is unavailable. Install the Qt "
                              "image codec runtime.")
            .arg(QString::fromLatin1(format).toUpper());
    return { };
}
QString validateExportDestination(const QString& path, const QString& projectPath)
{
    if (path.trimmed().isEmpty())
        return QStringLiteral("Choose an export destination.");
    const QFileInfo file(path);
    if (file.suffix().compare("vulkana", Qt::CaseInsensitive) == 0)
        return QStringLiteral("Export cannot overwrite a .vulkana project. Choose "
                              "an image filename.");
    if (!projectPath.isEmpty()) {
        const QFileInfo project(projectPath);
        if (file.absoluteFilePath() == project.absoluteFilePath()
            || (!file.canonicalFilePath().isEmpty()
                && file.canonicalFilePath() == project.canonicalFilePath()))
            return QStringLiteral("Export cannot overwrite the active project.");
    }
    if (file.isDir())
        return QStringLiteral("The export destination is a directory, not a file.");
    if (!QDir(file.absolutePath()).exists())
        return QStringLiteral("The export folder does not exist.");
    return { };
}
FlattenedDocumentResult renderExport(
    const core::Document& document, const ExportSettings& settings, FlattenedDocumentProgress progress)
{
    if(settings.format==ExportFormat::Pdf)return {{},QStringLiteral("PDF export requires a page plan"),false,{}};
    const auto extent = document.canvas().extent;
    if (const auto error = validateExport(settings, { int(extent.width), int(extent.height) });
        !error.isEmpty())
        return { { }, error, false, { } };
    std::optional<core::Rgba8> matte;
    const auto color = exportMatteColor(settings);
    if (settings.format == ExportFormat::Jpeg || (settings.format == ExportFormat::Png && settings.pngMatte))
        matte = core::Rgba8 { std::uint8_t(color.red()), std::uint8_t(color.green()),
            std::uint8_t(color.blue()), 255 };
    FlattenedDocumentLimits limits;
    limits.outputPixels = maximumPixels;
    auto result = flattenDocumentAtSize(document,
        { std::uint32_t(settings.size.width()), std::uint32_t(settings.size.height()) }, std::move(progress),
        limits, matte);
    if (result)
        result.image.setColorSpace(QColorSpace::SRgb);
    return result;
}
EncodedExport encodeExport(
    const QImage& source, const ExportSettings& settings, const std::atomic_bool& cancel)
{
    if(settings.format==ExportFormat::Pdf)return {{},{},QStringLiteral("PDF export uses the streaming page writer"),false};
    try {
        checkCancel(cancel);
        const auto error = validateExport(settings, source.size());
        if (!error.isEmpty())
            return { { }, { }, error, false };
        require(!source.isNull() && source.size() == settings.size,
            "Export buffer dimensions differ from the selected output size");
        // Allocate fresh metadata, not source.copy(): even callers outside the
        // dialog cannot propagate camera EXIF, orientation, text or file paths.
        const auto rgba = source.convertToFormat(QImage::Format_RGBA8888);
        QImage clean(source.size(), QImage::Format_RGBA8888);
        require(!clean.isNull(), "Not enough memory for encoder input");
        for (int y = 0; y < clean.height(); ++y) {
            if (y % 64 == 0)
                checkCancel(cancel);
            std::memcpy(clean.scanLine(y), rgba.constScanLine(y), std::size_t(clean.width()) * 4);
            auto* row = clean.scanLine(y);
            for (int x = 0; x < clean.width(); ++x)
                if (row[x * 4 + 3] == 0)
                    row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
        }
        clean.setColorSpace(QColorSpace::SRgb);
        clean.setDotsPerMeterX(source.dotsPerMeterX());
        clean.setDotsPerMeterY(source.dotsPerMeterY());
        QByteArray bytes;
        if (settings.format == ExportFormat::WebP)
            bytes = encodeWebp(clean, settings, cancel);
        else {
            if (settings.format == ExportFormat::Jpeg || settings.pngMatte)
                clean = clean.convertToFormat(QImage::Format_RGB888);
            QBuffer buffer(&bytes);
            require(buffer.open(QIODevice::WriteOnly), "Cannot open encoder buffer");
            QImageWriter writer(&buffer, exportFormatName(settings.format));
            if (settings.format == ExportFormat::Png)
                writer.setCompression((settings.pngEffort * 91 + 8) / 9);
            else {
                writer.setQuality(settings.jpegQuality);
                writer.setOptimizedWrite(true);
            }
            if (!writer.write(clean))
                return { { }, { }, writer.errorString(), false };
        }
        checkCancel(cancel);
        require(bytes.size() <= maximumEncodedBytes, "Encoded image exceeds the 256 MiB byte budget");
        QImage decoded;
        if (settings.format == ExportFormat::WebP) {
            decoded = QImage(settings.size, QImage::Format_RGBA8888);
            require(!decoded.isNull()
                    && WebPDecodeRGBAInto(reinterpret_cast<const uint8_t*>(bytes.constData()),
                        std::size_t(bytes.size()), decoded.bits(), std::size_t(decoded.sizeInBytes()),
                        int(decoded.bytesPerLine())),
                "Cannot decode the encoded WebP preview");
        } else
            decoded = QImage::fromData(bytes, exportFormatName(settings.format).constData());
        checkCancel(cancel);
        require(
            !decoded.isNull() && decoded.size() == settings.size, "Cannot decode the encoded export preview");
        decoded.setColorSpace(QColorSpace::SRgb);
        return { std::move(bytes), std::move(decoded), { }, false };
    } catch (const Cancelled&) {
        return { { }, { }, { }, true };
    } catch (const std::bad_alloc&) {
        return { { }, { }, QStringLiteral("Not enough memory for image encoding"), false };
    } catch (const std::exception& e) {
        return { { }, { }, QString::fromUtf8(e.what()), false };
    }
}
ExportWriteResult writeExportAtomically(
    const QString& path, const QByteArray& bytes, const std::atomic_bool& cancel)
{
    try {
        if (cancel.load())
            return { { }, true };
        if (bytes.isEmpty())
            return { QStringLiteral("There is no encoded output to write."), false };
        if (const auto error = validateExportDestination(path); !error.isEmpty())
            return { error, false };
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return { file.errorString(), false };
        constexpr qsizetype chunk = 1024 * 1024;
        for (qsizetype offset = 0; offset < bytes.size(); offset += chunk) {
            if (cancel.load()) {
                file.cancelWriting();
                return { { }, true };
            }
            const auto count = std::min(chunk, bytes.size() - offset);
            if (file.write(bytes.constData() + offset, count) != count) {
                const auto error = file.errorString();
                file.cancelWriting();
                return { error, false };
            }
        }
        if (cancel.load()) {
            file.cancelWriting();
            return { { }, true };
        }
        // This is the atomic publication point. A cancellation after a successful
        // commit cannot retroactively turn a published file into a failure.
        if (!file.commit())
            return { file.errorString(), false };
        return { };
    } catch (const std::bad_alloc&) {
        return { QStringLiteral("Not enough memory to write the image. The previous "
                                "file was not replaced."),
            false };
    } catch (const std::exception& e) {
        return { QString::fromUtf8(e.what()), false };
    }
}
bool hasExportPreferences()
{
    QSettings s;
    return s.contains("export/v1/destination");
}
ExportSettings loadExportPreferences(QSize canvas, const QString& suggestedName)
{
    QSettings s;
    s.beginGroup("export/v1");
    ExportSettings out;
    out.size = canvas;
    out.format = ExportFormat(std::clamp(s.value("format", 0).toInt(), 0, 3));
    const auto previous = s.value("destination").toString();
    auto name = previous.isEmpty() ? QFileInfo(suggestedName).completeBaseName() + ".png"
                                   : QFileInfo(previous).fileName();
    if (name.isEmpty() || name == ".png") name = QStringLiteral("Untitled.png");
    out.destination = QDir(lastExportDirectory()).filePath(name);
    out.aspectLocked = s.value("aspectLocked", true).toBool();
    out.pngMatte = s.value("pngMatte", false).toBool();
    out.pngMatteColor = s.value("pngMatteColor", QColor(Qt::white)).value<QColor>();
    out.jpegMatteColor = s.value("jpegMatteColor", QColor(Qt::white)).value<QColor>();
    if (!out.pngMatteColor.isValid())
        out.pngMatteColor = Qt::white;
    if (!out.jpegMatteColor.isValid())
        out.jpegMatteColor = Qt::white;
    out.pngEffort = std::clamp(s.value("pngEffort", 6).toInt(), 0, 9);
    out.jpegQuality = std::clamp(s.value("jpegQuality", 92).toInt(), 1, 100);
    out.webpLossless = s.value("webpLossless", false).toBool();
    out.webpQuality = std::clamp(s.value("webpQuality", 90).toInt(), 0, 100);
    out.webpEffort = std::clamp(s.value("webpEffort", 4).toInt(), 0, 6);
    out.pdf.mode=PdfExportMode(std::clamp(s.value("pdf/mode",0).toInt(),0,1));
    out.pdf.pageSize=PdfExportPageSize(std::clamp(s.value("pdf/pageSize",0).toInt(),0,1));
    out.pdf.text=PdfExportText(std::clamp(s.value("pdf/text",0).toInt(),0,1));
    out.pdf.reverse=s.value("pdf/reverse",false).toBool();
    out.pdf.ignoreHidden=s.value("pdf/ignoreHidden",true).toBool();
    out.pdf.matte=s.value("pdf/matte",false).toBool();
    out.pdf.matteColor=s.value("pdf/matteColor",QColor(Qt::white)).value<QColor>();
    if(!out.pdf.matteColor.isValid())out.pdf.matteColor=Qt::white;
    return out;
}
void saveExportPreferences(const ExportSettings& value)
{
    QSettings s;
    s.beginGroup("export/v1");
    s.setValue("destination", value.destination);
    s.setValue("format", int(value.format));
    s.setValue("width", value.size.width());
    s.setValue("height", value.size.height());
    s.setValue("aspectLocked", value.aspectLocked);
    s.setValue("pngMatte", value.pngMatte);
    s.setValue("pngMatteColor", value.pngMatteColor);
    s.setValue("jpegMatteColor", value.jpegMatteColor);
    s.setValue("pngEffort", value.pngEffort);
    s.setValue("jpegQuality", value.jpegQuality);
    s.setValue("webpLossless", value.webpLossless);
    s.setValue("webpQuality", value.webpQuality);
    s.setValue("webpEffort", value.webpEffort);
    s.setValue("pdf/mode",int(value.pdf.mode));
    s.setValue("pdf/pageSize",int(value.pdf.pageSize));
    s.setValue("pdf/text",int(value.pdf.text));
    s.setValue("pdf/reverse",value.pdf.reverse);
    s.setValue("pdf/ignoreHidden",value.pdf.ignoreHidden);
    s.setValue("pdf/matte",value.pdf.matte);
    s.setValue("pdf/matteColor",value.pdf.matteColor);
    s.endGroup();
    s.sync();
    rememberExportDirectory(value.destination);
}
} // namespace imageeditor::ui
