#include "imageeditor/project/ProjectPreview.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>
#include <cstring>

namespace imageeditor::project {
namespace {
bool boundedPng(const QByteArray& png)
{
    if (!png.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) return false;
    // The thumbnail format deliberately carries no compressed text/profile or
    // animation metadata. A small image must not hide an ancillary decompression
    // bomb; validate chunk framing/CRCs before calling the system PNG decoder.
    qsizetype offset = 8;
    bool header = false, pixels = false;
    while (png.size()-offset >= 12) {
        const auto size = qFromBigEndian<quint32>(png.constData()+offset);
        if (size > quint64(png.size()-offset-12)) return false;
        const auto kind = QByteArrayView(png.constData()+offset+4,4);
        if (kind == "IHDR") {
            if (header || offset != 8 || size != 13) return false;
            header = true;
        } else if (kind == "IDAT") pixels = true;
        else if (kind == "IEND") {
            if (!header || !pixels || size || offset+12 != png.size()) return false;
        } else if (!((kind == "pHYs" && size == 9) || (kind == "sRGB" && size == 1)
            || (kind == "gAMA" && size == 4) || (kind == "cHRM" && size == 32)
            || (kind == "PLTE" && size <= 768) || (kind == "tRNS" && size <= 256))) return false;
        const auto crc = mz_crypt_crc32_update(0,
            reinterpret_cast<const uint8_t*>(png.constData()+offset+4),int32_t(size+4));
        if (crc != qFromBigEndian<quint32>(png.constData()+offset+8+size)) return false;
        offset += qsizetype(size)+12;
        if (kind == "IEND") return true;
    }
    return false;
}
}
QByteArray previewFingerprint(QJsonObject manifest, const Entries& files)
{
    manifest.remove("preview");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("Vulkana preview v1\n"));
    hash.addData(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    for (const auto& [name, entry] : files) {
        if (name == "manifest.json") continue;
        // Entries are name-sorted; JSON quoting makes arbitrary UTF-8 paths
        // unambiguous. Decimal strings do not lose 64-bit sizes to JSON doubles.
        hash.addData(QJsonDocument(QJsonArray{name,QString::number(entry.bytes),QString::number(entry.crc)})
            .toJson(QJsonDocument::Compact));
    }
    return hash.result().toHex();
}
QJsonObject encodePreview(const QImage& image, const QJsonObject& manifest, const Entries& files)
{
    if (image.isNull() || image.width() > previewSide || image.height() > previewSide) return {};
    QByteArray png;
    QBuffer buffer(&png);
    // Do not inherit arbitrary image metadata into a file-manager thumbnail.
    const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QImage clean(image.size(),QImage::Format_RGBA8888);
    if (rgba.isNull() || clean.isNull()) return {};
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(clean.scanLine(y),rgba.constScanLine(y),std::size_t(image.width())*4);
    if (!buffer.open(QIODevice::WriteOnly) || !clean.save(&buffer,"PNG") || png.size() > previewPngBudget
        || !boundedPng(png)) return {};
    return {{"version",1},{"contentSha256",QString::fromLatin1(previewFingerprint(manifest,files))},
        {"png",QString::fromLatin1(png.toBase64())}};
}
QImage decodePreview(const QJsonObject& manifest, const Entries& files, int maximumSide)
{
    if (maximumSide < 1 || maximumSide > 4096 || manifest["format"] != "org.vulkana.project"
        || manifest["version"] != 1) return {};
    const auto preview = manifest["preview"].toObject();
    const auto encoded = preview["png"].toString();
    if (preview["version"] != 1 || encoded.isEmpty() || encoded.size() > (previewPngBudget+2)/3*4
        || preview["contentSha256"].toString().toLatin1() != previewFingerprint(manifest,files)) return {};
    const auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > previewPngBudget
        || !boundedPng(decoded.decoded)) return {};
    QBuffer input;
    input.setData(decoded.decoded);
    if (!input.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&input,"PNG");
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > previewSide || size.height() > previewSide) return {};
    const auto image = reader.read();
    if (image.isNull()) return {};
    return image.width() > maximumSide || image.height() > maximumSide
        ? image.scaled(maximumSide,maximumSide,Qt::KeepAspectRatio,Qt::SmoothTransformation) : image;
}
QImage readPreview(const QString& path, int maximumSide)
{
    if (path.isEmpty() || path.contains(QChar::Null)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return readPreview(file, maximumSide);
}
QImage readPreview(QIODevice& file, int maximumSide)
{
    try {
        if (!file.isReadable() || file.isSequential() || maximumSide < 1 || maximumSide > 4096) return {};
        Archive zip(file,false);
        const auto files = entries(zip);
        const auto found = files.find("manifest.json");
        if (found == files.end() || found->second.bytes > manifestBudget) return {};
        QByteArray json;
        json.reserve(qsizetype(found->second.bytes));
        Progress progress;
        readEntry(zip,found->second,[&](const char* bytes,int n) { json.append(bytes,n); },progress);
        QJsonParseError error;
        const auto manifest = QJsonDocument::fromJson(json,&error);
        if (error.error != QJsonParseError::NoError || !manifest.isObject()) return {};
        return decodePreview(manifest.object(),files,maximumSide);
    } catch (const std::exception&) {
        return {}; // Damaged/missing optional previews always fall back to the file icon.
    }
}
}
