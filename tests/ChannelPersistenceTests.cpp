#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/project/ProjectArchive.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"

#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace p = imageeditor::project;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

std::vector<std::byte> bytes(const c::RasterSurface& surface)
{
    const auto e = surface.extent();
    std::vector<std::byte> result(std::size_t(e.width) * e.height * 4);
    surface.copyRgba8({0, 0, int(e.width), int(e.height)}, result, std::size_t(e.width) * 4);
    return result;
}
std::vector<std::byte> values()
{
    std::vector<std::byte> result(256 * 2 * 4);
    for (std::size_t i = 0; i < 512; ++i) {
        result[i * 4] = std::byte(i % 256);
        result[i * 4 + 1] = std::byte(255 - i % 256);
        result[i * 4 + 2] = std::byte((i * 37) % 256);
        result[i * 4 + 3] = std::byte(i % 3 ? i % 256 : 0);
    }
    return result;
}

using Payloads = std::map<QString, QByteArray>;
Payloads readArchive(const QString& path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read fixture");
    p::Archive archive(file, false);
    const auto entries = p::entries(archive);
    p::Progress progress;
    Payloads result;
    for (const auto& [name, entry] : entries)
        p::readEntry(archive, entry, [&](const char* data, int n) { result[name].append(data, n); }, progress);
    archive.finish();
    return result;
}
void writeArchive(const QString& path, const Payloads& payloads)
{
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write fixture");
    p::Archive archive(file, true);
    p::Progress progress;
    for (const auto& [name, data] : payloads)
        p::writeEntry(archive, name, quint64(data.size()), [&](const auto& write) {
            write(data.constData(), int(data.size()));
        }, progress);
    archive.finish();
}

void persistenceAndPacking()
{
    QTemporaryDir directory; CHECK(directory.isValid());
    c::Document doc({{256, 2}, 96});
    const auto original = values();
    auto source = c::Layer::raster("Packed", std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u {256, 2}, original, c::RasterDataKind::PackedData));
    source.localToDocument.m02 = 7;
    source.localToDocument.m12 = -3;
    const auto groupId = source.id;
    CHECK(doc.insertLayer(0, std::move(source)));
    c::History history;
    auto split = c::convertToChannelGroup(doc, {{groupId}, groupId, groupId});
    CHECK(history.execute(doc, std::move(split)));
    CHECK(bytes(*c::packChannelGroup(doc, groupId)) == original);
    CHECK(history.undo(doc));
    CHECK(bytes(*std::get<c::RasterLayer>(doc.layer(groupId)->payload).surface) == original);
    CHECK(history.redo(doc));
    CHECK(bytes(*c::packChannelGroup(doc, groupId)) == original);
    const auto roles = doc.tree().container(groupId)->channelRoles;
    CHECK(doc.renameLayer(roles[0], "Roughness label"));
    const auto path = directory.filePath("channels.vulkana");
    const auto saved = u::saveProject(path, doc);
    if (!saved) std::cerr << saved.error.toStdString() << '\n';
    CHECK(saved);
    auto loaded = u::loadProject(path);
    if (!loaded) std::cerr << loaded.error.toStdString() << '\n';
    CHECK(loaded);
    if (!loaded) return;
    CHECK(loaded.document->tree().container(groupId)->channelRoles == roles);
    CHECK(loaded.document->layer(roles[0])->name == "Roughness label");
    CHECK(bytes(*c::packChannelGroup(*loaded.document, groupId)) == original);
    for (const auto role : roles) {
        const auto& surface = *std::get<c::RasterLayer>(loaded.document->layer(role)->payload).surface;
        CHECK(surface.nativeBytesPerPixel() == 1);
        CHECK(surface.dataKind() == c::RasterDataKind::Scalar);
    }
    const auto payloads = readArchive(path);
    for (const auto role : roles)
        CHECK(payloads.at(QStringLiteral("rasters/%1.r8").arg(role)).size() == 512);
    const auto manifest = QJsonDocument::fromJson(payloads.at("manifest.json")).object();
    CHECK(manifest["required"].toArray().contains("channel-group-v1"));
    CHECK(manifest["required"].toArray().contains("scalar-raster-v1"));

    int index = 0;
    const auto reject = [&](const std::function<void(QJsonObject&)>& edit) {
        auto changed = payloads;
        auto metadata = manifest;
        metadata.remove("preview");
        edit(metadata);
        changed["manifest.json"] = QJsonDocument(metadata).toJson();
        const auto badPath = directory.filePath(QStringLiteral("bad-%1.vulkana").arg(++index));
        writeArchive(badPath, changed);
        const auto result = u::loadProject(badPath);
        CHECK(!result && !result.error.isEmpty());
    };
    const auto editGroup = [&](QJsonObject& metadata, const std::function<void(QJsonObject&)>& edit) {
        auto hierarchy = metadata["hierarchy"].toObject();
        auto containers = hierarchy["containers"].toArray();
        auto group = containers[0].toObject(); edit(group); containers[0] = group;
        hierarchy["containers"] = containers; metadata["hierarchy"] = hierarchy;
    };
    {
        auto reordered = payloads;
        auto metadata = manifest; metadata.remove("preview");
        editGroup(metadata, [](auto& group) {
            const auto children = group["children"].toArray(); QJsonArray reverse;
            for (auto it = children.end(); it != children.begin();) reverse.append(*--it);
            group["children"] = reverse;
        });
        reordered["manifest.json"] = QJsonDocument(metadata).toJson();
        const auto reorderedPath = directory.filePath("reordered.vulkana"); writeArchive(reorderedPath, reordered);
        const auto result = u::loadProject(reorderedPath); CHECK(result);
        if (result) CHECK(bytes(*c::packChannelGroup(*result.document, groupId)) == original);
    }
    {
        auto withoutAlpha = payloads;
        auto metadata = manifest; metadata.remove("preview");
        const auto alphaId = QString::number(roles[3]);
        editGroup(metadata, [&](auto& group) {
            auto mapping = group["channelRoles"].toArray(); mapping[3] = QJsonValue::Null; group["channelRoles"] = mapping;
            auto children = group["children"].toArray();
            for (int i = int(children.size()) - 1; i >= 0; --i) if (children[i] == alphaId) children.removeAt(i);
            group["children"] = children;
        });
        auto layers = metadata["layers"].toArray();
        for (int i = int(layers.size()) - 1; i >= 0; --i)
            if (layers[i].toObject()["id"] == alphaId) layers.removeAt(i);
        metadata["layers"] = layers;
        withoutAlpha.erase(QStringLiteral("rasters/%1.r8").arg(roles[3]));
        withoutAlpha["manifest.json"] = QJsonDocument(metadata).toJson();
        const auto rgbPath = directory.filePath("rgb.vulkana"); writeArchive(rgbPath, withoutAlpha);
        const auto result = u::loadProject(rgbPath); CHECK(result);
        if (result) {
            auto opaque = original;
            for (std::size_t i = 3; i < opaque.size(); i += 4) opaque[i] = std::byte {255};
            CHECK(bytes(*c::packChannelGroup(*result.document, groupId)) == opaque);
        }
    }
    reject([&](auto& metadata) { editGroup(metadata, [](auto& group) {
        auto mapping = group["channelRoles"].toArray(); mapping[1] = mapping[0]; group["channelRoles"] = mapping;
    }); });
    reject([&](auto& metadata) { editGroup(metadata, [](auto& group) {
        auto mapping = group["channelRoles"].toArray(); mapping[0] = QJsonValue::Null; group["channelRoles"] = mapping;
    }); });
    reject([](auto& metadata) {
        auto required = metadata["required"].toArray();
        for (int i = int(required.size()) - 1; i >= 0; --i) if (required[i] == "scalar-raster-v1") required.removeAt(i);
        metadata["required"] = required;
    });
    reject([](auto& metadata) {
        auto required = metadata["required"].toArray();
        for (int i = int(required.size()) - 1; i >= 0; --i) if (required[i] == "channel-group-v1") required.removeAt(i);
        metadata["required"] = required;
    });
    reject([](auto& metadata) {
        auto layers = metadata["layers"].toArray(); auto layer = layers[0].toObject();
        auto transform = layer["transform"].toArray(); transform[2] = 33; layer["transform"] = transform;
        layers[0] = layer; metadata["layers"] = layers;
    });
    reject([](auto& metadata) {
        auto layers = metadata["layers"].toArray(); auto layer = layers[0].toObject();
        auto raster = layer["raster"].toObject(); raster["interpretation"] = "color"; layer["raster"] = raster;
        layers[0] = layer; metadata["layers"] = layers;
    });

    auto combine = c::convertChannelGroupToRaster(*loaded.document, {{groupId}, groupId, groupId});
    CHECK(combine->apply(*loaded.document));
    const auto& combined = *std::get<c::RasterLayer>(loaded.document->layer(groupId)->payload).surface;
    CHECK(combined.dataKind() == c::RasterDataKind::PackedData && bytes(combined) == original);
    CHECK(u::saveProject(directory.filePath("packed.vulkana"), *loaded.document, loaded.metadata));
    auto packed = u::loadProject(directory.filePath("packed.vulkana")); CHECK(packed);
    if (packed) {
        const auto& restored = *std::get<c::RasterLayer>(packed.document->layer(groupId)->payload).surface;
        CHECK(restored.dataKind() == c::RasterDataKind::PackedData && bytes(restored) == original);
    }
}

void dataImportExport()
{
    const auto original = values();
    QImage image(256, 2, QImage::Format_RGBA8888);
    std::memcpy(image.bits(), original.data(), original.size());
    image.setColorSpace(QColorSpace::DisplayP3);
    auto loaded = u::rasterLayerFromImage(image, "Data", u::RasterImportMode::Data);
    CHECK(loaded);
    if (!loaded) return;
    CHECK(bytes(*std::get<c::RasterLayer>(loaded.layer->payload).surface) == original);
    CHECK(c::isPackedDataLayer(*loaded.layer));
    c::Document doc({{256, 2}, 96}); const auto id = loaded.layer->id;
    CHECK(doc.insertLayer(0, std::move(*loaded.layer)));
    u::ExportSettings settings; settings.size = image.size(); settings.preserveData = true; settings.dataLayerId = id;
    const auto rendered = u::renderExport(doc, settings); CHECK(rendered);
    std::atomic_bool cancel {false};
    auto encoded = u::encodeExport(rendered.image, settings, cancel); CHECK(encoded);
    if (!encoded) { std::cerr << encoded.error.toStdString() << '\n'; return; }
    CHECK(!encoded.decoded.colorSpace().isValid());
    auto reloaded = u::loadRasterLayerFromData(encoded.bytes, "Round trip", u::RasterImportMode::Data);
    CHECK(reloaded);
    if (reloaded) CHECK(bytes(*std::get<c::RasterLayer>(reloaded.layer->payload).surface) == original);
    settings.format = u::ExportFormat::Jpeg; CHECK(!u::renderExport(doc, settings));
    settings.format = u::ExportFormat::Png; settings.pngMatte = true; CHECK(!u::renderExport(doc, settings));
    settings.pngMatte = false; settings.size.setWidth(128); CHECK(!u::renderExport(doc, settings));

    QImage highPrecision(2, 2, QImage::Format_RGBA64);
    highPrecision.fill(Qt::white);
    CHECK(!u::rasterLayerFromImage(highPrecision, "16-bit", u::RasterImportMode::Data));
    CHECK(u::rasterLayerFromImage(highPrecision, "Colour"));
    QImage premultiplied(2, 2, QImage::Format_ARGB32_Premultiplied);
    premultiplied.fill(QColor(1, 2, 3, 0));
    CHECK(!u::rasterLayerFromImage(premultiplied, "Premultiplied", u::RasterImportMode::Data));
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try { persistenceAndPacking(); dataImportExport(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    return failures ? 1 : 0;
}
