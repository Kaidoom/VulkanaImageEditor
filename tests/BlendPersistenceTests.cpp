#include "imageeditor/core/BlendMode.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_os.h>
#include <mz_zip.h>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) { ++failures; std::cerr << "FAIL " << line << ": " << expression << '\n'; }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

std::shared_ptr<c::ContiguousRasterSurface> surface()
{ return std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {4, 3}, c::Rgba8 {71, 143, 221, 127}); }

void metadataAndHistory()
{
    // Lock the public shader IDs and wire names independently of the live enum:
    // extending the table must not silently renumber the original 14 modes.
    constexpr std::array<std::string_view,27> names {"normal","multiply","screen","overlay",
        "soft-light","hard-light","darken","lighten","difference","exclusion","hue",
        "saturation","color","luminosity","color-dodge","linear-dodge","color-burn",
        "linear-burn","subtract","divide","dissolve","darker-color","lighter-color","vivid-light","linear-light","pin-light","hard-mix"};
    CHECK(c::allBlendModes.size() == names.size());
    for (std::size_t i=0;i<names.size();++i) {
        CHECK(std::uint32_t(c::allBlendModes[i]) == i);
        CHECK(c::blendModeId(c::allBlendModes[i]) == names[i]);
    }
    CHECK(c::blendModeName(c::BlendMode::LinearDodge) == "Linear Dodge (Add)");
    c::Document doc({{4, 3}});
    auto pixels = surface();
    auto layer = c::Layer::raster("Source", pixels);
    const auto id = layer.id;
    CHECK(layer.blendMode == c::BlendMode::Normal);
    CHECK(doc.insertLayer(0, layer));
    const auto surfaceRevision = pixels->revision();
    c::History history;
    for (const auto mode : c::allBlendModes) {
        CHECK(c::isValidBlendMode(mode));
        CHECK(!c::blendModeName(mode).empty());
        CHECK(c::blendModeFromId(c::blendModeId(mode)) == mode);
        const auto before = doc.layer(id)->blendMode;
        const auto revision = doc.revision();
        const bool changed = history.execute(doc, std::make_unique<c::SetLayerBlendModeCommand>(id, mode));
        CHECK(changed == (before != mode));
        CHECK(doc.layer(id)->blendMode == mode);
        CHECK(doc.snapshot().layersBottomToTop[0].blendMode == mode);
        CHECK(doc.snapshot().layersBottomToTop[0].blendSeed == layer.blendSeed);
        CHECK(doc.revision() == revision + (changed ? 1U : 0U));
        CHECK(pixels->revision() == surfaceRevision);
        CHECK(std::get<c::RasterLayer>(doc.layer(id)->payload).surface == pixels);
        if (changed) {
            CHECK(history.latestUndoMemoryCost() == sizeof(c::SetLayerBlendModeCommand));
            CHECK(history.undo(doc));
            CHECK(doc.layer(id)->blendMode == before);
            CHECK(history.canRedo());
            CHECK(!history.execute(doc, std::make_unique<c::SetLayerBlendModeCommand>(id, before)));
            CHECK(history.canRedo());
            CHECK(history.redo(doc));
            CHECK(doc.layer(id)->blendMode == mode);
        }
    }
    CHECK(!c::isValidBlendMode(static_cast<c::BlendMode>(999)));
    CHECK(!c::isValidBlendMode(static_cast<c::BlendMode>(27)));
    CHECK(!c::blendModeFromId("future-mode"));
    CHECK(!c::blendModeFromId("Multiply"));
    CHECK(!doc.setLayerBlendMode(id, static_cast<c::BlendMode>(999)));
    CHECK(!doc.setLayerBlendMode(0, c::BlendMode::Screen));
    layer.id = c::makeLayerId(); layer.blendMode = static_cast<c::BlendMode>(999);
    CHECK(!doc.insertLayer(1, layer));
    c::SetLayerBlendModeCommand removed(id, c::BlendMode::Screen);
    CHECK(doc.takeLayer(id));
    CHECK(!removed.apply(doc));
    CHECK(!removed.undo(doc));
}

void copiesRetainUnblendedProperties()
{
    for (const auto mode : c::allBlendModes) for (const bool selected : {false, true}) {
        c::Document doc({{4, 3}});
        auto source = c::Layer::raster("Source", surface());
        source.blendMode = mode;
        source.opacity = .4F;
        CHECK(doc.insertLayer(0, source));
        if (selected) CHECK(doc.setSelection(c::SelectionMask::rectangle({4,3}, {1,0,2,3})));
        c::LayerViaCopyCommand copy(source.id, source.id);
        CHECK(copy.apply(doc));
        CHECK(doc.layers().size() == 2);
        const auto created = *copy.createdLayerId();
        CHECK(doc.layer(created)->blendMode == source.blendMode);
        CHECK(doc.layer(created)->blendSeed == source.blendSeed);
        CHECK(doc.layer(created)->opacity == source.opacity);
        CHECK(copy.undo(doc));
        CHECK(copy.apply(doc));
        CHECK(doc.layer(created)->blendMode == source.blendMode);
        CHECK(doc.layer(created)->blendSeed == source.blendSeed);
    }
}

void allTypesRoundTrip()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    c::Document doc({{4,3}});
    for (const auto mode : c::allBlendModes) {
        auto raster = c::Layer::raster("Raster", surface());
        raster.blendMode = mode;
        CHECK(doc.insertLayer(doc.layers().size(), std::move(raster)));
        auto text = c::Layer::text("Text", c::TextLayer {.utf8="Rich text"});
        text.blendMode = mode;
        CHECK(doc.insertLayer(doc.layers().size(), std::move(text)));
        auto shape = c::Layer::shape("Shape", c::ShapeLayer {});
        shape.blendMode = mode;
        CHECK(doc.insertLayer(doc.layers().size(), std::move(shape)));
        auto adjustment=c::Layer::adjustment("Correction");
        adjustment.blendMode=mode;
        adjustment.blendSeed=0xffffffffU;
        CHECK(doc.insertLayer(doc.layers().size(),std::move(adjustment)));
    }
    // Keep a mixed triple inside a group nested in a folder. Containers never
    // consume/replace the leaf modes or gain a blending state of their own.
    auto hierarchy = doc.tree();
    const auto groupId = c::makeLayerId(), folderId = c::makeLayerId();
    c::LayerContainer group {groupId, "Mixed group", c::ContainerKind::Group};
    group.children.assign(hierarchy.roots.begin(), hierarchy.roots.begin()+3);
    hierarchy.roots.erase(hierarchy.roots.begin(), hierarchy.roots.begin()+3);
    hierarchy.roots.insert(hierarchy.roots.begin(), folderId);
    hierarchy.containers.push_back(std::move(group));
    c::LayerContainer folder {folderId, "Folder", c::ContainerKind::Folder};
    folder.children = {groupId};
    hierarchy.containers.push_back(std::move(folder));
    CHECK(doc.replaceStructure(doc.tree(), hierarchy));
    CHECK(!doc.setLayerBlendMode(groupId, c::BlendMode::Multiply));
    CHECK(!doc.setLayerBlendMode(folderId, c::BlendMode::Multiply));
    const auto path = directory.filePath("all-modes.vulkana");
    CHECK(u::saveProject(path, doc));
    auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (!loaded) { std::cerr << loaded.error.toStdString() << '\n'; return; }
    CHECK(loaded.metadata["required"].toArray().contains("layer-blend-modes-v1"));
    CHECK(loaded.metadata["required"].toArray().contains("layer-blend-modes-v2"));
    CHECK(loaded.metadata["required"].toArray().contains("blend-modes-v3"));
    CHECK(loaded.document->tree() == doc.tree());
    CHECK(loaded.document->layers().size() == doc.layers().size());
    for (std::size_t i=0; i<doc.layers().size(); ++i) {
        CHECK(loaded.document->layers()[i].blendMode == doc.layers()[i].blendMode);
        CHECK(loaded.document->layers()[i].blendSeed == doc.layers()[i].blendSeed);
        CHECK(loaded.document->layers()[i].payload.index() == doc.layers()[i].payload.index());
        CHECK(loaded.metadata["layers"].toArray()[qsizetype(i)].toObject()["blendMode"].toString()
            == QString::fromUtf8(c::blendModeId(doc.layers()[i].blendMode)));
    }
    // Saving only original modes must remove v2 even when compatible metadata
    // was loaded from an extended-mode project; retain the v1 base capability.
    for (const auto& layer : loaded.document->layers())
        if (layer.blendMode >= c::BlendMode::ColorDodge)
            CHECK(loaded.document->setLayerBlendMode(layer.id,c::BlendMode::Multiply));
    CHECK(u::saveProject(path,*loaded.document,loaded.metadata));
    loaded = u::loadProject(path);
    CHECK(loaded); if (!loaded) return;
    CHECK(loaded.metadata["required"].toArray().contains("layer-blend-modes-v1"));
    CHECK(!loaded.metadata["required"].toArray().contains("layer-blend-modes-v2"));
    // Returning every layer to Normal drops both required features even when
    // re-saving metadata from a non-Normal document; owned fields cannot leak.
    for (const auto& layer : loaded.document->layers())
        (void)loaded.document->setLayerBlendMode(layer.id, c::BlendMode::Normal);
    CHECK(u::saveProject(path, *loaded.document, loaded.metadata));
    loaded = u::loadProject(path);
    CHECK(loaded);
    if (loaded) {
        CHECK(!loaded.metadata["required"].toArray().contains("layer-blend-modes-v1"));
        CHECK(!loaded.metadata["required"].toArray().contains("layer-blend-modes-v2"));
        CHECK(!loaded.metadata["required"].toArray().contains("blend-modes-v3"));
    }
}

void zipOkay(int code)
{ if (code != MZ_OK) throw std::runtime_error("Cannot write independent ZIP fixture"); }

// Independent tiny transport for old and malformed manifests. Production's
// serializer is not used to manufacture compatibility/rejection fixtures.
void writeFixture(const QString& path, const QJsonObject& manifest)
{
    auto* stream = mz_stream_os_create();
    auto* zip = mz_zip_create();
    try {
        zipOkay(mz_stream_open(stream, QFile::encodeName(path).constData(), MZ_OPEN_MODE_CREATE|MZ_OPEN_MODE_WRITE));
        zipOkay(mz_zip_open(zip, stream, MZ_OPEN_MODE_CREATE|MZ_OPEN_MODE_WRITE));
        const std::array entries {
            std::pair {QByteArray("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Compact)},
            std::pair {QByteArray("rasters/17.rgba"), QByteArray::fromHex("478fdd7f")},
        };
        for (const auto& [name, bytes] : entries) {
            mz_zip_file info {};
            info.filename = name.constData();
            info.filename_size = uint16_t(name.size());
            info.compression_method = MZ_COMPRESS_METHOD_STORE;
            info.uncompressed_size = bytes.size();
            info.external_fa = 0100644U << 16U;
            info.version_madeby = uint16_t((3U<<8U)|45U);
            zipOkay(mz_zip_entry_write_open(zip, &info, 0, 0, nullptr));
            if (mz_zip_entry_write(zip, bytes.constData(), int32_t(bytes.size())) != bytes.size())
                throw std::runtime_error("Short ZIP fixture write");
            zipOkay(mz_zip_entry_write_close(zip, 0, -1, -1));
        }
        zipOkay(mz_zip_close(zip));
        mz_zip_delete(&zip);
        zipOkay(mz_stream_close(stream));
        mz_stream_os_delete(&stream);
    } catch (...) {
        mz_zip_delete(&zip);
        mz_stream_close(stream);
        mz_stream_os_delete(&stream);
        throw;
    }
}

void compatibilityAndRejection()
{
    QTemporaryDir directory;
    auto leaf = QJsonObject {{"id","17"},{"name","Old layer"},{"type","raster"},
        {"raster",QJsonObject {{"width",1},{"height",1},{"path","rasters/17.rgba"}}}};
    QJsonObject manifest {{"format","org.vulkana.project"},{"version",1},
        {"required",QJsonArray {"rgba8"}},
        {"canvas",QJsonObject {{"width",1},{"height",1}}},{"layers",QJsonArray {leaf}}};
    const auto path = directory.filePath("fixture.vulkana");
    writeFixture(path, manifest);
    auto result = u::loadProject(path);
    CHECK(result);
    if (result) CHECK(result.document->layers()[0].blendMode == c::BlendMode::Normal);
    for (const auto value : {QJsonValue("future-mode"),QJsonValue(3),QJsonValue(QJsonValue::Null)}) {
        leaf["blendMode"] = value; manifest["layers"] = QJsonArray {leaf};
        writeFixture(path, manifest);
        result = u::loadProject(path);
        CHECK(!result);
        CHECK(result.error.contains("blend mode", Qt::CaseInsensitive));
    }
    leaf["blendMode"] = "multiply"; manifest["layers"] = QJsonArray {leaf};
    writeFixture(path, manifest);
    result = u::loadProject(path);
    CHECK(!result);
    CHECK(result.error.contains("capability"));
    manifest["required"] = QJsonArray {"rgba8","layer-blend-modes-v1"};
    writeFixture(path, manifest);
    result = u::loadProject(path);
    CHECK(result);
    if (result) CHECK(result.document->layers()[0].blendMode == c::BlendMode::Multiply);
    for (const auto mode : c::allBlendModes) {
        if (mode < c::BlendMode::ColorDodge) continue;
        leaf["blendMode"] = QString::fromUtf8(c::blendModeId(mode));
        manifest["layers"] = QJsonArray {leaf};
        manifest["required"] = QJsonArray {"rgba8","layer-blend-modes-v1"};
        writeFixture(path,manifest);
        result = u::loadProject(path);
        CHECK(!result);
        CHECK(result.error.contains("layer-blend-modes-v2"));
        manifest["required"] = QJsonArray {"rgba8","layer-blend-modes-v2"};
        writeFixture(path,manifest);
        result = u::loadProject(path);
        CHECK(!result);
        CHECK(result.error.contains("layer-blend-modes-v1"));
        manifest["required"] = QJsonArray {"rgba8","layer-blend-modes-v1","layer-blend-modes-v2"};
        if(mode>=c::BlendMode::Dissolve) {
            writeFixture(path,manifest);CHECK(!u::loadProject(path));
            manifest["required"]=QJsonArray {"rgba8","layer-blend-modes-v1","layer-blend-modes-v2","blend-modes-v3"};
            leaf["blendSeed"]=123456789;manifest["layers"]=QJsonArray{leaf};
        }
        writeFixture(path,manifest);
        result = u::loadProject(path);
        CHECK(result);
        if (result) CHECK(result.document->layers()[0].blendMode == mode);
    }
    leaf["blendMode"] = "future-mode"; manifest["layers"] = QJsonArray {leaf};
    writeFixture(path, manifest);
    result = u::loadProject(path);
    CHECK(!result);
    CHECK(result.error.contains("Unsupported layer blend mode"));
}
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        metadataAndHistory();
        copiesRetainUnblendedProperties();
        allTypesRoundTrip();
        compatibilityAndRejection();
    } catch (const std::exception& error) {
        ++failures; std::cerr << error.what() << '\n';
    }
    std::cout << (failures ? "Blend metadata/persistence tests FAILED\n" : "Blend metadata/persistence tests passed\n");
    return failures ? 1 : 0;
}
