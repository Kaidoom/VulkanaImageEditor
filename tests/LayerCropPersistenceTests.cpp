#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerCrop.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>
#include <mz.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

QByteArray pixels(const c::RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
    surface.copyRgba8({0, 0, int(extent.width), int(extent.height)}, bytes, std::size_t(extent.width) * 4);
    return {reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())};
}

c::Document sampleDocument()
{
    c::Document document({{16, 16}, 96});
    std::vector<std::byte> bytes(8 * 6 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        bytes[i] = std::byte(30 + i % 180);
        bytes[i + 1] = std::byte(70 + i % 120);
        bytes[i + 2] = std::byte(20 + i % 200);
        // Include hidden RGB and partial alpha outside the visible crop.
        bytes[i + 3] = std::byte(i == 0 ? 0 : i % 3 == 0 ? 128 : 255);
    }
    auto raster = c::Layer::raster("Full original source",
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {8, 6}, std::move(bytes)));
    raster.localToDocument = {1.25, .2, 2, -.1, 1.1, 3};
    raster.crop = c::RectD {1.25, .5, 3.75, 2.25};
    CHECK(document.insertLayer(0, raster));

    c::TextLayer text; text.utf8 = "Still editable beyond the crop"; text.defaultStyle.sizePixels = 5;
    auto editable = c::Layer::text("Editable text", text);
    editable.localToDocument = {.8, -.4, 4, .3, 1.2, 2};
    editable.crop = c::RectD {-2.5, -1.25, 6.5, 3.25};
    CHECK(document.insertLayer(1, editable));

    c::ShapeLayer shape; shape.size = {6, 4}; shape.fillColor = {36, 75, 150, 192};
    auto vector = c::Layer::shape("Editable shape", shape);
    // Explicitly empty is not equivalent to an absent crop.
    vector.crop = c::RectD {1.5, 2.25, 0, 2};
    CHECK(document.insertLayer(2, vector));
    return document;
}

using Payloads = std::map<QString, QByteArray>;
Payloads sourcePayloads(const c::Document& document)
{
    Payloads result;
    for (const auto& layer : document.layers())
        if (const auto* raster = std::get_if<c::RasterLayer>(&layer.payload))
            result[QStringLiteral("rasters/%1.rgba").arg(layer.id)] = pixels(*raster->surface);
    return result;
}

void zipOkay(int result)
{
    if (result != MZ_OK) throw std::runtime_error("ZIP fixture failure: " + std::to_string(result));
}

// Independent transport can present invalid crop descriptors without the
// production writer sanitizing them or rejecting the fixture first.
QByteArray archive(const QJsonObject& manifest, Payloads content)
{
    struct Handles {
        void* memory {mz_stream_mem_create()};
        void* zip {mz_zip_create()};
        bool opened {false};
        ~Handles() { if (opened) mz_zip_close(zip); mz_zip_delete(&zip); mz_stream_mem_delete(&memory); }
    } handles;
    if (!handles.memory || !handles.zip) throw std::bad_alloc();
    zipOkay(mz_stream_mem_open(handles.memory, nullptr, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_WRITE));
    zipOkay(mz_zip_open(handles.zip, handles.memory, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_WRITE));
    handles.opened = true;
    content["manifest.json"] = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    for (const auto& [path, bytes] : content) {
        const auto name = path.toUtf8();
        mz_zip_file info {};
        info.filename = name.constData(); info.filename_size = std::uint16_t(name.size());
        info.flag = MZ_ZIP_FLAG_UTF8; info.compression_method = MZ_COMPRESS_METHOD_STORE;
        info.uncompressed_size = bytes.size(); info.zip64 = MZ_ZIP64_AUTO;
        zipOkay(mz_zip_entry_write_open(handles.zip, &info, 0, 0, nullptr));
        if (mz_zip_entry_write(handles.zip, bytes.constData(), std::int32_t(bytes.size())) != bytes.size())
            throw std::runtime_error("Short ZIP fixture write");
        zipOkay(mz_zip_entry_write_close(handles.zip, 0, -1, -1));
    }
    zipOkay(mz_zip_close(handles.zip)); handles.opened = false;
    const auto length = mz_stream_mem_tell(handles.memory);
    const void* data = nullptr;
    zipOkay(mz_stream_mem_get_buffer(handles.memory, &data));
    return {static_cast<const char*>(data), qsizetype(length)};
}

void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write fixture");
}
QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read fixture");
    return file.readAll();
}
void editFirstLayer(QJsonObject& manifest, const std::function<void(QJsonObject&)>& edit)
{
    auto layers = manifest["layers"].toArray();
    auto layer = layers[0].toObject(); edit(layer); layers[0] = layer; manifest["layers"] = layers;
}

void typedCropRoundTripsKeepFullSourceAndStableLocalOrigin()
{
    QTemporaryDir directory; CHECK(directory.isValid());
    auto document = sampleDocument();
    const auto sourceBytes = sourcePayloads(document);
    const auto path = directory.filePath("crop.vulkana");
    CHECK(u::saveProject(path, document));
    const auto loaded = u::loadProject(path);
    CHECK(loaded); if (!loaded) { std::cerr << loaded.error.toStdString() << '\n'; return; }
    CHECK(loaded.metadata["required"].toArray().contains("layer-crop-v1"));
    CHECK(loaded.document->layers().size() == 3);
    CHECK(sourcePayloads(*loaded.document) == sourceBytes);
    CHECK(sourcePayloads(document) == sourceBytes);
    for (const auto& layer : document.layers()) {
        const auto* restored = loaded.document->layer(layer.id);
        CHECK(restored); if (!restored) continue;
        CHECK(restored->crop == layer.crop);
        CHECK(restored->localToDocument == layer.localToDocument);
    }
    CHECK(std::get<c::TextLayer>(loaded.document->layers()[1].payload)
        == std::get<c::TextLayer>(document.layers()[1].payload));
    CHECK(std::get<c::ShapeLayer>(loaded.document->layers()[2].payload)
        == std::get<c::ShapeLayer>(document.layers()[2].payload));
    CHECK(loaded.document->layers()[2].crop && loaded.document->layers()[2].crop->empty());

    // The loaded opaque metadata must not resurrect an owned crop after reset.
    for (const auto& layer : document.layers()) CHECK(loaded.document->setLayerCrop(layer.id, {}));
    const auto resetPath = directory.filePath("reset.vulkana");
    CHECK(u::saveProject(resetPath, *loaded.document, loaded.metadata));
    const auto reset = u::loadProject(resetPath);
    CHECK(reset); if (!reset) return;
    CHECK(!reset.metadata["required"].toArray().contains("layer-crop-v1"));
    for (const auto& layer : reset.document->layers()) CHECK(!layer.crop);
    for (const auto layer : reset.metadata["layers"].toArray()) CHECK(!layer.toObject().contains("crop"));
    CHECK(sourcePayloads(*reset.document) == sourceBytes);

    // Even natural-bounds and wholly off-source crops are explicit metadata,
    // since later typed-content edits can grow beyond today's source bounds.
    CHECK(reset.document->setLayerCrop(document.layers()[0].id, c::RectD {0, 0, 8, 6}));
    CHECK(reset.document->setLayerCrop(document.layers()[1].id, c::RectD {-1e9, 1e9, 0, 0}));
    CHECK(reset.document->setLayerCrop(document.layers()[2].id, c::RectD {500.25, 600.5, 10.75, 20.25}));
    CHECK(u::saveProject(resetPath, *reset.document, reset.metadata));
    const auto explicitCrops = u::loadProject(resetPath);
    CHECK(explicitCrops); if (!explicitCrops) return;
    for (const auto& layer : reset.document->layers())
        CHECK(explicitCrops.document->layer(layer.id)->crop == layer.crop);
}

void capabilityAndDescriptorValidationFailClosed()
{
    QTemporaryDir directory; CHECK(directory.isValid());
    auto document = sampleDocument();
    const auto path = directory.filePath("fixture.vulkana");
    CHECK(u::saveProject(path, document));
    const auto saved = u::loadProject(path); CHECK(saved); if (!saved) return;
    const auto payloads = sourcePayloads(document);
    write(path, archive(saved.metadata, payloads));
    CHECK(u::loadProject(path)); // Verify the independent fixture before corruption.
    const auto reject = [&](const std::function<void(QJsonObject&)>& mutate) {
        auto manifest = saved.metadata; mutate(manifest);
        write(path, archive(manifest, payloads));
        const auto loaded = u::loadProject(path);
        CHECK(!loaded); CHECK(!loaded.error.isEmpty());
    };
    const auto rejectCrop = [&](const std::function<void(QJsonObject&)>& mutate) {
        reject([&](QJsonObject& manifest) {
            editFirstLayer(manifest, [&](QJsonObject& layer) {
                auto crop = layer["crop"].toObject(); mutate(crop); layer["crop"] = crop;
            });
        });
    };
    for (const auto field : {"x", "y", "width", "height", "version"}) {
        rejectCrop([&](QJsonObject& crop) { crop.remove(field); });
        rejectCrop([&](QJsonObject& crop) { crop[field] = "1"; });
        rejectCrop([&](QJsonObject& crop) { crop[field] = QJsonValue(QJsonValue::Null); });
    }
    rejectCrop([](QJsonObject& crop) { crop["version"] = 2; });
    rejectCrop([](QJsonObject& crop) { crop["version"] = 1.25; });
    rejectCrop([](QJsonObject& crop) { crop["width"] = -.125; });
    rejectCrop([](QJsonObject& crop) { crop["height"] = -.125; });
    rejectCrop([](QJsonObject& crop) { crop["x"] = -1e9 - 1; });
    rejectCrop([](QJsonObject& crop) { crop["y"] = 1e9 + 1; });
    rejectCrop([](QJsonObject& crop) { crop["width"] = 1e9 + 1; });
    rejectCrop([](QJsonObject& crop) { crop["height"] = 1e9 + 1; });
    rejectCrop([](QJsonObject& crop) { crop["x"] = 1e9; crop["width"] = 1; });
    rejectCrop([](QJsonObject& crop) { crop["y"] = 1e9; crop["height"] = 1; });
    for (const auto invalid : {QJsonValue(QJsonValue::Null), QJsonValue(QJsonArray {}), QJsonValue(true)})
        reject([&](QJsonObject& manifest) { editFirstLayer(manifest, [&](QJsonObject& layer) { layer["crop"] = invalid; }); });
    reject([](QJsonObject& manifest) {
        auto required = manifest["required"].toArray();
        for (qsizetype i = required.size(); i-- > 0;) if (required[i] == "layer-crop-v1") required.removeAt(i);
        manifest["required"] = required;
    });
    reject([](QJsonObject& manifest) {
        auto required = manifest["required"].toArray(); required.append("layer-crop-v2"); manifest["required"] = required;
    });

    // A legacy project needs neither a crop descriptor nor its capability.
    auto legacy = saved.metadata;
    auto required = legacy["required"].toArray();
    for (qsizetype i = required.size(); i-- > 0;) if (required[i] == "layer-crop-v1") required.removeAt(i);
    legacy["required"] = required;
    auto layers = legacy["layers"].toArray();
    for (qsizetype i = 0; i < layers.size(); ++i) { auto layer = layers[i].toObject(); layer.remove("crop"); layers[i] = layer; }
    legacy["layers"] = layers;
    write(path, archive(legacy, payloads));
    const auto loadedLegacy = u::loadProject(path);
    CHECK(loadedLegacy); if (loadedLegacy) for (const auto& layer : loadedLegacy.document->layers()) CHECK(!layer.crop);

    // Writer validation also fails atomically when callers violate model invariants.
    const auto originalFile = read(path);
    document.layer(document.layers()[0].id)->crop = c::RectD {0, 0, std::numeric_limits<double>::infinity(), 1};
    CHECK(!u::saveProject(path, document));
    CHECK(read(path) == originalFile);
}

void snapshotsAndRetainedHistoryKeepCropWithoutSourceChurn()
{
    auto document = sampleDocument();
    const auto snapshot = document.snapshot();
    CHECK(snapshot.layersBottomToTop.size() == document.layers().size());
    for (std::size_t i = 0; i < document.layers().size(); ++i)
        CHECK(snapshot.layersBottomToTop[i].crop == document.layers()[i].crop);
    const auto originals = document.layers();
    for (const auto& original : originals) {
        CHECK(document.setLayerCrop(original.id, c::RectD {0, 0, .5, .5}));
        const auto* changed = document.layer(original.id);
        CHECK(changed->textRevision == original.textRevision);
        CHECK(changed->shapeRevision == original.shapeRevision);
        CHECK(changed->renderCache == original.renderCache);
        CHECK(changed->localToDocument == original.localToDocument);
        CHECK(document.setLayerCrop(original.id, original.crop));
    }
    for (std::size_t i = 0; i < originals.size(); ++i)
        CHECK(snapshot.layersBottomToTop[i].crop == originals[i].crop);
    c::History history;
    document.markSaved();
    for (const auto& original : originals) {
        CHECK(history.execute(document, std::make_unique<c::RemoveLayerCommand>(original.id)));
        CHECK(document.isModified());
        CHECK(history.undo(document));
        CHECK(!document.isModified());
        CHECK(document.layer(original.id)->crop == original.crop);
        CHECK(history.redo(document)); CHECK(history.undo(document));
        CHECK(document.layer(original.id)->crop == original.crop);
        auto duplicate = original; duplicate.id = c::makeLayerId();
        CHECK(history.execute(document, std::make_unique<c::AddLayerCommand>(duplicate, document.layers().size())));
        CHECK(document.layer(duplicate.id)->crop == original.crop);
        CHECK(document.setLayerCrop(duplicate.id, {}));
        CHECK(document.layer(original.id)->crop == original.crop);
        CHECK(history.undo(document));
        CHECK(!document.isModified());
        // Crop storage is inline; command estimates already account for Layer.
        auto withoutCrop = original; withoutCrop.crop.reset();
        CHECK(c::retainedLayerMemory(original) == c::retainedLayerMemory(withoutCrop));
    }
    const auto& original = originals[0];
    const auto surface = std::get<c::RasterLayer>(original.payload).surface;
    const auto revision = surface->revision();
    const auto bytes = pixels(*surface);
    CHECK(document.setLayerCrop(original.id, c::RectD {0, 0, 1, 1}));
    CHECK(document.setLayerCrop(original.id, original.crop));
    CHECK(std::get<c::RasterLayer>(document.layer(original.id)->payload).surface == surface);
    CHECK(surface->revision() == revision); CHECK(pixels(*surface) == bytes);
    c::LayerViaCopyCommand copy(original.id, original.id);
    CHECK(copy.apply(document));
    const auto copiedId = copy.createdLayerId(); CHECK(copiedId); if (!copiedId) return;
    const auto* copied = document.layer(*copiedId); CHECK(copied); if (!copied) return;
    CHECK(copied->crop == original.crop);
    CHECK(copied->localToDocument == original.localToDocument);
    CHECK(pixels(*std::get<c::RasterLayer>(copied->payload).surface) == bytes);
    CHECK(std::get<c::RasterLayer>(copied->payload).surface != surface);
    CHECK(copy.undo(document)); CHECK(copy.apply(document));
    CHECK(document.layer(*copiedId)->crop == original.crop);
    CHECK(document.setLayerCrop(*copiedId, {}));
    CHECK(document.layer(original.id)->crop == original.crop);
}
void chamferRoundTripAndCapability()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("chamfer.vulkana");
    auto doc = sampleDocument();
    const auto payloads = sourcePayloads(doc);
    for (const auto& l : doc.layers()) {
        auto crop = *l.crop;
        crop.corners = { .25, 1.5, 2.25, 11 };
        CHECK(doc.setLayerCrop(l.id, crop));
    }
    CHECK(u::saveProject(path, doc));
    auto saved = u::loadProject(path);
    CHECK(saved);
    if (!saved)
        return;
    CHECK(saved.metadata["required"].toArray().contains("layer-crop-chamfer-v1"));
    CHECK(sourcePayloads(*saved.document) == payloads);
    for (const auto& l : doc.layers())
        CHECK(saved.document->layer(l.id)->crop == l.crop);
    const auto reject = [&](const std::function<void(QJsonObject&)>& edit) {
        auto manifest = saved.metadata;
        edit(manifest);
        write(path, archive(manifest, payloads));
        CHECK(!u::loadProject(path));
    };
    reject([](auto& m) {
        auto caps = m["required"].toArray();
        for (qsizetype i = caps.size() - 1; i >= 0; --i) {
            if (caps[i] == "layer-crop-chamfer-v1")
                caps.removeAt(i);
        }
        m["required"] = caps;
    });
    for (const auto& invalid : { QJsonValue(QJsonArray { 1, 2 }), QJsonValue(QJsonArray { 1, -2, 0, 0 }),
             QJsonValue(QJsonArray { 1, 2, 0, "4" }), QJsonValue(QJsonArray { 1, 2, 0, 1e10 }), QJsonValue(QJsonValue::Null) })
        reject([&](auto& m) { editFirstLayer(m, [&](auto& l) {auto crop=l["crop"].toObject();crop["corners"]=invalid;l["crop"]=crop; }); });
    reject([](auto& m) { editFirstLayer(m, [](auto& l) {auto crop=l["crop"].toObject();crop["version"]=1;l["crop"]=crop; }); });
    for (const auto& l : doc.layers()) {
        auto crop = *l.crop;
        crop.corners = { };
        CHECK(doc.setLayerCrop(l.id, crop));
    }
    CHECK(u::saveProject(path, doc, saved.metadata));
    const auto rectangular = u::loadProject(path);
    CHECK(rectangular);
    CHECK(!rectangular.metadata["required"].toArray().contains("layer-crop-chamfer-v1"));
}
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        typedCropRoundTripsKeepFullSourceAndStableLocalOrigin();
        capabilityAndDescriptorValidationFailClosed();
        snapshotsAndRetainedHistoryKeepCropWithoutSourceChurn();
        chamferRoundTripAndCapability();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    if (failures) return EXIT_FAILURE;
    std::cout << "Layer crop persistence tests passed\n";
    return EXIT_SUCCESS;
}
