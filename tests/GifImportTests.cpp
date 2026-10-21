#include "imageeditor/ui/GifImport.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/SnapshotPlayback.hpp"
#include "GifFixture.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures{};
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
QImage snapshot(const c::SnapshotFrame& frame)
{
    return QImage::fromData(reinterpret_cast<const uchar*>(frame.image->png.data()), int(frame.image->png.size()), "PNG")
        .convertToFormat(QImage::Format_RGBA8888);
}
QImage raster(const c::Layer& layer)
{
    const auto surface = std::get<c::RasterLayer>(layer.payload).surface;
    const auto size = surface->extent();
    QImage image(int(size.width), int(size.height), QImage::Format_RGBA8888);
    surface->copyRgba8({0,0,int(size.width),int(size.height)},
        {reinterpret_cast<std::byte*>(image.bits()), std::size_t(image.sizeInBytes())}, std::size_t(image.bytesPerLine()));
    return image;
}
QString write(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
{
    const auto path = dir.filePath(name);
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write(bytes) == bytes.size());
    return path;
}
void importTests()
{
    QTemporaryDir dir;
    const auto bytes = imageeditor::test::gifFixture();
    const auto path = write(dir, "source.gif", bytes);
    auto result = u::loadGifDocument(path, "Frame %1");
    CHECK(result);
    if (!result) { std::cerr << result.error.toStdString(); return; }
    auto& doc = *result.document;
    const auto& frames = doc.snapshotTimeline().frames;
    CHECK(frames.size() == 5 && doc.layers().size() == 5);
    if (frames.size() != 5) return;
    const std::uint32_t delays[] {40, 70, 10, 120, 100};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(frames[i].durationMs == delays[i]);
        CHECK(frames[i].id == i + 1);
        const auto& layer = doc.layers()[4-i];
        CHECK(layer.name == "Frame " + std::to_string(i));
        CHECK(layer.visible == (i == 0));
        CHECK(raster(layer) == snapshot(frames[i]));
    }
    CHECK(snapshot(frames[0]).pixelColor(1,1) == QColor(Qt::red));
    CHECK(snapshot(frames[1]).pixelColor(1,1) == QColor(Qt::green));
    CHECK(snapshot(frames[2]).pixelColor(1,1).alpha() == 0);
    CHECK(snapshot(frames[2]).pixelColor(3,0) == QColor(Qt::blue));
    CHECK(snapshot(frames[3]).pixelColor(3,0) == QColor(Qt::red));
    CHECK(snapshot(frames[3]).pixelColor(0,2) == QColor(Qt::green));
    CHECK(snapshot(frames[4]) == snapshot(frames[3]));
    CHECK(!doc.isModified());
    auto fromData = u::loadGifDocumentFromData(bytes, "Frame %1");
    CHECK(fromData && fromData.document->snapshotTimeline().frames.size() == frames.size());
    if (fromData) for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& copy = fromData.document->snapshotTimeline().frames[i];
        CHECK(copy.durationMs == frames[i].durationMs && snapshot(copy) == snapshot(frames[i]));
    }
    const auto native = dir.filePath("frames.vulkana");
    CHECK(u::saveProject(native, doc));
    auto restored = u::loadProject(native);
    CHECK(restored);
    if (restored) {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto& saved = restored.document->snapshotTimeline().frames[i];
            CHECK(saved.durationMs == delays[i]);
            CHECK(saved.image->png == frames[i].image->png);
            CHECK(restored.document->layers()[i].visible == doc.layers()[i].visible);
        }
    }
    auto cancelled = u::loadGifDocument(path, "Frame %1", [](auto done, auto total) { return !total || done < 2; });
    CHECK(!cancelled && cancelled.cancelled && cancelled.error.isEmpty());
    CHECK(!u::loadGifDocument(path, "Frame %1", {}, 1024));
    CHECK(!u::loadGifDocument(write(dir,"truncated.gif", bytes.chopped(1)), "Frame %1"));
    CHECK(!u::loadGifDocument(write(dir,"invalid.gif", "not a GIF"), "Frame %1"));
    CHECK(!u::loadGifDocument(dir.filePath("missing.gif"), "Frame %1"));
    CHECK(u::isGifFile(write(dir,"renamed.bin",bytes)));
    // A one-frame GIF is still a complete editable document and timeline.
    const auto second = bytes.indexOf(QByteArray::fromHex("21f904"), 20);
    const auto third = bytes.indexOf(QByteArray::fromHex("21f904"), second + 1);
    auto single = u::loadGifDocument(write(dir,"single.gif",bytes.left(third) + char(0x3b)), "Frame %1");
    CHECK(single && single.document->snapshotTimeline().frames.size() == 1);
}
void clockTests()
{
    c::SnapshotTimelineState state;
    auto image = std::make_shared<c::SnapshotImage>();
    state.frames = {{1,80,image}, {2,250,image}, {3,60,image}, {4,20,{}}}; state.nextId=5;
    u::SnapshotPlayback playback;
    std::vector<c::SnapshotFrameId> seen;
    playback.onFrame = [&](auto id) { seen.push_back(id); };
    playback.setTimeline(state);
    playback.play(1);
    CHECK(playback.playing() && playback.currentFrame() == 1);
    QTest::qWait(130);
    CHECK(playback.currentFrame() == 2);
    playback.pause();
    const auto count = seen.size();
    QTest::qWait(280);
    CHECK(playback.currentFrame() == 2 && seen.size() == count);
    playback.seek(3);
    CHECK(!playback.playing() && playback.currentFrame() == 3);
    playback.play(3);
    QTest::qWait(85);
    CHECK(playback.currentFrame() == 1); // Wrap, skipping the empty slot.
    playback.seek(2);
    CHECK(playback.playing() && playback.currentFrame() == 2);
    QTest::qWait(70);
    CHECK(playback.currentFrame() == 2);
    playback.clear();
    const auto stopped = seen.size();
    QTest::qWait(100);
    CHECK(!playback.playing() && !playback.currentFrame() && seen.size() == stopped);
    state.frames = {{1,100,image}}; state.nextId=2;
    playback.setTimeline(state); playback.play(1);
    QTest::qWait(240);
    CHECK(playback.playing() && playback.currentFrame() == 1);
    playback.clear();
}
void sample(const QString& path)
{
    QElapsedTimer clock; clock.start();
    auto result = u::loadGifDocument(path, "Frame %1");
    CHECK(result);
    if (!result) { std::cerr << result.error.toStdString() << '\n'; return; }
    const auto& frames = result.document->snapshotTimeline().frames;
    std::uint64_t duration = 0, compressed = 0;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& frame : frames) {
        auto image = snapshot(frame);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()));
        duration += frame.durationMs; compressed += frame.image->png.size();
    }
    std::cout << "Frames: " << frames.size() << ", duration: " << duration << " ms, import/validation: " << clock.elapsed()
              << " ms, compressed snapshots: " << compressed << " bytes\nRGBA sequence SHA256: "
              << hash.result().toHex().toStdString() << '\n';
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    importTests(); clockTests();
    if (app.arguments().size() > 1) sample(app.arguments()[1]);
    return failures ? 1 : 0;
}
