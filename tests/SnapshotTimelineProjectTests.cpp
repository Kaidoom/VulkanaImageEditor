#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/SnapshotTimeline.hpp"
#include "imageeditor/project/ProjectArchive.hpp"
#include "imageeditor/project/ProjectPreview.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>

namespace c = imageeditor::core;
namespace p = imageeditor::project;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
using Payloads = std::map<QString, QByteArray>;

std::shared_ptr<const c::SnapshotImage> capture(const QImage& input, double dpi = 96)
{
    const auto rgba = input.convertToFormat(QImage::Format_RGBA8888);
    auto result = std::make_shared<c::SnapshotImage>();
    result->extent = {std::uint32_t(rgba.width()), std::uint32_t(rgba.height())};
    result->dotsPerInch = dpi;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !rgba.save(&buffer, "PNG"))
        throw std::runtime_error("Cannot encode snapshot fixture");
    result->png.resize(std::size_t(bytes.size()));
    std::memcpy(result->png.data(), bytes.constData(), result->png.size());
    return result;
}
QImage decode(const c::SnapshotImage& image)
{
    return QImage::fromData(reinterpret_cast<const uchar*>(image.png.data()), int(image.png.size()), "PNG")
        .convertToFormat(QImage::Format_RGBA8888);
}
c::Document document()
{
    c::Document result({{8, 6}, 300});
    auto pixels = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{8, 6}, c::Rgba8{30, 60, 90, 128});
    CHECK(result.insertLayer(0, c::Layer::raster("Editable pixels", pixels)));
    result.markSaved();
    return result;
}
Payloads readArchive(const QString& path, bool checkStored = false)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read fixture");
    p::Archive archive(file, false);
    const auto entries = p::entries(archive);
    Payloads payloads;
    p::Progress progress;
    for (const auto& [name, entry] : entries) {
        QByteArray bytes;
        p::readEntry(archive, entry, [&](const char* data, int n) { bytes.append(data, n); }, progress);
        payloads.emplace(name, std::move(bytes));
        if (checkStored && name.startsWith("snapshots/")) {
            mz_zip_file* info = nullptr;
            p::checked(mz_zip_entry_get_info(archive.handle, &info), "Cannot inspect fixture entry");
            CHECK(info && info->compression_method == MZ_COMPRESS_METHOD_STORE);
        }
    }
    archive.finish();
    return payloads;
}
void writeArchive(const QString& path, const Payloads& payloads)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write fixture");
    p::Archive archive(file, true);
    p::Progress progress;
    for (const auto& [name, bytes] : payloads)
        p::writeEntry(archive, name, quint64(bytes.size()),
            [&](const auto& write) { write(bytes.constData(), int(bytes.size())); }, progress);
    archive.finish();
}
void changeManifest(Payloads& payloads, const std::function<void(QJsonObject&)>& change)
{
    auto manifest = QJsonDocument::fromJson(payloads.at("manifest.json")).object();
    change(manifest);
    payloads["manifest.json"] = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}
void changeFrame(Payloads& payloads, const std::function<void(QJsonObject&)>& change)
{
    changeManifest(payloads, [&](auto& manifest) {
        auto timeline = manifest["snapshotTimeline"].toObject();
        auto frames = timeline["frames"].toArray();
        auto frame = frames[0].toObject();
        change(frame);
        frames[0] = frame;
        timeline["frames"] = frames;
        manifest["snapshotTimeline"] = timeline;
    });
}

void roundtripAndLegacy(const QTemporaryDir& files)
{
    auto live = document();
    const auto legacy = files.filePath("legacy.vulkana");
    CHECK(u::saveProject(legacy, live));
    const auto legacyPayloads = readArchive(legacy);
    CHECK(!QJsonDocument::fromJson(legacyPayloads.at("manifest.json")).object().contains("snapshotTimeline"));
    auto openedLegacy = u::loadProject(legacy);
    CHECK(openedLegacy && openedLegacy.document->snapshotTimeline() == c::SnapshotTimelineState{});

    const auto rendered = u::flattenDocument(live);
    CHECK(rendered);
    if (!rendered) return;
    auto timeline = live.snapshotTimeline();
    timeline.frames[0].image = capture(rendered.image, 300);
    const auto frozen = timeline.frames[0].image;
    timeline.frames.push_back({timeline.nextId++, 250, {}});
    QImage other(5, 9, QImage::Format_RGBA8888);
    other.fill(QColor(200, 120, 60, 70));
    timeline.frames.push_back({timeline.nextId++, 750, capture(other, 72)});
    std::swap(timeline.frames[0], timeline.frames[2]);
    c::History history;
    CHECK(history.execute(live, std::make_unique<c::ChangeSnapshotTimelineCommand>(live.snapshotTimeline(), timeline, "Capture")));
    CHECK(live.isModified());
    const auto path = files.filePath("timeline.vulkana");
    const auto saved = u::saveProject(path, live);
    CHECK(saved);
    if (!saved) { std::cerr << saved.error.toStdString() << '\n'; return; }
    const auto payloads = readArchive(path, true);
    CHECK(payloads.contains("snapshots/1.png") && payloads.contains("snapshots/3.png") && !payloads.contains("snapshots/2.png"));
    CHECK(!p::readPreview(path).isNull());
    auto opened = u::loadProject(path);
    CHECK(opened);
    if (!opened) { std::cerr << opened.error.toStdString() << '\n'; return; }
    CHECK(!opened.document->isModified());
    const auto& restored = opened.document->snapshotTimeline();
    CHECK(restored.nextId == timeline.nextId && restored.frames.size() == 3);
    for (std::size_t i = 0; i < timeline.frames.size(); ++i) {
        CHECK(restored.frames[i].id == timeline.frames[i].id && restored.frames[i].durationMs == timeline.frames[i].durationMs);
        CHECK(bool(restored.frames[i].image) == bool(timeline.frames[i].image));
        if (restored.frames[i].image) {
            CHECK(restored.frames[i].image->extent == timeline.frames[i].image->extent);
            CHECK(restored.frames[i].image->dotsPerInch == timeline.frames[i].image->dotsPerInch);
            CHECK(restored.frames[i].image->png == timeline.frames[i].image->png);
            CHECK(decode(*restored.frames[i].image) == decode(*timeline.frames[i].image));
        }
    }
    CHECK(u::flattenDocument(*opened.document).image == rendered.image); // Timeline does not enter composition.
    CHECK(live.setLayerOpacity(live.layers()[0].id, .2F));
    CHECK(live.setCanvas({{2, 3}, 72}));
    CHECK(decode(*frozen) == rendered.image);
    CHECK(u::flattenDocument(live).image != decode(*frozen));
    CHECK(decode(*opened.document->snapshotTimeline().frames[2].image) == rendered.image);

    // Old compatible metadata cannot revive deleted timeline contents.
    CHECK(opened.document->setSnapshotTimeline(restored, c::SnapshotTimelineState{}));
    const auto cleared = files.filePath("cleared.vulkana");
    CHECK(u::saveProject(cleared, *opened.document, opened.metadata));
    const auto clearedPayloads = readArchive(cleared);
    CHECK(clearedPayloads.size() == 2 && !clearedPayloads.contains("snapshots/1.png"));
    CHECK(!QJsonDocument::fromJson(clearedPayloads.at("manifest.json")).object().contains("snapshotTimeline"));

    // Cancelled writes preserve the existing file, including every capture.
    CHECK(u::saveProject(path, live, {}, [](quint64, quint64) { return false; }).cancelled);
    CHECK(readArchive(path) == payloads);
    auto cancelled = u::loadProject(path, [](quint64, quint64) { return false; });
    CHECK(!cancelled && cancelled.cancelled);
}

void malformedAndBudgets(const QTemporaryDir& files)
{
    const auto valid = readArchive(files.filePath("timeline.vulkana"));
    const auto malformed = files.filePath("invalid.vulkana");
    const auto reject = [&](const std::function<void(Payloads&)>& change) {
        auto payloads = valid;
        change(payloads);
        writeArchive(malformed, payloads);
        const auto loaded = u::loadProject(malformed);
        CHECK(!loaded && !loaded.error.isEmpty() && !loaded.cancelled);
    };
    reject([](auto& p) { p.erase("snapshots/3.png"); });
    reject([](auto& p) { p["snapshots/3.png"].chop(12); }); // Missing IEND, otherwise intact pixels.
    reject([](auto& p) { p["snapshots/3.png"][45] = char(p["snapshots/3.png"][45] ^ 1); });
    reject([](auto& p) { changeFrame(p, [](auto& f) { f["durationMs"] = 0; }); });
    reject([](auto& p) { changeFrame(p, [](auto& f) { f["durationMs"] = 2.5; }); });
    reject([](auto& p) { changeFrame(p, [](auto& f) { f["id"] = "2"; }); });
    reject([](auto& p) { changeFrame(p, [](auto& f) {
        auto image = f["image"].toObject(); image["width"] = 7; f["image"] = image;
    }); });
    reject([](auto& p) { changeFrame(p, [](auto& f) {
        auto image = f["image"].toObject(); image["width"] = 32769; f["image"] = image;
    }); });
    reject([](auto& p) { changeFrame(p, [](auto& f) {
        auto image = f["image"].toObject(); image["path"] = "../other.png"; f["image"] = image;
    }); });
    reject([](auto& p) { changeManifest(p, [](auto& m) {
        auto t = m["snapshotTimeline"].toObject(); t["nextId"] = "3"; m["snapshotTimeline"] = t;
    }); });
    reject([](auto& p) { changeManifest(p, [](auto& m) {
        auto t = m["snapshotTimeline"].toObject(); t["frames"] = QJsonArray{}; m["snapshotTimeline"] = t;
    }); });
    reject([](auto& p) { changeManifest(p, [](auto& m) { m.remove("snapshotTimeline"); }); });
    reject([](auto& p) { changeManifest(p, [](auto& m) { m["required"] = QJsonArray{"rgba8", "rich-text-v1"}; }); });
    // ZIP CRC is correct, as is PNG's chunk CRC, but the pixel stream is corrupt.
    reject([](auto& p) {
        auto& png = p["snapshots/3.png"];
        for (qsizetype offset = 8; offset + 12 <= png.size();) {
            const auto size = qFromBigEndian<quint32>(png.constData() + offset);
            if (QByteArrayView(png.constData() + offset + 4, 4) == "IDAT" && size > 2) {
                png[offset + 8] = '\0'; png[offset + 9] = '\0';
                const auto crc = mz_crypt_crc32_update(0,
                    reinterpret_cast<const uint8_t*>(png.constData() + offset + 4), int(size + 4));
                qToBigEndian(crc, png.data() + offset + 8 + size);
                break;
            }
            offset += qsizetype(size) + 12;
        }
    });
    const auto rejected = u::loadProject(files.filePath("timeline.vulkana"), {}, {1});
    CHECK(!rejected && rejected.error.contains("working memory"));

    auto largeTimeline = document();
    auto timeline = largeTimeline.snapshotTimeline();
    QImage image(1024, 1024, QImage::Format_RGBA8888); image.fill(QColor(30, 100, 240, 128));
    const auto compressed = capture(image);
    timeline.frames[0].image = compressed;
    for (int i = 0; i < 31; ++i) timeline.frames.push_back({timeline.nextId++, 100, compressed});
    CHECK(largeTimeline.setSnapshotTimeline(largeTimeline.snapshotTimeline(), timeline));
    const auto manyImages = files.filePath("many-images.vulkana");
    CHECK(u::saveProject(manyImages, largeTimeline));
    // 32 decoded frames would need 128 MiB. Loading retains PNGs and decodes
    // only one at a time, within a 16 MiB increment over the normal allowance.
    auto bounded = u::loadProject(manyImages, {}, {80ULL * 1024 * 1024});
    CHECK(bounded && bounded.document->snapshotTimeline().frames.size() == 32);
    if (!bounded) std::cerr << bounded.error.toStdString() << '\n';

    auto manySlots = document();
    timeline = manySlots.snapshotTimeline();
    for (int i = 0; i < 2048; ++i) timeline.frames.push_back({timeline.nextId++, 100, {}});
    CHECK(manySlots.setSnapshotTimeline(manySlots.snapshotTimeline(), timeline));
    const auto manyPath = files.filePath("many-slots.vulkana");
    CHECK(u::saveProject(manyPath, manySlots));
    auto many = u::loadProject(manyPath);
    CHECK(many && many.document->snapshotTimeline() == timeline);
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir files;
    CHECK(files.isValid());
    try {
        roundtripAndLegacy(files);
        malformedAndBudgets(files);
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << e.what() << '\n';
    }
    std::cout << "Snapshot timeline project: " << failures << " failures\n";
    return failures ? 1 : 0;
}
