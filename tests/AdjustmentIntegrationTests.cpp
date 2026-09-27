#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
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
    const auto e = surface.extent();
    std::vector<std::byte> bytes(std::size_t(e.width) * e.height * 4);
    surface.copyRgba8({0, 0, int(e.width), int(e.height)}, bytes, std::size_t(e.width) * 4);
    return {reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())};
}
std::shared_ptr<c::RasterSurface> raster(c::Extent2u extent = {8, 6})
{
    std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        bytes[i] = std::byte(30 + i % 180);
        bytes[i + 1] = std::byte(70 + i % 120);
        bytes[i + 2] = std::byte(20 + i % 200);
        bytes[i + 3] = std::byte(i == 0 ? 0 : i % 3 == 0 ? 128 : 255);
    }
    return std::make_shared<c::ContiguousRasterSurface>(extent, std::move(bytes));
}
c::AdjustmentState fullStack()
{
    auto stack = std::make_shared<c::AdjustmentStack>();
    for (auto& item : stack->items) item.enabled = true;
    std::get<c::ExposureParameters>(stack->items[0].parameters).stops = 1.25;
    stack->items[1].parameters = c::BrightnessContrastParameters {.125, -.25};
    std::get<c::LevelsParameters>(stack->items[2].parameters).channels[1] = {.1, 1.4, .9, .05, .95};
    std::get<c::CurvesParameters>(stack->items[3].parameters).channels[0].points = {{0, .1}, {.3, .8}, {.7, .2}, {1, .9}};
    auto& hue = std::get<c::HueSaturationParameters>(stack->items[4].parameters);
    hue.ranges[0] = {25, .2, -.1}; hue.ranges[4] = {-90, -.25, .1};
    hue.colorize = true; hue.colorizeHue = 220; hue.colorizeSaturation = .35;
    stack->items[5].parameters = c::VibranceParameters {.45};
    auto& balance = std::get<c::ColorBalanceParameters>(stack->items[6].parameters);
    balance.tones[0] = {.1, -.2, .3}; balance.tones[2] = {-.15, .05, .2}; balance.preserveLuminosity = false;
    stack->items[7].parameters = c::WarmthTintParameters {.25, -.1};
    auto& mono = std::get<c::BlackWhiteParameters>(stack->items[8].parameters);
    mono.contributions = {-.2, .1, .3, -.1, .2, .05}; mono.tintStrength = .3; mono.tintColor = {170, 120, 65, 255};
    std::vector<std::uint8_t> coverage(16 * 16, 0);
    for (int y = 2; y < 12; ++y) for (int x = 1; x < 14; ++x)
        if (!(x >= 5 && x < 8 && y >= 5 && y < 8)) coverage[std::size_t(y) * 16 + std::size_t(x)] = std::uint8_t((x % 4) * 85);
    stack->items[0].mask = c::AdjustmentMask {c::SelectionMask::fromR8({16, 16}, coverage, 16), {1.2, .1, 2, -.2, .9, 3}};
    stack->items[9].mask = c::AdjustmentMask {c::SelectionMask::filled({16, 16}, 0), {}};
    CHECK(c::validAdjustments(*stack));
    return stack;
}
c::AdjustmentState exposure(double stops, c::SelectionState mask = {}, c::AffineTransform transform = {})
{
    auto stack = std::make_shared<c::AdjustmentStack>();
    stack->items[0].enabled = true;
    stack->items[0].parameters = c::ExposureParameters {stops};
    if (mask) stack->items[0].mask = c::AdjustmentMask {std::move(mask), transform};
    return stack;
}
c::Document sampleDocument()
{
    c::Document doc({{16, 16}, 96});
    auto bitmap = c::Layer::raster("Untouched source", raster());
    bitmap.localToDocument = {1.25, .2, 2, -.1, 1.1, 3};
    bitmap.adjustments = fullStack();
    CHECK(doc.insertLayer(0, bitmap));
    c::TextLayer text; text.utf8 = "Editable text"; text.defaultStyle.sizePixels = 5;
    auto editable = c::Layer::text("Text", text); editable.adjustments = fullStack();
    CHECK(doc.insertLayer(1, editable));
    c::ShapeLayer shape; shape.size = {6, 4}; shape.fillColor = {36, 75, 150, 192};
    auto vector = c::Layer::shape("Shape", shape); vector.adjustments = fullStack();
    CHECK(doc.insertLayer(2, vector));
    return doc;
}

using Payloads = std::map<QString, QByteArray>;
Payloads payloads(const c::Document& doc)
{
    Payloads result;
    for (const auto& layer : doc.layers()) {
        if (const auto* source = std::get_if<c::RasterLayer>(&layer.payload))
            result[QStringLiteral("rasters/%1.rgba").arg(layer.id)] = pixels(*source->surface);
        if (!layer.adjustments) continue;
        for (const auto& item : layer.adjustments->items) {
            if (!item.mask) continue;
            const auto extent = item.mask->coverage->extent();
            QByteArray bytes(qsizetype(extent.width) * extent.height, char(0));
            for (std::uint32_t y = 0; y < extent.height; ++y) for (std::uint32_t x = 0; x < extent.width; ++x)
                bytes[qsizetype(y) * extent.width + x] = char(item.mask->coverage->coverageAtDocumentPixel(int(x), int(y)));
            const auto identifier = c::adjustmentIdentifier(item.type);
            result[QStringLiteral("adjustments/%1/%2.r8").arg(layer.id)
                .arg(QString::fromUtf8(identifier.data(), qsizetype(identifier.size())))] = std::move(bytes);
        }
    }
    return result;
}
void zipOkay(int result)
{
    if (result != MZ_OK) throw std::runtime_error("ZIP fixture failure: " + std::to_string(result));
}
// Independent archive transport allows corrupt descriptors/payloads without
// routing them through production writer validation.
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
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) throw std::runtime_error("Cannot write fixture");
}
void editStack(QJsonObject& manifest, const std::function<void(QJsonObject&)>& edit)
{
    auto layers = manifest["layers"].toArray();
    auto layer = layers[0].toObject(); auto stack = layer["adjustments"].toObject();
    edit(stack); layer["adjustments"] = stack; layers[0] = layer; manifest["layers"] = layers;
}
void editItem(QJsonObject& manifest, const std::function<void(QJsonObject&)>& edit)
{
    editStack(manifest, [&](QJsonObject& stack) {
        auto items = stack["items"].toArray(); auto item = items[0].toObject();
        edit(item); items[0] = item; stack["items"] = items;
    });
}

void projectRoundTripsTypedParametersMasksAndUntouchedSources()
{
    QTemporaryDir directory; CHECK(directory.isValid());
    auto doc = sampleDocument();
    const auto source = pixels(*std::get<c::RasterLayer>(doc.layers()[0].payload).surface);
    const auto path = directory.filePath("complete.vulkana");
    CHECK(u::saveProject(path, doc));
    auto loaded = u::loadProject(path);
    CHECK(loaded); if (!loaded) { std::cerr << loaded.error.toStdString() << '\n'; return; }
    CHECK(loaded.metadata["required"].toArray().contains("adjustments-v1"));
    CHECK(loaded.document->layers().size() == doc.layers().size());
    for (const auto& layer : doc.layers()) {
        const auto* restored = loaded.document->layer(layer.id);
        CHECK(restored); if (!restored) continue;
        CHECK(c::equivalentAdjustments(layer.adjustments, restored->adjustments));
        CHECK(restored->localToDocument == layer.localToDocument);
    }
    CHECK(std::get<c::TextLayer>(loaded.document->layers()[1].payload) == std::get<c::TextLayer>(doc.layers()[1].payload));
    CHECK(std::get<c::ShapeLayer>(loaded.document->layers()[2].payload) == std::get<c::ShapeLayer>(doc.layers()[2].payload));
    CHECK(pixels(*std::get<c::RasterLayer>(loaded.document->layers()[0].payload).surface) == source);
    CHECK(pixels(*std::get<c::RasterLayer>(doc.layers()[0].payload).surface) == source);
    const auto& empty = loaded.document->layers()[0].adjustments->items[9].mask;
    CHECK(empty && empty->coverage && empty->coverage->bounds().empty());
    for (const auto& layer : doc.layers()) CHECK(loaded.document->setLayerAdjustments(layer.id, {}));
    const auto resetPath = directory.filePath("reset.vulkana");
    CHECK(u::saveProject(resetPath, *loaded.document, loaded.metadata));
    const auto reset = u::loadProject(resetPath);
    CHECK(reset); if (!reset) return;
    CHECK(!reset.metadata["required"].toArray().contains("adjustments-v1"));
    for (const auto& layer : reset.document->layers()) CHECK(!layer.adjustments);
    for (const auto& layer : reset.metadata["layers"].toArray()) CHECK(!layer.toObject().contains("adjustments"));
}

void unsupportedOrMalformedAdjustmentsAreRejectedBeforePayloadAllocation()
{
    QTemporaryDir directory;
    const auto doc = sampleDocument();
    const auto path = directory.filePath("base.vulkana");
    CHECK(u::saveProject(path, doc));
    const auto saved = u::loadProject(path); CHECK(saved); if (!saved) return;
    const auto files = payloads(doc);
    int sequence = 0;
    const auto reject = [&](QJsonObject manifest, Payloads content) {
        const auto fixture = directory.filePath(QStringLiteral("invalid-%1.vulkana").arg(++sequence));
        write(fixture, archive(manifest, std::move(content)));
        const auto loaded = u::loadProject(fixture);
        CHECK(!loaded && !loaded.document && !loaded.error.isEmpty());
    };
    // The independent fixture itself must first pass the real reader.
    const auto independent = directory.filePath("independent.vulkana");
    write(independent, archive(saved.metadata, files)); CHECK(u::loadProject(independent));
    auto manifest = saved.metadata;
    editItem(manifest, [](QJsonObject& item) { item["type"] = "future-essential-adjustment"; }); reject(manifest, files);
    manifest = saved.metadata;
    editStack(manifest, [](QJsonObject& stack) { stack["algorithmVersion"] = 99; }); reject(manifest, files);
    manifest = saved.metadata;
    editItem(manifest, [](QJsonObject& item) { item["algorithmVersion"] = 99; }); reject(manifest, files);
    manifest = saved.metadata;
    editStack(manifest, [](QJsonObject& stack) { auto items = stack["items"].toArray(); const QJsonValue first = items[0]; items[0] = items[1]; items[1] = first; stack["items"] = items; }); reject(manifest, files);
    manifest = saved.metadata;
    editItem(manifest, [](QJsonObject& item) { item["enabled"] = "true"; }); reject(manifest, files);
    manifest = saved.metadata;
    editItem(manifest, [](QJsonObject& item) { item["parameters"] = QJsonObject {{"stops", "NaN"}}; }); reject(manifest, files);
    for (const auto& key : {QStringLiteral("width"), QStringLiteral("height")}) {
        manifest = saved.metadata;
        editItem(manifest, [&](QJsonObject& item) { auto mask = item["mask"].toObject(); mask[key] = 32769; item["mask"] = mask; });
        reject(manifest, files);
    }
    manifest = saved.metadata;
    editItem(manifest, [](QJsonObject& item) { auto mask = item["mask"].toObject(); mask["localToMask"] = QJsonArray {0, 0, 0, 0, 0, 0}; item["mask"] = mask; }); reject(manifest, files);
    manifest = saved.metadata;
    editItem(manifest, [](QJsonObject& item) { auto mask = item["mask"].toObject(); mask["path"] = "../wrong.r8"; item["mask"] = mask; }); reject(manifest, files);
    manifest = saved.metadata;
    auto required = manifest["required"].toArray();
    for (qsizetype i = required.size(); i > 0; --i) if (required[i - 1] == "adjustments-v1") required.removeAt(i - 1);
    manifest["required"] = required; reject(manifest, files);
    auto truncated = files;
    auto mask = std::find_if(truncated.begin(), truncated.end(), [](const auto& entry) { return entry.first.endsWith(".r8"); });
    CHECK(mask != truncated.end()); if (mask != truncated.end()) { mask->second.chop(1); reject(saved.metadata, truncated); }
    auto missing = files;
    mask = std::find_if(missing.begin(), missing.end(), [](const auto& entry) { return entry.first.endsWith(".r8"); });
    if (mask != missing.end()) { missing.erase(mask); reject(saved.metadata, missing); }
}

void copiesKeepEditableAdjustmentsAndRemapCapturedMasks()
{
    for (const bool selected : {false, true}) for (const bool transformed : {false, true}) {
        c::Document doc({{20, 20}, 96});
        auto source = c::Layer::raster("Copy source", raster());
        source.localToDocument = transformed ? c::AffineTransform {1.25, .2, 3, -.1, 1.5, 4}
                                            : c::AffineTransform {1, 0, 3, 0, 1, 4};
        source.adjustments = exposure(1, c::SelectionMask::rectangle({20, 20}, {4, 5, 5, 5}, 128), source.localToDocument);
        source.opacity = .6F; source.blendMode = c::BlendMode::Multiply;
        const auto sourceBytes = pixels(*std::get<c::RasterLayer>(source.payload).surface);
        CHECK(doc.insertLayer(0, source));
        if (selected) CHECK(doc.setSelection(c::SelectionMask::rectangle({20, 20}, {4, 5, 4, 3}, 128)));
        c::LayerViaCopyCommand command(source.id, source.id);
        CHECK(command.apply(doc)); const auto id = command.createdLayerId(); CHECK(id); if (!id) continue;
        const auto* copy = doc.layer(*id); CHECK(copy && copy->adjustments); if (!copy || !copy->adjustments) continue;
        CHECK(copy->adjustments != source.adjustments);
        CHECK(copy->opacity == source.opacity && copy->blendMode == source.blendMode);
        const auto expected = selected ? c::composeAffine(source.adjustments->items[0].mask->localToMask,
            c::composeAffine(*source.localToDocument.inverted(), copy->localToDocument))
            : source.adjustments->items[0].mask->localToMask;
        const auto& copiedMask = copy->adjustments->items[0].mask;
        CHECK(copiedMask && copiedMask->localToMask == expected);
        CHECK(copiedMask && copiedMask->coverage->equivalent(*source.adjustments->items[0].mask->coverage));
        if (!selected) CHECK(pixels(*std::get<c::RasterLayer>(copy->payload).surface) == sourceBytes);
        else {
            const c::PreparedRasterSampler rawSource(source);
            const auto copied = pixels(*std::get<c::RasterLayer>(copy->payload).surface);
            const auto e = std::get<c::RasterLayer>(copy->payload).surface->extent();
            for (std::uint32_t y = 0; y < e.height; ++y) for (std::uint32_t x = 0; x < e.width; ++x) {
                const auto point = copy->localToDocument.map({x + .5, y + .5});
                const auto raw = rawSource.sample(point);
                const auto alpha = (unsigned(raw.alpha) * 128 + 127) / 255;
                const auto offset = qsizetype((std::size_t(y) * e.width + x) * 4);
                CHECK(std::uint8_t(copied[offset + 3]) == alpha);
                if (alpha) {
                    CHECK(std::uint8_t(copied[offset]) == raw.red);
                    CHECK(std::uint8_t(copied[offset + 1]) == raw.green);
                    CHECK(std::uint8_t(copied[offset + 2]) == raw.blue);
                }
            }
        }
        CHECK(command.memoryCost() >= c::adjustmentMemoryCost(copy->adjustments));
        auto modified = std::make_shared<c::AdjustmentStack>(*copy->adjustments);
        std::get<c::ExposureParameters>(modified->items[0].parameters).stops = 2;
        CHECK(doc.setLayerAdjustments(copy->id, modified));
        CHECK(std::get<c::ExposureParameters>(doc.layer(source.id)->adjustments->items[0].parameters).stops == 1);
        CHECK(pixels(*std::get<c::RasterLayer>(doc.layer(source.id)->payload).surface) == sourceBytes);
    }
}

void flattenMergeAndThumbnailUseAdjustedAppearance()
{
    auto doc = std::make_unique<c::Document>(c::CanvasSpec {{8, 6}, 96});
    auto source = c::Layer::raster("Source", raster());
    CHECK(doc->insertLayer(0, source));
    const auto raw = u::flattenDocument(*doc); CHECK(raw);
    CHECK(doc->setLayerAdjustments(source.id, exposure(1)));
    const auto adjusted = u::flattenDocument(*doc); CHECK(adjusted);
    CHECK(raw.image != adjusted.image);
    CHECK(pixels(*std::get<c::RasterLayer>(source.payload).surface) == pixels(*std::get<c::RasterLayer>(doc->layer(source.id)->payload).surface));
    const auto active = c::sampleDocumentColor(*doc, source.id, {2.5, 2.5}, c::ColorSampleSource::ActiveLayer);
    CHECK(active.color == c::PreparedRasterSampler(source).sample({2.5, 2.5}));
    const std::array ids {source.id};
    const auto flattened = u::flattenLayerItems(*doc, ids); CHECK(flattened);
    CHECK(flattened.image == adjusted.image);
    std::vector<std::byte> baked(std::size_t(flattened.image.width()) * std::size_t(flattened.image.height()) * 4);
    for (int y = 0; y < flattened.image.height(); ++y)
        std::memcpy(baked.data() + std::size_t(y) * std::size_t(flattened.image.width()) * 4,
            flattened.image.constScanLine(y), std::size_t(flattened.image.width()) * 4);
    auto merged = c::Layer::raster("Merged", std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {8, 6}, std::move(baked)));
    CHECK(!merged.adjustments);
    c::Document output({{8, 6}, 96}); CHECK(output.insertLayer(0, merged));
    CHECK(u::flattenDocument(output).image == adjusted.image);
    auto plain = *doc->layer(source.id); plain.adjustments.reset();
    CHECK(c::retainedLayerMemory(*doc->layer(source.id)) >= c::retainedLayerMemory(plain) + c::adjustmentMemoryCost(doc->layer(source.id)->adjustments));

    auto tree = doc->tree();
    const auto group = c::makeLayerId();
    tree.containers.push_back({group, "Group", c::ContainerKind::Group, c::ColorLabel::None, {source.id}});
    tree.roots = {group}; CHECK(doc->replaceStructure(doc->tree(), tree));
    c::EditorSession session; session.replaceDocument(std::move(doc));
    u::LayerListModel model; model.setSession(&session);
    const auto icon = [&] { return qvariant_cast<QIcon>(model.index(model.rowForLayer(group)).data(Qt::DecorationRole)).pixmap(40, 40).toImage(); };
    const auto before = icon();
    CHECK(session.document()->setLayerAdjustments(source.id, exposure(-2)));
    model.refresh(); QTest::qWait(190);
    CHECK(icon() != before);
    CHECK(session.document()->setLayerAdjustments(source.id, exposure(1)));
    model.refresh(); QTest::qWait(190);
    CHECK(icon() == before);
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    u::applyEditorTheme(app);
    try {
        projectRoundTripsTypedParametersMasksAndUntouchedSources();
        unsupportedOrMalformedAdjustmentsAreRejectedBeforePayloadAllocation();
        copiesKeepEditableAdjustmentsAndRemapCapturedMasks();
        flattenMergeAndThumbnailUseAdjustedAppearance();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; ++failures; }
    if (failures) return EXIT_FAILURE;
    std::cout << "Adjustment integration tests passed\n";
    return EXIT_SUCCESS;
}
