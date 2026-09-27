#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <iostream>
namespace c=imageeditor::core;namespace u=imageeditor::ui;
int failures=0;
#define CHECK(x) do{if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";}}while(false)
int main(int argc,char** argv)
{
    QApplication app(argc,argv);QTemporaryDir files;
    c::Document source({{60,40},72});
    auto pixels=std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{20,12},c::Rgba8{70,40,20,150});
    auto raster=c::Layer::raster("Cutout",pixels);const auto rasterId=raster.id;
    raster.localToDocument={.7,.2,-13.5,-.1,1.3,8.25};raster.opacity=.35f;raster.blendMode=c::BlendMode::ColorDodge;
    raster.crop=c::LayerCrop{1,1,16,8};raster.colorLabel=3;
    auto effects=std::make_shared<c::LayerEffectStack>();effects->items[0].enabled=true;raster.effects=effects;
    auto adjustments=std::make_shared<c::AdjustmentStack>();raster.adjustments=adjustments;
    auto filters=std::make_shared<c::SpatialFilterStack>();raster.filters=filters;
    CHECK(source.insertLayer(0,raster));
    c::TextLayer text;text.utf8="Editable Ω";auto letters=c::Layer::text("Caption",text);const auto textId=letters.id;
    CHECK(source.insertLayer(1,letters));
    c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={16,9};shape.strokeEnabled=true;
    auto ellipse=c::Layer::shape("Ring",shape);const auto shapeId=ellipse.id;CHECK(source.insertLayer(2,ellipse));
    auto tree=source.tree();const auto folder=c::makeLayerId(),group=c::makeLayerId();
    tree.containers={{group,"Inner",c::ContainerKind::Group,c::ColorLabel::Red,{rasterId,textId},true},
        {folder,"Folder",c::ContainerKind::Folder,c::ColorLabel::Blue,{group,shapeId},false}};
    tree.roots={folder};CHECK(source.replaceStructure(source.tree(),tree));source.markSaved();
    const std::array ids{folder,rasterId,textId};auto transfer=c::captureLayerTransfer(source,ids);
    CHECK(transfer.tree.roots.size()==1&&transfer.layers.size()==3&&transfer.tree.containers.size()==2);
    CHECK(!transfer.tree.container(transfer.tree.roots.front())->visible);
    CHECK(transfer.layers[0].id!=rasterId&&transfer.layers[1].id!=textId);
    CHECK(transfer.layers[0].localToDocument==raster.localToDocument);
    CHECK(transfer.layers[0].crop==raster.crop&&transfer.layers[0].effects==effects);
    CHECK(transfer.layers[0].adjustments==adjustments&&transfer.layers[0].filters==filters);
    CHECK(transfer.layers[0].opacity==raster.opacity&&transfer.layers[0].blendMode==raster.blendMode);
    CHECK(std::get<c::TextLayer>(transfer.layers[1].payload)==std::get<c::TextLayer>(source.layer(textId)->payload));
    CHECK(std::get<c::ShapeLayer>(transfer.layers[2].payload)==shape);
    const auto copiedPixels=std::get<c::RasterLayer>(transfer.layers[0].payload).surface;
    CHECK(copiedPixels!=pixels&&copiedPixels->id()!=pixels->id());
    std::array<std::byte,4> original{},copy{};
    pixels->copyRgba8({1,1,1,1},original,4);copiedPixels->copyRgba8({1,1,1,1},copy,4);CHECK(original==copy);
    const std::array altered{std::byte{255},std::byte{0},std::byte{0},std::byte{255}};
    CHECK(!copiedPixels->replaceRgba8({1,1,1,1},altered,4).empty());pixels->copyRgba8({1,1,1,1},copy,4);CHECK(original==copy);
    c::EditorSession destination;destination.replaceDocument(std::make_unique<c::Document>(c::CanvasSpec{{90,80},300}));
    const auto root=transfer.tree.roots.front(),newRaster=transfer.layers.front().id;
    CHECK(destination.execute(c::insertLayerTransfer(*destination.document(),std::move(transfer),{0,0},destination.layerSelectionState(),{20,-5})));
    CHECK(destination.history().undoDepth()==1&&destination.activeLayer()==root);
    CHECK(destination.document()->layer(newRaster)->localToDocument.m02==raster.localToDocument.m02+20);
    CHECK(destination.document()->layer(newRaster)->localToDocument.m00==raster.localToDocument.m00); // PPI never scales copies.
    CHECK(!source.isModified());CHECK(destination.undo());CHECK(destination.document()->layers().empty());
    CHECK(destination.redo());CHECK(destination.document()->tree().roots==std::vector<c::LayerId>{root});
    const auto path=files.filePath("transfer.vulkana");CHECK(u::saveProject(path,*destination.document()));auto reopened=u::loadProject(path);CHECK(reopened);
    if(reopened){CHECK(reopened.document->tree()==destination.document()->tree());CHECK(reopened.document->layer(newRaster)->effects&&*reopened.document->layer(newRaster)->effects==*effects);}
    // Detaching just a descendant retains inherited visibility and absolute geometry.
    const std::array child{rasterId};auto detached=c::captureLayerTransfer(source,child);
    CHECK(!detached.layers.front().visible&&detached.layers.front().localToDocument==raster.localToDocument);
    std::cout<<"Document transfer: "<<failures<<" failures\n";return failures?1:0;
}
