#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(v) do { if (!(v)) {++failures;std::cerr << "FAIL " << __LINE__ << ": " << #v << '\n';} } while(false)
bool near(double a,double b) {return std::abs(a-b)<1e-7;}
struct Fixture {
    u::MainWindow window{nullptr,false,false};
    u::McpController controller{window};
    Fixture() {
        window.setUnsavedPromptEnabled(false);window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        request("vulkana_document_create",{{"width",240},{"height",160}},false,false);
    }
    ~Fixture() {window.close();QCoreApplication::processEvents();}
    c::EditorSession& session() {return const_cast<c::EditorSession&>(window.editorSession());}
    c::Document& doc() {return *session().document();}
    QJsonObject request(const char* name,QJsonObject args={},bool error=false,bool scoped=true) {
        if (scoped&&!args.contains("documentId")) args["documentId"]=QString::number(window.activeDocumentId());
        const auto response=controller.registry().call(name,args);
        if(response["isError"].toBool()!=error)std::cerr<<QJsonDocument(response).toJson().constData();
        CHECK(response["isError"].toBool()==error);return response["structuredContent"].toObject();
    }
    QString shape(double x,double y,double w=10,double h=10) {
        return request("vulkana_layer_create",{{"kind","shape"},{"x",x},{"y",y},{"shape",QJsonObject{{"width",w},{"height",h}}}})["layerId"].toString();
    }
    c::Layer& layer(const QString& id) {return *doc().layer(id.toULongLong());}
    QString group(QJsonArray ids,const char* kind="folder") {
        return request("vulkana_layer_group",{{"layerIds",ids},{"kind",kind}})["layerId"].toString();
    }
};
void alignmentAndNoOps()
{
    Fixture f; const auto a=f.shape(5,15,10,20), b=f.shape(85,65,30,10);
    const auto selection=f.session().layerSelectionState();
    const auto history=f.session().history().undoDepth();
    const auto aligned=f.request("vulkana_layers_align",{{"layerIds",QJsonArray{a,b}},{"horizontal","left"}});
    CHECK(aligned["changed"]==true && aligned["items"].toArray().size()==2);
    CHECK(f.layer(a).localToDocument.m02==5 && f.layer(b).localToDocument.m02==5);
    CHECK(f.session().history().undoDepth()==history+1 && f.session().layerSelectionState()==selection);
    CHECK(f.session().undo());CHECK(f.layer(b).localToDocument.m02==85);
    const auto revision=f.doc().revision();const auto redo=f.session().history().redoDepth();
    CHECK(f.request("vulkana_layers_align",{{"layerIds",QJsonArray{a}},{"horizontal","left"}})["changed"]==false);
    CHECK(f.doc().revision()==revision && f.session().history().redoDepth()==redo);
    f.request("vulkana_layers_align",{{"layerIds",QJsonArray{a}},{"reference","canvas"},{"horizontal","center"},{"vertical","center"}});
    CHECK(near(f.layer(a).localToDocument.m02,115)&&near(f.layer(a).localToDocument.m12,70));
    const auto depth=f.session().history().undoDepth();
    CHECK(f.request("vulkana_layers_align",{{"layerIds",QJsonArray{a}},{"reference","canvas"},{"horizontal","center"},{"vertical","center"}})["changed"]==false);
    CHECK(f.session().history().undoDepth()==depth);
    CHECK(f.session().undo());CHECK(f.layer(a).localToDocument.m02==5);
}
void containerBoundsAndDistribution()
{
    Fixture f; const auto a=f.shape(0,0,10,20), b=f.shape(30,20,30,10);
    const auto adjustment=f.request("vulkana_layer_create",{{"kind","adjustment"}})["layerId"].toString();
    const auto folder=f.group({a,b,adjustment});
    f.request("vulkana_layer_set_visibility",{{"layerId",folder},{"visible",false}});
    f.request("vulkana_layers_align",{{"layerIds",QJsonArray{a,folder}},{"reference","canvas"},{"horizontal","right"}});
    CHECK(near(f.layer(a).localToDocument.m02,180)&&near(f.layer(b).localToDocument.m02,210));
    CHECK(near(f.layer(adjustment).localToDocument.m02,180));
    CHECK(!f.doc().isEffectivelyVisible(a.toULongLong()));
    CHECK(f.session().undo());CHECK(f.layer(a).localToDocument.m02==0 && f.layer(b).localToDocument.m02==30);

    Fixture g; const auto x=g.shape(0,10,10,10), y=g.shape(30,50,20,20), z=g.shape(170,100,30,30);
    g.request("vulkana_layers_distribute",{{"layerIds",QJsonArray{z,x,y}},{"axis","horizontal"},{"spacing","centers"}});
    CHECK(near(g.layer(x).localToDocument.m02,0)&&near(g.layer(y).localToDocument.m02,85)&&near(g.layer(z).localToDocument.m02,170));
    CHECK(g.session().undo());
    g.request("vulkana_layers_distribute",{{"layerIds",QJsonArray{z,y,x}},{"axis","horizontal"},{"spacing","gaps"}});
    CHECK(near(g.layer(y).localToDocument.m02,80));CHECK(g.session().undo());
    g.request("vulkana_layers_distribute",{{"layerIds",QJsonArray{x,y,z}},{"axis","vertical"},{"reference","canvas"}});
    CHECK(near(g.layer(x).localToDocument.m12,0)&&near(g.layer(y).localToDocument.m12,60)&&near(g.layer(z).localToDocument.m12,130));
    const auto revision=g.doc().revision();
    CHECK(g.request("vulkana_layers_distribute",{{"layerIds",QJsonArray{x,y,z}},{"axis","vertical"},{"reference","canvas"}})["changed"]==false);
    CHECK(g.doc().revision()==revision);
}
void independentRepetition()
{
    Fixture f;
    const auto raster=f.request("vulkana_layer_create",{{"kind","raster"},{"width",8},{"height",8},{"x",10},{"y",20},{"color","#123456ff"}})["layerId"].toString();
    const auto shape=f.shape(30,40,12,16);
    f.request("vulkana_layer_mask",{{"layerId",raster},{"operation","reveal_all"}});
    f.request("vulkana_effect_set",{{"layerId",shape},{"type","stroke"},{"parameters",QJsonObject{{"size",2},{"color","#ff0000"}}}});
    const auto source=f.group({raster,shape},"clipping_group");const auto parent=f.group({source});
    f.request("vulkana_layer_set_visibility",{{"layerId",parent},{"visible",false}});
    const auto tree=f.doc().tree();const auto history=f.session().history().undoDepth();const auto selected=f.session().layerSelectionState();
    const auto result=f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{source,raster}},{"mode","linear"},{"copies",2},{"stepX",20},{"stepY",30}});
    const auto roots=result["layerIds"].toArray();CHECK(roots.size()==2 && result["instances"].toArray().size()==2);
    CHECK(f.session().history().undoDepth()==history+1);
    for(int i=0;i<roots.size();++i) {
        const auto id=roots[i].toString().toULongLong();const auto* copy=f.doc().tree().container(id);CHECK(copy);
        if(!copy)continue;
        CHECK(copy->kind==c::ContainerKind::ClippingMaskGroup && copy->children.size()==2 && copy->visible);
        CHECK(!f.doc().isEffectivelyVisible(id));
        const auto* pixels=f.doc().layer(copy->children[0]);const auto* geometry=f.doc().layer(copy->children[1]);
        CHECK(near(pixels->localToDocument.m02,10+20*(i+1))&&near(pixels->localToDocument.m12,20+30*(i+1)));
        CHECK(std::get<c::ShapeLayer>(geometry->payload)==std::get<c::ShapeLayer>(f.layer(shape).payload) && geometry->effects==f.layer(shape).effects);
        CHECK(pixels->mask==f.layer(raster).mask);
        const auto original=std::get<c::RasterLayer>(f.layer(raster).payload).surface;
        const auto duplicate=std::get<c::RasterLayer>(pixels->payload).surface;
        CHECK(original->id()!=duplicate->id());
        const std::array patch{std::byte{255},std::byte{0},std::byte{0},std::byte{255}};
        duplicate->replaceRgba8({0,0,1,1},patch,4);
        std::array<std::byte,4> value{};original->copyRgba8({0,0,1,1},value,4);CHECK(value[0]==std::byte{0x12});
    }
    CHECK(f.layer(raster).localToDocument.m02==10 && f.layer(shape).localToDocument.m02==30);
    CHECK(f.session().undo());CHECK(f.doc().tree()==tree && f.session().layerSelectionState()==selected);
    CHECK(f.session().redo());CHECK(f.session().selectedLayers().size()==2);
}
void radialAndGuards()
{
    Fixture f;
    const auto source=f.request("vulkana_layer_create",{{"kind","shape"},{"x",130},{"y",75},{"shape",QJsonObject{{"kind","arc"},{"width",20},{"height",10},{"startAngle",25},{"sweepAngle",240},{"strokeEnabled",true}}}})["layerId"].toString();
    const auto original=f.layer(source);
    auto repeat=QJsonObject{{"layerIds",QJsonArray{source}},{"mode","radial"},{"copies",1},{"centerX",100},{"centerY",80},{"angleStep",90}};
    auto result=f.request("vulkana_layers_repeat",repeat);auto copied=result["layerIds"].toArray()[0].toString();
    const auto matrix=f.layer(copied).localToDocument;
    CHECK(near(matrix.m00,0)&&near(matrix.m01,-1)&&near(matrix.m10,1)&&near(matrix.m11,0));
    CHECK(near(matrix.map({10,5}).x,100)&&near(matrix.map({10,5}).y,120));
    CHECK(std::get<c::ShapeLayer>(f.layer(copied).payload)==std::get<c::ShapeLayer>(original.payload));
    repeat["rotateCopies"]=false;result=f.request("vulkana_layers_repeat",repeat);copied=result["layerIds"].toArray()[0].toString();
    CHECK(f.layer(copied).localToDocument.m00==1 && f.layer(copied).localToDocument.m01==0);
    CHECK(near(f.layer(copied).localToDocument.m02,90)&&near(f.layer(copied).localToDocument.m12,115));
    CHECK(f.layer(source).localToDocument==original.localToDocument);
    const auto revision=f.doc().revision(), history=f.session().history().undoDepth();
    f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{source}},{"mode","linear"},{"copies",1},{"stepX",2}},true);
    repeat["copies"]=129;f.request("vulkana_layers_repeat",repeat,true);
    f.request("vulkana_layers_align",{{"layerIds",QJsonArray{source,source}},{"horizontal","left"}},true);
    f.request("vulkana_layers_distribute",{{"layerIds",QJsonArray{source}},{"axis","horizontal"}},true);
    f.request("vulkana_layers_align",{{"layerIds",QJsonArray{source}},{"horizontal","left"},{"expectedRevision","stale"}},true);
    CHECK(f.doc().revision()==revision && f.session().history().undoDepth()==history);
    QJsonArray many;for(int i=0;i<8;++i)many.append(f.shape(i*10,0));
    const auto count=f.doc().layers().size();
    f.request("vulkana_layers_repeat",{{"layerIds",many},{"mode","linear"},{"copies",128},{"stepX",10},{"stepY",0}},true);
    CHECK(f.doc().layers().size()==count);
    const auto large=f.request("vulkana_layer_create",{{"kind","raster"},{"width",2048},{"height",2048}})["layerId"].toString();
    const auto before=f.doc().revision();
    f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{large}},{"mode","linear"},{"copies",17},{"stepX",10},{"stepY",0}},true);
    CHECK(f.doc().revision()==before);
}
void repetitionKeepsMotifStacking()
{
    Fixture f; const auto a=f.shape(0,0),b=f.shape(3,0);
    f.request("vulkana_shape_set",{{"layerId",a},{"properties",QJsonObject{{"fillColor","#ff0000"}}}});
    f.request("vulkana_shape_set",{{"layerId",b},{"properties",QJsonObject{{"fillColor","#0000ff"}}}});
    const auto before=f.doc().tree();const auto depth=f.session().history().undoDepth();
    const auto result=f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{a,b}},{"mode","linear"},{"copies",2},{"stepX",5},{"stepY",0}});
    const auto copies=result["layerIds"].toArray();CHECK(copies.size()==4);
    auto expected=before.roots;for(const auto copy:copies)expected.push_back(copy.toString().toULongLong());
    CHECK(f.doc().tree().roots==expected && f.session().history().undoDepth()==depth+1);
    const auto composite=u::flattenDocument(f.doc());CHECK(composite);
    // A2 must paint over B1 in this overlap. Interleaving all A copies below
    // all B copies would incorrectly leave this pixel blue.
    if(composite)CHECK(composite.image.pixelColor(11,5)==QColor(Qt::red));
    CHECK(f.session().undo());CHECK(f.doc().tree()==before);
}
void generatedNamesArePreflighted()
{
    Fixture f; const auto source=f.shape(0,0);
    auto request=QJsonObject{{"layerIds",QJsonArray{source}},{"mode","linear"},{"copies",1},{"stepX",5},{"stepY",0}};
    for (const auto& name : {QString(4096,'x'),QString(QChar(0x732b)).repeated(1365)+"a"}) {
        CHECK(name.toUtf8().size()==4096);
        f.request("vulkana_layer_rename",{{"layerId",source},{"name",name}});
        const auto revision=f.doc().revision();const auto depth=f.session().history().undoDepth();
        const auto error=f.request("vulkana_layers_repeat",request,true)["error"].toObject()["message"].toString();
        CHECK(error.contains("4096 UTF-8 bytes") && error.contains(source));
        CHECK(f.doc().revision()==revision && f.session().history().undoDepth()==depth);
    }
    // Three-byte characters fit exactly with the seven-byte first-copy suffix.
    const auto fits=QString(QChar(0x732b)).repeated(1363);
    f.request("vulkana_layer_rename",{{"layerId",source},{"name",fits}});
    const auto copy=f.request("vulkana_layers_repeat",request)["layerIds"].toArray()[0].toString();
    CHECK(f.layer(copy).name.size()==4096 && QString::fromStdString(f.layer(copy).name)==fits+" copy 1");
    request["copies"]=10;
    CHECK(f.request("vulkana_layers_repeat",request,true)["error"].toObject()["message"].toString().contains("copy 10"));
    const auto group=f.group({source});
    f.request("vulkana_layer_rename",{{"layerId",group},{"name",QString(4096,'g')}});
    request["layerIds"]=QJsonArray{group};request["copies"]=1;
    const auto tree=f.doc().tree();f.request("vulkana_layers_repeat",request,true);CHECK(f.doc().tree()==tree);
}
void inspectionGeometryMatchesLayout()
{
    Fixture f;
    const auto raster=f.request("vulkana_layer_create",{{"kind","raster"},{"width",8},{"height",6}})["layerId"].toString();
    const auto pixels=std::get<c::RasterLayer>(f.layer(raster).payload).surface;
    CHECK(f.doc().setLayerRasterStorage(raster.toULongLong(),pixels,{-5,-2},{}));
    CHECK(f.doc().setLayerTransform(raster.toULongLong(),{0,-1,50,1,0,30}));
    const auto shape=f.shape(100,5,10,20);
    f.request("vulkana_effect_set",{{"layerId",shape},{"type","stroke"},{"parameters",QJsonObject{{"size",75}}}});
    const auto adjustment=f.request("vulkana_layer_create",{{"kind","adjustment"}})["layerId"].toString();
    const auto inner=f.group({raster,shape,adjustment}), outer=f.group({inner});
    f.request("vulkana_layer_set_visibility",{{"layerId",outer},{"visible",false}});
    const auto revision=f.doc().revision(), depth=f.session().history().undoDepth();
    const auto original=f.request("vulkana_layer_get",{{"layerId",outer}})["geometry"].toObject();
    CHECK(original["localBounds"].isNull() && original["includesEffects"]==false);
    const auto bounds=original["documentBounds"].toObject();
    CHECK(bounds["x"]==46 && bounds["y"]==5 && bounds["width"]==64 && bounds["height"]==28);
    const auto quad=original["documentQuad"].toArray();
    CHECK(quad.size()==4 && quad[0].toObject()["x"]==46 && quad[2].toObject()["y"]==33);
    CHECK(f.request("vulkana_layer_get",{{"layerId",inner}})["geometry"].toObject()==original);
    CHECK(f.doc().revision()==revision && f.session().history().undoDepth()==depth);
    const auto aligned=f.request("vulkana_layers_align",{{"layerIds",QJsonArray{outer,raster}},{"reference","canvas"},{"horizontal","right"}});
    const auto item=aligned["items"].toArray()[0].toObject();
    CHECK(aligned["layerIds"].toArray()==QJsonArray{outer});
    CHECK(item["geometry"]==f.request("vulkana_layer_get",{{"layerId",outer}})["geometry"]);
    CHECK(item["documentBounds"]==item["geometry"].toObject()["documentBounds"]);
    CHECK(item["documentBounds"].toObject()["x"]==176);
    CHECK(f.session().undo());
    CHECK(f.request("vulkana_layer_get",{{"layerId",outer}})["geometry"].toObject()==original);
}
void nullContainerGeometry()
{
    Fixture f;
    const auto adjustment=f.request("vulkana_layer_create",{{"kind","adjustment"}})["layerId"].toString();
    const auto grouped=f.request("vulkana_layer_group",{{"layerIds",QJsonArray{adjustment}},{"kind","folder"}});
    CHECK(grouped.contains("geometry") && grouped["geometry"].isNull());
    const auto onlyAdjustment=grouped["layerId"].toString();
    auto tree=f.doc().tree();const auto empty=c::makeLayerId();
    tree.containers.push_back({empty,"Empty",c::ContainerKind::Folder,c::ColorLabel::None,{}});tree.roots.push_back(empty);
    CHECK(f.doc().replaceStructure(f.doc().tree(),tree));
    for (const auto& item : {adjustment,onlyAdjustment,QString::number(empty)}) {
        const auto result=f.request("vulkana_layer_get",{{"layerId",item}});
        CHECK(result.contains("geometry") && result["geometry"].isNull());
    }
    const auto revision=f.doc().revision(), depth=f.session().history().undoDepth();
    const auto error=f.request("vulkana_layers_align",{{"layerIds",QJsonArray{onlyAdjustment}},{"horizontal","center"}},true)["error"].toObject();
    CHECK(error["details"].toObject()["noChanges"]==true);
    CHECK(f.doc().revision()==revision && f.session().history().undoDepth()==depth);
    const auto repeated=f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{onlyAdjustment}},{"mode","linear"},{"copies",1},{"stepX",5},{"stepY",0}});
    CHECK(repeated["layers"].toArray().size()==1 && repeated["layers"].toArray()[0].toObject()["geometry"].isNull());
}
void groupingGeometryResponses()
{
    for (const auto* kind : {"folder", "group", "clipping_group"}) {
        Fixture f;
        const auto first=f.shape(20,10,12,8), second=f.shape(75,60,10,30);
        f.request("vulkana_layer_set_visibility",{{"layerId",second},{"visible",false}});
        const auto tree=f.doc().tree();const auto selection=f.session().layerSelectionState();
        const auto depth=f.session().history().undoDepth();
        const QJsonObject arguments{{"layerIds",QJsonArray{first,second}},{"kind",kind}};
        const auto grouped=f.request("vulkana_layer_group",arguments);
        CHECK(grouped["changed"]==true && grouped["geometry"].isObject());
        CHECK(grouped["geometry"]==f.request("vulkana_layer_get",{{"layerId",grouped["layerId"]}})["geometry"]);
        const auto bounds=grouped["geometry"].toObject()["documentBounds"].toObject();
        CHECK(bounds["x"]==20 && bounds["y"]==10 && bounds["width"]==65 && bounds["height"]==80);
        CHECK(f.session().history().undoDepth()==depth+1);
        CHECK(f.session().undo() && f.doc().tree()==tree && f.session().layerSelectionState()==selection);
        CHECK(f.session().redo());
        CHECK(grouped["geometry"]==f.request("vulkana_layer_get",{{"layerId",grouped["layerId"]}})["geometry"]);
        CHECK(f.session().undo());
        auto compactArguments=arguments;compactArguments["response"]="ids";
        const auto compact=f.request("vulkana_layer_group",compactArguments);
        CHECK(compact["changed"]==true && compact.contains("layerId") && compact.contains("revision") && !compact.contains("geometry"));
    }
}
void geometryAndCompactResponses()
{
    Fixture f;const auto leaf=f.shape(20,10,12,8), other=f.shape(75,60,10,30);
    const auto group=f.group({leaf,other},"clipping_group");
    const auto before=f.request("vulkana_layer_get",{{"layerId",group}})["geometry"].toObject();
    const auto repeated=f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{group}},{"mode","linear"},{"copies",2},{"stepX",7},{"stepY",11}});
    const auto ids=repeated["layerIds"].toArray(), layers=repeated["layers"].toArray();
    CHECK(ids.size()==2 && layers.size()==2 && repeated["instances"].toArray().size()==2);
    for (qsizetype i=0;i<ids.size();++i) {
        const auto item=layers[i].toObject();
        CHECK(item["layerId"]==ids[i]);
        CHECK(item["geometry"]==f.request("vulkana_layer_get",{{"layerId",ids[i]}})["geometry"]);
        CHECK(near(item["geometry"].toObject()["documentBounds"].toObject()["x"].toDouble(),20+7*double(i+1)));
        CHECK(item["geometry"].toObject()["localBounds"].isNull());
    }
    CHECK(f.request("vulkana_layer_get",{{"layerId",group}})["geometry"].toObject()==before);
    auto compact=f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{leaf}},{"mode","radial"},{"copies",1},{"centerX",0},{"centerY",0},{"angleStep",90},{"response","ids"}});
    CHECK(compact["changed"]==true && !compact.contains("layers") && compact["layerIds"].toArray().size()==1);
    CHECK(compact.contains("documentId") && compact.contains("revision") && compact.contains("instances"));
    const auto rotated=f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{leaf}},{"mode","radial"},{"copies",1},{"centerX",0},{"centerY",0},{"angleStep",90}});
    CHECK(rotated["layers"].toArray()[0].toObject()["geometry"]==f.request("vulkana_layer_get",{{"layerId",rotated["layerIds"].toArray()[0]}})["geometry"]);
    compact=f.request("vulkana_layers_align",{{"layerIds",ids},{"horizontal","left"},{"response","ids"}});
    CHECK(!compact.contains("items") && compact["layerIds"].toArray()==ids && compact.contains("revision"));
    compact=f.request("vulkana_layers_distribute",{{"layerIds",ids},{"axis","vertical"},{"response","ids"}});
    CHECK(!compact.contains("items") && compact["layerIds"].toArray()==ids && compact.contains("changed"));
    const auto distributed=f.request("vulkana_layers_distribute",{{"layerIds",ids},{"axis","vertical"}});
    for (const auto value : distributed["items"].toArray()) {
        const auto item=value.toObject();
        CHECK(item["geometry"]==f.request("vulkana_layer_get",{{"layerId",item["layerId"]}})["geometry"]);
    }
    const auto revision=f.doc().revision();
    f.request("vulkana_layers_repeat",{{"layerIds",QJsonArray{leaf}},{"mode","linear"},{"copies",1},{"stepX",1},{"stepY",0},{"response","unknown"}},true);
    CHECK(f.doc().revision()==revision);
}
}
int main(int argc,char** argv)
{
    QApplication app(argc,argv);app.setOrganizationName("ImageEditorTests");app.setApplicationName("McpLayout");
    QStandardPaths::setTestModeEnabled(true);QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    alignmentAndNoOps();containerBoundsAndDistribution();independentRepetition();radialAndGuards();repetitionKeepsMotifStacking();generatedNamesArePreflighted();
    inspectionGeometryMatchesLayout();nullContainerGeometry();geometryAndCompactResponses();groupingGeometryResponses();
    std::cout << "MCP layout: " << failures << " failures\n";return failures?1:0;
}
