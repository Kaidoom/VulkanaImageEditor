#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/Shape.hpp"
#include "imageeditor/core/ShapeCommands.hpp"
#include "imageeditor/core/ShapeResize.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool result, const char* expression, int line)
{
    if (!result) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

QByteArray read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read shape project test fixture");
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write shape project test fixture");
}

// A minimal independent Store ZIP fixture transport. Corrupt schema inputs do
// not go through the production writer's validation and cannot validate their
// own codec by construction. Production CRC, paths and ZIP limits stay enabled.
void append16(QByteArray& bytes, uint16_t value)
{
    for (unsigned i = 0; i < 2; ++i)
        bytes.append(char((value >> (8U * i)) & 255U));
}
void append32(QByteArray& bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes.append(char((value >> (8U * i)) & 255U));
}
uint32_t crc32(const QByteArray& bytes)
{
    uint32_t value = 0xffffffffU;
    for (const char byte : bytes) {
        value ^= uint8_t(byte);
        for (unsigned i = 0; i < 8; ++i)
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}
QByteArray archive(const QJsonObject& manifest, bool withRaster = false)
{
    std::vector<std::pair<QByteArray, QByteArray>> entries { { "manifest.json",
        QJsonDocument(manifest).toJson(QJsonDocument::Compact) } };
    if (withRaster)
        entries.emplace_back("rasters/19.rgba", QByteArray::fromHex("ff008040"));
    QByteArray bytes, directory;
    for (const auto& [name, data] : entries) {
        const auto offset = uint32_t(bytes.size());
        const auto crc = crc32(data);
        append32(bytes, 0x04034b50);
        append16(bytes, 20); // extract version
        append16(bytes, 0); // flags
        append16(bytes, 0); // Store
        append16(bytes, 0);
        append16(bytes, 0); // DOS timestamp
        append32(bytes, crc);
        append32(bytes, uint32_t(data.size()));
        append32(bytes, uint32_t(data.size()));
        append16(bytes, uint16_t(name.size()));
        append16(bytes, 0);
        bytes += name;
        bytes += data;
        append32(directory, 0x02014b50);
        append16(directory, 20);
        append16(directory, 20);
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append32(directory, crc);
        append32(directory, uint32_t(data.size()));
        append32(directory, uint32_t(data.size()));
        append16(directory, uint16_t(name.size()));
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append32(directory, 0);
        append32(directory, offset);
        directory += name;
    }
    const auto directoryOffset = uint32_t(bytes.size());
    bytes += directory;
    append32(bytes, 0x06054b50);
    append16(bytes, 0);
    append16(bytes, 0);
    append16(bytes, uint16_t(entries.size()));
    append16(bytes, uint16_t(entries.size()));
    append32(bytes, uint32_t(directory.size()));
    append32(bytes, directoryOffset);
    append16(bytes, 0);
    return bytes;
}
QJsonObject shapeDescriptor()
{
    return { { "version", 1 }, { "kind", "rounded-rectangle" }, { "size", QJsonArray { 80.25, 60.75 } },
        { "points", QJsonArray { } }, { "cornerRadius", 12.5 }, { "fillEnabled", true },
        { "fillRgba", QJsonArray { 10, 30, 70, 123 } }, { "strokeEnabled", true },
        { "strokeRgba", QJsonArray { 255, 100, 0, 41 } }, { "strokeWidth", 5.25 },
        { "strokePlacement", "center" }, { "cap", "round" }, { "join", "round" } };
}
QJsonObject manifest()
{
    return { { "format", "org.vulkana.project" }, { "version", 1 }, { "required", QJsonArray { "shape-v1" } },
        { "canvas", QJsonObject { { "width", 256 }, { "height", 192 } } },
        { "layers",
            QJsonArray { QJsonObject { { "id", "19" }, { "type", "shape" }, { "name", "Editable shape" },
                { "shape", shapeDescriptor() } } } } };
}
void editShape(QJsonObject& document, const std::function<void(QJsonObject&)>& edit)
{
    auto layers = document["layers"].toArray();
    auto layer = layers[0].toObject();
    auto shape = layer["shape"].toObject();
    edit(shape);
    layer["shape"] = shape;
    layers[0] = layer;
    document["layers"] = layers;
}
struct Fixture {
    QTemporaryDir directory;
    unsigned sequence { 0 };
    Fixture()
    {
        if (!directory.isValid())
            throw std::runtime_error("Cannot create shape project test directory");
    }
    QString path(const QString& name) const { return directory.filePath(name); }
    u::ProjectLoadResult load(const QJsonObject& document, bool raster = false)
    {
        const auto file = path(QStringLiteral("independent-%1.vulkana").arg(++sequence));
        write(file, archive(document, raster));
        return u::loadProject(file);
    }
    void rejects(const QJsonObject& document)
    {
        const auto result = load(document);
        CHECK(!result);
        CHECK(!result.document);
        CHECK(!result.error.isEmpty());
    }
};
void roundTrip(Fixture& fixture)
{
    c::Document document({ { 512, 384 }, 144 });
    const std::array kinds { c::ShapeKind::Rectangle, c::ShapeKind::RoundedRectangle, c::ShapeKind::Ellipse,
        c::ShapeKind::Triangle, c::ShapeKind::Line, c::ShapeKind::Polygon };
    for (const auto kind : kinds) {
        for (unsigned style = 0; style != 4; ++style) {
            c::ShapeLayer shape;
            shape.kind = kind;
            shape.size = { 85.125, 40.875 };
            shape.cornerRadius = 72.25; // Retained, not permanently render-clamped.
            shape.strokeWidth = style == 0 ? 0 : style == 1 ? 0.125 : 47.25;
            shape.fillEnabled = (style & 1U) != 0;
            shape.strokeEnabled = (style & 2U) != 0;
            shape.fillColor = { 20, 90, 180, 67 };
            shape.strokeColor = { 130, 45, 7, 0 }; // Hidden RGB is still authoritative.
            if (kind == c::ShapeKind::Line)
                shape.points = { { 2.25, 30.75 }, { 80.25, 5.125 } };
            else if (kind == c::ShapeKind::Polygon)
                shape.points = { { 0, 0 }, { 85.125, 40.875 }, { 0, 40.875 }, { 85.125, 0 } };
            auto layer = c::Layer::shape("Shape " + std::to_string(document.layers().size()), shape);
            layer.localToDocument = { -.625, .375, -123.625, .25, 1.75, 456.875 };
            layer.opacity = .375F;
            layer.visible = style != 0;
            CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
        }
    }
    auto path = fixture.path("all-shapes.vulkana");
    const auto saved = u::saveProject(path, document);
    CHECK(saved);
    if (!saved)
        std::cerr << saved.error.toStdString() << '\n';
    auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (!loaded) {
        std::cerr << loaded.error.toStdString() << '\n';
        return;
    }
    CHECK(loaded.metadata["version"].toInt() == 1);
    CHECK(loaded.metadata["required"].toArray().contains("shape-v1"));
    CHECK(loaded.document->canvas() == document.canvas());
    CHECK(loaded.document->layers().size() == document.layers().size());
    CHECK(!loaded.document->isModified());
    for (size_t i = 0; i < document.layers().size(); ++i) {
        const auto& a = document.layers()[i];
        const auto& b = loaded.document->layers()[i];
        CHECK(a.id == b.id && a.name == b.name && a.opacity == b.opacity && a.visible == b.visible);
        CHECK(a.localToDocument == b.localToDocument);
        CHECK(std::get<c::ShapeLayer>(a.payload) == std::get<c::ShapeLayer>(b.payload));
        CHECK(!b.renderCache); // Published through the same cache path as text.
    }
    // Render caches never enter the archive and are not prerequisites for save.
    auto cache = std::make_shared<c::LayerRenderCache>();
    cache->surface
        = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 20, 10 }, c::Rgba8 { 0, 255, 0, 255 });
    document.layer(document.layers()[0].id)->renderCache = cache;
    const auto second = fixture.path("all-shapes-with-cache.vulkana");
    CHECK(u::saveProject(second, document));
    const auto reloaded = u::loadProject(second);
    CHECK(reloaded && reloaded.metadata == loaded.metadata);
}

void resizedGeometryRoundTrip(Fixture& fixture)
{
    c::Document document({ { 1600, 1200 }, 144 });
    for (const auto kind : { c::ShapeKind::Rectangle, c::ShapeKind::RoundedRectangle,
             c::ShapeKind::Ellipse, c::ShapeKind::Triangle, c::ShapeKind::Line, c::ShapeKind::Polygon }) {
        // These are the authoritative states after intrinsic resize, not a
        // normalized shape plus an accumulated matrix scale. Each intentionally
        // retains a different pre-existing whole-layer transform baseline.
        for (const c::Extent2d size : { c::Extent2d { 100, 300 }, { 300, 100 }, { 7.25, 24.75 } }) {
            c::ShapeLayer shape;
            shape.kind = kind;
            shape.size = size;
            shape.fillEnabled = false;
            shape.strokeEnabled = true;
            shape.strokeWidth = 4;
            shape.cornerRadius = 13.25;
            shape.strokeColor = { 89, 147, 223, 193 };
            if (kind == c::ShapeKind::Line)
                shape.points = { { 0, size.height }, { size.width, 0 } };
            else if (kind == c::ShapeKind::Polygon)
                shape.points = { { 0, 0 }, { size.width, 0 }, { size.width, size.height },
                    { size.width / 2, size.height / 3 }, { 0, size.height } };
            auto layer = c::Layer::shape("Resized geometry", shape);
            layer.localToDocument = size.width == 100
                ? c::AffineTransform { 0, -1, 560.25, 1, 0, 101.75 }
                : size.width == 300 ? c::AffineTransform { -1, 0, 625.5, 0, 1, -10.25 }
                                    : c::AffineTransform { -1.5, .3, 295.25, .4, .6, 715.125 };
            CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
        }
    }
    const auto path = fixture.path("resized-shape-geometry.vulkana");
    CHECK(u::saveProject(path, document));
    auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (!loaded)
        return;
    CHECK(loaded.document->layers().size() == document.layers().size());
    for (const auto& source : document.layers()) {
        auto* restored = loaded.document->layer(source.id);
        CHECK(restored && std::holds_alternative<c::ShapeLayer>(restored->payload));
        if (!restored)
            continue;
        const auto original = std::get<c::ShapeLayer>(source.payload);
        CHECK(std::get<c::ShapeLayer>(restored->payload) == original);
        CHECK(restored->localToDocument == source.localToDocument);
        CHECK(!restored->renderCache);

        // Loaded state remains editable, including unclamped requested radius,
        // fractional vertices and width, without relying on a saved bitmap.
        auto edited = original;
        edited.strokeWidth = 6.5;
        edited.cornerRadius = 27.125;
        c::History history;
        CHECK(history.execute(*loaded.document,
            std::make_unique<c::SetShapeCommand>(source.id, original, edited)));
        CHECK(std::get<c::ShapeLayer>(restored->payload) == edited);
        CHECK(restored->localToDocument == source.localToDocument);
        CHECK(history.undo(*loaded.document));
        CHECK(std::get<c::ShapeLayer>(restored->payload) == original);
        CHECK(history.redo(*loaded.document));
        CHECK(std::get<c::ShapeLayer>(restored->payload) == edited);
        CHECK(history.undo(*loaded.document));
    }
    const auto secondPath = fixture.path("resized-shape-geometry-reopened.vulkana");
    CHECK(u::saveProject(secondPath, *loaded.document, loaded.metadata));
    const auto twice = u::loadProject(secondPath);
    CHECK(twice);
    if (twice) {
        for (const auto& source : document.layers()) {
            const auto* restored = twice.document->layer(source.id);
            CHECK(restored && std::get<c::ShapeLayer>(restored->payload) == std::get<c::ShapeLayer>(source.payload)
                && restored->localToDocument == source.localToDocument);
        }
    }
}

void resizeGestureHistoryRoundTrip(Fixture& fixture)
{
    c::Document document({ { 1600, 1200 }, 96 });
    c::History history;
    std::vector<c::LayerId> ids;
    std::vector<c::ShapeGeometryState> starts, completed;
    for (const auto kind : { c::ShapeKind::RoundedRectangle, c::ShapeKind::Line, c::ShapeKind::Polygon }) {
        c::ShapeLayer shape;
        shape.kind = kind;
        shape.size = { 100.125, kind == c::ShapeKind::Line ? 0 : 80.75 };
        shape.cornerRadius = 13.25;
        shape.strokeEnabled = true;
        shape.strokeWidth = 4;
        shape.fillEnabled = false;
        if (kind == c::ShapeKind::Line)
            shape.points = { { 0, 0 }, { 100.125, 0 } };
        else if (kind == c::ShapeKind::Polygon)
            shape.points = { { 0, 0 }, { 100.125, 0 }, { 40, 20.5 }, { 100.125, 80.75 }, { 0, 80.75 } };
        auto layer = c::Layer::shape("Gesture-resized editable shape", shape);
        layer.localToDocument = { -.8, .25, 521.5, .6, 1.2, 330.25 };
        const auto id = layer.id;
        const c::ShapeGeometryState before { shape, layer.localToDocument };
        CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
        c::ShapeResizeGesture gesture(before.shape, before.transform,
            c::TransformHandle::TopLeft, before.transform.map({ 0, 0 }));
        auto latest = before;
        for (const auto point : { c::Vec2d { -5, -10 }, { -15, -30 }, { -35, -100 } }) {
            const auto next = gesture.resolve(before.transform.map(point), {});
            CHECK(next);
            if (!next)
                continue;
            CHECK(document.setLayerShapeGeometry(id, next->shape, next->transform));
            latest = *next;
        }
        std::unique_ptr<c::Command> command = std::make_unique<c::ResizeShapeCommand>(id, before, latest);
        CHECK(history.adoptApplied(document, command));
        CHECK(latest.shape.strokeWidth == 4 && latest.shape.cornerRadius == 13.25);
        ids.push_back(id);
        starts.push_back(before);
        completed.push_back(latest);
    }
    CHECK(history.undoDepth() == ids.size());
    for (std::size_t i = ids.size(); i-- > 0;) {
        CHECK(history.undo(document));
        const auto* layer = document.layer(ids[i]);
        CHECK(layer && std::get<c::ShapeLayer>(layer->payload) == starts[i].shape
            && layer->localToDocument == starts[i].transform);
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(history.redo(document));
        const auto* layer = document.layer(ids[i]);
        CHECK(layer && std::get<c::ShapeLayer>(layer->payload) == completed[i].shape
            && layer->localToDocument == completed[i].transform);
    }
    const auto path = fixture.path("resize-gesture-history.vulkana");
    CHECK(u::saveProject(path, document));
    const auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (!loaded)
        return;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto* layer = loaded.document->layer(ids[i]);
        CHECK(layer && std::get<c::ShapeLayer>(layer->payload) == completed[i].shape
            && layer->localToDocument == completed[i].transform && !layer->renderCache);
    }
}

void independentAndDegenerate(Fixture& fixture)
{
    auto result = fixture.load(manifest());
    CHECK(result);
    if (!result) {
        std::cerr << result.error.toStdString() << '\n';
        return;
    }
    const auto& shape = std::get<c::ShapeLayer>(result.document->layers()[0].payload);
    CHECK(shape.kind == c::ShapeKind::RoundedRectangle && shape.cornerRadius == 12.5);
    CHECK(shape.fillColor == c::Rgba8 { 10, 30, 70, 123 });
    CHECK(shape.strokeColor == c::Rgba8 { 255, 100, 0, 41 });
    for (const auto kind : { "rectangle", "ellipse", "line", "polygon" }) {
        auto data = manifest();
        editShape(data, [&](auto& descriptor) {
            descriptor["kind"] = kind;
            descriptor["size"] = QJsonArray { 0, 0 };
            descriptor["strokeWidth"] = 0;
            if (QLatin1String(kind) == QLatin1String("line"))
                descriptor["points"] = QJsonArray { QJsonArray { 0, 0 }, QJsonArray { 0, 0 } };
            if (QLatin1String(kind) == QLatin1String("polygon"))
                descriptor["points"]
                    = QJsonArray { QJsonArray { 0, 0 }, QJsonArray { 0, 0 }, QJsonArray { 0, 0 } };
        });
        CHECK(fixture.load(data)); // Degenerate but editable is not a corrupt file.
    }
}
void malformed(Fixture& fixture)
{
    for (const auto* key : { "version", "kind", "size", "points", "cornerRadius", "fillEnabled", "fillRgba",
             "strokeEnabled", "strokeRgba", "strokeWidth", "strokePlacement", "cap", "join" }) {
        auto data = manifest();
        editShape(data, [&](auto& shape) { shape.remove(key); });
        fixture.rejects(data);
    }
    const std::vector<std::function<void(QJsonObject&)>> changes { [](auto& s) { s["version"] = 2; },
        [](auto& s) { s["version"] = "1"; }, [](auto& s) { s["kind"] = "bezier"; },
        [](auto& s) { s["size"] = QJsonArray { 1 }; }, [](auto& s) { s["size"] = QJsonArray { -1, 20 }; },
        [](auto& s) { s["size"] = QJsonArray { 1e7, 20 }; },
        [](auto& s) { s["points"] = QJsonArray { QJsonArray { 0, 0 } }; },
        [](auto& s) { s["fillEnabled"] = 1; }, [](auto& s) { s["strokeEnabled"] = "false"; },
        [](auto& s) { s["fillRgba"] = QJsonArray { 255, 0, 0 }; },
        [](auto& s) { s["fillRgba"] = QJsonArray { 255, 0, 0, -1 }; },
        [](auto& s) { s["strokeRgba"] = QJsonArray { 0, 0, 0, 127.5 }; },
        [](auto& s) { s["strokeRgba"] = QJsonArray { 0, 0, 0, 256 }; },
        [](auto& s) { s["strokeWidth"] = -1; }, [](auto& s) { s["strokeWidth"] = 1e6; },
        [](auto& s) { s["cornerRadius"] = QJsonValue(QJsonValue::Null); },
        [](auto& s) { s["strokePlacement"] = "inside"; }, [](auto& s) { s["cap"] = "square"; },
        [](auto& s) { s["join"] = "miter"; },
        [](auto& s) {
            s["kind"] = "line";
            s["points"] = QJsonArray { QJsonArray { 0, 0 } };
        },
        [](auto& s) {
            s["kind"] = "line";
            s["points"] = QJsonArray { QJsonArray { 0, 0 }, QJsonArray { 90, 0 } };
        },
        [](auto& s) {
            s["kind"] = "polygon";
            s["points"] = QJsonArray { QJsonArray { 0, 0 }, QJsonArray { 10, 20 } };
        },
        [](auto& s) {
            s["kind"] = "polygon";
            QJsonArray points;
            for (size_t i = 0; i <= c::maximumShapePoints; ++i)
                points.append(QJsonArray { 0, 0 });
            s["points"] = points;
        } };
    for (const auto& change : changes) {
        auto data = manifest();
        editShape(data, change);
        fixture.rejects(data);
    }
    auto data = manifest();
    data["required"] = QJsonArray { };
    fixture.rejects(data);
    data["required"] = QJsonArray { "shape-v2" };
    fixture.rejects(data);
    data = manifest();
    data["version"] = 2;
    fixture.rejects(data);
}
void compatibilityAndOpaqueMetadata(Fixture& fixture)
{
    auto data = manifest();
    data["producerNote"] = "untouched";
    editShape(data, [](auto& shape) { shape["futureOptional"] = QJsonObject { { "note", "preserve me" } }; });
    auto result = fixture.load(data);
    CHECK(result);
    if (!result)
        return;
    const auto file = fixture.path("metadata.vulkana");
    CHECK(u::saveProject(file, *result.document, result.metadata));
    const auto restored = u::loadProject(file);
    CHECK(restored);
    if (!restored)
        return;
    CHECK(restored.metadata["producerNote"] == "untouched");
    CHECK(restored.metadata["layers"]
              .toArray()[0]
              .toObject()["shape"]
              .toObject()["futureOptional"]
              .toObject()["note"]
        == "preserve me");

    auto legacy = manifest();
    legacy.remove("required");
    legacy["layers"]
        = QJsonArray { QJsonObject { { "id", "19" }, { "name", "Legacy raster" }, { "type", "raster" },
            { "raster", QJsonObject { { "width", 1 }, { "height", 1 }, { "path", "rasters/19.rgba" } } } } };
    const auto old = fixture.load(legacy, true);
    CHECK(old);
    if (!old)
        return;
    const auto oldSave = fixture.path("legacy-saved.vulkana");
    CHECK(u::saveProject(oldSave, *old.document, old.metadata));
    const auto oldReload = u::loadProject(oldSave);
    CHECK(oldReload && !oldReload.metadata["required"].toArray().contains("shape-v1"));
}
void failedSaveIsAtomic(Fixture& fixture)
{
    c::Document document({ { 200, 100 }, 96 });
    auto layer = c::Layer::shape("Atomic", { });
    const auto id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    const auto file = fixture.path("atomic.vulkana");
    CHECK(u::saveProject(file, document));
    const auto before = read(file);
    std::get<c::ShapeLayer>(document.layer(id)->payload).strokeWidth
        = std::numeric_limits<double>::infinity();
    CHECK(!u::saveProject(file, document));
    CHECK(read(file) == before);
    std::get<c::ShapeLayer>(document.layer(id)->payload).strokeWidth = 2;
    const auto cancelled = u::saveProject(file, document, { }, [](quint64, quint64) { return false; });
    CHECK(cancelled.cancelled && read(file) == before);
}

void optionalReviewProject()
{
    const auto path = qEnvironmentVariable("IMAGEEDITOR_SHAPE_REVIEW_PROJECT");
    if (path.isEmpty())
        return;
    c::Document document({ { 1200, 800 }, 96 });
    auto add = [&](c::Layer layer) {
        if (!document.insertLayer(document.layers().size(), std::move(layer)))
            throw std::runtime_error("Cannot construct editable shape review layer");
    };
    add(c::Layer::raster("Review background", std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u { 1200, 800 }, c::Rgba8 { 25, 29, 39, 255 })));
    const auto label = [&](std::string name, std::string text, double x, double y,
                           double size, c::Rgba8 color) {
        c::TextLayer data;
        data.utf8 = std::move(text);
        data.defaultStyle.sizePixels = size;
        data.defaultStyle.color = color;
        auto layer = c::Layer::text(std::move(name), std::move(data));
        layer.localToDocument.m02 = x;
        layer.localToDocument.m12 = y;
        add(std::move(layer));
    };
    label("Review title", "Editable shapes", 64, 42, 32, { 238, 242, 253, 255 });
    label("Review introduction", "Six shape modes. Independent styles. Original geometry preserved.",
        66, 91, 16, { 156, 168, 191, 255 });
    const std::array<const char*, 6> titles {
        "01  Rectangle", "02  Rounded rectangle", "03  Ellipse / circle",
        "04  Triangle", "05  Line", "06  Polygon"
    };
    const std::array<const char*, 6> descriptions {
        "Separate fill + stroke alpha", "Hollow / editable 28 px radius", "Smooth large and tiny contours",
        "Rotated and horizontally flipped", "Centered stroke / round end caps", "Concave editable vertices"
    };
    const std::array<double, 3> columns { 66, 444, 822 };
    for (std::size_t i = 0; i < titles.size(); ++i) {
        const double y = i < 3 ? 160 : 456;
        label(std::string("Section ") + titles[i], titles[i], columns[i % 3], y, 18,
            { 225, 231, 246, 255 });
        label(std::string("Note ") + titles[i], descriptions[i], columns[i % 3], y + 31, 13,
            { 147, 161, 184, 255 });
    }
    label("Review footer", "Choose Shapes (U) to edit. Shift creates a separate shape. Move and Ctrl+T stay non-destructive.",
        66, 746, 14, { 156, 168, 191, 255 });

    const auto placed = [&](std::string name, c::ShapeLayer shape, double x, double y,
                            double angle = 0, bool flipped = false) {
        auto layer = c::Layer::shape(std::move(name), shape);
        const double radians = angle * std::numbers::pi / 180;
        const double cosine = std::cos(radians), sine = std::sin(radians);
        const double sign = flipped ? -1 : 1;
        layer.localToDocument = { cosine * sign, -sine, 0, sine * sign, cosine, 0 };
        const c::Vec2d center { shape.size.width / 2, shape.size.height / 2 };
        const auto mapped = layer.localToDocument.map(center);
        layer.localToDocument.m02 = x + center.x - mapped.x;
        layer.localToDocument.m12 = y + center.y - mapped.y;
        add(std::move(layer));
    };
    c::ShapeLayer shape;
    shape.size = { 230, 134 };
    shape.fillColor = { 104, 155, 249, 205 };
    shape.strokeEnabled = true;
    shape.strokeColor = { 185, 211, 255, 180 };
    shape.strokeWidth = 5;
    placed("Rectangle · translucent fill and stroke", shape, 88, 232, -6);

    shape.kind = c::ShapeKind::RoundedRectangle;
    shape.size = { 245, 128 };
    shape.cornerRadius = 28;
    shape.fillEnabled = false;
    shape.strokeColor = { 82, 211, 174, 235 };
    shape.strokeWidth = 8;
    placed("Rounded rectangle · hollow · radius 28", shape, 462, 237);

    shape.kind = c::ShapeKind::Ellipse;
    shape.size = { 215, 122 };
    shape.fillEnabled = true;
    shape.fillColor = { 198, 151, 250, 205 };
    shape.strokeColor = { 224, 200, 255, 245 };
    shape.strokeWidth = 2;
    placed("Ellipse · subpixel contour", shape, 844.25, 236.5, 8);
    shape.size = { 18.5, 18.5 };
    shape.strokeWidth = .65;
    placed("Tiny circle · 18.5 px · 0.65 px stroke", shape, 1095.25, 284.75);

    shape.kind = c::ShapeKind::Triangle;
    shape.size = { 206, 158 };
    shape.fillColor = { 249, 180, 78, 228 };
    shape.strokeColor = { 255, 222, 170, 245 };
    shape.strokeWidth = 4;
    placed("Triangle · rotated and flipped", shape, 100, 539, 7, true);

    shape.kind = c::ShapeKind::Line;
    shape.size = { 243, 147 };
    shape.points = { { 9, 124 }, { 232, 19 } };
    shape.fillEnabled = false;
    shape.strokeColor = { 244, 139, 155, 228 };
    shape.strokeWidth = 19;
    placed("Line · 19 px round stroke", shape, 455, 539);

    shape.kind = c::ShapeKind::Polygon;
    shape.size = { 236, 151 };
    shape.points = { { 0, 6 }, { 236, 6 }, { 136, 70 }, { 229, 151 }, { 3, 151 }, { 69, 72 } };
    shape.fillEnabled = true;
    shape.fillColor = { 85, 198, 207, 190 };
    shape.strokeColor = { 170, 234, 238, 245 };
    shape.strokeWidth = 3.5;
    placed("Polygon · concave geometry · editable points", shape, 840, 540);

    const auto saved = u::saveProject(path, document);
    if (!saved)
        throw std::runtime_error("Cannot write review project: " + saved.error.toStdString());
    const auto verified = u::loadProject(path);
    CHECK(verified && verified.document->layers().size() == document.layers().size());
    if (!verified)
        throw std::runtime_error("Review project failed read-back: " + verified.error.toStdString());
    for (const auto& original : document.layers()) {
        const auto* restored = verified.document->layer(original.id);
        CHECK(restored != nullptr);
        if (const auto* geometry = std::get_if<c::ShapeLayer>(&original.payload))
            CHECK(restored && std::get<c::ShapeLayer>(restored->payload) == *geometry
                && restored->localToDocument == original.localToDocument);
    }
    std::cout << "Verified editable shape review project: " << path.toStdString()
              << " (" << document.layers().size() << " layers)\n";
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        Fixture fixture;
        roundTrip(fixture);
        resizedGeometryRoundTrip(fixture);
        resizeGestureHistoryRoundTrip(fixture);
        independentAndDegenerate(fixture);
        malformed(fixture);
        compatibilityAndOpaqueMetadata(fixture);
        failedSaveIsAtomic(fixture);
        if (failures == 0)
            optionalReviewProject();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ++failures;
    }
    if (failures == 0)
        std::cout << "Shape project round-trip, capability, malformed input and "
                     "atomic-save tests passed\n";
    return failures == 0 ? 0 : 1;
}
