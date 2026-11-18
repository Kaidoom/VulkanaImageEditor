#include "imageeditor/core/BrushSymmetry.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/project/ProjectArchive.hpp"
#include "imageeditor/ui/BrushSymmetryCodec.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <array>
#include <iostream>
#include <limits>
#include <map>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace p = imageeditor::project;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL " << __LINE__ << ": " #condition "\n"; ++failures; } } while(false)

void stateAndHistory()
{
    c::Document document({{120,80}}), other({{120,80}});
    c::History history;
    const auto empty = document.brushSymmetry();
    CHECK(empty == c::BrushSymmetry{} && c::validBrushSymmetry(empty));
    const c::BrushSymmetry vertical {true,c::BrushSymmetryAxis::Vertical,c::Vec2d{45.5,31.25}};
    CHECK(c::reflectedBrushPoint({20,13},vertical) == (c::Vec2d{71,13}));
    auto horizontal = vertical; horizontal.axis = c::BrushSymmetryAxis::Horizontal;
    CHECK(c::reflectedBrushPoint({20,13},horizontal) == (c::Vec2d{20,49.5}));
    CHECK(c::reflectedBrushPoint({20,13},empty) == (c::Vec2d{20,13}));
    CHECK(history.execute(document,std::make_unique<c::ChangeBrushSymmetryCommand>(empty,vertical)));
    CHECK(document.isModified() && document.brushSymmetry() == vertical);
    CHECK(other.brushSymmetry() == empty);
    CHECK(document.snapshot().brushSymmetry == vertical);
    CHECK(history.undo(document) && !document.isModified() && document.brushSymmetry() == empty);
    CHECK(history.redo(document) && document.brushSymmetry() == vertical);
    CHECK(history.execute(document,std::make_unique<c::ChangeBrushSymmetryCommand>(vertical,horizontal)));
    auto disabled = horizontal; disabled.enabled = false;
    CHECK(history.execute(document,std::make_unique<c::ChangeBrushSymmetryCommand>(horizontal,disabled)));
    CHECK(document.brushSymmetry().center == horizontal.center);
    CHECK(c::reflectedBrushPoint({20,13},disabled) == (c::Vec2d{20,13}));
    CHECK(history.undo(document) && document.brushSymmetry() == horizontal);
    CHECK(history.undo(document) && document.brushSymmetry() == vertical);
    const auto revision = document.revision();
    CHECK(!history.execute(document,std::make_unique<c::ChangeBrushSymmetryCommand>(empty,horizontal)));
    CHECK(!document.setBrushSymmetry(vertical,vertical) && document.revision() == revision);
    for (int invalid = 0; invalid < 4; ++invalid) {
        auto value = vertical;
        if (invalid == 0) value.center.reset();
        if (invalid == 1) value.axis = c::BrushSymmetryAxis(55);
        if (invalid == 2) value.center->x = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 3) value.center->y = 1e10;
        CHECK(!c::validBrushSymmetry(value));
        CHECK(!document.setBrushSymmetry(vertical,value));
        CHECK(document.brushSymmetry() == vertical && document.revision() == revision);
    }
}

void codec()
{
    for (const auto state : {c::BrushSymmetry{},
             c::BrushSymmetry{true,c::BrushSymmetryAxis::Vertical,c::Vec2d{-50.25,31.5}},
             c::BrushSymmetry{false,c::BrushSymmetryAxis::Horizontal,c::Vec2d{92.25,61.5}}})
        CHECK(u::decodeBrushSymmetry(u::encodeBrushSymmetry(state)) == state);
    const auto original = u::encodeBrushSymmetry({true,c::BrushSymmetryAxis::Horizontal,c::Vec2d{7,11}});
    for (int invalid = 0; invalid < 8; ++invalid) {
        auto value = original;
        if (invalid < 4) value.remove(std::array{"version","enabled","axis","center"}[std::size_t(invalid)]);
        if (invalid == 4) value["version"] = 2;
        if (invalid == 5) value["enabled"] = "true";
        if (invalid == 6) value["axis"] = "diagonal";
        if (invalid == 7) value["center"] = QJsonValue::Null;
        bool rejected = false;
        try { (void)u::decodeBrushSymmetry(value); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
}

using Payloads = std::map<QString,QByteArray>;
Payloads readArchive(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read test project");
    p::Archive archive(file,false); p::Progress progress; Payloads result;
    for (const auto& [name,entry] : p::entries(archive))
        p::readEntry(archive,entry,[&](const char* bytes,int count){result[name].append(bytes,count);},progress);
    archive.finish(); return result;
}
void rewriteManifest(const QString& path, Payloads payloads, const QJsonObject& manifest)
{
    payloads["manifest.json"] = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write test project");
    p::Archive archive(file,true); p::Progress progress;
    for (const auto& [name,bytes] : payloads)
        p::writeEntry(archive,name,quint64(bytes.size()),[&](const auto& write){write(bytes.constData(),int(bytes.size()));},progress);
    archive.finish();
}

void persistence(const QString& root)
{
    c::Document document({{32,24}});
    auto layer = c::Layer::raster("Pixels",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{32,24},c::Rgba8{113,42,203,175}));
    CHECK(document.insertLayer(0,std::move(layer)));
    const auto before = u::flattenDocument(document); CHECK(before);
    const auto path = root + "/symmetry.vulkana";
    CHECK(u::saveProject(path,document));
    auto legacy = u::loadProject(path); CHECK(legacy);
    if (legacy) CHECK(legacy.document->brushSymmetry() == c::BrushSymmetry{} && !legacy.metadata.contains("brushSymmetry"));
    const c::BrushSymmetry state {true,c::BrushSymmetryAxis::Horizontal,c::Vec2d{7.25,11.75}};
    CHECK(document.setBrushSymmetry({},state));
    CHECK(u::saveProject(path,document));
    auto loaded = u::loadProject(path); CHECK(loaded); if (!loaded) return;
    CHECK(loaded.document->brushSymmetry() == state && !loaded.document->isModified());
    CHECK(loaded.metadata["required"].toArray().contains("brush-symmetry-v1"));
    const auto after = u::flattenDocument(*loaded.document); CHECK(after && after.image == before.image);
    auto disabled = state; disabled.enabled = false;
    CHECK(loaded.document->setBrushSymmetry(state,disabled));
    CHECK(u::saveProject(path,*loaded.document,loaded.metadata));
    auto remembered = u::loadProject(path); CHECK(remembered);
    if (remembered) CHECK(remembered.document->brushSymmetry() == disabled);
    CHECK(loaded.document->setBrushSymmetry(disabled,{}));
    CHECK(u::saveProject(root + "/cleared.vulkana",*loaded.document,loaded.metadata));
    auto cleared = u::loadProject(root + "/cleared.vulkana"); CHECK(cleared);
    if (cleared) CHECK(cleared.document->brushSymmetry() == c::BrushSymmetry{}
        && !cleared.metadata.contains("brushSymmetry")
        && !cleared.metadata["required"].toArray().contains("brush-symmetry-v1"));
    const auto payloads = readArchive(path);
    const auto manifest = QJsonDocument::fromJson(payloads.at("manifest.json")).object();
    for (int invalid = 0; invalid < 3; ++invalid) {
        auto edited = manifest;
        if (invalid == 0) edited.remove("brushSymmetry");
        if (invalid == 1) {
            auto required = edited["required"].toArray();
            for (int i = required.size()-1; i >= 0; --i) if (required[i] == "brush-symmetry-v1") required.removeAt(i);
            edited["required"] = required;
        }
        if (invalid == 2) { auto setup = edited["brushSymmetry"].toObject(); setup["center"] = QJsonArray{1,1e10}; edited["brushSymmetry"] = setup; }
        const auto invalidPath = root + "/invalid.vulkana";
        rewriteManifest(invalidPath,payloads,edited);
        const auto rejected = u::loadProject(invalidPath); CHECK(!rejected);
        CHECK(rejected.error.contains("symmetry",Qt::CaseInsensitive));
    }
}
}

int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv); QTemporaryDir files;
    try { stateAndHistory(); codec(); persistence(files.path()); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << failures << " brush symmetry state/persistence failures\n";
    return failures ? 1 : 0;
}
