#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/TextClipboard.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

// Independent Store ZIP transport: malformed graphs must bypass the writer's
// production validation, while retaining the reader's real CRC/path checks.
void integer(QByteArray& bytes, uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
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
QByteArray archive(const QJsonObject& manifest)
{
    const std::vector<std::pair<QByteArray, QByteArray>> entries {
        { "manifest.json", QJsonDocument(manifest).toJson(QJsonDocument::Compact) },
        { "rasters/19.rgba", QByteArray::fromHex("ff008040") }
    };
    QByteArray bytes, directory;
    for (const auto& [name, data] : entries) {
        const auto offset = uint32_t(bytes.size()), crc = crc32(data);
        integer(bytes, 0x04034b50, 4);
        for (auto value : { 20, 0, 0, 0, 0 })
            integer(bytes, uint32_t(value), 2);
        integer(bytes, crc, 4);
        integer(bytes, uint32_t(data.size()), 4);
        integer(bytes, uint32_t(data.size()), 4);
        integer(bytes, uint32_t(name.size()), 2);
        integer(bytes, 0, 2);
        bytes += name;
        bytes += data;
        integer(directory, 0x02014b50, 4);
        for (auto value : { 20, 20, 0, 0, 0, 0 })
            integer(directory, uint32_t(value), 2);
        integer(directory, crc, 4);
        integer(directory, uint32_t(data.size()), 4);
        integer(directory, uint32_t(data.size()), 4);
        integer(directory, uint32_t(name.size()), 2);
        for (unsigned i = 0; i < 4; ++i)
            integer(directory, 0, 2);
        integer(directory, 0, 4);
        integer(directory, offset, 4);
        directory += name;
    }
    const auto offset = uint32_t(bytes.size());
    bytes += directory;
    integer(bytes, 0x06054b50, 4);
    integer(bytes, 0, 2);
    integer(bytes, 0, 2);
    integer(bytes, uint32_t(entries.size()), 2);
    integer(bytes, uint32_t(entries.size()), 2);
    integer(bytes, uint32_t(directory.size()), 4);
    integer(bytes, offset, 4);
    integer(bytes, 0, 2);
    return bytes;
}
QJsonObject flatManifest()
{
    return { { "format", "org.vulkana.project" }, { "version", 1 },
        { "canvas", QJsonObject { { "width", 8 }, { "height", 6 } } },
        { "layers", QJsonArray { QJsonObject { { "id", "19" }, { "type", "raster" },
            { "name", "Legacy raster" }, { "raster", QJsonObject { { "width", 1 },
                { "height", 1 }, { "path", "rasters/19.rgba" } } } } } } };
}
QJsonObject container(const char* id, const char* kind, QJsonArray children)
{
    return { { "id", id }, { "kind", kind }, { "name", QStringLiteral("Container %1").arg(id) },
        { "colorLabel", 0 }, { "children", children } };
}
QJsonObject hierarchicalManifest()
{
    auto result = flatManifest();
    result["required"] = QJsonArray { "hierarchy-v1" };
    result["hierarchy"] = QJsonObject { { "version", 1 }, { "roots", QJsonArray { "41" } },
        { "containers", QJsonArray { container("41", "folder", { "42" }), container("42", "group", { "19" }) } } };
    return result;
}
void editHierarchy(QJsonObject& document, const std::function<void(QJsonObject&)>& edit)
{
    auto hierarchy = document["hierarchy"].toObject();
    edit(hierarchy);
    document["hierarchy"] = hierarchy;
}
void editContainer(QJsonObject& document, const std::function<void(QJsonObject&)>& edit)
{
    editHierarchy(document, [&](auto& hierarchy) {
        auto records = hierarchy["containers"].toArray();
        auto record = records[0].toObject();
        edit(record);
        records[0] = record;
        hierarchy["containers"] = records;
    });
}
struct Fixture {
    QTemporaryDir directory;
    unsigned sequence { 0 };
    Fixture()
    {
        if (!directory.isValid())
            throw std::runtime_error("Cannot create hierarchy project test directory");
    }
    QString path(const QString& name) const { return directory.filePath(name); }
    u::ProjectLoadResult load(const QJsonObject& manifest, u::ProjectProgress progress = {})
    {
        const auto filePath = path(QStringLiteral("fixture-%1.vulkana").arg(++sequence));
        QFile file(filePath);
        const auto bytes = archive(manifest);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            throw std::runtime_error("Cannot write hierarchy project fixture");
        file.close();
        return u::loadProject(filePath, std::move(progress));
    }
    void rejects(const QJsonObject& manifest)
    {
        quint64 bytesRead = 0;
        auto result = load(manifest, [&](quint64 done, quint64) { bytesRead = done; return true; });
        CHECK(!result && !result.document && !result.error.isEmpty());
        // The raster follows manifest validation, including malformed hierarchy.
        CHECK(bytesRead <= quint64(QJsonDocument(manifest).toJson(QJsonDocument::Compact).size()));
    }
};

void legacyAndTreeAuthority(Fixture& fixture)
{
    auto flat = fixture.load(flatManifest());
    CHECK(flat && flat.document->tree().roots == std::vector<c::LayerId> { 19 });
    if (!flat)
        return;
    CHECK(flat.document->tree().containers.empty());
    CHECK(flat.document->layers()[0].colorLabel == 0);
    const auto path = fixture.path("flat-resaved.vulkana");
    CHECK(u::saveProject(path, *flat.document, flat.metadata));
    auto reloaded = u::loadProject(path);
    CHECK(reloaded && !reloaded.metadata.contains("hierarchy"));
    CHECK(reloaded && !reloaded.metadata["required"].toArray().contains("hierarchy-v1"));
    CHECK(reloaded && !reloaded.metadata["required"].toArray().contains("container-visibility-v1"));

    auto manifest = hierarchicalManifest();
    c::TextLayer text;
    text.utf8 = "Keep me editable";
    auto layers = manifest["layers"].toArray();
    layers.append(QJsonObject { { "id", "23" }, { "type", "text" }, { "name", "Text" },
        { "text", QJsonDocument::fromJson(u::encodeTextClipboard(text)).object() } });
    manifest["layers"] = layers; // Intentionally payload order 19,23; tree order 23,19.
    editHierarchy(manifest, [](auto& hierarchy) {
        auto containers = hierarchy["containers"].toArray();
        auto group = containers[1].toObject();
        group["children"] = QJsonArray { "23", "19" };
        containers[1] = group;
        hierarchy["containers"] = containers;
    });
    auto loaded = fixture.load(manifest);
    CHECK(loaded);
    if (loaded) {
        CHECK(loaded.document->layers()[0].id == 23 && loaded.document->layers()[1].id == 19);
        CHECK(std::get<c::TextLayer>(loaded.document->layer(23)->payload) == c::normalizedText(text));
        for (const auto& item : loaded.document->tree().containers)
            CHECK(item.visible); // Older hierarchy-v1 files omitted this property.
    }
}

void containerVisibilityRoundTrip(Fixture& fixture)
{
    auto manifest = hierarchicalManifest();
    manifest["required"] = QJsonArray { "hierarchy-v1", "container-visibility-v1" };
    c::TextLayer text;
    text.utf8 = "Independently hidden";
    auto layers = manifest["layers"].toArray();
    layers.append(QJsonObject { { "id", "23" }, { "type", "text" }, { "name", "Text" },
        { "visible", false }, { "text", QJsonDocument::fromJson(u::encodeTextClipboard(text)).object() } });
    manifest["layers"] = layers;
    editHierarchy(manifest, [](auto& hierarchy) {
        auto records = hierarchy["containers"].toArray();
        for (int i = 0; i < records.size(); ++i) {
            auto item = records[i].toObject();
            item["visible"] = false;
            item["futureOptional"] = "keep container note";
            if (item["kind"] == "group")
                item["children"] = QJsonArray { "19", "23" };
            records[i] = item;
        }
        hierarchy["containers"] = records;
    });
    auto loaded = fixture.load(manifest);
    CHECK(loaded);
    if (!loaded)
        return;
    CHECK(!loaded.document->tree().container(41)->visible);
    CHECK(!loaded.document->tree().container(42)->visible);
    CHECK(loaded.document->layer(19)->visible && !loaded.document->layer(23)->visible);
    const auto path = fixture.path("hidden-containers.vulkana");
    CHECK(u::saveProject(path, *loaded.document, loaded.metadata));
    auto saved = u::loadProject(path);
    CHECK(saved);
    if (!saved)
        return;
    CHECK(saved.document->tree() == loaded.document->tree());
    CHECK(saved.metadata["required"].toArray().contains("container-visibility-v1"));
    CHECK(saved.document->layer(19)->visible && !saved.document->layer(23)->visible);
    for (const auto& value : saved.metadata["hierarchy"].toObject()["containers"].toArray()) {
        const auto item = value.toObject();
        CHECK(item["visible"].isBool() && !item["visible"].toBool());
        CHECK(item["futureOptional"] == "keep container note");
    }

    // The visibility property is owned data, while unrelated metadata survives.
    // Revealing the containers must not revive stale false bits on save, reveal
    // independently hidden leaves, or retain an unnecessary required capability.
    auto revealed = saved.document->tree();
    for (auto& item : revealed.containers)
        item.visible = true;
    CHECK(saved.document->replaceStructure(saved.document->tree(), revealed));
    CHECK(u::saveProject(path, *saved.document, saved.metadata));
    auto visible = u::loadProject(path);
    CHECK(visible);
    if (!visible)
        return;
    CHECK(visible.document->tree() == revealed);
    CHECK(visible.document->layer(19)->visible && !visible.document->layer(23)->visible);
    CHECK(!visible.metadata["required"].toArray().contains("container-visibility-v1"));
    for (const auto& value : visible.metadata["hierarchy"].toObject()["containers"].toArray()) {
        const auto item = value.toObject();
        CHECK(item["visible"].isBool() && item["visible"].toBool());
        CHECK(item["futureOptional"] == "keep container note");
    }
}

void mixedRoundTripAndUngroup(Fixture& fixture)
{
    c::Document document({ { 128, 96 }, 144 });
    auto raster = c::Layer::raster("Raster", std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u { 4, 3 }, c::Rgba8 { 20, 40, 60, 120 }));
    raster.colorLabel = 2;
    raster.visible = false;
    raster.opacity = .375F;
    raster.localToDocument = { -.8, .3, -25.25, .4, 1.5, 17.75 };
    c::TextLayer text;
    text.utf8 = "Editable \xE2\x86\x92 text";
    text.defaultStyle.sizePixels = 29.5;
    text.defaultStyle.color = { 50, 90, 120, 160 };
    auto type = c::Layer::text("Typography", text);
    type.localToDocument = { 1.25, -.4, 22.5, .3, -.75, 61.25 };
    type.colorLabel = 5;
    c::ShapeLayer shape;
    shape.kind = c::ShapeKind::RoundedRectangle;
    shape.size = { 51.25, 29.75 };
    shape.cornerRadius = 7.5;
    shape.strokeEnabled = true;
    shape.strokeWidth = 3.25;
    auto geometry = c::Layer::shape("Editable shape", shape);
    geometry.localToDocument = { .7, .2, 74, -.3, 1.5, -16 };
    const std::vector<c::Layer> originals { raster, type, geometry };
    for (const auto& leaf : originals)
        CHECK(document.insertLayer(document.layers().size(), leaf));
    const auto folder = c::makeLayerId(), group = c::makeLayerId(), nested = c::makeLayerId(), empty = c::makeLayerId();
    c::LayerTree tree;
    tree.roots = { folder, empty };
    tree.containers = { { folder, "Artwork", c::ContainerKind::Folder, c::ColorLabel::Blue, { group } },
        { group, "Mixed group", c::ContainerKind::Group, c::ColorLabel::Purple, { raster.id, nested } },
        { nested, "Inner group", c::ContainerKind::Group, c::ColorLabel::Green, { type.id, geometry.id } },
        { empty, "Empty folder", c::ContainerKind::Folder, c::ColorLabel::None, {} } };
    CHECK(document.replaceStructure(document.tree(), tree));
    const auto path = fixture.path("mixed-group.vulkana");
    CHECK(u::saveProject(path, document));
    auto loaded = u::loadProject(path);
    CHECK(loaded);
    if (!loaded)
        return;
    CHECK(loaded.metadata["required"].toArray().contains("hierarchy-v1"));
    CHECK(loaded.metadata["required"].toArray().contains("shape-v1"));
    CHECK(loaded.document->tree() == tree);
    CHECK(!loaded.document->isModified());
    for (const auto& original : originals) {
        const auto* leaf = loaded.document->layer(original.id);
        CHECK(leaf && leaf->name == original.name && leaf->colorLabel == original.colorLabel);
        CHECK(leaf && leaf->localToDocument == original.localToDocument
            && leaf->opacity == original.opacity && leaf->visible == original.visible);
    }
    CHECK(std::get<c::TextLayer>(loaded.document->layer(type.id)->payload) == c::normalizedText(text));
    CHECK(std::get<c::ShapeLayer>(loaded.document->layer(geometry.id)->payload) == shape);
    std::vector<std::byte> pixels(4 * 3 * 4);
    std::get<c::RasterLayer>(loaded.document->layer(raster.id)->payload).surface->copyRgba8({ 0, 0, 4, 3 }, pixels, 16);
    CHECK(pixels[0] == std::byte(20) && pixels[3] == std::byte(120));

    // Reopening never flattens groups. Dissolution exposes the same typed leafs
    // and document-space matrices, with no parent transform to remove.
    auto ungrouped = loaded.document->tree();
    CHECK(ungrouped.dissolve(group));
    CHECK(ungrouped.dissolve(nested));
    CHECK(loaded.document->replaceStructure(loaded.document->tree(), ungrouped));
    CHECK(loaded.document->tree().container(folder)->children
        == std::vector<c::LayerId> { raster.id, type.id, geometry.id });
    for (const auto& original : originals)
        CHECK(loaded.document->layer(original.id)->localToDocument == original.localToDocument);
    CHECK(u::saveProject(path, *loaded.document, loaded.metadata));
    auto twice = u::loadProject(path);
    CHECK(twice && twice.document->tree() == ungrouped);
}

void malformed(Fixture& fixture)
{
    for (const auto* field : { "version", "roots", "containers" }) {
        auto manifest = hierarchicalManifest();
        editHierarchy(manifest, [&](auto& hierarchy) { hierarchy.remove(field); });
        fixture.rejects(manifest);
    }
    const std::vector<std::function<void(QJsonObject&)>> changes {
        [](auto& h) { h["version"] = 2; },
        [](auto& h) { h["roots"] = QJsonArray { "41", "19" }; }, // Duplicate membership.
        [](auto& h) { h["roots"] = QJsonArray { "999" }; },
        [](auto& h) { h["roots"] = QJsonArray { "42" }; }, // Orphan folder.
        [](auto& h) { h["roots"] = QJsonArray { 41 }; },
        [](auto& h) { h["containers"] = QJsonArray {}; },
        [](auto& h) { auto a = h["containers"].toArray(); a.append(a[0]); h["containers"] = a; },
        [](auto& h) { auto a = h["containers"].toArray(); auto c = a[1].toObject(); c["children"] = QJsonArray { "41" }; a[1] = c; h["containers"] = a; },
    };
    for (const auto& edit : changes) {
        auto manifest = hierarchicalManifest();
        editHierarchy(manifest, edit);
        fixture.rejects(manifest);
    }
    const std::vector<std::function<void(QJsonObject&)>> badContainer {
        [](auto& c) { c["id"] = "19"; }, // Collides with a leaf.
        [](auto& c) { c["id"] = "0"; },
        [](auto& c) { c["kind"] = "isolated-group"; },
        [](auto& c) { c["name"] = ""; },
        [](auto& c) { c["name"] = QString(4097, QChar('x')); },
        [](auto& c) { c["colorLabel"] = -1; },
        [](auto& c) { c["colorLabel"] = 7; },
        [](auto& c) { c["colorLabel"] = 1.5; },
        [](auto& c) { c["visible"] = "false"; },
        [](auto& c) { c["visible"] = 0; },
        [](auto& c) { c["visible"] = QJsonValue(QJsonValue::Null); },
        [](auto& c) { c["visible"] = QJsonArray {}; },
        [](auto& c) { c["children"] = QJsonArray { "19", "19" }; },
        [](auto& c) { c.remove("children"); },
    };
    for (const auto& edit : badContainer) {
        auto manifest = hierarchicalManifest();
        editContainer(manifest, edit);
        fixture.rejects(manifest);
    }
    auto manifest = hierarchicalManifest();
    manifest.remove("required");
    fixture.rejects(manifest);
    manifest = hierarchicalManifest();
    manifest["required"] = QJsonArray { "hierarchy-v2" };
    fixture.rejects(manifest);
    for (int hiddenIndex = 0; hiddenIndex < 2; ++hiddenIndex) {
        manifest = hierarchicalManifest();
        editHierarchy(manifest, [&](auto& hierarchy) {
            auto records = hierarchy["containers"].toArray();
            auto item = records[hiddenIndex].toObject();
            item["visible"] = false;
            records[hiddenIndex] = item;
            hierarchy["containers"] = records;
        });
        fixture.rejects(manifest); // Old readers must not silently reveal hidden containers.
    }
    manifest = hierarchicalManifest();
    manifest["required"] = QJsonArray { "container-visibility-v1" };
    fixture.rejects(manifest); // Visibility cannot substitute for required hierarchy support.
    for (const auto& label : { QJsonValue(-1), QJsonValue(7), QJsonValue("blue"), QJsonValue(2.5) }) {
        manifest = hierarchicalManifest();
        auto leaves = manifest["layers"].toArray();
        auto leaf = leaves[0].toObject();
        leaf["colorLabel"] = label;
        leaves[0] = leaf;
        manifest["layers"] = leaves;
        fixture.rejects(manifest);
    }
    manifest = flatManifest();
    auto leaves = manifest["layers"].toArray();
    auto leaf = leaves[0].toObject();
    leaf["colorLabel"] = 1;
    leaves[0] = leaf;
    manifest["layers"] = leaves;
    fixture.rejects(manifest); // Label cannot silently disappear in an older build.

    manifest = hierarchicalManifest();
    QJsonArray containers;
    for (size_t i = 0; i <= c::LayerTree::maxDepth; ++i) {
        auto entry = container("1", "folder", {});
        entry["id"] = QString::number(1000 + i);
        entry["children"] = QJsonArray { i == c::LayerTree::maxDepth ? QStringLiteral("19") : QString::number(1001 + i) };
        containers.append(entry);
    }
    manifest["hierarchy"] = QJsonObject { { "version", 1 }, { "roots", QJsonArray { "1000" } }, { "containers", containers } };
    fixture.rejects(manifest);

    manifest = hierarchicalManifest();
    editHierarchy(manifest, [](auto& hierarchy) {
        QJsonArray oversized;
        for (size_t i = 0; i < c::LayerTree::maxItems; ++i)
            oversized.append(container("41", "folder", {}));
        hierarchy["containers"] = oversized;
    });
    fixture.rejects(manifest);

    const auto marker = c::makeLayerId();
    manifest = hierarchicalManifest();
    editContainer(manifest, [](auto& c) { c["id"] = "9000000000000000"; });
    fixture.rejects(manifest);
    CHECK(c::makeLayerId() == marker + 1); // Rejected graphs never reserve IDs.
}

void ownedAndOptionalMetadata(Fixture& fixture)
{
    auto manifest = hierarchicalManifest();
    manifest["producerNote"] = "preserved";
    editHierarchy(manifest, [](auto& h) { h["futureOptional"] = "tree note"; });
    editContainer(manifest, [](auto& c) { c["futureOptional"] = "folder note"; c["colorLabel"] = 4; });
    auto layers = manifest["layers"].toArray();
    auto layer = layers[0].toObject();
    layer["futureOptional"] = "leaf note";
    layer["colorLabel"] = 3;
    layers[0] = layer;
    manifest["layers"] = layers;
    auto loaded = fixture.load(manifest);
    CHECK(loaded);
    if (!loaded)
        return;
    CHECK(loaded.document->setItemMetadata(19, "Renamed", c::ColorLabel::None));
    CHECK(loaded.document->setItemMetadata(41, "Folder renamed", c::ColorLabel::None));
    const auto path = fixture.path("metadata.vulkana");
    CHECK(u::saveProject(path, *loaded.document, loaded.metadata));
    auto saved = u::loadProject(path);
    CHECK(saved);
    if (!saved)
        return;
    CHECK(saved.document->layer(19)->colorLabel == 0);
    CHECK(saved.document->tree().container(41)->colorLabel == c::ColorLabel::None);
    CHECK(saved.metadata["producerNote"] == "preserved");
    CHECK(saved.metadata["layers"].toArray()[0].toObject()["futureOptional"] == "leaf note");
    CHECK(saved.metadata["hierarchy"].toObject()["futureOptional"] == "tree note");
    CHECK(saved.metadata["hierarchy"].toObject()["containers"].toArray()[0].toObject()["futureOptional"] == "folder note");
    auto tree = saved.document->tree();
    CHECK(tree.dissolve(42) && tree.dissolve(41));
    CHECK(saved.document->replaceStructure(saved.document->tree(), tree));
    CHECK(u::saveProject(path, *saved.document, saved.metadata));
    const auto flat = u::loadProject(path);
    CHECK(flat && !flat.metadata.contains("hierarchy")
        && !flat.metadata["required"].toArray().contains("hierarchy-v1"));

    // A successful load reserves container IDs alongside leaf IDs, avoiding
    // collisions when new layers or folders are created after reopening.
    manifest = hierarchicalManifest();
    editHierarchy(manifest, [](auto& h) {
        h["roots"] = QJsonArray { "2000000000" };
        auto records = h["containers"].toArray();
        auto folder = records[0].toObject();
        folder["id"] = "2000000000";
        records[0] = folder;
        h["containers"] = records;
    });
    CHECK(fixture.load(manifest));
    // Imported IDs are reserved sparsely: a hostile high ID must not advance
    // the ordinary allocator to exhaustion. Prove both properties explicitly.
    const auto next = c::makeLayerId();
    CHECK(next < 2000000000ULL);
    const auto reserved = next + 10;
    manifest = hierarchicalManifest();
    editHierarchy(manifest, [&](auto& h) {
        const auto id = QString::number(reserved);
        h["roots"] = QJsonArray { id };
        auto records = h["containers"].toArray();
        auto folder = records[0].toObject();
        folder["id"] = id;
        records[0] = folder;
        h["containers"] = records;
    });
    CHECK(fixture.load(manifest));
    c::LayerId generated = next;
    for (unsigned i = 0; i < 20 && generated < reserved; ++i) {
        generated = c::makeLayerId();
        CHECK(generated != reserved);
    }
    CHECK(generated > reserved);
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        Fixture fixture;
        legacyAndTreeAuthority(fixture);
        containerVisibilityRoundTrip(fixture);
        mixedRoundTripAndUngroup(fixture);
        malformed(fixture);
        ownedAndOptionalMetadata(fixture);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ++failures;
    }
    if (!failures)
        std::cout << "Hierarchy project roundtrip, legacy migration, metadata, malformed graph and ID reservation tests passed\n";
    return failures ? 1 : 0;
}
