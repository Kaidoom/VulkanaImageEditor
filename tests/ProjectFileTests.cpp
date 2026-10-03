#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/TextClipboard.hpp"

#include <QBuffer>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read project fixture: " + path.toStdString());
    return file.readAll();
}
void writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write project fixture: " + path.toStdString());
}
QString fixturePath(const QString& name)
{
#ifdef IMAGEEDITOR_PROJECT_FIXTURE_DIR
    return QDir(QString::fromUtf8(IMAGEEDITOR_PROJECT_FIXTURE_DIR)).filePath(name);
#else
    return QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("fixtures/projects/" + name);
#endif
}
QJsonObject baseManifest()
{
    const auto document = QJsonDocument::fromJson(readBytes(fixturePath("v1-manifest.json")));
    if (!document.isObject())
        throw std::runtime_error("Invalid frozen project manifest");
    return document.object();
}
QByteArray referencePixels()
{
    const auto pixels = QByteArray::fromHex(readBytes(fixturePath("v1-raster.hex")));
    if (pixels.size() != 24)
        throw std::runtime_error("Invalid frozen RGBA fixture");
    return pixels;
}

// Independent fixture transport. Production saveProject is deliberately not
// used to create invalid files or the frozen schema compatibility fixture.
struct MemoryStream {
    mz_stream base;
    QBuffer buffer;
    MemoryStream();
    static MemoryStream& self(void* stream) { return *static_cast<MemoryStream*>(stream); }
    static int32_t open(void*, const char*, int32_t) { return MZ_OK; }
    static int32_t isOpen(void* p) { return self(p).buffer.isOpen() ? MZ_OK : MZ_OPEN_ERROR; }
    static int32_t read(void* p, void* bytes, int32_t size)
    {
        return int32_t(self(p).buffer.read(static_cast<char*>(bytes), size));
    }
    static int32_t write(void* p, const void* bytes, int32_t size)
    {
        return int32_t(self(p).buffer.write(static_cast<const char*>(bytes), size));
    }
    static int64_t tell(void* p) { return self(p).buffer.pos(); }
    static int32_t seek(void* p, int64_t offset, int32_t origin)
    {
        auto& buffer = self(p).buffer;
        const qint64 start = origin == MZ_SEEK_SET ? 0
            : origin == MZ_SEEK_CUR               ? buffer.pos()
                                                 : buffer.size();
        if ((offset < 0 && offset < -start)
            || (offset > 0 && start > std::numeric_limits<qint64>::max() - offset))
            return MZ_SEEK_ERROR;
        return buffer.seek(start + offset) ? MZ_OK : MZ_SEEK_ERROR;
    }
    static int32_t close(void*) { return MZ_OK; }
    static int32_t error(void*) { return MZ_OK; }
    static int32_t get(void*, int32_t property, int64_t* value)
    {
        if (property != MZ_STREAM_PROP_DISK_NUMBER)
            return MZ_EXIST_ERROR;
        *value = 0;
        return MZ_OK;
    }
    static int32_t set(void*, int32_t property, int64_t value)
    {
        return property == MZ_STREAM_PROP_DISK_NUMBER && (value == -1 || value == 0)
            ? MZ_OK
            : MZ_EXIST_ERROR;
    }
};
MemoryStream::MemoryStream()
{
    static mz_stream_vtbl table { open, isOpen, read, write, tell, seek, close, error,
        nullptr, nullptr, get, set };
    base = { &table, nullptr };
    if (!buffer.open(QIODevice::ReadWrite))
        throw std::runtime_error("Cannot open ZIP fixture buffer");
}
struct ZipEntry {
    QByteArray name;
    QByteArray bytes;
    uint16_t method { MZ_COMPRESS_METHOD_STORE };
    uint16_t zip64 { MZ_ZIP64_AUTO };
    uint32_t attributes { 0100644U << 16U };
};
void zipOkay(int32_t result)
{
    if (result != MZ_OK)
        throw std::runtime_error("ZIP fixture operation failed: " + std::to_string(result));
}
QByteArray makeZip(const std::vector<ZipEntry>& entries)
{
    MemoryStream stream;
    void* zip = mz_zip_create();
    if (!zip)
        throw std::bad_alloc();
    try {
        zipOkay(mz_zip_open(zip, &stream, MZ_OPEN_MODE_WRITE | MZ_OPEN_MODE_CREATE));
        for (const auto& entry : entries) {
            mz_zip_file info { };
            info.filename = entry.name.constData();
            info.filename_size = uint16_t(entry.name.size());
            info.uncompressed_size = entry.bytes.size();
            info.compression_method = entry.method;
            info.zip64 = entry.zip64;
            info.flag = MZ_ZIP_FLAG_UTF8;
            info.version_madeby = uint16_t((3U << 8U) | 45U);
            info.external_fa = entry.attributes;
            info.modified_date = 946684800; // Fixed 2000-01-01 UTC.
            zipOkay(mz_zip_entry_write_open(zip, &info,
                entry.method == MZ_COMPRESS_METHOD_STORE ? 0 : 1, 0, nullptr));
            if (!entry.bytes.isEmpty()) {
                const auto written = mz_zip_entry_write(zip, entry.bytes.constData(), int32_t(entry.bytes.size()));
                if (written != entry.bytes.size())
                    throw std::runtime_error("Short ZIP fixture write");
            }
            zipOkay(mz_zip_entry_write_close(zip, 0, -1, -1));
        }
        zipOkay(mz_zip_close(zip));
        mz_zip_delete(&zip);
        return stream.buffer.data();
    } catch (...) {
        mz_zip_close(zip);
        mz_zip_delete(&zip);
        throw;
    }
}
std::vector<ZipEntry> entriesFor(const QJsonObject& manifest = baseManifest())
{
    return { { "manifest.json", QJsonDocument(manifest).toJson(QJsonDocument::Compact) },
        { "rasters/17.rgba", referencePixels() } };
}
struct Fixture {
    QTemporaryDir directory;
    int sequence { 0 };
    Fixture()
    {
        if (!directory.isValid())
            throw std::runtime_error("Cannot create project-test directory");
    }
    QString path(const QString& name) const { return directory.filePath(name); }
    QString store(const QByteArray& bytes)
    {
        const auto file = path(QString("fixture-%1.vulkana").arg(++sequence));
        writeBytes(file, bytes);
        return file;
    }
    u::ProjectLoadResult load(const QJsonObject& manifest)
    {
        return u::loadProject(store(makeZip(entriesFor(manifest))));
    }
    QString reject(const QString& label, const QByteArray& bytes)
    {
        const auto result = u::loadProject(store(bytes));
        if (result || result.document || result.error.isEmpty()) {
            std::cerr << "Rejected-project case failed: " << label.toStdString()
                      << " error=" << result.error.toStdString() << '\n';
            ++failures;
        }
        return result.error;
    }
    QString reject(const QString& label, const QJsonObject& manifest)
    {
        return reject(label, makeZip(entriesFor(manifest)));
    }
};
void editLayer(QJsonObject& manifest, const std::function<void(QJsonObject&)>& edit)
{
    auto layers = manifest["layers"].toArray();
    auto layer = layers[0].toObject();
    edit(layer);
    layers[0] = layer;
    manifest["layers"] = layers;
}
void editRaster(QJsonObject& manifest, const std::function<void(QJsonObject&)>& edit)
{
    editLayer(manifest, [&](QJsonObject& layer) {
        auto raster = layer["raster"].toObject();
        edit(raster);
        layer["raster"] = raster;
    });
}
QByteArray pixels(const c::RasterSurface& surface)
{
    const auto size = surface.extent();
    std::vector<std::byte> bytes(std::size_t(size.width) * size.height * 4);
    surface.copyRgba8({ 0, 0, int(size.width), int(size.height) }, bytes, std::size_t(size.width) * 4);
    return { reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()) };
}
std::shared_ptr<c::RasterSurface> referenceSurface()
{
    const auto bytes = referencePixels();
    std::vector<std::byte> data(std::size_t(bytes.size()));
    std::transform(bytes.begin(), bytes.end(), data.begin(), [](char v) { return std::byte(uint8_t(v)); });
    return std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 3, 2 }, std::move(data));
}
c::TextLayer richText()
{
    c::TextLayer text;
    text.utf8 = "Aé🙂\nאב";
    text.defaultStyle.font = { "Requested unavailable Ω font", "Book", 430, false };
    text.defaultStyle.sizePixels = 9;
    text.defaultStyle.color = { 13, 47, 211, 123 };
    auto different = text.defaultStyle;
    different.font = { "Another requested font", "Italic", 600, true };
    different.sizePixels = 20.125;
    different.color = { 225, 12, 94, 0 };
    text.runs = { { 0, 3, text.defaultStyle }, { 3, text.utf8.size() - 3, different } };
    text.paragraphs = { { 0, c::TextAlignment::Center }, { 8, c::TextAlignment::Right } };
    return c::normalizedText(std::move(text));
}
c::Document sampleDocument()
{
    c::Document document({ { 13, 9 }, 300.5 });
    auto raster = c::Layer::raster("Raw Ω pixels", referenceSurface());
    raster.id = 9007199254741009ULL; // Deliberately not representable as a JSON double.
    raster.visible = false;
    raster.opacity = 0.375F;
    raster.localToDocument = { -1.25, 0.375, 8.125, 0.5, 0.875, -2.25 };
    CHECK(document.insertLayer(0, std::move(raster)));
    auto text = c::Layer::text("Editable שלום", richText());
    text.id = 9007199254741011ULL;
    text.opacity = 0.625F;
    text.localToDocument = { 0.375, -1.25, 6.75, -0.875, -0.5, 3.125 };
    CHECK(document.insertLayer(1, std::move(text)));
    return document;
}
void sameDocument(const c::Document& actual, const c::Document& expected)
{
    CHECK(actual.canvas() == expected.canvas());
    CHECK(actual.layers().size() == expected.layers().size());
    for (std::size_t i = 0; i < std::min(actual.layers().size(), expected.layers().size()); ++i) {
        const auto& a = actual.layers()[i];
        const auto& b = expected.layers()[i];
        CHECK(a.id == b.id);
        CHECK(a.name == b.name);
        CHECK(a.visible == b.visible);
        CHECK(a.opacity == b.opacity);
        CHECK(a.localToDocument == b.localToDocument);
        CHECK(a.payload.index() == b.payload.index());
        CHECK(!a.renderCache); // Disposable layout/GPU caches are not persisted.
        if (const auto* r = std::get_if<c::RasterLayer>(&a.payload)) {
            const auto* original = std::get_if<c::RasterLayer>(&b.payload);
            CHECK(original != nullptr);
            if (original) {
                CHECK(r->surface->extent() == original->surface->extent());
                CHECK(pixels(*r->surface) == pixels(*original->surface));
            }
        } else if (const auto* original = std::get_if<c::TextLayer>(&b.payload))
            CHECK(std::get<c::TextLayer>(a.payload) == *original);
    }
}

void frozenFixtureDefaultsAndZipVariants()
{
    Fixture f;
    auto source = entriesFor();
    CHECK(makeZip(source) == makeZip(source));
    for (int variant = 0; variant < 3; ++variant) {
        auto entries = source;
        for (auto& entry : entries) {
            if (variant == 1)
                entry.method = MZ_COMPRESS_METHOD_DEFLATE;
            if (variant == 2)
                entry.zip64 = MZ_ZIP64_FORCE;
        }
        const auto result = u::loadProject(f.store(makeZip(entries)));
        CHECK(result);
        if (!result)
            continue;
        CHECK(result.document->canvas() == (c::CanvasSpec { { 11, 7 }, 144 }));
        CHECK(result.document->layers().size() == 1);
        const auto* layer = result.document->layer(17);
        CHECK(layer != nullptr);
        if (layer) {
            CHECK(layer->localToDocument == (c::AffineTransform { -1.25, 0.375, 8.125, 0.5, 0.875, -2.25 }));
            CHECK(layer->opacity == 0.625F);
            CHECK(pixels(*std::get<c::RasterLayer>(layer->payload).surface) == referencePixels());
        }
    }
    auto defaults = baseManifest();
    defaults.remove("required");
    auto canvas = defaults["canvas"].toObject();
    canvas.remove("ppi");
    defaults["canvas"] = canvas;
    editLayer(defaults, [](QJsonObject& layer) {
        layer.remove("visible");
        layer.remove("opacity");
        layer.remove("transform");
    });
    const auto result = f.load(defaults);
    CHECK(result);
    if (result) {
        CHECK(result.document->canvas().dotsPerInch == 96);
        CHECK(result.document->layers()[0].visible);
        CHECK(result.document->layers()[0].opacity == 1.0F);
        CHECK(result.document->layers()[0].localToDocument == c::AffineTransform { });
    }
}

void largeStreamingSave()
{
    // Exercise >1 GiB of real ZIP payloads without retaining that much RAM in
    // the routine test suite. copyRgba8 must only be asked for bounded chunks.
    struct ZeroSurface final : c::RasterSurface {
        c::SurfaceId id() const noexcept override { return 1; }
        c::Extent2u extent() const noexcept override { return {8192, 8192}; }
        c::Revision revision() const noexcept override { return 1; }
        c::DirtySet dirtySince(c::Revision) const override { return {}; }
        void copyRgba8(c::RectI r, std::span<std::byte> target, size_t stride) const override {
            CHECK(r.x == 0 && r.width == 8192 && r.height > 0);
            CHECK(target.size() <= 256 * 1024);
            CHECK(target.size() == size_t(r.height) * stride);
            std::fill(target.begin(), target.end(), std::byte{});
        }
        c::DirtySet replaceRgba8Batch(std::span<const c::RasterPatch>) override {
            throw std::runtime_error("Read-only generated fixture");
        }
        c::DirtySet swapRgba8Batch(std::span<c::MutableRasterPatch>) override {
            throw std::runtime_error("Read-only generated fixture");
        }
    };
    Fixture f;
    c::Document doc(c::CanvasSpec{{8192, 8192}, 72});
    auto surface = std::make_shared<ZeroSurface>();
    for (unsigned i = 0; i < 5; ++i) {
        c::Layer layer;
        layer.id = c::makeLayerId();
        layer.name = "Generated large raster";
        layer.payload = c::RasterLayer{surface};
        CHECK(doc.insertLayer(doc.layers().size(), std::move(layer)));
    }
    const auto revision = doc.revision();
    quint64 done = 0, total = 0;
    auto saved = u::saveProject(f.path("large.vulkana"), doc, {}, [&](quint64 d, quint64 t) {
        CHECK(d <= t && d >= done);
        done = d;
        total = t;
        return true;
    });
    CHECK(saved);
    CHECK(done == total && total > 2ULL * 1024 * 1024 * 1024);
    CHECK(doc.revision() == revision);
    CHECK(QFileInfo(f.path("large.vulkana")).size() > 0);
    const auto constrained = u::loadProject(f.path("large.vulkana"), {}, {1});
    CHECK(!constrained && constrained.error.contains("available load allowance"));
}

void exactRasterAndRichTextRoundTrip()
{
    Fixture f;
    const auto document = sampleDocument();
    const auto path = f.path("editable Ω.vulkana");
    const auto revision = document.revision();
    int progressCalls = 0;
    const auto saved = u::saveProject(path, document, { }, [&](quint64 completed, quint64 total) {
        CHECK(completed <= total);
        ++progressCalls;
        return true;
    });
    CHECK(saved);
    CHECK(saved.error.isEmpty());
    CHECK(!saved.cancelled);
    CHECK(progressCalls > 0);
    CHECK(document.revision() == revision);
    const auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (loaded) {
        sameDocument(*loaded.document, document);
        const auto second = f.path("second.vulkana");
        CHECK(u::saveProject(second, *loaded.document, loaded.metadata));
        const auto reloaded = u::loadProject(second);
        CHECK(reloaded);
        if (reloaded)
            sameDocument(*reloaded.document, document);
    }
    // The pure codec must also support a document containing only editable text.
    c::Document onlyText({ { 11, 7 }, 96 });
    CHECK(onlyText.insertLayer(0, c::Layer::text("Only text", richText())));
    CHECK(u::saveProject(path, onlyText));
    const auto textResult = u::loadProject(path);
    CHECK(textResult);
    if (textResult)
        sameDocument(*textResult.document, onlyText);
}

void unknownMetadataSurvivesKnownFieldChanges()
{
    Fixture f;
    auto manifest = baseManifest();
    const QJsonObject extension { { "version", 4 }, { "note", "未来 Ω" },
        { "values", QJsonArray { 1, false, QJsonValue::Null, "data" } } };
    manifest["vendor-extension"] = extension;
    auto canvas = manifest["canvas"].toObject();
    canvas["future-unit"] = "document-pixel";
    manifest["canvas"] = canvas;
    editLayer(manifest, [&](QJsonObject& layer) { layer["future-layer"] = extension; });
    editRaster(manifest, [&](QJsonObject& raster) { raster["future-storage"] = extension; });
    auto loaded = f.load(manifest);
    CHECK(loaded);
    if (!loaded)
        return;
    CHECK(loaded.document->renameLayer(17, "Changed name"));
    CHECK(loaded.document->setLayerOpacity(17, 0.25F));
    CHECK(loaded.document->setCanvas({ { 19, 12 }, 222 }));
    const auto path = f.path("retained.vulkana");
    CHECK(u::saveProject(path, *loaded.document, loaded.metadata));
    const auto reread = u::loadProject(path);
    CHECK(reread);
    if (!reread)
        return;
    CHECK(reread.document->canvas() == (c::CanvasSpec { { 19, 12 }, 222 }));
    CHECK(reread.document->layer(17)->name == "Changed name");
    CHECK(reread.document->layer(17)->opacity == 0.25F);
    CHECK(reread.metadata["vendor-extension"] == extension);
    CHECK(reread.metadata["canvas"].toObject()["future-unit"] == "document-pixel");
    const auto layer = reread.metadata["layers"].toArray()[0].toObject();
    CHECK(layer["future-layer"] == extension);
    CHECK(layer["raster"].toObject()["future-storage"] == extension);
}

void requiredFieldsTypesAndFeatures()
{
    Fixture f;
    for (const auto& key : { "format", "version", "canvas", "layers" }) {
        auto manifest = baseManifest();
        manifest.remove(key);
        f.reject(QString("missing %1").arg(key), manifest);
    }
    for (const auto& key : { "id", "type", "name", "raster" }) {
        auto manifest = baseManifest();
        editLayer(manifest, [&](QJsonObject& layer) { layer.remove(key); });
        f.reject(QString("missing layer %1").arg(key), manifest);
    }
    for (const auto& key : { "width", "height", "path" }) {
        auto manifest = baseManifest();
        editRaster(manifest, [&](QJsonObject& raster) { raster.remove(key); });
        f.reject(QString("missing raster %1").arg(key), manifest);
    }
    const std::vector<std::pair<QString, QJsonValue>> badTop {
        { "format", "another.project" }, { "version", 2 }, { "version", 0 },
        { "version", 1.5 }, { "version", "1" }, { "required", "rgba8" },
        { "required", QJsonArray { "rgba8", "future-required-feature" } },
        { "required", QJsonArray { true } }, { "canvas", QJsonArray { } },
        { "layers", QJsonObject { } }, { "layers", QJsonArray { } },
        { "layers", QJsonArray { true } }
    };
    for (const auto& [key, value] : badTop) {
        auto manifest = baseManifest();
        manifest[key] = value;
        f.reject("bad top " + key, manifest);
    }
    const std::vector<std::pair<QString, QJsonValue>> badLayer {
        { "type", "future-kind" }, { "type", 0 }, { "name", 77 },
        { "visible", "true" }, { "opacity", "1" }, { "opacity", -0.1 },
        { "opacity", 1.1 }, { "transform", "identity" },
        { "transform", QJsonArray { 1, 0, 0, 0, 1 } },
        { "transform", QJsonArray { 1, 0, 0, 0, 1, "0" } },
        { "raster", QJsonArray { 3, 2 } }
    };
    for (const auto& [key, value] : badLayer) {
        auto manifest = baseManifest();
        editLayer(manifest, [&](QJsonObject& layer) { layer[key] = value; });
        f.reject("bad layer " + key, manifest);
    }
    const std::vector<QJsonValue> badIds { "", "0", "-1", "17.0", "17x", " 17",
        "9223372036854775808", "18446744073709551614", "18446744073709551615",
        "18446744073709551616", 17, 17.5, QJsonValue::Null };
    for (const auto& id : badIds) {
        auto manifest = baseManifest();
        editLayer(manifest, [&](QJsonObject& layer) { layer["id"] = id; });
        f.reject("bad stable layer ID", manifest);
    }
    auto duplicate = baseManifest();
    auto layers = duplicate["layers"].toArray();
    layers.append(layers[0]);
    duplicate["layers"] = layers;
    f.reject("duplicate layer ID", duplicate);
    for (const auto& [key, value] : std::vector<std::pair<QString, QJsonValue>> {
             { "colorSpace", "display-p3" }, { "colorSpace", true },
             { "pixelFormat", "rgba8-premultiplied" }, { "pixelFormat", "rgba16f" },
             { "pixelFormat", QJsonValue::Null } }) {
        auto manifest = baseManifest();
        auto canvas = manifest["canvas"].toObject();
        canvas[key] = value;
        manifest["canvas"] = canvas;
        f.reject("unsupported canvas " + key, manifest);
    }
}

void dimensionAndAllocationLimits()
{
    Fixture f;
    const std::vector<QJsonValue> invalid { 0, -1, 1.5, "3", QJsonValue::Null,
        32769, 4294967296.0, 1e30 };
    for (const auto& value : invalid) {
        for (const auto& axis : { "width", "height" }) {
            auto manifest = baseManifest();
            auto canvas = manifest["canvas"].toObject();
            canvas[axis] = value;
            manifest["canvas"] = canvas;
            f.reject("invalid canvas dimension", manifest);
            manifest = baseManifest();
            editRaster(manifest, [&](QJsonObject& raster) { raster[axis] = value; });
            f.reject("invalid raster dimension", manifest);
        }
    }
    for (const auto& value : std::vector<QJsonValue> { 0, -1, "96", QJsonValue::Null }) {
        auto manifest = baseManifest();
        auto canvas = manifest["canvas"].toObject();
        canvas["ppi"] = value;
        manifest["canvas"] = canvas;
        f.reject("invalid resolution", manifest);
    }
    auto large = baseManifest();
    editRaster(large, [](QJsonObject& raster) {
        raster["width"] = 8193;
        raster["height"] = 8192;
    });
    f.reject("raster exceeds 64 Mi pixels", large);
    large = baseManifest();
    QJsonArray layers;
    for (int i = 0; i < 1025; ++i) {
        auto layer = large["layers"].toArray()[0].toObject();
        layer["id"] = QString::number(i + 100);
        auto raster = layer["raster"].toObject();
        raster["path"] = QString("rasters/%1.rgba").arg(i + 100);
        layer["raster"] = raster;
        layers.append(layer);
    }
    large["layers"] = layers;
    f.reject("layer count exceeds 1024", large);
    layers = { };
    for (int i = 0; i < 5; ++i) {
        auto layer = baseManifest()["layers"].toArray()[0].toObject();
        layer["id"] = QString::number(i + 100);
        layer["raster"] = QJsonObject { { "width", 8192 }, { "height", 8192 },
            { "path", QString("rasters/%1.rgba").arg(i + 100) } };
        layers.append(layer);
    }
    large = baseManifest();
    large["layers"] = layers;
    std::vector<ZipEntry> budgetEntries {
        { "manifest.json", QJsonDocument(large).toJson(QJsonDocument::Compact) }
    };
    for (int i = 0; i < 5; ++i)
        budgetEntries.push_back({ QString("rasters/%1.rgba").arg(i + 100).toUtf8(), referencePixels() });
    auto budgetArchive = makeZip(budgetEntries);
    // The central directory declares five individually legal 256 MiB rasters.
    // An explicit constrained-memory load must reject BEFORE decoding/allocating.
    // There is no fixed 1 GiB file or aggregate-pixel cap anymore.
    qsizetype central = 0;
    int changed = 0;
    while ((central = budgetArchive.indexOf(QByteArray("PK\1\2", 4), central)) >= 0) {
        if (budgetArchive.mid(central + 46, 8) == "rasters/") {
            const quint32 declared = 256U * 1024U * 1024U;
            for (int byte = 0; byte < 4; ++byte)
                budgetArchive[central + 24 + byte] = char((declared >> (8U * unsigned(byte))) & 255U);
            ++changed;
        }
        central += 4;
    }
    CHECK(changed == 5);
    const auto budgetPath = f.store(budgetArchive);
    const auto constrained = u::loadProject(budgetPath, {}, { 1024ULL * 1024 * 1024 });
    CHECK(!constrained && constrained.error.contains("available load allowance"));
    // With sufficient declared headroom it reaches payload verification, where
    // this intentionally truncated fixture fails, rather than a 1 GiB veto.
    const auto admitted = u::loadProject(budgetPath, {}, { 8ULL * 1024 * 1024 * 1024 });
    CHECK(!admitted && !admitted.error.isEmpty());
    CHECK(!admitted.error.contains("allowance") && !admitted.error.contains("budget"));
    large = baseManifest();
    large["too-large"] = QString(8 * 1024 * 1024, QChar('x'));
    f.reject("manifest exceeds 8 MiB", large);
}

void memberPathsAndPayloadValidation()
{
    Fixture f;
    for (const auto& path : { "../outside.rgba", "/tmp/outside.rgba", "rasters/../17.rgba",
             "rasters\\17.rgba", "rasters//17.rgba", "rasters/./17.rgba",
             "C:/outside.rgba", "rasters/18.rgba", "", "manifest.json" }) {
        auto manifest = baseManifest();
        editRaster(manifest, [&](QJsonObject& raster) { raster["path"] = path; });
        auto entries = entriesFor(manifest);
        entries[1].name = path;
        f.reject("invalid or aliased path " + QString::fromUtf8(path), makeZip(entries));
    }
    auto manifest = baseManifest();
    editRaster(manifest, [](QJsonObject& raster) {
        raster["path"] = QString("rasters/17.rgba") + QChar::Null + "hidden";
    });
    f.reject("embedded NUL in manifest path", manifest);
    f.reject("missing manifest", makeZip({ entriesFor()[1] }));
    f.reject("missing raster", makeZip({ entriesFor()[0] }));
    auto entries = entriesFor();
    entries.push_back(entries[0]);
    f.reject("duplicate manifest member", makeZip(entries));
    entries = entriesFor();
    entries.push_back(entries[1]);
    f.reject("duplicate raster member", makeZip(entries));
    entries = entriesFor();
    entries.push_back({ "unexpected.bin", "unknown payload" });
    f.reject("unexpected member", makeZip(entries));
    entries = entriesFor();
    entries[1].bytes.chop(1);
    f.reject("short RGBA payload", makeZip(entries));
    entries = entriesFor();
    entries[1].bytes.append('x');
    f.reject("oversized RGBA payload", makeZip(entries));
    entries = entriesFor();
    entries[1].attributes = 0120777U << 16U;
    f.reject("symlink member", makeZip(entries));
    entries = entriesFor();
    entries[0].bytes = "{not valid json";
    f.reject("invalid JSON", makeZip(entries));
    entries[0].bytes = "[]";
    f.reject("JSON root is not object", makeZip(entries));
}

quint16 le16(const QByteArray& data, qsizetype at)
{
    if (at < 0 || at + 2 > data.size())
        throw std::runtime_error("Fixture header bounds");
    return quint16(quint8(data[at])) | quint16(quint16(quint8(data[at + 1])) << 8U);
}
qsizetype localHeader(const QByteArray& data, const QByteArray& name)
{
    qsizetype from = 0;
    while (true) {
        const auto found = data.indexOf(QByteArray("PK\3\4", 4), from);
        if (found < 0)
            throw std::runtime_error("ZIP fixture local header not found");
        if (data.mid(found + 30, le16(data, found + 26)) == name)
            return found;
        from = found + 4;
    }
}
qsizetype centralHeader(const QByteArray& data, const QByteArray& name)
{
    qsizetype from = 0;
    while (true) {
        const auto found = data.indexOf(QByteArray("PK\1\2", 4), from);
        if (found < 0)
            throw std::runtime_error("ZIP fixture central header not found");
        if (data.mid(found + 46, le16(data, found + 28)) == name)
            return found;
        from = found + 4;
    }
}
void corruptionTruncationAndEncryption()
{
    Fixture f;
    const auto original = makeZip(entriesFor());
    const auto header = localHeader(original, "rasters/17.rgba");
    const auto payload = header + 30 + le16(original, header + 26) + le16(original, header + 28);
    auto corrupt = original;
    corrupt[payload + 4] = char(quint8(corrupt[payload + 4]) ^ 0x55U);
    f.reject("stored raster CRC mismatch", corrupt);
    corrupt = original;
    const auto manifestHeader = localHeader(corrupt, "manifest.json");
    const auto manifestPayload = manifestHeader + 30 + le16(corrupt, manifestHeader + 26)
        + le16(corrupt, manifestHeader + 28);
    // Changing a name keeps the JSON/schema valid: only integrity validation
    // can reject this otherwise legitimate document.
    const auto name = corrupt.indexOf("Four corners", manifestPayload);
    if (name < 0)
        throw std::runtime_error("Fixture layer-name payload not found");
    corrupt[name] = 'Y';
    f.reject("manifest corruption", corrupt);
    for (const qsizetype removed : { qsizetype(1), qsizetype(8), qsizetype(22), original.size() / 2 })
        f.reject("truncated ZIP", original.left(original.size() - removed));
    f.reject("truncated raster stream", original.left(payload + 7));
    f.reject("not ZIP", QByteArray("not an archive"));
    auto compressedEntries = entriesFor();
    for (auto& entry : compressedEntries)
        entry.method = MZ_COMPRESS_METHOD_DEFLATE;
    const auto compressed = makeZip(compressedEntries);
    const auto compressedHeader = centralHeader(compressed, "rasters/17.rgba");
    const quint32 compressedSize = quint32(le16(compressed, compressedHeader + 20))
        | (quint32(le16(compressed, compressedHeader + 22)) << 16U);
    CHECK(compressedSize > 1 && compressedSize < 1024);
    for (const int delta : { -1, 1 }) {
        corrupt = compressed;
        const auto wrongSize = quint32(qint64(compressedSize) + delta);
        for (int byte = 0; byte < 4; ++byte)
            corrupt[compressedHeader + 20 + byte] = char((wrongSize >> (8U * unsigned(byte))) & 255U);
        // Payload, uncompressed size and CRC remain valid. The inflater's
        // consumed-byte count must still agree with the declared stream size.
        f.reject("Deflate compressed-size mismatch", corrupt);
    }
    corrupt = original;
    corrupt[header + 6] = char(quint8(corrupt[header + 6]) | 1U);
    const auto central = centralHeader(corrupt, "rasters/17.rgba");
    corrupt[central + 8] = char(quint8(corrupt[central + 8]) | 1U);
    f.reject("encrypted entry forbidden", corrupt);
}

void malformedText()
{
    Fixture f;
    const auto originalText = QJsonDocument::fromJson(u::encodeTextClipboard(richText())).object();
    auto manifest = baseManifest();
    editLayer(manifest, [&](QJsonObject& layer) {
        layer["type"] = "text";
        layer.remove("raster");
        layer["text"] = originalText;
    });
    auto checkText = [&](const QString& label, const QJsonValue& value) {
        auto bad = manifest;
        editLayer(bad, [&](QJsonObject& layer) { layer["text"] = value; });
        f.reject(label, makeZip({ entriesFor(bad)[0] }));
    };
    checkText("text object wrong type", "flattened");
    for (const auto& key : { "text", "default", "runs", "paragraphs" }) {
        auto bad = originalText;
        bad.remove(key);
        checkText(QString("missing text %1").arg(key), bad);
    }
    auto bad = originalText;
    auto format = bad["default"].toObject();
    format["size"] = -1;
    bad["default"] = format;
    checkText("invalid font size", bad);
    bad = originalText;
    auto runs = bad["runs"].toArray();
    auto run = runs[0].toObject();
    run["start"] = 2; // Inside UTF-8 é, not a scalar boundary.
    runs[0] = run;
    bad["runs"] = runs;
    checkText("format offset inside UTF-8 scalar", bad);
}

void loadedIdsCannotCollideWithNewLayers()
{
    Fixture f;
    const auto nextId = c::makeLayerId() + 1;
    auto manifest = baseManifest();
    const auto idText = QString::number(qulonglong(nextId));
    const auto rasterPath = QString("rasters/%1.rgba").arg(idText);
    editLayer(manifest, [&](QJsonObject& layer) { layer["id"] = idText; });
    editRaster(manifest, [&](QJsonObject& raster) { raster["path"] = rasterPath; });
    auto entries = entriesFor(manifest);
    entries[1].name = rasterPath.toUtf8();
    const auto loaded = u::loadProject(f.store(makeZip(entries)));
    CHECK(loaded);
    if (!loaded)
        return;
    const auto extra = c::Layer::raster("New layer after loading", referenceSurface());
    CHECK(!loaded.document->containsLayer(extra.id));
    CHECK(loaded.document->insertLayer(1, extra));
}

void maximumImportedIdDoesNotExhaustFactories()
{
    Fixture f;
    constexpr auto maximum = c::LayerId(std::numeric_limits<int64_t>::max());
    auto manifest = baseManifest();
    const auto maximumText = QString::number(qulonglong(maximum));
    const auto rasterPath = QString("rasters/%1.rgba").arg(maximumText);
    editLayer(manifest, [&](QJsonObject& layer) { layer["id"] = maximumText; });
    editRaster(manifest, [&](QJsonObject& raster) { raster["path"] = rasterPath; });
    auto entries = entriesFor(manifest);
    entries[1].name = rasterPath.toUtf8();
    const auto loaded = u::loadProject(f.store(makeZip(entries)));
    CHECK(loaded);
    if (!loaded)
        return;
    CHECK(loaded.document->containsLayer(maximum));
    auto raster = c::Layer::raster("New raster after maximum imported ID", referenceSurface());
    auto text = c::Layer::text("New text after maximum imported ID", richText());
    for (const auto id : { raster.id, text.id }) {
        CHECK(id > 0 && id <= maximum);
        CHECK(!loaded.document->containsLayer(id));
    }
    CHECK(raster.id != text.id);
    CHECK(loaded.document->insertLayer(1, std::move(raster)));
    CHECK(loaded.document->insertLayer(2, std::move(text)));
    const auto savedPath = f.path("maximum-ID-and-new-layers.vulkana");
    CHECK(u::saveProject(savedPath, *loaded.document, loaded.metadata));
    const auto reread = u::loadProject(savedPath);
    CHECK(reread);
    if (reread)
        sameDocument(*reread.document, *loaded.document);
}

c::Document incompressibleDocument()
{
    constexpr c::Extent2u extent { 256, 256 };
    std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
    uint32_t random = 0x76ba53d1U;
    for (auto& byte : bytes) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        byte = std::byte(random & 255U);
    }
    c::Document document({ extent, 96 });
    auto layer = c::Layer::raster("Fixed-seed incompressible RGBA",
        std::make_shared<c::ContiguousRasterSurface>(extent, std::move(bytes)));
    CHECK(document.insertLayer(0, std::move(layer)));
    return document;
}

int writeFailureChild(const QString& path)
{
    // Only this subprocess changes its soft resource limit. No special device,
    // disk-filling allocation, parent-process limit or machine setting is used.
    const auto before = readBytes(path);
    const auto directory = QFileInfo(path).dir();
    const auto filesBefore = directory.entryList(QDir::Files | QDir::Hidden);
    const auto document = incompressibleDocument();
    const auto revision = document.revision();
    struct rlimit original { };
    if (::getrlimit(RLIMIT_FSIZE, &original) != 0 || original.rlim_max < 4096
        || std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR)
        throw std::runtime_error("Cannot establish child-only write-failure limit");
    const struct rlimit limited { 4096, original.rlim_max };
    if (::setrlimit(RLIMIT_FSIZE, &limited) != 0)
        throw std::runtime_error("Cannot set child-only RLIMIT_FSIZE");
    const auto result = u::saveProject(path, document);
    CHECK(!result);
    CHECK(!result.cancelled);
    CHECK(!result.error.isEmpty());
    CHECK(readBytes(path) == before);
    CHECK(document.revision() == revision);
    CHECK(directory.entryList(QDir::Files | QDir::Hidden) == filesBefore);
    std::cout << "Child-only RLIMIT_FSIZE write failure: " << result.error.toStdString() << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

void actualWriteFailurePreservesDestination()
{
    Fixture f;
    const auto baselinePath = f.path("unlimited-baseline.vulkana");
    // First prove the exact input/codec succeeds without a fault and cannot fit
    // under the child limit; a generic codec failure cannot satisfy this test.
    const auto document = incompressibleDocument();
    CHECK(u::saveProject(baselinePath, document));
    CHECK(QFileInfo(baselinePath).size() > 4096);
    const auto destination = f.path("previous.vulkana");
    const QByteArray sentinel("Existing destination must survive a real EFBIG write failure.\n");
    writeBytes(destination, sentinel);
    const auto filesBefore = QDir(f.directory.path()).entryList(QDir::Files | QDir::Hidden);
    struct rlimit before { }, after { };
    CHECK(::getrlimit(RLIMIT_FSIZE, &before) == 0);
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(), { "--write-failure", destination });
    CHECK(child.waitForStarted(5000));
    const bool finished = child.waitForFinished(15000);
    if (!finished) {
        child.kill();
        child.waitForFinished(5000);
    }
    CHECK(finished);
    CHECK(child.exitStatus() == QProcess::NormalExit);
    CHECK(child.exitCode() == EXIT_SUCCESS);
    const auto output = child.readAllStandardOutput();
    CHECK(output.contains("Child-only RLIMIT_FSIZE write failure:"));
    if (!finished || child.exitStatus() != QProcess::NormalExit || child.exitCode() != EXIT_SUCCESS)
        std::cerr << "Write-failure child output:\n" << output.constData()
                  << child.readAllStandardError().constData();
    CHECK(readBytes(destination) == sentinel);
    CHECK(QDir(f.directory.path()).entryList(QDir::Files | QDir::Hidden) == filesBefore);
    CHECK(::getrlimit(RLIMIT_FSIZE, &after) == 0);
    CHECK(before.rlim_cur == after.rlim_cur && before.rlim_max == after.rlim_max);
}

void cancellationAndFailurePreserveDestination()
{
    Fixture f;
    const auto path = f.path("previous.vulkana");
    const auto document = sampleDocument();
    CHECK(u::saveProject(path, document));
    const auto original = readBytes(path);
    const auto originalFiles = QDir(f.directory.path()).entryList(QDir::Files | QDir::Hidden);
    const auto revision = document.revision();
    int calls = 0;
    auto result = u::saveProject(path, document, { }, [&](quint64, quint64) {
        ++calls;
        return false;
    });
    CHECK(!result);
    CHECK(result.cancelled);
    CHECK(calls > 0);
    CHECK(readBytes(path) == original);
    calls = 0;
    result = u::saveProject(path, document, { }, [&](quint64 completed, quint64) {
        ++calls;
        return completed == 0;
    });
    CHECK(!result);
    CHECK(result.cancelled);
    CHECK(calls > 0);
    CHECK(readBytes(path) == original);
    calls = 0;
    result = u::saveProject(path, document, { }, [&](quint64 completed, quint64 total) {
        ++calls;
        return completed <= total / 2; // Cancel during post-write CRC verification.
    });
    CHECK(!result);
    CHECK(result.cancelled);
    CHECK(calls > 0);
    CHECK(readBytes(path) == original);
    CHECK(document.revision() == revision);
    CHECK(QDir(f.directory.path()).entryList(QDir::Files | QDir::Hidden) == originalFiles);
    calls = 0;
    const auto loaded = u::loadProject(path, [&](quint64, quint64) {
        ++calls;
        return false;
    });
    CHECK(!loaded);
    CHECK(!loaded.document);
    CHECK(loaded.cancelled);
    CHECK(calls > 0);
    CHECK(readBytes(path) == original);
    const auto duringLoad = u::loadProject(path, [](quint64 completed, quint64) { return completed == 0; });
    CHECK(!duringLoad);
    CHECK(duringLoad.cancelled);
    CHECK(!duringLoad.document);
    CHECK(readBytes(path) == original);
    auto different = sampleDocument();
    CHECK(different.renameLayer(different.layers()[0].id, "Must never replace the original"));
    const auto nulPath = path + QChar::Null + "not-the-requested-file";
    const auto nulResult = u::saveProject(nulPath, different);
    CHECK(!nulResult);
    CHECK(!nulResult.cancelled);
    CHECK(!nulResult.error.isEmpty());
    CHECK(readBytes(path) == original);
    const auto callbackFailure = u::saveProject(path, different, { }, [](quint64, quint64) -> bool {
        throw std::runtime_error("Injected progress callback failure");
    });
    CHECK(!callbackFailure);
    CHECK(!callbackFailure.cancelled);
    CHECK(!callbackFailure.error.isEmpty());
    CHECK(readBytes(path) == original);
    const auto failed = u::saveProject(f.directory.path(), document);
    CHECK(!failed);
    CHECK(!failed.cancelled);
    CHECK(!failed.error.isEmpty());
    CHECK(readBytes(path) == original);
    CHECK(!u::loadProject(f.path("does-not-exist.vulkana")));
}
}

void rememberedSelectionRoundTrip()
{
    QTemporaryDir directory;
    c::Document doc({{32,24}});
    doc.insertLayer(0,c::Layer::raster("Source",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{32,24})));
    const auto mask=c::SelectionMask::rectangle({32,24},{3,4,10,8},127);
    doc.setLastSelection(mask);
    auto path=directory.filePath("remembered.vulkana");
    CHECK(u::saveProject(path,doc));
    auto read=u::loadProject(path); CHECK(read);
    if (!read) return;
    CHECK(!read.document->selection());
    CHECK(read.document->lastSelection() && read.document->lastSelection()->equivalent(*mask));
    // Removing memory must not resurrect opaque old manifest data on save.
    read.document->setLastSelection({});
    CHECK(u::saveProject(path,*read.document,read.metadata));
    read=u::loadProject(path); CHECK(read && !read.document->lastSelection());
    doc.setSelection(c::SelectionMask::filled({32,24},0));
    CHECK(u::saveProject(path,doc)); read=u::loadProject(path);
    CHECK(read && read.document->lastSelection() && read.document->lastSelection()->bounds().empty());
}

int main(int argc, char** argv)
{
    qputenv("TZ", "UTC");
    ::tzset(); // ZIP DOS timestamps must not depend on the test host's timezone.
    QGuiApplication application(argc, argv);
    try {
        if (application.arguments().size() == 3 && application.arguments()[1] == "--write-failure")
            return writeFailureChild(application.arguments()[2]);
        frozenFixtureDefaultsAndZipVariants();
        rememberedSelectionRoundTrip();
        exactRasterAndRichTextRoundTrip();
        largeStreamingSave();
        unknownMetadataSurvivesKnownFieldChanges();
        requiredFieldsTypesAndFeatures();
        dimensionAndAllocationLimits();
        memberPathsAndPayloadValidation();
        corruptionTruncationAndEncryption();
        malformedText();
        loadedIdsCannotCollideWithNewLayers();
        maximumImportedIdDoesNotExhaustFactories();
        cancellationAndFailurePreserveDestination();
        actualWriteFailurePreservesDestination();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected project-test exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << "Project file checks: " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
