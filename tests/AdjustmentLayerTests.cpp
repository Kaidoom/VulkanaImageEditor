#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/PsdExport.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/PdfExport.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QVulkanInstance>
#include <QElapsedTimer>
#include <QThread>
#include <QComboBox>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QApplication>
#include <QAction>
#include <QTemporaryDir>
#include <QTabWidget>
#include <iostream>

namespace c=imageeditor::core;namespace u=imageeditor::ui;
int failures=0;
#define CHECK(...) do{if(!(__VA_ARGS__)){++failures;std::cerr<<__LINE__<<": " #__VA_ARGS__ "\n";}}while(false)
c::Layer pixels(c::Rgba8 color){return c::Layer::raster("Pixels",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{8,8},color));}
c::AdjustmentState exposure(double stops) {
    auto state=std::make_shared<c::AdjustmentStack>();auto& a=state->items[0];a.enabled=true;a.parameters=c::ExposureParameters{stops};return state;
}
c::PremultipliedColor sample(const c::Document& d) {return c::PinnedDocumentSampler(d,{},c::ColorSampleSource::MergedVisible).sampleLinear({3.5,3.5});}
bool close(c::PremultipliedColor a,c::PremultipliedColor b) {for(size_t i=0;i<4;++i)if(std::abs(a[i]-b[i])>1e-6F)return false;return true;}
template<class F> bool waitFor(F predicate) {
    QElapsedTimer timer;timer.start();while(!predicate()&&timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(1);}return predicate();
}
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);QTemporaryDir temp;QVulkanInstance vulkan;
    const bool native=app.arguments().contains("--native");
    if(native) {
        vulkan.setApiVersion(QVersionNumber(1,2));vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
        CHECK(vulkan.create());if(!vulkan.isValid())return 1;
        vulkan.installDebugOutputFilter([](auto severity,auto type,const void* data){
            if(type.testFlag(QVulkanInstance::ValidationMessage)&&(severity.testFlag(QVulkanInstance::WarningSeverity)||severity.testFlag(QVulkanInstance::ErrorSeverity))) {
                ++failures;std::cerr<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(data)->pMessage<<'\n';
            }return false;
        });
    }
    c::Document d({{8,8},96});auto lower=pixels({60,100,160,128}),upper=pixels({180,60,30,128});
    d.insertLayer(0,lower);d.insertLayer(1,upper);
    const auto original=sample(d);
    auto a=c::Layer::adjustment("Exposure");a.adjustments=exposure(1);
    CHECK(d.insertLayer(2,a));
    const auto corrected=sample(d);
    // Independent exposure oracle: linear RGB doubles; alpha is invariant.
    for(size_t i=0;i<3;++i)CHECK(std::abs(corrected[i]-std::min(original[i]*2,original[3]))<1e-6);
    CHECK(corrected[3]==original[3]);
    CHECK(!c::intrinsicSurface(*d.layer(a.id)));
    CHECK(c::sampleDocumentColor(d,{}, {3.5,3.5},c::ColorSampleSource::MergedVisible).color==c::encodeColor(corrected));
    for(float opacity:{0.0F,.5F,1.0F}) {
        d.setLayerOpacity(a.id,opacity);const auto color=sample(d);
        CHECK(color[3]==original[3]);
        for(size_t i=0;i<3;++i)CHECK(std::abs(color[i]-(original[i]+(corrected[i]-original[i])*opacity))<1e-6);
    }
    auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({8,8},128);mask->outside=0;
    CHECK(d.setLayerMask(a.id,mask));const auto soft=sample(d);
    for(size_t i=0;i<3;++i)CHECK(std::abs(soft[i]-(original[i]+(corrected[i]-original[i])*128/255.0F))<1e-6);
    CHECK(soft[3]==original[3]);
    mask=std::make_shared<c::LayerMask>(*mask);mask->coverage=c::SelectionMask::filled({8,8},0);d.setLayerMask(a.id,mask);
    CHECK(sample(d)==original);
    mask=std::make_shared<c::LayerMask>(*mask);mask->enabled=false;d.setLayerMask(a.id,mask);CHECK(sample(d)==corrected);
    d.setLayerMask(a.id,{});
    d.setLayerVisibility(a.id,false);CHECK(sample(d)==original);d.setLayerVisibility(a.id,true);
    d.setLayerAdjustments(a.id,{});CHECK(sample(d)==original);d.setLayerAdjustments(a.id,exposure(1));
    c::PinnedDocumentSampler pinned(d,{},c::ColorSampleSource::MergedVisible);
    CHECK(pinned.sampleAdjustmentInput({3.5,3.5},a.id)==original);
    const auto inputKey=c::adjustmentInputKey(d,a.id);
    d.setLayerOpacity(a.id,.3F);CHECK(c::adjustmentInputKey(d,a.id)==inputKey);d.setLayerOpacity(a.id,1);
    std::array<c::PremultipliedColor,8> row;pinned.sampleRow(0,3,row);CHECK(close(row[3],corrected));
    auto image=u::flattenDocument(d);CHECK(image&&image.image.pixelColor(3,3)==QColor(c::encodeColor(corrected).red,c::encodeColor(corrected).green,c::encodeColor(corrected).blue,c::encodeColor(corrected).alpha));
    const auto selected=u::flattenLayerItems(d,std::array{lower.id,a.id});CHECK(selected);
    CHECK(!u::prepareRasterizeLayers(d,{{a.id},a.id,a.id},256ULL*1024*1024).error.isEmpty());
    // Above content is not part of the correction input.
    auto above=pixels({23,45,67,255});d.insertLayer(d.layers().size(),above);CHECK(sample(d)==c::decodeColor({23,45,67,255}));
    CHECK(c::adjustmentInputKey(d,a.id)==inputKey);
    CHECK(d.takeLayer(above.id).has_value());
    // Ordering nonlinear corrections differs: invert then expose vs expose then invert.
    auto inv=c::Layer::adjustment("Invert");auto invert=std::make_shared<c::AdjustmentStack>();invert->items[9].enabled=true;inv.adjustments=invert;
    d.insertLayer(3,inv);const auto ordered=sample(d);d.moveLayer(inv.id,2);CHECK(!close(sample(d),ordered));CHECK(d.takeLayer(inv.id).has_value());
    // Local group domains exclude external backdrop, including neutral/bypassed operators.
    c::Document grouped({{8,8},96});auto outside=pixels({40,150,210,255}),inside=pixels({160,70,100,128});inside.blendMode=c::BlendMode::Multiply;
    auto local=c::Layer::adjustment("Local",c::AdjustmentScope::ThisGroup);local.adjustments=exposure(1);
    grouped.insertLayer(0,outside);grouped.insertLayer(1,inside);grouped.insertLayer(2,local);
    auto tree=grouped.tree();const auto group=c::makeLayerId();tree.roots={outside.id,group};
    tree.containers.push_back({group,"Local domain",c::ContainerKind::Folder,c::ColorLabel::None,{inside.id,local.id}});
    CHECK(grouped.replaceStructure(grouped.tree(),tree));
    auto base=c::decodeColor({160,70,100,128});auto program=c::compileAdjustmentStack(local.adjustments);
    auto expected=c::compositeLayer(c::decodeColor({40,150,210,255}),c::evaluateAdjustments(program,base,{3.5,3.5}),1,c::BlendMode::Normal);
    CHECK(close(sample(grouped),expected));
    grouped.setLayerVisibility(local.id,false);
    CHECK(close(sample(grouped),c::compositeLayer(c::decodeColor({40,150,210,255}),base,1,c::BlendMode::Normal)));
    c::History history;CHECK(history.execute(grouped,std::make_unique<c::SetAdjustmentScopeCommand>(local.id,c::AdjustmentScope::ThisGroup,c::AdjustmentScope::AllBelow)));
    CHECK(!close(sample(grouped),c::compositeLayer(c::decodeColor({40,150,210,255}),base,1,c::BlendMode::Normal)));
    CHECK(history.undo(grouped));grouped.setLayerVisibility(local.id,true);
    {
        // Containing-group thumbnails use the same operator plan, not just
        // drawable children. The center avoids the lower-right group badge.
        auto copy=std::make_unique<c::Document>(grouped);auto tree=copy->tree();tree.container(group)->kind=c::ContainerKind::Group;
        CHECK(copy->replaceStructure(copy->tree(),tree));c::EditorSession session;session.replaceDocument(std::move(copy));
        u::LayerListModel model;model.setSession(&session);
        const auto row=model.rowForLayer(group);CHECK(row>=0);
        const auto image=model.data(model.index(row),Qt::DecorationRole).value<QIcon>().pixmap(120,120).toImage();
        CHECK(!image.isNull());const auto color=c::encodeColor(sample(*session.document()));
        if(!image.isNull())CHECK(image.pixelColor(50,50)==QColor(color.red,color.green,color.blue,color.alpha));
    }
    // Scope, mask, and corrections persist without raster payload for the operator.
    const auto path=temp.filePath("adjustments.vulkana");CHECK(u::saveProject(path,grouped));auto loaded=u::loadProject(path);
    CHECK(loaded&&close(sample(*loaded.document),sample(grouped)));
    CHECK(loaded&&std::get<c::AdjustmentLayer>(loaded.document->layer(local.id)->payload).scope==c::AdjustmentScope::ThisGroup);
    auto transfer=c::captureLayerTransfer(grouped,std::array{group});c::Document destination(grouped.canvas());
    auto insert=c::insertLayerTransfer(destination,std::move(transfer),{0,0},{});c::History h;CHECK(h.execute(destination,std::move(insert)));
    CHECK(destination.layers().size()==2&&destination.layers().back().id!=local.id);
    CHECK(std::holds_alternative<c::AdjustmentLayer>(destination.layers().back().payload));
    const auto bytes=c::retainedLayerMemory(*grouped.layer(local.id));CHECK(bytes<65536);
    {
        c::AdjustmentEditTransaction edit(grouped,local.id);const auto before=sample(grouped);
        CHECK(edit.update(exposure(2)));CHECK(edit.cancel());CHECK(sample(grouped)==before);
    }
    c::Document alone({{8,8},96});alone.insertLayer(0,a);CHECK(sample(alone)==c::PremultipliedColor{});
    CHECK(u::flattenDocument(alone).image.pixelColor(3,3).alpha()==0);
    {
        u::AdjustmentsPanel panel;panel.setTarget(d.layer(a.id),false,&d,1);panel.show();
        auto* navigation=panel.findChild<QComboBox*>("AdjustmentNavigation0");CHECK(navigation);navigation->setCurrentIndex(3);
        panel.requestHistogram(d.layer(a.id));CHECK(waitFor([&]{return panel.completedHistogramJobs()==1;}));
        d.insertLayer(d.layers().size(),above);panel.requestHistogram(d.layer(a.id));QCoreApplication::processEvents();
        CHECK(panel.completedHistogramJobs()==1);
        auto own=std::make_shared<c::AdjustmentStack>(*d.layer(a.id)->adjustments);own->items[3].enabled=true;
        std::get<c::CurvesParameters>(own->items[3].parameters).channels[0].points[0].output=.1;
        d.setLayerAdjustments(a.id,own);panel.setTarget(d.layer(a.id),false,&d,1);panel.requestHistogram(d.layer(a.id));QCoreApplication::processEvents();
        CHECK(panel.completedHistogramJobs()==1);
        d.setLayerAdjustments(a.id,exposure(2));panel.setTarget(d.layer(a.id),false,&d,1);panel.requestHistogram(d.layer(a.id));
        CHECK(waitFor([&]{return panel.completedHistogramJobs()==2;}));
        CHECK(d.takeLayer(above.id).has_value());d.setLayerAdjustments(a.id,exposure(1));
    }
    {
        c::Document clipping({{8,8},96});auto outside=pixels({45,80,140,255});
        auto base=pixels({120,70,30,128});auto op=c::Layer::adjustment("Clipped correction");op.adjustments=exposure(1);
        clipping.insertLayer(0,outside);clipping.insertLayer(1,base);clipping.insertLayer(2,op);
        auto tree=clipping.tree();const auto group=c::makeLayerId();tree.roots={outside.id,group};
        tree.containers.push_back({group,"Clip",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{base.id,op.id}});
        CHECK(clipping.replaceStructure(clipping.tree(),tree));
        const auto original=c::decodeColor({120,70,30,128});auto adjusted=original;for(size_t i=0;i<3;++i)adjusted[i]*=2;
        CHECK(close(sample(clipping),c::compositeLayer(c::decodeColor({45,80,140,255}),adjusted,1,c::BlendMode::Normal)));
        tree=clipping.tree();std::swap(tree.container(group)->children[0],tree.container(group)->children[1]);
        CHECK(clipping.replaceStructure(clipping.tree(),tree));
        CHECK(sample(clipping)==c::decodeColor({45,80,140,255})); // Operator is not an opaque base.
        auto rendered=u::flattenDocument(clipping);CHECK(rendered&&rendered.image.pixelColor(3,3)==QColor(45,80,140,255));
    }
    {
        auto paintMask=std::make_shared<c::LayerMask>();paintMask->coverage=c::SelectionMask::filled({8,8},0);paintMask->outside=0;
        d.setLayerMask(a.id,paintMask);c::History history;const auto lowerRevision=std::get<c::RasterLayer>(d.layer(lower.id)->payload).surface->revision();
        c::LayerMaskEdit edit(d,a.id,"Paint adjustment mask");CHECK(edit.valid());
        auto settings=c::proceduralBrushPreset(c::ProceduralBrushPreset::HardRound);
        settings.foreground={255,255,255,255};settings.sizePixels=8;settings.opacity=1;settings.flow=1;
        settings.pressureToSize=false;settings.pressureToFlow=false;
        c::RasterEditTransactionOptions options;options.coverageValues=true;
        c::BasicPixelBrushStroke stroke(edit.proxy(),a.id,settings,c::BrushCompositeMode::MaskCoverage,
            std::make_unique<c::BasicPixelBrushEngine>(),{},{},nullptr,options);
        c::NormalizedPointerSample pointer{.documentPosition={3.5,3.5},.pressure=1,.buttons=c::PointerButtonPrimary};
        CHECK(stroke.begin(pointer));CHECK(stroke.end(pointer,edit.provisionalHistory())==c::RasterEditCommitResult::Committed);
        CHECK(edit.commit(history)==c::RasterEditCommitResult::Committed);CHECK(sample(d)!=original);
        CHECK(history.undo(d));CHECK(sample(d)==original);CHECK(history.redo(d));CHECK(sample(d)!=original);
        CHECK(std::get<c::RasterLayer>(d.layer(lower.id)->payload).surface->revision()==lowerRevision);
        d.setLayerMask(a.id,{});
    }
    {
        u::PixelPreview preview;std::shared_ptr<const c::RasterSurface> result;QString error;
        preview.onReady=[&](auto surface,auto message){result=std::move(surface);error=message;};
        preview.setDocumentInstance(101);preview.setEnabled(true);preview.request(grouped.snapshot());
        CHECK(waitFor([&]{return bool(result)||!error.isEmpty();}));CHECK(error.isEmpty());
        if(result) {
            auto reference=u::flattenDocument(grouped);std::array<std::byte,4> bytes;
            result->copyRgba8({3,3,1,1},bytes,4);
            CHECK(reference&&std::equal(bytes.begin(),bytes.end(),reinterpret_cast<const std::byte*>(reference.image.constScanLine(3))+12));
        }
        // Identical serialized IDs in a different live document must not reuse its output.
        auto other=*loaded.document;other.setLayerAdjustments(local.id,exposure(-2));
        preview.setDocumentInstance(102);preview.request(other.snapshot());
        CHECK(waitFor([&]{return bool(result)||!error.isEmpty();}));CHECK(error.isEmpty());
        if(result){std::array<std::byte,4> bytes;result->copyRgba8({3,3,1,1},bytes,4);auto reference=u::flattenDocument(other);
            CHECK(reference&&std::equal(bytes.begin(),bytes.end(),reinterpret_cast<const std::byte*>(reference.image.constScanLine(3))+12));}
        preview.setDocumentInstance(101);preview.request(grouped.snapshot());CHECK(result);
    }
    {
        // Real adjustment records, not a saved-scene raster masquerading as a layer.
        const auto output=qEnvironmentVariable("VULKANA_ADJUSTMENT_FIXTURES",temp.path());QDir().mkpath(output);
        std::atomic_bool cancel=false;
        for(auto* doc:{&d,&grouped}) {
            const auto snapshot=u::capturePsdExport(*doc,321);
            const auto plan=u::planPsdExport(snapshot,{},cancel);CHECK(plan);
            const auto file=output+(doc==&d?"/global.psd":"/scoped.psd");
            const auto written=u::writePsdExport(snapshot,plan,file,cancel);CHECK(written);
            if(!written)std::cerr<<written.error.toStdString()<<'\n';
            const auto inspection=u::inspectPsd(file,{});CHECK(inspection.error.isEmpty());
            if(!inspection.error.isEmpty())std::cerr<<inspection.error.toStdString()<<'\n';
            const auto imported=u::convertPsd(inspection,u::defaultPsdOptions(inspection),{});
            CHECK(imported.document);
            if(!imported.document)std::cerr<<imported.error.toStdString()<<'\n';
            if(imported.document)CHECK(close(sample(*imported.document),sample(*doc)));
        }
        auto combined=std::make_shared<c::AdjustmentStack>(*d.layer(a.id)->adjustments);combined->items[9].enabled=true;
        d.setLayerAdjustments(a.id,combined);
        const auto snapshot=u::capturePsdExport(d,321);
        CHECK(!u::planPsdExport(snapshot,{},cancel));
        u::PsdExportOptions flat;flat.mode=u::PsdExportMode::Flattened;CHECK(u::planPsdExport(snapshot,flat,cancel));
        d.setLayerAdjustments(a.id,exposure(1));
        const auto pdf=u::capturePdfExport(d,321,{});CHECK(pdf.entries.size()==2);
        u::PdfExportOptions options;options.mode=u::PdfExportMode::Pages;options.ppi=96;
        const auto pages=u::planPdfExport(pdf,options,cancel);CHECK(pages.pages.size()==2);
        for(const auto& page:pages.pages)CHECK(std::ranges::find(page.leaves,a.id)!=page.leaves.end());
        const auto pdfImage=u::renderPdfExportPage(pdf,pages,1,cancel);
        CHECK(pdfImage&&selected&&pdfImage.image.size()==selected.image.size());
        if(pdfImage&&selected)for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
            CHECK(pdfImage.image.pixelColor(x,y)==selected.image.pixelColor(x,y));
        }
        d.insertLayer(d.layers().size(),inv);
        const auto inverseSnapshot=u::capturePsdExport(d,321);const auto inversePlan=u::planPsdExport(inverseSnapshot,{},cancel);CHECK(inversePlan);
        CHECK(u::writePsdExport(inverseSnapshot,inversePlan,output+"/invert.psd",cancel));CHECK(d.takeLayer(inv.id).has_value());
    }
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("VulkanaAdjustmentLayerTests");QCoreApplication::setApplicationName("VulkanaAdjustmentLayerTests");QSettings().clear();
    {
        u::applyEditorTheme(app);
        u::MainWindow window(native?&vulkan:nullptr,false,false);window.setUnsavedPromptEnabled(false);
        if(native){window.resize(1100,780);window.show();CHECK(waitFor([&]{return window.isVisible();}));}
        QImage im(8,8,QImage::Format_RGBA8888);im.fill(Qt::red);const auto source=temp.filePath("ui.png");CHECK(im.save(source));CHECK(window.openImageFromPath(source));
        auto& session=const_cast<c::EditorSession&>(window.editorSession());
        auto* action=window.findChild<QAction*>("NewAdjustmentLayerAction");CHECK(action);if(action)action->trigger();
        auto* layer=session.document()->layer(*session.activeLayer());CHECK(layer&&std::holds_alternative<c::AdjustmentLayer>(layer->payload));
        CHECK(layer&&layer->mask&&layer->mask->coverage->memoryCost()<65536);
        auto* tabs=window.findChild<QTabWidget*>("AdjustmentTabs");CHECK(tabs&&!tabs->isTabEnabled(3)&&!tabs->isTabEnabled(4));
        CHECK(!window.findChild<QAction*>("ApplyLayerMaskAction")->isEnabled());
        window.findChild<QAction*>("InvertLayerMaskAction")->trigger();CHECK(layer->mask->coverage->coverageAtDocumentPixel(2,2)==0);
        const auto id=layer->id,instance=window.activeDocumentId();
        session.document()->setLayerAdjustments(id,exposure(1));session.setActiveTool(c::ToolId::Brush);
        imageeditor::render::CanvasWindow* canvas=nullptr;
        for(auto* candidate:QGuiApplication::allWindows())if(candidate->objectName()=="VulkanCanvasWindow")
            canvas=dynamic_cast<imageeditor::render::CanvasWindow*>(candidate);
        CHECK(canvas);
        if(canvas) {
            c::NormalizedPointerSample pointer{.documentPosition={3.5,3.5},.pressure=1,.buttons=c::PointerButtonPrimary};
            session.setEditingLayerMask(false);CHECK(!canvas->onBrushStrokeBegan(pointer));
            session.setEditingLayerMask(true);session.setForegroundColor({255,255,255,255});
            CHECK(canvas->onBrushStrokeBegan(pointer));CHECK(canvas->onBrushStrokeEnded(pointer));
        }
        const auto savedMask=session.document()->layer(id)->mask;
        CHECK(window.openImageFromPath(source));CHECK(window.activeDocumentId()!=instance);
        CHECK(window.activateDocument(instance));CHECK(session.document()->layer(id)->mask==savedMask);
        CHECK(window.editorSession().editingLayerMask());
        if(native){QElapsedTimer settle;settle.start();while(settle.elapsed()<350){QCoreApplication::processEvents();QThread::msleep(2);}}
        window.close();
    }
    std::cout<<"Adjustment layer checks: "<<failures<<" failures; platform="<<app.platformName().toStdString()<<"; Vulkan validation="<<(native?"enabled":"separate GPU test")<<'\n';
    return failures?1:0;
}
