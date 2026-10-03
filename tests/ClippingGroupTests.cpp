#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/PdfExport.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include <QAction>
#include <QSettings>
#include <QListView>
#include <QStandardPaths>
#include <QApplication>
#include <QTemporaryDir>
#include <iostream>
#include <cstring>
namespace c=imageeditor::core;namespace u=imageeditor::ui;
int failures=0;
#define CHECK(...) do{if(!(__VA_ARGS__)){++failures;std::cerr<<__LINE__<<": " #__VA_ARGS__ "\n";}}while(false)
c::Layer pixels(c::Rgba8 color){return c::Layer::raster("Pixels",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{8,8},color));}
c::Rgba8 sample(const c::Document& doc){return c::PinnedDocumentSampler(doc,{},c::ColorSampleSource::MergedVisible).sample({3.5,3.5});}
int main(int argc,char** argv)
{
    QApplication app(argc,argv);QTemporaryDir temp;
    c::Document doc({{8,8},96});
    auto base=pixels({0,0,0,128}),first=pixels({255,0,0,255}),second=pixels({0,255,0,255});
    for(const auto& l:{base,first,second})CHECK(doc.insertLayer(doc.layers().size(),l));
    auto tree=doc.tree();const auto group=c::makeLayerId();tree.roots={group};
    tree.containers.push_back({group,"Clipping",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::Blue,{base.id,first.id,second.id}});
    c::History history;CHECK(history.execute(doc,std::make_unique<c::LayerStructureCommand>("Clipping",doc,tree)));
    // Independent alpha reference: upper opaque colors replace RGB, never coverage.
    CHECK(sample(doc)==c::Rgba8{0,255,0,128});
    CHECK(u::flattenDocument(doc).image.pixelColor(3,3)==QColor(0,255,0,128));
    CHECK(history.undo(doc));CHECK(sample(doc).alpha==255);CHECK(history.redo(doc));
    CHECK(sample(doc).alpha==128);
    doc.setLayerVisibility(second.id,false);CHECK(sample(doc)==c::Rgba8{255,0,0,128});
    doc.setLayerOpacity(base.id,.5F);CHECK(sample(doc)==c::Rgba8{255,0,0,64});
    doc.setLayerOpacity(base.id,0);CHECK(sample(doc).alpha==0);
    doc.setLayerOpacity(base.id,1);doc.setLayerVisibility(base.id,false);CHECK(sample(doc).alpha==0);
    CHECK(c::hitTestRasterLayer(doc,{3.5,3.5})==std::nullopt);
    doc.setLayerVisibility(base.id,true);
    auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({8,8},128);mask->outside=0;
    doc.setLayerMask(base.id,mask);CHECK(sample(doc).alpha==64);
    auto partial=u::flattenLayerItems(doc,std::array{first.id});CHECK(partial&&partial.image.pixelColor(3,3)==QColor(255,0,0,255));
    auto whole=u::flattenLayerItems(doc,std::array{group});CHECK(whole&&whole.image.pixelColor(3,3)==QColor(255,0,0,64));
    const auto before=u::flattenDocument(doc).image;
    const auto path=temp.filePath("clipping.vulkana");CHECK(u::saveProject(path,doc));auto loaded=u::loadProject(path);
    CHECK(loaded && loaded.document->tree()==doc.tree());CHECK(loaded&&u::flattenDocument(*loaded.document).image==before);
    const c::LayerSelectionState selected{{group},group,group};
    auto bake=u::prepareRasterizeLayers(doc,selected,256ULL*1024*1024);CHECK(bake.command);
    CHECK(bake.command&&history.execute(doc,std::move(bake.command)));CHECK(doc.tree().containers.empty()&&doc.layers().size()==1);
    CHECK(u::flattenDocument(doc).image==before);CHECK(history.undo(doc));CHECK(doc.tree()==tree);CHECK(u::flattenDocument(doc).image==before);
    const auto pdf=u::capturePdfExport(doc,42,std::array{group});CHECK(pdf.entries.size()==1);
    u::PdfExportOptions options;options.mode=u::PdfExportMode::Pages;options.ppi=96;
    std::atomic_bool cancel{false};auto plan=u::planPdfExport(pdf,options,cancel);CHECK(plan.pages.size()==1);
    const auto pdfPixels=u::renderPdfExportPage(pdf,plan,0,cancel);
    CHECK(pdfPixels&&pdfPixels.image.size()==before.size());
    if(pdfPixels&&pdfPixels.image.size()==before.size())for(int y=0;y<before.height();++y)
        CHECK(std::memcmp(pdfPixels.image.constScanLine(y),before.constScanLine(y),std::size_t(before.width())*4)==0);
    c::Document destination(doc.canvas());auto transfer=c::captureLayerTransfer(doc,std::array{group});
    c::History copied;auto command=c::insertLayerTransfer(destination,std::move(transfer),{0,0},{});CHECK(command&&copied.execute(destination,std::move(command)));
    CHECK(destination.tree().containers.size()==1 && destination.tree().containers.front().kind==c::ContainerKind::ClippingMaskGroup);
    CHECK(destination.tree().roots.front()!=group);CHECK(u::flattenDocument(destination).image==before);
    const auto transferred=destination.layers().front().id;destination.setLayerVisibility(transferred,false);
    CHECK(sample(destination).alpha==0 && sample(doc).alpha==64);
    CHECK(copied.undo(destination));CHECK(destination.layers().empty());CHECK(copied.redo(destination));
    tree=doc.tree();CHECK(tree.reparent(std::array{first.id},{group,0}));CHECK(doc.replaceStructure(doc.tree(),tree));
    CHECK(sample(doc).alpha==255); // Explicit reorder chooses a new base.
    tree=doc.tree();CHECK(tree.dissolve(group));CHECK(doc.replaceStructure(doc.tree(),tree));CHECK(doc.tree().containers.empty());
    // Empty and one-child groups have ordinary pass-through semantics.
    tree=doc.tree();tree.roots={group,first.id,second.id};tree.containers.push_back({group,"Empty",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{base.id}});
    CHECK(doc.replaceStructure(doc.tree(),tree));const auto one=u::flattenDocument(doc).image;
    tree.container(group)->kind=c::ContainerKind::Group;CHECK(doc.replaceStructure(doc.tree(),tree));CHECK(u::flattenDocument(doc).image==one);
    {
        c::Document emptyBase({{8,8},96});auto paint=pixels({250,20,40,255});emptyBase.insertLayer(0,paint);
        auto hierarchy=emptyBase.tree();const auto outer=c::makeLayerId(),empty=c::makeLayerId();
        hierarchy.roots={outer};hierarchy.containers={{outer,"Clip",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{empty,paint.id}},
            {empty,"Empty base",c::ContainerKind::Folder,c::ColorLabel::None,{}}};
        CHECK(emptyBase.replaceStructure(emptyBase.tree(),hierarchy));CHECK(sample(emptyBase).alpha==0);
        CHECK(u::flattenLayerItems(emptyBase,std::array{outer}).image.pixelColor(3,3).alpha()==0);
        CHECK(u::flattenLayerItems(emptyBase,std::array{paint.id}).image.pixelColor(3,3).alpha()==255);
        hierarchy.container(outer)->children={empty};hierarchy.roots.push_back(paint.id);
        CHECK(emptyBase.replaceStructure(emptyBase.tree(),hierarchy));CHECK(sample(emptyBase)==c::Rgba8{250,20,40,255});
    }
    {
        // Independent Normal reference: opaque uppers replace color before
        // the shared half coverage and base opacity are applied once.
        for(const auto baseColor:{c::Rgba8{0,0,0,128},c::Rgba8{255,255,255,128}}) {
            c::Document colorTest({{8,8},96});auto b=pixels(baseColor),p=pixels({255,0,0,255});b.opacity=.5F;
            colorTest.insertLayer(0,b);colorTest.insertLayer(1,p);auto hierarchy=colorTest.tree();const auto id=c::makeLayerId();
            hierarchy.roots={id};hierarchy.containers={{id,"Clip",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{b.id,p.id}}};
            CHECK(colorTest.replaceStructure(colorTest.tree(),hierarchy));CHECK(sample(colorTest)==c::Rgba8{255,0,0,64});
            c::PinnedDocumentSampler raw(colorTest,p.id,c::ColorSampleSource::ActiveLayer);
            c::PinnedDocumentSampler rendered(colorTest,p.id,c::ColorSampleSource::ActiveLayer,
                c::SampleFiltering::AlphaAware,{},c::ActiveReferenceAppearance::Rendered);
            CHECK(raw.sample({3.5,3.5}).alpha==255);CHECK(rendered.sample({3.5,3.5}).alpha==64);
        }
    }
    QStandardPaths::setTestModeEnabled(true);QSettings().clear();
    {
        c::Document insertion({{8,8},96});auto b=pixels({255,255,255,255});insertion.insertLayer(0,b);
        auto hierarchy=insertion.tree();const auto id=c::makeLayerId();hierarchy.roots={id};
        hierarchy.containers={{id,"Clipping",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{b.id}}};
        CHECK(insertion.replaceStructure(insertion.tree(),hierarchy));c::History edits;
        auto extra=pixels({255,0,0,255});const auto extraId=extra.id;
        CHECK(edits.execute(insertion,std::make_unique<c::AddLayerCommand>(extra,1,id)));
        CHECK(insertion.tree().container(id)->children==std::vector<c::LayerId>{b.id,extraId});
        c::TextLayer words;words.utf8="Editable";auto text=c::Layer::text("Text",words);const auto textId=text.id;
        CHECK(edits.execute(insertion,std::make_unique<c::TextEditCommand>(text,2,b.id,c::TextEditHint{textId,8,8},1)));
        CHECK(insertion.tree().container(id)->children==std::vector<c::LayerId>{b.id,textId,extraId});
        const auto placed=insertion.tree();CHECK(edits.undo(insertion));CHECK(edits.redo(insertion));CHECK(insertion.tree()==placed);
    }
    {
        u::MainWindow window(nullptr,false,false);window.setUnsavedPromptEnabled(false);
        QImage image(8,8,QImage::Format_RGBA8888);image.fill(Qt::red);const auto imagePath=temp.filePath("source.png");CHECK(image.save(imagePath));
        CHECK(window.openImageFromPath(imagePath));auto& session=const_cast<c::EditorSession&>(window.editorSession());auto* document=session.document();
        const auto bottom=document->layers().front().id;
        auto middle=pixels({10,20,30,255}),top=pixels({80,90,100,255});
        CHECK(document->insertLayer(document->layers().size(),middle));CHECK(document->insertLayer(document->layers().size(),top));
        session.setLayerSelection(std::array{top.id,bottom},top.id);
        window.findChild<QAction*>("ClippingGroupAction")->trigger();
        auto id=*session.activeLayer();const auto* container=document->tree().container(id);
        CHECK(container&&container->children==std::vector<c::LayerId>{bottom,top.id});
        CHECK(document->tree().placement(middle.id)->parent==0);
        auto* view=window.findChild<QListView*>("LayerList");auto* model=view?dynamic_cast<u::LayerListModel*>(view->model()):nullptr;CHECK(model);
        if(model){CHECK(model->rowForLayer(bottom)>=0);CHECK(model->index(model->rowForLayer(bottom)).data(u::LayerListModel::ClippingBaseRole).toBool());}
        window.findChild<QAction*>("ClippingGroupAction")->trigger();CHECK(document->tree().container(id)->kind==c::ContainerKind::Group);
        window.findChild<QAction*>("ClippingGroupAction")->trigger();CHECK(document->tree().container(id)->kind==c::ContainerKind::ClippingMaskGroup);
    }
    std::cout<<"Clipping group failures: "<<failures<<'\n';return failures?1:0;
}
