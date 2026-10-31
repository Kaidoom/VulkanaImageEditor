#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(v) do { if (!(v)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #v << '\n'; } } while (false)
struct Fixture {
    u::MainWindow window {nullptr, false, false};
    u::McpController controller {window};
    Fixture() {
        window.setUnsavedPromptEnabled(false); window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        CHECK(!controller.registry().call("vulkana_document_create", {{"width",96},{"height",64}})["isError"].toBool());
    }
    ~Fixture() { window.close(); QCoreApplication::processEvents(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& doc() { return *session().document(); }
    QJsonObject request(const char* name, QJsonObject args, bool error = false) {
        args["documentId"] = QString::number(window.activeDocumentId());
        const auto response = controller.registry().call(QLatin1String(name),args);
        if (response["isError"].toBool() != error) std::cerr << QJsonDocument(response).toJson(QJsonDocument::Compact).constData() << '\n';
        CHECK(response["isError"].toBool() == error);
        return response["structuredContent"].toObject();
    }
    QJsonObject batch(QJsonArray layers, bool error = false) { return request("vulkana_layers_create",{{"layers",layers}},error); }
};
QJsonObject pixel(QString name = "Pixel") { return {{"kind","raster"},{"name",name},{"width",8},{"height",6},{"color","#ff000080"}}; }
void mixedCreationIsOneUndo()
{
    Fixture f;
    const auto originalTree = f.doc().tree();
    const auto originalSelection = f.session().layerSelectionState();
    const auto originalSource = std::get<c::RasterLayer>(f.doc().layers()[0].payload).surface;
    const auto originalSourceRevision = originalSource->revision();
    const auto originalId = f.doc().layers()[0].id;
    CHECK(f.doc().setSelection(c::SelectionMask::rectangle(f.doc().canvas().extent,{2,2,10,10})));
    const auto pixelSelection = f.doc().selection();
    const auto history = f.session().history().undoDepth();
    auto raster = pixel("Raster"); raster["x"] = 5; raster["y"] = 7;
    const auto result = f.batch({raster,
        QJsonObject{{"kind","text"},{"name","Caption"},{"text",QJsonObject{{"text","Batch"},{"fontSize",18}}}},
        QJsonObject{{"kind","shape"},{"name","Arc"},{"shape",QJsonObject{{"kind","arc"},{"width",40},{"height",30}}}},
        QJsonObject{{"kind","adjustment"},{"name","Adjustment"}}});
    CHECK(result["changed"].toBool());
    const auto ids = result["layerIds"].toArray(), items = result["layers"].toArray();
    CHECK(ids.size() == 4 && items.size() == 4 && f.doc().layers().size() == 5);
    if (ids.size() != 4 || items.size() != 4) return;
    for (qsizetype i=0;i<4;++i) {
        CHECK(items[i].toObject()["layerId"] == ids[i]);
        CHECK(f.doc().layers()[std::size_t(i+1)].id == ids[i].toString().toULongLong());
        CHECK(i==3 ? items[i].toObject()["geometry"].isNull() : items[i].toObject()["geometry"].isObject());
    }
    CHECK(f.session().activeLayer() == ids[3].toString().toULongLong() && f.session().selectedLayers().size() == 4);
    CHECK(f.session().history().undoDepth() == history + 1 && f.doc().selection() == pixelSelection);
    CHECK(std::get<c::RasterLayer>(f.doc().layer(originalId)->payload).surface == originalSource && originalSource->revision() == originalSourceRevision);
    const auto arc = f.request("vulkana_layer_get",{{"layerId",ids[2]}})["shape"].toObject();
    CHECK(arc["kind"] == "arc" && arc["startAngle"] == 0 && arc["sweepAngle"] == 270);
    CHECK(f.session().undo());
    CHECK(f.doc().tree() == originalTree && f.session().layerSelectionState() == originalSelection);
    CHECK(f.doc().selection() == pixelSelection && f.doc().layers().size() == 1);
    CHECK(f.session().redo());
    CHECK(f.doc().layers().size() == 5 && f.session().history().undoDepth() == history + 1);
    const auto rasterId = ids[0].toString().toULongLong();
    CHECK(c::PreparedRasterSampler(*f.doc().layer(rasterId)).sample({6,8}) == (c::Rgba8{255,0,0,128}));
    QTemporaryDir files; const auto path = files.filePath("batch.vulkana");
    CHECK(u::saveProject(path,f.doc()));
    const auto reopened = u::loadProject(path); CHECK(reopened);
    if (reopened) {
        CHECK(reopened.document->tree() == f.doc().tree());
        CHECK(std::get<c::ShapeLayer>(reopened.document->layer(ids[2].toString().toULongLong())->payload).sweepAngle == 270);
    }
}
void parentAndIndexAreSequential()
{
    Fixture f;
    const auto initial = f.batch({pixel("A"),pixel("B")})["layerIds"].toArray();
    const auto folder = f.request("vulkana_layer_group",{{"layerIds",initial},{"kind","folder"}}).value("layerId");
    auto first = pixel("Inserted first"); first["parentId"] = folder; first["index"] = 1;
    auto second = pixel("Inserted second"); second["parentId"] = folder; second["index"] = 1;
    const auto before = f.doc().tree();
    const auto created = f.batch({first,second,pixel("Root")})["layerIds"].toArray();
    CHECK(created.size() == 3); if (created.size() != 3) return;
    const auto* parent = f.doc().tree().container(folder.toString().toULongLong()); CHECK(parent);
    if (parent) CHECK((parent->children == std::vector<c::LayerId>{initial[0].toString().toULongLong(),
        created[1].toString().toULongLong(), created[0].toString().toULongLong(), initial[1].toString().toULongLong()}));
    CHECK(f.doc().tree().roots.back() == created[2].toString().toULongLong());
    CHECK(f.session().undo() && f.doc().tree() == before);
}
void rejectionPreservesDocumentAndRedo()
{
    Fixture f;
    f.request("vulkana_layer_create",pixel("Redo candidate")); CHECK(f.session().undo());
    const auto tree = f.doc().tree();
    const auto selection = f.doc().selection();
    const auto selected = f.session().layerSelectionState();
    const auto revision = f.doc().revision(), content = f.doc().contentState();
    const auto undo = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    const auto unchanged = [&] {
        CHECK(f.doc().tree() == tree && f.doc().selection() == selection && f.session().layerSelectionState() == selected);
        CHECK(f.doc().revision() == revision && f.doc().contentState() == content);
        CHECK(f.session().history().undoDepth() == undo && f.session().history().redoDepth() == redo);
    };
    const auto wrongKind = f.batch({pixel(),QJsonObject{{"kind","text"},{"width",10}}},true);
    CHECK(wrongKind["error"].toObject()["message"].toString().contains("layers[1]")); unchanged();
    auto parent = pixel(); parent["parentId"] = "999999999";
    f.batch({pixel(),parent},true); unchanged();
    auto index = pixel(); index["index"] = 4000;
    f.batch({pixel(),index},true); unchanged();
    f.batch({pixel(),QJsonObject{{"name","Missing kind"}}},true); unchanged();
    auto blank = pixel(); blank["name"] = "   "; f.batch({pixel(),blank},true); unchanged();
    auto oversizedName = pixel(); oversizedName["name"] = QString(4097,'A'); f.batch({pixel(),oversizedName},true); unchanged();
    auto unicodeName = pixel(); unicodeName["name"] = QString::fromUtf8("é").repeated(2049); f.batch({pixel(),unicodeName},true); unchanged();
    const auto existing = QString::number(f.doc().layers()[0].id);
    f.request("vulkana_layer_rename",{{"layerId",existing},{"name",unicodeName["name"]}},true); unchanged();
    f.request("vulkana_layer_group",{{"layerIds",QJsonArray{existing}},{"kind","folder"},{"name",unicodeName["name"]}},true); unchanged();
    f.batch({pixel(),QJsonObject{{"kind","shape"},{"shape",QJsonObject{{"kind","arc"},{"sweepAngle",361}}}}},true); unchanged();
    // Aggregate admission rejects before allocating even the first huge raster.
    const auto rejected = f.batch({QJsonObject{{"kind","raster"},{"width",8192},{"height",8192}},pixel()},true);
    CHECK(rejected["error"].toObject()["message"].toString().contains("64-megapixel")); unchanged();
    f.batch({},true); unchanged();
    QJsonArray tooMany; for(int i=0;i<257;++i) tooMany.append(pixel()); f.batch(tooMany,true); unchanged();
    f.request("vulkana_layers_create",{{"layers",QJsonArray{pixel()}},{"expectedRevision","stale"}},true); unchanged();
    CHECK(f.session().redo() && f.doc().layers().back().name == "Redo candidate");
}
void nativeLimitsAndArcIdempotence()
{
    Fixture f;
    const auto arc = f.request("vulkana_layer_create",{{"kind","shape"},{"shape",QJsonObject{{"kind","arc"},{"strokeEnabled",false}}}}).value("layerId");
    const auto depth = f.session().history().undoDepth();
    CHECK(!f.request("vulkana_shape_set",{{"layerId",arc},{"properties",QJsonObject{{"kind","arc"}}}})["changed"].toBool());
    CHECK(!std::get<c::ShapeLayer>(f.doc().layer(arc.toString().toULongLong())->payload).strokeEnabled);
    CHECK(f.session().history().undoDepth() == depth);
    CHECK(f.session().undo());
    const auto shared = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{1,1},c::Rgba8{});
    auto tree = f.doc().tree(); std::vector<c::Layer> added;
    for (int i=0;i<1022;++i) { auto layer = c::Layer::raster("Existing",shared); tree.roots.push_back(layer.id); added.push_back(std::move(layer)); }
    CHECK(f.doc().replaceStructure(f.doc().tree(),std::move(tree),{},added));
    CHECK(f.doc().layers().size() == 1023);
    auto final = pixel(); final["name"] = QString::fromUtf8("é").repeated(2048);
    CHECK(f.batch({final})["changed"].toBool() && f.doc().layers().size() == 1024);
    const auto revision = f.doc().revision();
    const auto rejected = f.batch({pixel()},true)["error"].toObject()["message"].toString();
    CHECK(rejected.contains("1024") && f.doc().revision() == revision);
    QTemporaryDir files; const auto path = files.filePath("limit.vulkana");
    CHECK(u::saveProject(path,f.doc())); const auto reopened = u::loadProject(path); CHECK(reopened);
    if (reopened) CHECK(reopened.document->layers().size() == 1024 && reopened.document->layers().back().name.size() == 4096);
}
void hierarchicalCreationAndForwardReferences()
{
    Fixture f;
    const auto tree = f.doc().tree(); const auto selection = f.session().layerSelectionState();
    const auto history = f.session().history().undoDepth();
    const auto original = std::get<c::RasterLayer>(f.doc().layers().front().payload).surface;
    auto base = pixel("Base"); base["ref"] = "base"; base["parentRef"] = "clip"; base["x"] = 20; base["y"] = 30;
    const QJsonArray entries{
        QJsonObject{{"kind","text"},{"ref","caption"},{"parentRef","card"},{"x",5},{"y",8},
            {"text",QJsonObject{{"text","Caption"},{"fontSize",14}}}},
        QJsonObject{{"kind","folder"},{"name","Root"},{"ref","root"}},
        QJsonObject{{"kind","group"},{"ref","card"},{"parentRef","root"}},
        QJsonObject{{"kind","clipping_group"},{"ref","clip"},{"parentRef","root"}},
        base,
        QJsonObject{{"kind","shape"},{"ref","accent"},{"parentRef","clip"},{"x",22},{"y",32},
            {"shape",QJsonObject{{"kind","rectangle"},{"width",6},{"height",4}}}},
        QJsonObject{{"kind","folder"},{"ref","empty"},{"parentRef","root"}},
        QJsonObject{{"kind","group"},{"ref","adjustments"},{"parentRef","root"}},
        QJsonObject{{"kind","adjustment"},{"parentRef","adjustments"}}};
    const auto created = f.batch(entries);
    const auto ids = created["layerIds"].toArray(), results = created["layers"].toArray();
    const auto refs = created["refs"].toObject();
    CHECK(ids.size() == entries.size() && results.size() == entries.size() && refs.size() == 8);
    if(ids.size()!=entries.size() || results.size()!=entries.size()) return;
    const auto target = [&](const char* ref) { return refs[QLatin1String(ref)].toString().toULongLong(); };
    CHECK(refs["caption"] == ids[0] && refs["root"] == ids[1] && refs["base"] == ids[4]);
    CHECK((f.session().selectedLayers() == std::vector<c::LayerId>{target("root")}));
    CHECK(f.session().activeLayer() == target("root") && f.session().history().undoDepth() == history+1);
    const auto* card = f.doc().tree().container(target("card"));
    const auto* clip = f.doc().tree().container(target("clip"));
    CHECK(card && clip);
    if (!card || !clip) return;
    CHECK(card->kind == c::ContainerKind::Group);
    CHECK(clip->kind == c::ContainerKind::ClippingMaskGroup);
    CHECK((clip->children == std::vector<c::LayerId>{target("base"),target("accent")}));
    CHECK(f.doc().layer(target("base"))->localToDocument.m02 == 20 && f.doc().layer(target("base"))->localToDocument.m12 == 30);
    CHECK(results[1].toObject()["geometry"].toObject()["localBounds"].isNull());
    CHECK(results[1].toObject()["geometry"].toObject()["documentBounds"].isObject());
    CHECK(results[6].toObject()["geometry"].isNull() && results[7].toObject()["geometry"].isNull());
    const auto clipBounds = results[3].toObject()["geometry"].toObject()["documentBounds"].toObject();
    CHECK(clipBounds["x"] == 20 && clipBounds["y"] == 30 && clipBounds["width"] == 8 && clipBounds["height"] == 6);
    for(qsizetype i=0;i<ids.size();++i) CHECK(results[i].toObject()["layerId"] == ids[i]);
    CHECK(f.session().undo() && f.doc().tree() == tree && f.session().layerSelectionState() == selection);
    CHECK(std::get<c::RasterLayer>(f.doc().layers().front().payload).surface == original);
    CHECK(f.session().redo() && f.doc().tree().container(target("root")));
    QTemporaryDir files; const auto path=files.filePath("hierarchy.vulkana");
    CHECK(u::saveProject(path,f.doc())); const auto read=u::loadProject(path); CHECK(read);
    if(read) CHECK(read.document->tree()==f.doc().tree() && !read.metadata.contains("refs"));
}
void hierarchyFailuresDoNotPublish()
{
    Fixture f; f.request("vulkana_layer_create",pixel("Redo")); CHECK(f.session().undo());
    const auto tree=f.doc().tree(); const auto selection=f.session().layerSelectionState();
    const auto revision=f.doc().revision(), content=f.doc().contentState();
    const auto undo=f.session().history().undoDepth(), redo=f.session().history().redoDepth();
    const auto rejected=[&](QJsonArray entries) {
        const auto error=f.batch(entries,true)["error"].toObject();
        CHECK(error["details"].toObject()["noChanges"] == true);
        CHECK(!error["details"].toObject()["recovery"].toString().contains("may have committed"));
        CHECK(f.doc().tree()==tree && f.doc().revision()==revision && f.doc().contentState()==content);
        CHECK(f.session().layerSelectionState()==selection && f.session().history().undoDepth()==undo && f.session().history().redoDepth()==redo);
    };
    auto first=pixel(); first["ref"]="same";
    auto second=pixel(); second["ref"]="same"; rejected({first,second});
    second=pixel(); second["parentRef"]="missing"; rejected({first,second});
    second["parentRef"]="same"; rejected({first,second}); // A leaf is not a parent.
    rejected({QJsonObject{{"kind","folder"},{"ref","self"},{"parentRef","self"}}});
    rejected({QJsonObject{{"kind","folder"},{"ref","a"},{"parentRef","b"}},
        QJsonObject{{"kind","group"},{"ref","b"},{"parentRef","a"}}});
    second=pixel(); second["parentRef"]="root"; second["parentId"]=QString::number(f.doc().layers().front().id);
    rejected({second,QJsonObject{{"kind","folder"},{"ref","root"}}});
    rejected({QJsonObject{{"kind","folder"},{"ref",""}}});
    rejected({QJsonObject{{"kind","folder"},{"ref","has spaces"}}});
    rejected({QJsonObject{{"kind","folder"},{"x",4}}});
    rejected({QJsonObject{{"kind","group"},{"width",5}}});
    second=pixel(); second["parentRef"]="root"; second["index"]=1;
    rejected({second,QJsonObject{{"kind","folder"},{"ref","root"}}});
    QJsonArray tooDeep;
    for(int i=0;i<66;++i) {
        QJsonObject entry{{"kind","folder"},{"ref",QStringLiteral("r%1").arg(i)}};
        if(i) entry["parentRef"]=QStringLiteral("r%1").arg(i-1);
        tooDeep.append(entry);
    }
    rejected(tooDeep);
    // Even a large first raster stays unallocated until all hierarchy edges
    // and aggregate admission have been validated.
    second=QJsonObject{{"kind","folder"},{"parentRef","absent"}};
    rejected({QJsonObject{{"kind","raster"},{"width",8192},{"height",8192}},second});
    CHECK(f.session().redo() && f.doc().layers().back().name=="Redo");
    f.request("vulkana_layer_create",{{"kind","folder"}},true);
    f.request("vulkana_layer_create",{{"kind","raster"},{"ref","single"}},true);
}
void hierarchyIndicesAndCompactResponses()
{
    Fixture f;
    auto first=pixel("First"); first["parentRef"]="Parent"; first["index"]=0;
    auto second=pixel("Second"); second["parentRef"]="Parent"; second["index"]=0;
    const auto made=f.batch({first,QJsonObject{{"kind","folder"},{"ref","Parent"}},second,
        QJsonObject{{"kind","folder"},{"ref","parent"}}});
    const auto refs=made["refs"].toObject();
    const auto ids=made["layerIds"].toArray();
    CHECK(ids.size()==4 && refs.size()==2); if(ids.size()!=4) return;
    CHECK(refs["Parent"] != refs["parent"]);
    const auto* folder=f.doc().tree().container(refs["Parent"].toString().toULongLong());
    CHECK(folder && folder->children == (std::vector<c::LayerId>{ids[2].toString().toULongLong(),ids[0].toString().toULongLong()}));
    auto child=pixel(); child["parentId"]=refs["Parent"];
    const auto compact=f.request("vulkana_layers_create",{{"layers",QJsonArray{child}},{"response","ids"}});
    CHECK(compact["changed"].toBool() && compact["layerIds"].toArray().size()==1);
    CHECK(compact.contains("documentId") && compact.contains("revision") && !compact.contains("layers") && !compact.contains("geometry"));
    const auto single=f.request("vulkana_layer_create",{{"kind","text"},{"text",QJsonObject{{"text","No geometry requested"}}},{"response","ids"}});
    CHECK(single.contains("layerId") && single.contains("revision") && !single.contains("geometry"));
    QJsonArray many; for(int i=0;i<256;++i) many.append(pixel());
    const auto small=f.request("vulkana_layers_create",{{"layers",many},{"response","ids"}});
    CHECK(small["layerIds"].toArray().size()==256 && !small.contains("layers"));
    CHECK(QJsonDocument(small).toJson(QJsonDocument::Compact).size() < 8192);
    const auto empty=f.request("vulkana_layers_create",{{"layers",QJsonArray{QJsonObject{{"kind","folder"},{"ref","only"}}}},{"response","ids"}});
    CHECK(!empty.contains("layers") && empty["refs"].toObject().contains("only"));
    CHECK(f.doc().tree().container(empty["refs"].toObject()["only"].toString().toULongLong()));
}

void bulkStyleIsAtomicAndLightweight()
{
    Fixture f;
    auto raster = pixel("Styled raster"); raster["width"] = 512; raster["height"] = 512;
    const QJsonObject shape{{"kind","shape"},{"shape",QJsonObject{{"kind","rectangle"},{"width",30},{"height",20}}}};
    const auto created = f.batch({raster,shape})["layerIds"].toArray();
    CHECK(created.size()==2); if(created.size()!=2)return;
    const auto first = created[0].toString(), second = created[1].toString();
    f.request("vulkana_effect_set",{{"layerId",first},{"type","stroke"},{"parameters",QJsonObject{{"size",2},{"blendMode","multiply"}}}});
    f.request("vulkana_effect_set",{{"layerId",second},{"type","stroke"},{"parameters",QJsonObject{{"size",9},{"opacity",.8}}}});
    f.request("vulkana_effect_set",{{"layerId",second},{"type","outer-glow"},{"parameters",QJsonObject{{"size",4}}}});
    const auto firstBefore = f.doc().layer(first.toULongLong())->effects, secondBefore = f.doc().layer(second.toULongLong())->effects;
    const auto source = std::get<c::RasterLayer>(f.doc().layer(first.toULongLong())->payload).surface;
    const auto sourceRevision = source->revision();
    const auto selected = f.session().layerSelectionState(); const auto depth = f.session().history().undoDepth();
    const QJsonObject patch{{"layerIds",created},{"visible",false},{"opacity",.4},{"fillOpacity",.6},{"blendMode","screen"},
        {"effect",QJsonObject{{"type","stroke"},{"parameters",QJsonObject{{"color","#aabbcc"}}}}}};
    const auto styled = f.request("vulkana_layers_style",patch);
    CHECK(styled["changed"].toBool() && styled["changedLayerIds"].toArray() == created);
    CHECK(f.session().history().undoDepth() == depth+1 && f.session().history().latestUndoMemoryCost() < 1024*1024);
    CHECK(f.session().layerSelectionState() == selected && source->revision() == sourceRevision);
    for(const auto value:created) {
        const auto* layer = f.doc().layer(value.toString().toULongLong());
        CHECK(!layer->visible && layer->opacity == .4F && layer->fillOpacity == .6F && layer->blendMode == c::BlendMode::Screen);
        CHECK(layer->effects->items[0].color == (c::Rgba8{170,187,204,255}));
    }
    CHECK(f.doc().layer(first.toULongLong())->effects->items[0].size==2);
    CHECK(f.doc().layer(first.toULongLong())->effects->items[0].blendMode==c::BlendMode::Multiply);
    CHECK(f.doc().layer(second.toULongLong())->effects->items[0].size==9 && f.doc().layer(second.toULongLong())->effects->items[3].enabled);
    CHECK(!f.request("vulkana_layers_style",patch)["changed"].toBool());
    CHECK(f.session().history().undoDepth()==depth+1);
    // If any target no longer matches, undo must not touch earlier targets.
    CHECK(f.doc().setLayerOpacity(second.toULongLong(),.9F));
    CHECK(!f.session().undo());
    CHECK(f.doc().layer(first.toULongLong())->opacity==.4F && f.doc().layer(second.toULongLong())->opacity==.9F);
    CHECK(f.doc().setLayerOpacity(second.toULongLong(),.4F)); CHECK(f.session().undo());
    CHECK(f.doc().layer(first.toULongLong())->visible && f.doc().layer(first.toULongLong())->opacity==1);
    CHECK(c::equivalentLayerEffects(f.doc().layer(first.toULongLong())->effects,firstBefore));
    CHECK(c::equivalentLayerEffects(f.doc().layer(second.toULongLong())->effects,secondBefore));
    const auto redo = f.session().history().redoDepth();
    CHECK(!f.request("vulkana_layers_style",{{"layerIds",created},{"visible",true},{"opacity",1}})["changed"].toBool());
    CHECK(f.session().history().redoDepth()==redo && f.session().redo());
    CHECK(std::get<c::RasterLayer>(f.doc().layer(first.toULongLong())->payload).surface == source);
}
void bulkStyleRejectsBeforeEditing()
{
    Fixture f;
    const auto created = f.batch({pixel(),QJsonObject{{"kind","adjustment"}}})["layerIds"].toArray();
    const auto selected = f.session().layerSelectionState();
    const auto revision = f.doc().revision(), content = f.doc().contentState();
    const auto depth = f.session().history().undoDepth();
    const auto unchanged = [&] {
        CHECK(f.doc().revision()==revision && f.doc().contentState()==content && f.session().history().undoDepth()==depth);
        CHECK(f.session().layerSelectionState()==selected);
        for(const auto value:created) CHECK(f.doc().layer(value.toString().toULongLong())->opacity==1);
    };
    f.request("vulkana_layers_style",{{"layerIds",created},{"opacity",.2},{"effect",QJsonObject{{"type","stroke"},{"parameters",QJsonObject{{"size",3}}}}}},true); unchanged();
    f.request("vulkana_layers_style",{{"layerIds",QJsonArray{created[0],"999999999"}},{"opacity",.2}},true); unchanged();
    f.request("vulkana_layers_style",{{"layerIds",created},{"opacity",2}},true); unchanged();
    f.request("vulkana_layers_style",{{"layerIds",created}},true); unchanged();
    f.request("vulkana_layers_style",{{"layerIds",QJsonArray{created[0],created[0]}},{"opacity",.2}},true); unchanged();
    f.request("vulkana_layers_style",{{"layerIds",QJsonArray{created[0]}},{"effect",QJsonObject{{"type","stroke"},{"enabled",false},{"parameters",QJsonObject{{"enabled",true}}}}}},true); unchanged();
    f.request("vulkana_layers_style",{{"layerIds",QJsonArray{created[0]}},{"effect",QJsonObject{{"type","stroke"},{"parameters",QJsonObject{{"distance",3}}}}}},true); unchanged();
    const QJsonArray descendingStops{QJsonObject{{"position",1},{"color","#ffffff"}},QJsonObject{{"position",0},{"color","#000000"}}};
    f.request("vulkana_layers_style",{{"layerIds",QJsonArray{created[0]}},{"effect",QJsonObject{{"type","gradient-overlay"},
        {"parameters",QJsonObject{{"gradientStops",descendingStops}}}}}},true); unchanged();
    const auto folder=f.request("vulkana_layer_group",{{"layerIds",created},{"kind","folder"}}).value("layerId");
    const auto groupedRevision=f.doc().revision();
    f.request("vulkana_layers_style",{{"layerIds",QJsonArray{created[0],folder}},{"visible",false}},true);
    CHECK(f.doc().revision()==groupedRevision && f.doc().layer(created[0].toString().toULongLong())->visible);
}
void effectStateEquality()
{
    const auto defaults=std::make_shared<c::LayerEffectStack>();
    CHECK(c::equivalentLayerEffects({},defaults) && c::equivalentLayerEffects(defaults,{}));
    CHECK(c::equivalentLayerEffects(defaults,std::make_shared<c::LayerEffectStack>(*defaults)));
    auto changed=std::make_shared<c::LayerEffectStack>(*defaults); changed->items[0].size=7;
    CHECK(!c::equivalentLayerEffects(changed,{}) && !c::equivalentLayerEffects(defaults,changed));
}
}
int main(int argc, char** argv)
{
    QApplication app(argc,argv); app.setOrganizationName("ImageEditorTests"); app.setApplicationName("McpBatchLayers");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    mixedCreationIsOneUndo(); parentAndIndexAreSequential(); rejectionPreservesDocumentAndRedo();
    nativeLimitsAndArcIdempotence(); bulkStyleIsAtomicAndLightweight(); bulkStyleRejectsBeforeEditing(); effectStateEquality();
    hierarchicalCreationAndForwardReferences(); hierarchyFailuresDoNotPublish(); hierarchyIndicesAndCompactResponses();
    std::cout << "MCP layer batching: " << failures << " failures\n";
    return failures ? 1 : 0;
}
