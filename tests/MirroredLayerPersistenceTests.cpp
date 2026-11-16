#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/ui/MirrorCodec.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/PsdExport.hpp"
#include "imageeditor/project/ProjectArchive.hpp"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <iostream>
#include <atomic>
namespace c=imageeditor::core;
namespace u=imageeditor::ui;
namespace {
int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";++failures;}}while(false)
using Payloads=std::map<QString,QByteArray>;
Payloads readArchive(const QString& path) {
 QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read fixture");
 imageeditor::project::Archive zip(file,false);imageeditor::project::Progress progress;Payloads result;
 for(const auto& [name,entry]:imageeditor::project::entries(zip))
  imageeditor::project::readEntry(zip,entry,[&](const char* bytes,int count){result[name].append(bytes,count);},progress);
 zip.finish();return result;
}
void writeArchive(const QString& path,Payloads data,const QJsonObject& manifest) {
 data["manifest.json"]=QJsonDocument(manifest).toJson(QJsonDocument::Compact);
 QFile file(path);if(!file.open(QIODevice::WriteOnly))throw std::runtime_error("Cannot write fixture");
 imageeditor::project::Archive zip(file,true);imageeditor::project::Progress progress;
 for(const auto& [name,bytes]:data)imageeditor::project::writeEntry(zip,name,quint64(bytes.size()),
  [&](const auto& write){write(bytes.constData(),int(bytes.size()));},progress);
 zip.finish();
}
std::vector<std::byte> pixels(const c::Layer& layer) {
 const auto surface=std::get<c::RasterLayer>(layer.payload).surface;const auto e=surface->extent();
 std::vector<std::byte> data(std::size_t(e.width)*e.height*4);surface->copyRgba8({0,0,int(e.width),int(e.height)},data,std::size_t(e.width)*4);return data;
}
void roundTrip(const QString& root) {
 auto doc=std::make_unique<c::Document>(c::CanvasSpec{{64,64}});
 std::vector<std::byte> bytes(16*12*4);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=std::byte((i*73+31)%256);
 for(std::size_t i=0;i<bytes.size();i+=16)bytes[i+3]=std::byte{0};
 auto source=c::Layer::raster("Source",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{16,12},bytes));
 const auto sourceId=source.id;source.localToDocument={.m02=4,.m12=5};
 auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({16,12},180);source.mask=mask;
 CHECK(doc->insertLayer(0,std::move(source)));c::EditorSession session;session.replaceDocument(std::move(doc));
 CHECK(session.execute(c::createMirroredLayer(*session.document(),session.layerSelectionState())));
 const auto mirror=*session.activeLayer();auto& document=*session.document();
 CHECK(document.setLayerTransform(mirror,{.m02=27,.m12=19}));
 CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(mirror,.4F)));
 auto stack=std::make_shared<c::LayerEffectStack>();stack->items[0].enabled=true;stack->items[0].size=2;
 CHECK(session.execute(std::make_unique<c::SetLayerEffectsCommand>(mirror,document.layer(mirror)->effects,stack,true)));
 CHECK(document.moveLayer(mirror,0)); // Source can occur after its mirror in file order.
 const auto before=u::flattenDocument(document);CHECK(before);
 const auto path=root+"/mirrored.vulkana";const auto saved=u::saveProject(path,document);if(!saved)std::cerr<<saved.error.toStdString()<<'\n';CHECK(saved);
 auto loaded=u::loadProject(path);if(!loaded)std::cerr<<loaded.error.toStdString()<<'\n';CHECK(loaded);if(!loaded)return;
 CHECK(loaded.metadata["required"].toArray().contains("mirrored-layer-v1"));
 for(const auto& value:loaded.metadata["layers"].toArray()) {
  const auto record=value.toObject();if(record["id"].toString()!=QString::number(mirror))continue;
  CHECK(record["type"]=="mirror" && record.contains("mirror"));
  CHECK(!record.contains("raster") && !record.contains("layerMask") && !record.contains("text") && !record.contains("shape"));
 }
 auto& restored=*loaded.document;const auto* m=restored.layer(mirror);CHECK(m && m->mirror);if(!m)return;
 CHECK(m->mirror==document.layer(mirror)->mirror);CHECK(m->opacity==.4F);
 CHECK(m->localToDocument==document.layer(mirror)->localToDocument);
 CHECK(m->mask==restored.layer(sourceId)->mask);
 CHECK(pixels(*m)==bytes && pixels(*restored.layer(sourceId))==bytes);
 CHECK(c::equivalentLayerEffects(m->effects,document.layer(mirror)->effects));
 const auto after=u::flattenDocument(restored);CHECK(after && after.image==before.image);
 const auto sourceSurface=std::get<c::RasterLayer>(restored.layer(sourceId)->payload).surface;
 CHECK(std::get<c::RasterLayer>(m->payload).surface->id()==sourceSurface->id());
 const auto psd=u::capturePsdExport(restored,1);CHECK(psd.sourceBytes==bytes.size());
 CHECK(!psd.document->layer(mirror)->mirror);
 std::atomic_bool cancel{false};const auto plan=u::planPsdExport(psd,{},cancel);CHECK(plan);
 const auto exported=u::writePsdExport(psd,plan,root+"/mirrored.psd",cancel);CHECK(exported);
 c::EditorSession promoted;promoted.replaceDocument(std::move(loaded.document));promoted.setActiveLayer(mirror);
 CHECK(promoted.execute(c::promoteMirroredLayer(*promoted.document(),promoted.layerSelectionState())));
 const auto promotedPath=root+"/promoted.vulkana";
 CHECK(u::saveProject(promotedPath,*promoted.document(),loaded.metadata));
 auto plain=u::loadProject(promotedPath);CHECK(plain);if(plain)CHECK(!plain.document->layer(mirror)->mirror);
}
void typedSources(const QString& root) {
 c::EditorSession session;auto doc=std::make_unique<c::Document>(c::CanvasSpec{{128,64}});
 auto text=c::Layer::text("Text",c::TextLayer{.utf8="Hello\nMirror"});const auto source=text.id;
 CHECK(doc->insertLayer(0,text));session.replaceDocument(std::move(doc));
 CHECK(session.execute(c::createMirroredLayer(*session.document(),session.layerSelectionState())));const auto mirror=*session.activeLayer();
 auto shape=c::Layer::shape("Shape",c::ShapeLayer{});const auto shapeId=shape.id;
 CHECK(session.document()->insertLayer(session.document()->layers().size(),shape));session.setActiveLayer(shapeId);
 CHECK(session.execute(c::createMirroredLayer(*session.document(),session.layerSelectionState())));const auto shapeMirror=*session.activeLayer();
 CHECK(session.document()->moveLayer(shapeMirror,0));
 CHECK(u::saveProject(root+"/typed.vulkana",*session.document()));auto loaded=u::loadProject(root+"/typed.vulkana");CHECK(loaded);if(!loaded)return;
 CHECK(loaded.document->layer(mirror)->mirror->source==source);
 CHECK(std::get<c::TextLayer>(loaded.document->layer(mirror)->payload)==std::get<c::TextLayer>(text.payload));
 CHECK(loaded.document->layer(shapeMirror)->mirror->source==shapeId);
 CHECK(std::get<c::ShapeLayer>(loaded.document->layer(shapeMirror)->payload)==std::get<c::ShapeLayer>(shape.payload));
}
void codec() {
 c::LayerMirror source{.source=123};source.effects[0]=c::MirrorEffectMode::Disabled;source.effects[1]=c::MirrorEffectMode::Override;
 source.opacityOverride=true;const auto json=u::encodeLayerMirror(source);CHECK(u::decodeLayerMirror(json)==source);
 for(const char* key:{"version","source","effects","opacityOverride"}) {auto bad=json;bad.remove(key);bool refused=false;try{(void)u::decodeLayerMirror(bad);}catch(const std::exception&){refused=true;}CHECK(refused);}
}
void malformedAndTypedMemory(const QString& root) {
 const auto data=readArchive(root+"/mirrored.vulkana");
 const auto original=QJsonDocument::fromJson(data.at("manifest.json")).object();
 for(int problem=0;problem<3;++problem) {
  auto manifest=original;auto layers=manifest["layers"].toArray();auto required=manifest["required"].toArray();
  for(int i=0;i<layers.size();++i) {
   auto layer=layers[i].toObject();if(layer["type"]!="mirror")continue;
   if(problem==0) {auto link=layer["mirror"].toObject();link["source"]=layer["id"];layer["mirror"]=link;}
   else if(problem==1) {
    layer["transform"]=QJsonArray{1,0,0,0,1,0,-.1,0,1}; // Invertible, but horizon at x=10 inside 16px source.
    required.append("projective-transform-v1");
   }
   layers[i]=layer;
  }
  if(problem==2)for(int i=required.size()-1;i>=0;--i)if(required[i]=="mirrored-layer-v1")required.removeAt(i);
  manifest["layers"]=layers;manifest["required"]=required;
  const auto path=root+"/invalid.vulkana";writeArchive(path,data,manifest);
  const auto loaded=u::loadProject(path);CHECK(!loaded);
  if(problem==1)CHECK(loaded.error.contains("horizon"));
 }
 const auto typedData=readArchive(root+"/typed.vulkana");
 const auto typedManifest=QJsonDocument::fromJson(typedData.at("manifest.json")).object();
 for(const QString kind:{QStringLiteral("text"),QStringLiteral("shape")}) {
  auto manifest=typedManifest;QJsonObject source,mirror;
  for(const auto value:manifest["layers"].toArray())if(value.toObject()["type"]==kind)source=value.toObject();
  CHECK(!source.isEmpty());
  for(const auto value:manifest["layers"].toArray())if(value.toObject()["mirror"].toObject()["source"]==source["id"])mirror=value.toObject();
  CHECK(!mirror.isEmpty());
  if(kind=="text") {
   auto text=source["text"].toObject();text["text"]=QString(200*1024,'x');text["runs"]=QJsonArray{};
   text["paragraphs"]=QJsonArray{QJsonObject{{"start",0},{"alignment",0}}};source["text"]=text;
  } else {
   auto shape=source["shape"].toObject();shape["kind"]="polygon";shape["size"]=QJsonArray{100,100};
   QJsonArray points;for(int i=0;i<10000;++i)points.append(QJsonArray{double(i%100),double((i/100)%100)});
   shape["points"]=points;source["shape"]=shape;
  }
  QJsonArray layers{source},roots{source["id"]};
  const auto first=source["id"].toString().toULongLong()+100;
  for(quint64 i=0;i<8;++i){auto copy=mirror;copy["id"]=QString::number(first+i);layers.append(copy);roots.append(copy["id"]);}
  manifest["layers"]=layers;manifest["hierarchy"]=QJsonObject{{"version",1},{"roots",roots},{"containers",QJsonArray{}}};
  auto required=manifest["required"].toArray();
  if(!required.contains("hierarchy-v1"))required.append("hierarchy-v1");
  manifest["required"]=required;
  const auto path=root+"/typed-budget.vulkana";writeArchive(path,typedData,manifest);
  const auto denied=u::loadProject(path,{},u::ProjectLoadLimits{.workingBytes=64ULL*1024*1024+512*1024});
  CHECK(!denied && denied.error.contains("working memory"));
  const auto allowed=u::loadProject(path,{},u::ProjectLoadLimits{.workingBytes=128ULL*1024*1024});
  if(!allowed)std::cerr<<allowed.error.toStdString()<<'\n';CHECK(allowed);
 }
}
}
int main(int argc,char**argv){QGuiApplication app(argc,argv);QTemporaryDir directory;try{codec();roundTrip(directory.path());typedSources(directory.path());malformedAndTypedMemory(directory.path());}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<failures<<" mirror persistence failures\n";return failures?1:0;}
