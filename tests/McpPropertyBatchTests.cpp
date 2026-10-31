#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RichText.hpp"
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
#define CHECK(v) do { if (!(v)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #v << '\n'; } } while(false)
struct Fixture {
    u::MainWindow window{nullptr,false,false};
    u::McpController controller{window};
    QString text, shape, raster;
    Fixture() {
        window.setUnsavedPromptEnabled(false);window.show();CHECK(imageeditor::test::waitForGraphicsStartup(window));
        CHECK(!controller.registry().call("vulkana_document_create",{{"width",200},{"height",160}})["isError"].toBool());
        const auto created = call("vulkana_layers_create",{{"layers",QJsonArray{
            QJsonObject{{"kind","text"},{"text",QJsonObject{{"text","Alpha\nBeta"},{"fontSize",18}}}},
            QJsonObject{{"kind","shape"},{"x",35},{"y",24},{"shape",QJsonObject{{"kind","rectangle"},{"width",40},{"height",20}}}},
            QJsonObject{{"kind","raster"},{"width",16},{"height",16}}}}})["layerIds"].toArray();
        CHECK(created.size()==3);if(created.size()!=3)throw std::runtime_error("fixture");
        text=created[0].toString();shape=created[1].toString();raster=created[2].toString();
    }
    ~Fixture(){window.close();QCoreApplication::processEvents();}
    c::EditorSession& session(){return const_cast<c::EditorSession&>(window.editorSession());}
    c::Document& doc(){return *session().document();}
    c::Layer& layer(const QString& id){return *doc().layer(id.toULongLong());}
    QJsonObject call(const char* name,QJsonObject args,bool error=false){
        args["documentId"]=QString::number(window.activeDocumentId());
        const auto reply=controller.registry().call(QLatin1String(name),args);
        if(reply["isError"].toBool()!=error)std::cerr<<QJsonDocument(reply).toJson(QJsonDocument::Compact).constData()<<'\n';
        CHECK(reply["isError"].toBool()==error);return reply["structuredContent"].toObject();
    }
};
QJsonObject textUpdate(const QString& id,QJsonObject properties){return {{"layerId",id},{"text",properties}};}
QJsonObject shapeUpdate(const QString& id,QJsonObject properties){return {{"layerId",id},{"shape",properties}};}

void oneUndoAndIndependentProperties()
{
    Fixture f;
    auto originalText=std::get<c::TextLayer>(f.layer(f.text).payload);
    auto red=originalText.defaultStyle;red.sizePixels=30;red.color={255,0,0,255};
    originalText.runs={{0,5,red}};originalText=c::normalizedText(std::move(originalText));
    CHECK(f.doc().setLayerText(f.text.toULongLong(),originalText));
    auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({40,20},180);
    CHECK(f.doc().setLayerMask(f.shape.toULongLong(),mask));
    auto effects=std::make_shared<c::LayerEffectStack>();effects->items[0].enabled=true;
    CHECK(f.doc().setLayerEffects(f.shape.toULongLong(),effects));
    CHECK(f.doc().setSelection(c::SelectionMask::rectangle({200,160},{1,2,3,4})));
    const auto pixelSelection=f.doc().selection();const auto selected=f.session().layerSelectionState();
    const auto originalShape=std::get<c::ShapeLayer>(f.layer(f.shape).payload);
    const auto matrix=f.layer(f.shape).localToDocument;
    const auto raster=std::get<c::RasterLayer>(f.layer(f.raster).payload).surface;
    const auto revision=raster->revision();const auto depth=f.session().history().undoDepth();
    const QJsonArray updates{textUpdate(f.text,{{"fontSize",24},{"alignment","center"}}),shapeUpdate(f.shape,{{"width",75},{"fillColor","#ff8800"}})};
    const auto reply=f.call("vulkana_layers_update",{{"updates",updates}});
    CHECK(reply["changed"].toBool() && reply["changedLayerIds"].toArray().size()==2 && reply["layers"].toArray().size()==2);
    CHECK(reply["layers"].toArray()[1].toObject()["geometry"].toObject()["documentBounds"].toObject()["width"]==75);
    const auto& changedText=std::get<c::TextLayer>(f.layer(f.text).payload);
    CHECK(changedText.defaultStyle.sizePixels==24 && changedText.utf8==originalText.utf8);
    for(const auto& run:changedText.runs)CHECK(run.style.sizePixels==24);
    CHECK(changedText.runs.front().style.color==red.color);
    CHECK(f.layer(f.shape).localToDocument==matrix && f.layer(f.shape).mask==mask && f.layer(f.shape).effects==effects);
    CHECK(f.doc().selection()==pixelSelection && f.session().layerSelectionState()==selected);
    CHECK(std::get<c::RasterLayer>(f.layer(f.raster).payload).surface==raster && raster->revision()==revision);
    CHECK(f.session().history().undoDepth()==depth+1);
    CHECK(f.session().undo());
    CHECK(std::get<c::TextLayer>(f.layer(f.text).payload)==originalText && std::get<c::ShapeLayer>(f.layer(f.shape).payload)==originalShape);
    CHECK(f.session().redo());
    QTemporaryDir files;const auto path=files.filePath("properties.vulkana");CHECK(u::saveProject(path,f.doc()));
    const auto loaded=u::loadProject(path);CHECK(loaded);
    if(loaded)CHECK(std::get<c::ShapeLayer>(loaded.document->layer(f.shape.toULongLong())->payload).size.width==75);
    // A repeated exact request is a no-op, including its lean response.
    const auto token=f.doc().revision();
    const auto lean=f.call("vulkana_layers_update",{{"updates",updates},{"response","ids"}});
    CHECK(!lean["changed"].toBool() && !lean.contains("layers") && lean["layerIds"].toArray().size()==2);
    CHECK(f.doc().revision()==token && f.session().history().undoDepth()==depth+1);
    CHECK(f.session().undo());const auto redo=f.session().history().redoDepth();
    const auto noop=f.call("vulkana_layers_update",{{"updates",QJsonArray{textUpdate(f.text,{})}}});
    CHECK(!noop["changed"].toBool() && f.session().history().redoDepth()==redo);
    const auto single=f.call("vulkana_text_set",{{"layerId",f.text},{"properties",QJsonObject{}},{"response","ids"}});
    CHECK(!single.contains("geometry") && !single["changed"].toBool());
}

void failureIsAtomic()
{
    Fixture f;
    f.call("vulkana_layers_update",{{"updates",QJsonArray{shapeUpdate(f.shape,{{"width",60}})}}});
    CHECK(f.session().undo());
    const auto shape=std::get<c::ShapeLayer>(f.layer(f.shape).payload);
    const auto text=std::get<c::TextLayer>(f.layer(f.text).payload);
    const auto revision=f.doc().revision(),content=f.doc().contentState();const auto redo=f.session().history().redoDepth();
    const auto selection=f.session().layerSelectionState();
    const QJsonArray first{textUpdate(f.text,{{"text","Changed"}})};
    const auto reject=[&](QJsonArray updates){
        const auto error=f.call("vulkana_layers_update",{{"updates",updates}},true)["error"].toObject();
        CHECK(error["details"].toObject()["noChanges"]==true && error["details"].toObject()["changed"]==false);
        CHECK(!error["details"].toObject()["recovery"].toString().contains("may have committed"));
        CHECK(f.doc().revision()==revision && f.doc().contentState()==content && f.session().history().redoDepth()==redo);
        CHECK(std::get<c::TextLayer>(f.layer(f.text).payload)==text && std::get<c::ShapeLayer>(f.layer(f.shape).payload)==shape);
        CHECK(f.session().layerSelectionState()==selection);
    };
    auto updates=first;updates.append(shapeUpdate(f.shape,{{"kind","polygon"},{"points",QJsonArray{}}}));reject(updates);
    updates=first;updates.append(textUpdate(f.raster,{{"text","No"}}));reject(updates);
    updates=first;updates.append(textUpdate(f.text,{{"fontSize",10}}));reject(updates);
    updates=first;updates.append(shapeUpdate("99999999",{{"width",5}}));reject(updates);
    updates=first;updates.append(QJsonObject{{"layerId",f.shape},{"shape",QJsonObject{}},{"text",QJsonObject{}}});reject(updates);
    updates=first;updates.append(textUpdate(f.text,{{"fontFamily","Missing_Font_472839"}}));reject(updates);
    reject({});
}

void guardedCorePublication()
{
    c::Document doc({{100,100},96});c::TextLayer text;text.utf8="Original";
    auto a=c::Layer::text("Text",text);const auto aId=a.id;CHECK(doc.insertLayer(0,std::move(a)));
    c::ShapeLayer shape;auto b=c::Layer::shape("Shape",shape);const auto bId=b.id;CHECK(doc.insertLayer(1,std::move(b)));
    const auto before=std::get<c::TextLayer>(doc.layer(aId)->payload);auto after=before;after.utf8="Updated";after.runs.clear();
    auto next=shape;next.size.width=50;
    c::SetLayerContentsCommand command({{aId,before,after},{bId,shape,next}});
    const auto revision=doc.revision();
    auto stale=shape;stale.fillEnabled=false;CHECK(doc.setLayerShape(bId,stale));
    const auto changed=doc.revision();CHECK(!command.apply(doc) && doc.revision()==changed);
    CHECK(std::get<c::TextLayer>(doc.layer(aId)->payload)==before);
    CHECK(doc.setLayerShape(bId,shape));CHECK(command.apply(doc));
    CHECK(doc.revision()>revision && std::get<c::ShapeLayer>(doc.layer(bId)->payload)==next);
    CHECK(command.undo(doc));CHECK(std::get<c::TextLayer>(doc.layer(aId)->payload)==before);
    // Invalid late geometry is rejected without publishing the earlier text.
    next.size.width=-1;const auto baseline=doc.revision();
    std::vector<c::LayerContentUpdate> invalid;
    invalid.push_back({aId,before,after}); invalid.push_back({bId,shape,next});
    CHECK(!doc.setLayerContents(std::move(invalid)) && doc.revision()==baseline);
}
}
int main(int argc,char** argv)
{
    QApplication app(argc,argv);app.setApplicationName("McpPropertyBatchTests");app.setOrganizationName("ImageEditorTests");
    QStandardPaths::setTestModeEnabled(true);QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    oneUndoAndIndependentProperties();failureIsAtomic();guardedCorePublication();
    std::cout<<"MCP property batches: "<<failures<<" failures\n";return failures?1:0;
}
