#include "imageeditor/ui/GifImport.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
struct Cancelled {};
void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
int byte(const QByteArray& data, int at) { return static_cast<unsigned char>(data[at]); }
int word(const QByteArray& data, int at) { return byte(data, at) | (byte(data, at + 1) << 8); }
struct GifInfo { QSize size; std::vector<std::uint32_t> durations; };

// Only scan framing and metadata here: pixel decoding, interlace, palettes and
// disposal are handled by Qt's GIF codec. Read raw delays because that codec
// changes 10 ms delays to 100 ms for browser-style playback compatibility.
GifInfo scan(QIODevice& file, const std::function<void()>& check, std::uint64_t budget)
{
    const auto read = [&](int count) {
        check();
        auto bytes = file.read(count);
        require(bytes.size() == count, "Truncated GIF data.");
        return bytes;
    };
    const auto skip = [&](int count) {
        check();
        require(count <= file.size() - file.pos() && file.seek(file.pos() + count), "Truncated GIF block.");
    };
    const auto blocks = [&] {
        for (int size = byte(read(1), 0); size; size = byte(read(1), 0)) skip(size);
    };
    const auto header = read(13);
    require(header.startsWith("GIF87a") || header.startsWith("GIF89a"), "Not a GIF image.");
    GifInfo info{{word(header, 6), word(header, 8)}, {}};
    require(rasterExtentWithinLimits(std::uint32_t(info.size.width()), std::uint32_t(info.size.height())),
        "GIF canvas exceeds the image size limits.");
    const auto frameBytes = std::uint64_t(info.size.width()) * std::uint64_t(info.size.height()) * 4;
    // Allow for the decoder, conversion, PNG encoding and presentation upload.
    const auto overhead = frameBytes * 6 + 64 * 1024 * 1024;
    require(overhead < budget, "Not enough working memory to import this GIF.");
    if (byte(header, 10) & 0x80) skip(3 * (2 << (byte(header, 10) & 7)));
    std::uint32_t delay = 100;
    for (;;) {
        const int tag = byte(read(1), 0);
        if (tag == 0x3b) break;
        if (tag == 0x21) {
            const int type = byte(read(1), 0);
            if (type == 0xf9) {
                require(byte(read(1), 0) == 4, "Invalid GIF graphics control block.");
                const auto control = read(4);
                delay = std::uint32_t(word(control, 1)) * 10;
                // GIF permits no specified delay; use the timeline's 100 ms default.
                if (!delay) delay = 100;
                require(byte(read(1), 0) == 0, "Invalid GIF graphics control terminator.");
            } else {
                require(type != 0x01, "GIF plain-text rendering is not supported.");
                blocks();
            }
        } else if (tag == 0x2c) {
            const auto descriptor = read(9);
            const int width = word(descriptor, 4), height = word(descriptor, 6);
            require(width > 0 && height > 0 && word(descriptor, 0) + width <= info.size.width()
                && word(descriptor, 2) + height <= info.size.height(), "GIF frame lies outside its canvas.");
            if (byte(descriptor, 8) & 0x80) skip(3 * (2 << (byte(descriptor, 8) & 7)));
            const int codeSize = byte(read(1), 0);
            require(codeSize >= 2 && codeSize <= 8, "Invalid GIF LZW code size.");
            blocks();
            require(info.durations.size() < (budget - overhead) / (frameBytes * 2 + sizeof(core::Layer)),
                "GIF frames exceed available working memory.");
            info.durations.push_back(delay);
            delay = 100;
        } else throw std::runtime_error("Invalid GIF block.");
    }
    require(!info.durations.empty(), "GIF contains no image frames.");
    return info;
}
}

bool isGifFile(const QString& path)
{
    if (QFileInfo(path).suffix().compare("gif", Qt::CaseInsensitive) == 0) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto header = file.read(6);
    return header == "GIF87a" || header == "GIF89a";
}

namespace {
GifImportResult readGifDocument(QIODevice& file, const QString& frameName,
    std::function<bool(std::size_t, std::size_t)> progress, std::uint64_t workingBytes)
try {
    const auto check = [&] { if (progress && !progress(0, 0)) throw Cancelled{}; };
    const auto budget = workingBytes ? workingBytes : platform::availableWorkingMemoryBytes();
    const auto info = scan(file, check, budget);
    require(file.seek(0), "Cannot rewind GIF data.");
    QImageReader reader(&file, "GIF");
    auto document = std::make_unique<core::Document>(core::CanvasSpec{
        {std::uint32_t(info.size.width()), std::uint32_t(info.size.height())}, 96.0});
    core::SnapshotTimelineState timeline;
    timeline.frames.clear();
    timeline.frames.reserve(info.durations.size());
    timeline.nextId = 1;
    std::vector<core::Layer> layers;
    layers.reserve(info.durations.size());
    std::uint64_t retained = 0;
    const auto frameBytes = std::uint64_t(info.size.width()) * std::uint64_t(info.size.height()) * 4;
    for (std::size_t index = 0; index < info.durations.size(); ++index) {
        if (progress && !progress(index, info.durations.size())) throw Cancelled{};
        // Sequential read() returns the fully composed logical screen, including
        // restore-background/restore-previous disposal, not just a delta rectangle.
        auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
        require(!image.isNull() && image.size() == info.size, "Cannot decode a complete GIF frame.");
        image.setColorSpace({}); // Pixel-only PNGs are native snapshot payloads.
        image.setDotsPerMeterX(qRound(96.0 / .0254));
        image.setDotsPerMeterY(qRound(96.0 / .0254));
        QByteArray png;
        QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        // QImageWriter uses a 0–100 compression scale, not zlib's 0–9.
        QImageWriter writer(&buffer, "PNG"); writer.setCompression(50);
        require(writer.write(image), "Cannot encode the GIF snapshot.");
        retained += frameBytes * 2 + std::uint64_t(png.size()) + sizeof(core::Layer);
        require(retained + frameBytes * 6 + 64 * 1024 * 1024 <= budget,
            "GIF frames exceed available working memory.");
        check();
        auto raster = rasterLayerFromImage(image, frameName.arg(qulonglong(index)));
        if (!raster) throw std::runtime_error(raster.error.toStdString());
        raster.layer->visible = index == 0;
        layers.push_back(std::move(*raster.layer));
        auto snapshot = std::make_shared<core::SnapshotImage>();
        snapshot->extent = document->canvas().extent;
        snapshot->png.resize(std::size_t(png.size()));
        std::memcpy(snapshot->png.data(), png.constData(), std::size_t(png.size()));
        timeline.frames.push_back({timeline.nextId++, info.durations[index], std::move(snapshot)});
    }
    if (progress && !progress(info.durations.size(), info.durations.size())) throw Cancelled{};
    // Publish the entire stack once, avoiding a tree rebuild per decoded frame.
    // Frame 0 is topmost and is the initial editing target.
    std::reverse(layers.begin(), layers.end());
    core::LayerTree tree;
    for (const auto& layer : layers) tree.roots.push_back(layer.id);
    require(document->replaceStructure(document->tree(), std::move(tree), {}, layers), "Cannot add GIF frame layers.");
    require(document->setSnapshotTimeline(document->snapshotTimeline(), std::move(timeline)),
        "Cannot initialize the GIF timeline.");
    document->markSaved();
    return {std::move(document), {}};
} catch (const Cancelled&) {
    return {{}, {}, true};
} catch (const std::exception& error) {
    return {{}, QString::fromUtf8(error.what())};
}
}

GifImportResult loadGifDocument(const QString& path, const QString& frameName,
    std::function<bool(std::size_t, std::size_t)> progress, std::uint64_t workingBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {{}, file.errorString()};
    return readGifDocument(file, frameName, std::move(progress), workingBytes);
}
GifImportResult loadGifDocumentFromData(const QByteArray& data, const QString& frameName,
    std::function<bool(std::size_t, std::size_t)> progress, std::uint64_t workingBytes)
{
    QBuffer buffer;
    buffer.setData(data); // Shared encoded bytes, no temporary file or pixel decode.
    if (!buffer.open(QIODevice::ReadOnly)) return {{}, buffer.errorString()};
    return readGifDocument(buffer, frameName, std::move(progress), workingBytes);
}
}
