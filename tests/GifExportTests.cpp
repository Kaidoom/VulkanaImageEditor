#include "imageeditor/ui/GifExport.hpp"
#include "imageeditor/ui/GifImport.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/SnapshotPlayback.hpp"
#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QImageReader>
#include <QTemporaryDir>
#include <QTest>
#include <cstring>
#include <iostream>
#include <set>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace
{
int failures{};
#define CHECK(...)                                                                                           \
    do {                                                                                                     \
        if (!(__VA_ARGS__)) {                                                                                \
            ++failures;                                                                                      \
            std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n";                                                 \
        }                                                                                                    \
    } while (false)
std::shared_ptr<const c::SnapshotImage> capture(QImage image)
{
    image.setColorSpace({});
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    CHECK(image.save(&buffer, "PNG"));
    auto result = std::make_shared<c::SnapshotImage>();
    result->extent = {std::uint32_t(image.width()), std::uint32_t(image.height())};
    result->png.resize(std::size_t(bytes.size()));
    std::memcpy(result->png.data(), bytes.constData(), std::size_t(bytes.size()));
    return result;
}
c::SnapshotTimelineState timeline(const std::vector<QImage> &images)
{
    c::SnapshotTimelineState state;
    state.frames.clear();
    state.nextId = 1;
    for (const auto &image : images)
        state.frames.push_back({state.nextId++, 100, capture(image)});
    return state;
}
std::vector<QImage> read(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "GIF");
    std::vector<QImage> images;
    while (reader.canRead()) {
        auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
        CHECK(!image.isNull());
        if (image.isNull())
            break;
        images.push_back(image);
    }
    return images;
}
bool samePixels(QImage a, QImage b)
{
    a.setColorSpace({});
    b.setColorSpace({});
    return a.convertToFormat(QImage::Format_RGBA8888_Premultiplied) ==
           b.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}
void codec()
{
    std::atomic_bool cancel{false};
    QImage random(512, 512, QImage::Format_RGBA8888);
    std::uint32_t seed = 12345;
    for (int y = 0; y < random.height(); ++y)
        for (int x = 0; x < random.width(); ++x) {
            seed = 1664525 * seed + 1013904223;
            const auto v = int(seed >> 24);
            random.setPixelColor(x, y, QColor(v, (v * 13) & 255, 255 - v));
        }
    auto source = timeline({random});
    u::GifExportSettings options;
    options.size = random.size();
    options.dither = u::GifDither::None;
    auto encoded = u::encodeGif(source, options, cancel);
    CHECK(encoded);
    if (!encoded) {
        std::cerr << encoded.error.toStdString() << '\n';
        return;
    }
    auto images = read(encoded.bytes);
    CHECK(images.size() == 1 && images[0] == random); // LZW width growth and repeated dictionary reset.
    CHECK(samePixels(u::decodeGifPreview(encoded, 0), random));
    CHECK(u::decodeGifPreview(encoded, 0, QSize(1000, 1000)).size() == random.size());
    CHECK(encoded.bytes.contains("NETSCAPE2.0"));
    options.loop = false;
    auto once = u::encodeGif(source, options, cancel);
    CHECK(once && !once.bytes.contains("NETSCAPE2.0"));
    for (int length = 1; length <= 80; ++length) {
        QImage line(length, 1, QImage::Format_RGBA8888);
        for (int x = 0; x < length; ++x)
            line.setPixelColor(x, 0,
                               QColor::fromRgb((x + x / 5) % 3 == 0   ? qRgb(255, 0, 0)
                                               : (x + x / 5) % 3 == 1 ? qRgb(0, 255, 0)
                                                                      : qRgb(0, 0, 255)));
        options.size = line.size();
        options.colors = 3;
        auto shortStream = u::encodeGif(timeline({line}), options, cancel);
        CHECK(shortStream);
        const auto decoded = read(shortStream.bytes);
        CHECK(decoded.size() == 1 && decoded[0] == line);
    }

    QImage a(8, 6, QImage::Format_RGBA8888), b = a;
    a.fill(Qt::red);
    b.fill(Qt::blue);
    a.setPixelColor(0, 0, QColor(0, 0, 0, 0));
    b.setPixelColor(7, 5, QColor(0, 0, 0, 0));
    auto masks = timeline({a, b});
    options.size = a.size();
    options.colors = 8;
    encoded = u::encodeGif(masks, options, cancel);
    CHECK(encoded);
    images = read(encoded.bytes);
    CHECK(images.size() == 2);
    if (images.size() == 2) {
        CHECK(images[0].pixelColor(0, 0).alpha() == 0 && images[1].pixelColor(7, 5).alpha() == 0);
        CHECK(images[1].pixelColor(0, 0) == QColor(Qt::blue));
        CHECK(images[0].pixelColor(7, 5) == QColor(Qt::red));
        CHECK(samePixels(images[1], u::decodeGifPreview(encoded, 1)));
    }
    options.transparency = false;
    options.background = Qt::green;
    encoded = u::encodeGif(masks, options, cancel);
    images = read(encoded.bytes);
    CHECK(images.size() == 2 && images[0].pixelColor(0, 0) == QColor(Qt::green) &&
          images[1].pixelColor(7, 5) == QColor(Qt::green));
    a.fill(QColor(255, 0, 0, 128));
    options.background = Qt::blue;
    options.colors = 256;
    encoded = u::encodeGif(timeline({a}), options, cancel);
    images = read(encoded.bytes);
    CHECK(images.size() == 1 && images[0].pixelColor(0, 0) == QColor(128, 0, 127));
    a.fill(Qt::transparent);
    options.transparency = true;
    options.colors = 2;
    encoded = u::encodeGif(timeline({a}), options, cancel);
    images = read(encoded.bytes);
    CHECK(images.size() == 1 && images[0].pixelColor(3, 3).alpha() == 0);

    QImage gradient(256, 64, QImage::Format_RGBA8888);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 256; ++x)
            gradient.setPixelColor(x, y, QColor(x, x, x));
    source = timeline({gradient});
    options.size = gradient.size();
    options.colors = 2;
    options.transparency = false;
    std::set<QByteArray> outputs;
    for (auto mode :
         {u::GifDither::None, u::GifDither::Diffusion, u::GifDither::Pattern, u::GifDither::Noise}) {
        options.dither = mode;
        options.strength = 100;
        encoded = u::encodeGif(source, options, cancel);
        CHECK(encoded);
        outputs.insert(encoded.bytes);
        images = read(encoded.bytes);
        CHECK(images.size() == 1);
        if (images.empty())
            continue;
        std::set<QRgb> colors;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 256; ++x)
                colors.insert(images[0].pixel(x, y));
        CHECK(colors.size() <= 2);
        if (mode == u::GifDither::Pattern || mode == u::GifDither::Noise) {
            options.strength = 0;
            CHECK(u::encodeGif(source, options, cancel).bytes == encoded.bytes);
        }
    }
    CHECK(outputs.size() == 4);
    options.dither = u::GifDither::Diffusion;
    options.strength = 0;
    auto zero = u::encodeGif(source, options, cancel);
    options.dither = u::GifDither::None;
    CHECK(zero.bytes == u::encodeGif(source, options, cancel).bytes);
    for (int count : {2, 3, 16, 255, 256}) {
        options.colors = count;
        options.size = {63, 31};
        encoded = u::encodeGif(source, options, cancel);
        CHECK(encoded);
        images = read(encoded.bytes);
        CHECK(images.size() == 1 && images[0].size() == options.size);
    }
    source.frames.clear();
    source.nextId = 1;
    for (int i = 0; i < 10; ++i)
        source.frames.push_back({source.nextId++, 101, capture(b)});
    source.frames.push_back({source.nextId++, 100, {}}); // Empty slots are not exported.
    options.size = b.size();
    encoded = u::encodeGif(source, options, cancel);
    CHECK(encoded && encoded.frames.size() == 10);
    auto roundtrip = u::loadGifDocumentFromData(encoded.bytes, "Frame %1");
    CHECK(roundtrip);
    std::uint32_t duration = 0;
    for (const auto &frame : encoded.frames)
        duration += frame.durationMs;
    CHECK(duration == 1010);
    if (roundtrip)
        for (std::size_t i = 0; i < encoded.frames.size(); ++i)
            CHECK(roundtrip.document->snapshotTimeline().frames[i].durationMs ==
                  encoded.frames[i].durationMs);
    cancel = true;
    CHECK(u::encodeGif(source, options, cancel).cancelled);
    cancel = false;
    CHECK(!u::encodeGif({}, options, cancel));
    options.colors = 1;
    CHECK(!u::encodeGif(source, options, cancel));
    options.colors = 256;
    options.size = {32768, 32768};
    CHECK(!u::encodeGif(source, options, cancel));
}
void oncePlayback()
{
    QImage image(1, 1, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    auto state = timeline({image, image, image});
    for (auto &frame : state.frames)
        frame.durationMs = 60;
    u::SnapshotPlayback clock;
    clock.setTimeline(state);
    clock.setLooping(false);
    clock.play(1);
    QTest::qWait(250);
    CHECK(!clock.playing() && clock.currentFrame() == 3);
    QTest::qWait(80);
    CHECK(clock.currentFrame() == 3);
    clock.play(3);
    CHECK(clock.playing() && clock.currentFrame() == 1);
    clock.seek(3);
    QTest::qWait(90);
    CHECK(!clock.playing() && clock.currentFrame() == 3);
    clock.setLooping(true);
    clock.play(3);
    QTest::qWait(230);
    CHECK(clock.playing());
    clock.clear();
}
void sample(const QString &input, const QString &output)
{
    auto loaded = u::loadGifDocument(input, "Frame %1");
    CHECK(loaded);
    if (!loaded)
        return;
    const auto size = loaded.document->canvas().extent;
    u::GifExportSettings options;
    options.size = {int(size.width), int(size.height)};
    std::atomic_bool cancel{false};
    QElapsedTimer elapsed;
    elapsed.start();
    auto result = u::encodeGif(loaded.document->snapshotTimeline(), options, cancel);
    CHECK(result);
    if (!result) {
        std::cerr << result.error.toStdString() << '\n';
        return;
    }
    CHECK(read(result.bytes).size() == loaded.document->snapshotTimeline().frames.size());
    CHECK(u::writeExportAtomically(output, result.bytes, cancel));
    std::cout << "GIF export: " << result.frames.size() << " frames, " << result.bytes.size() << " bytes, "
              << elapsed.elapsed() << " ms\n";
}
} // namespace
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    codec();
    oncePlayback();
    if (argc == 3)
        sample(app.arguments()[1], app.arguments()[2]);
    return failures ? 1 : 0;
}
