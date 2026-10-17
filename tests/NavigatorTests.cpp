#include "imageeditor/ui/NavigatorPanel.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QStyleOptionSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVulkanInstance>
#include <iostream>
#include <cmath>
#include <vulkan/vulkan.h>

namespace c=imageeditor::core;
namespace u=imageeditor::ui;
int failures=0,validationMessages=0;
#define CHECK(...) do {if(!(__VA_ARGS__)){++failures;std::cerr<<__LINE__<<": "<<#__VA_ARGS__<<'\n';}}while(false)
bool wait(const std::function<bool()>& condition,int timeout=10000)
{
    QElapsedTimer clock;clock.start();
    while(!condition() && clock.elapsed()<timeout)QTest::qWait(5);
    return condition();
}
bool near(double a,double b,double tolerance=.01){return std::abs(a-b)<tolerance;}
c::Layer raster(c::Extent2u extent,c::Rgba8 color)
{return c::Layer::raster("Pixels",std::make_shared<c::ContiguousRasterSurface>(extent,color));}
void sendMouse(QWidget* widget,QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons)
{
    QMouseEvent event(type,point,widget->mapToGlobal(point),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(widget,&event);
}
void previewEvaluation()
{
    c::Document doc({{1024,512},96});
    auto layer=raster({1024,512},{120,30,70,180});
    const auto id=layer.id;CHECK(doc.insertLayer(0,layer));
    c::ShapeLayer shape;shape.kind=c::ShapeKind::Rectangle;shape.size={200,100};shape.fillColor={0,240,0,255};
    auto vector=c::Layer::shape("Shape",shape);vector.localToDocument.m02=300;vector.localToDocument.m12=200;
    CHECK(doc.insertLayer(1,vector));
    auto native=u::flattenDocument(doc);auto overview=u::renderDocumentOverview(doc,{256,128});
    CHECK(native && overview);
    CHECK(overview.image.pixelColor(100,60)==QColor(0,240,0));
    CHECK(overview.image.pixelColor(5,5)==native.image.pixelColor(20,20));
    CHECK(!u::renderDocumentOverview(doc,{513,128}));
    CHECK(u::renderDocumentOverview(doc,{256,128},[](auto,auto){return false;}).cancelled);

    u::PixelPreview preview(nullptr,u::PixelPreview::Purpose::Navigator);
    std::shared_ptr<const c::RasterSurface> output;QString error;
    preview.onReady=[&](auto value,QString why){output=std::move(value);error=why;};
    preview.setDocumentInstance(1);preview.setEnabled(true);preview.request(doc.snapshot());
    CHECK(wait([&]{return !preview.busy();}));CHECK(error.isEmpty() && output);
    if(output)CHECK(output->extent()==c::Extent2u{512,256});
    const auto renders=preview.completedRenders(),bytes=preview.frozenBytes();
    auto snapshot=doc.snapshot();snapshot.documentRevision+=10;snapshot.selectionRevision+=1;
    snapshot.layersBottomToTop[0].name="Renamed";preview.request(snapshot);
    QTest::qWait(100);CHECK(preview.completedRenders()==renders && preview.frozenBytes()==bytes);
    CHECK(doc.setLayerOpacity(id,.25F));preview.request(doc.snapshot());
    CHECK(wait([&]{return !preview.busy();}));CHECK(preview.completedRenders()==renders+1);
    // Duplicate local IDs must not let another runtime document reuse the image.
    c::Document second({{64,32},96});auto blue=raster({64,32},{10,20,230,255});blue.id=id;
    CHECK(second.insertLayer(0,blue));
    CHECK(doc.setLayerOpacity(id,.75F));preview.request(doc.snapshot());
    preview.setDocumentInstance(2);preview.request(second.snapshot());
    CHECK(wait([&]{return !preview.busy();}));CHECK(output && error.isEmpty());
    if(output)CHECK(output->extent()==c::Extent2u{64,32});
    preview.setEnabled(false);const auto hidden=preview.completedRenders();preview.request(doc.snapshot());
    QTest::qWait(100);CHECK(preview.completedRenders()==hidden && !output);
}
void widgetMapping()
{
    u::NavigatorPanel panel;panel.resize(320,180);panel.show();
    c::Document doc({{1000,500},96});CHECK(doc.insertLayer(0,raster({1000,500},{60,110,180,255})));
    panel.setDocument(4,doc.snapshot());
    c::ViewportState view;view.setZoom(2);const c::Extent2d viewport{800,600};const QRectF area(200,20,500,500);
    auto refresh=[&]{panel.setView(view,viewport,area);};refresh();
    panel.onNavigate=[&](c::Vec2d p){const auto pos=view.documentToViewport(p,{1000,500},viewport);
        view.panBy({area.center().x()-pos.x,area.center().y()-pos.y});refresh();};
    panel.onZoom=[&](double zoom){view.zoomAround({area.center().x(),area.center().y()},zoom/view.zoom(),{1000,500},viewport);refresh();};
    CHECK(wait([&]{return panel.completedRenders()>0;}));
    CHECK(near(panel.imageRect().width()/panel.imageRect().height(),2));
    CHECK(near(panel.visibleRect().width()/panel.imageRect().width(),.25));
    auto* canvas=panel.findChild<QWidget*>("NavigatorView");
    const auto initial=view.pan();const auto start=panel.visibleRect().center();
    sendMouse(canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    CHECK(near(view.pan().x,initial.x) && near(view.pan().y,initial.y)); // No jump inside outline.
    sendMouse(canvas,QEvent::MouseMove,start+QPointF(20,0),Qt::NoButton,Qt::LeftButton);
    sendMouse(canvas,QEvent::MouseButtonRelease,start+QPointF(20,0),Qt::LeftButton,Qt::NoButton);
    CHECK(near(view.pan().x,initial.x-20/panel.imageRect().width()*1000*2));
    const auto target=panel.imageRect().topLeft()+QPointF(panel.imageRect().width()*.1,panel.imageRect().height()*.2);
    QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,target.toPoint());
    auto center=view.viewportToDocument({area.center().x(),area.center().y()},{1000,500},viewport);
    CHECK(std::abs(center.x-100)<5 && std::abs(center.y-100)<5);
    auto* zoom=panel.findChild<QSlider*>("NavigatorZoom");zoom->setValue(0);CHECK(near(view.zoom(),.01));
    zoom->setValue(1000);CHECK(near(view.zoom(),64));
    const auto after=view.viewportToDocument({area.center().x(),area.center().y()},{1000,500},viewport);
    CHECK(near(center.x,after.x) && near(center.y,after.y));
    // Keep the real viewport outline beyond the image, rather than drawing a
    // false edge on the canvas border. Pasteboard is also a navigation target.
    view.setZoom(2);
    // Keep the off-canvas edge inside the preview even when the footer gives
    // the image more room and reduces horizontal letterboxing.
    const auto offCanvasCenter=area.width()/(2*view.zoom())
        -panel.imageRect().left()*.5/panel.imageRect().width()*1000;
    panel.onNavigate({offCanvasCenter,100});
    const auto outline=panel.visibleRect(),image=panel.imageRect();
    CHECK(outline.left()<image.left() && outline.left()>0);
    const auto painted=canvas->grab().toImage();const auto dpr=painted.devicePixelRatio();
    bool outsideAccent=false;
    for(int x=int(std::floor(outline.left()*dpr))-2;x<=int(std::ceil(outline.left()*dpr))+2;++x)
        if(x>=0 && x<painted.width() && x<image.left()*dpr)
            outsideAccent|=painted.pixelColor(x,int(outline.center().y()*dpr))==u::themeColor(u::ThemeColor::Accent);
    CHECK(outsideAccent);
    const auto pasteboard=QPointF((outline.left()+image.left())/2,outline.center().y());
    const auto startingPan=view.pan();
    sendMouse(canvas,QEvent::MouseButtonPress,pasteboard,Qt::LeftButton,Qt::LeftButton);
    CHECK(near(view.pan().x,startingPan.x)); // Inside an off-canvas outline: no jump.
    const auto end=pasteboard-QPointF(image.width(),0);
    sendMouse(canvas,QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
    sendMouse(canvas,QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
    const auto outside=view.viewportToDocument({area.center().x(),area.center().y()},{1000,500},viewport);
    CHECK(near(outside.x,offCanvasCenter-1000)); // Not clamped at document X=0.
    int resetCount=0;panel.onResetView=[&]{++resetCount;};
    auto* reset=panel.findChild<QToolButton*>("NavigatorResetView");
    CHECK(reset && reset->isEnabled() && !reset->icon().isNull());
    CHECK(reset->iconSize()==QSize(16,16));
    auto* percentage=panel.findChild<QWidget*>("NavigatorZoomPercent");
    CHECK(percentage && zoom->geometry().right()<percentage->geometry().left());
    CHECK(percentage && percentage->geometry().right()<reset->geometry().left());
    CHECK(zoom->height()==22 && reset->size()==QSize(22,22));
    CHECK(panel.height()-canvas->geometry().bottom()-1<=26);
    CHECK(std::abs(zoom->geometry().center().y()-reset->geometry().center().y())<=1);
    CHECK(std::abs(percentage->geometry().center().y()-reset->geometry().center().y())<=1);
    QStyleOptionSlider sliderStyle;sliderStyle.initFrom(zoom);sliderStyle.orientation=Qt::Horizontal;
    sliderStyle.state|=QStyle::State_Horizontal;
    sliderStyle.minimum=zoom->minimum();sliderStyle.maximum=zoom->maximum();
    sliderStyle.sliderPosition=zoom->value();sliderStyle.sliderValue=zoom->value();
    const auto groove=zoom->style()->subControlRect(QStyle::CC_Slider,&sliderStyle,QStyle::SC_SliderGroove,zoom);
    const auto handle=zoom->style()->subControlRect(QStyle::CC_Slider,&sliderStyle,QStyle::SC_SliderHandle,zoom);
    CHECK(groove.height()==2 && handle.width()==12 && handle.height()==12);
    const auto oldZoom=zoom->value();
    sendMouse(zoom,QEvent::MouseButtonPress,handle.center(),Qt::LeftButton,Qt::LeftButton);
    sendMouse(zoom,QEvent::MouseMove,handle.center()+QPoint(16,0),Qt::NoButton,Qt::LeftButton);
    sendMouse(zoom,QEvent::MouseButtonRelease,handle.center()+QPoint(16,0),Qt::LeftButton,Qt::NoButton);
    CHECK(zoom->value()>oldZoom);
    const auto draggedZoom=zoom->value();QTest::keyClick(zoom,Qt::Key_Left);
    CHECK(zoom->value()<draggedZoom);
    QTest::mouseClick(reset,Qt::LeftButton);CHECK(resetCount==1);
    const auto renders=panel.completedRenders();QTest::qWait(150);CHECK(panel.completedRenders()==renders);
    panel.hide();CHECK(doc.setLayerOpacity(doc.layers()[0].id,.4F));panel.setDocument(4,doc.snapshot());
    QTest::qWait(120);CHECK(panel.completedRenders()==renders);
    panel.onSourceRequested=[&]{panel.setDocument(4,doc.snapshot());};panel.show();
    CHECK(wait([&]{return panel.completedRenders()>renders;}));
    panel.setDocument(0,{});CHECK(!zoom->isEnabled() && !reset->isEnabled() && panel.imageRect().isEmpty());
}
void editorIntegration(QVulkanInstance* instance,const QString& output)
{
    QTemporaryDir files;
    QImage source(1200,800,QImage::Format_RGBA8888);source.fill(QColor(38,52,73));
    {QPainter p(&source);p.setRenderHint(QPainter::Antialiasing);
        for(int y=0;y<4;++y)for(int x=0;x<6;++x){p.setBrush(QColor::fromHsv((x*45+y*18)%360,170,230));
            p.setPen(Qt::NoPen);p.drawRoundedRect(QRectF(x*200+20,y*200+20,160,160),30,30);}}
    const auto path=files.filePath("Navigator.png");CHECK(source.save(path));
    u::MainWindow window(instance,false);window.setUnsavedPromptEnabled(false);window.resize(1600,1000);window.show();
    CHECK(window.openImageFromPath(path));
    auto* navigator=static_cast<u::NavigatorPanel*>(window.findChild<QWidget*>("NavigatorPanel"));
    auto* shell=static_cast<u::WorkspacePanel*>(window.findChild<QWidget*>("NavigatorPanelShell"));
    auto* layers=static_cast<u::WorkspacePanel*>(window.findChild<QWidget*>("LayersPanel"));
    u::OverlayDockWorkspace* workspace=nullptr;
    // Find by type without requiring Q_OBJECT on these lightweight widgets.
    for(auto* widget:window.findChildren<QWidget*>())if(auto* found=dynamic_cast<u::OverlayDockWorkspace*>(widget))workspace=found;
    imageeditor::render::CanvasWindow* canvas=nullptr;
    for(auto* w:QGuiApplication::allWindows())if(auto* c=dynamic_cast<imageeditor::render::CanvasWindow*>(w))canvas=c;
    CHECK(navigator && shell && layers && workspace && canvas);
    if(!navigator || !workspace || !canvas)return;
    CHECK(wait([&]{return navigator->completedRenders()>0;}));QTest::qWait(200);
    CHECK(workspace->dockedPanelIndex(shell)==workspace->dockedPanelIndex(layers)+1);
    CHECK(shell->height()>=150 && shell->height()<=225);
    const auto* document=window.editorSession().document();const auto revision=document->revision();
    const auto saved=document->contentState(),history=window.editorSession().history().undoDepth();
    const auto first=window.activeDocumentId(),renders=navigator->completedRenders();
    const auto uploads=canvas->rendererStats().uploadedBytes;
    navigator->findChild<QSlider*>("NavigatorZoom")->setValue(700);
    CHECK(near(canvas->zoom(),.01*std::pow(6400.0,.7)));
    canvas->setZoomAt(3,navigator->viewAnchor());
    const auto beforeDrag=canvas->scene().viewport.pan();
    auto* navView=navigator->findChild<QWidget*>("NavigatorView");
    auto point=navigator->imageRect().center();
    sendMouse(navView,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
    sendMouse(navView,QEvent::MouseMove,point+QPointF(20,10),Qt::NoButton,Qt::LeftButton);
    sendMouse(navView,QEvent::MouseButtonRelease,point+QPointF(20,10),Qt::LeftButton,Qt::NoButton);
    QTest::qWait(150);const auto pan=canvas->scene().viewport.pan();
    CHECK(!near(pan.x,beforeDrag.x) && !near(pan.y,beforeDrag.y));
    CHECK(document->revision()==revision && document->contentState()==saved);
    CHECK(window.editorSession().history().undoDepth()==history);
    CHECK(navigator->completedRenders()==renders && canvas->rendererStats().uploadedBytes==uploads);
    CHECK(window.openImageFromPath(path));CHECK(window.activeDocumentId()!=first);
    CHECK(window.activateDocument(first));CHECK(near(canvas->zoom(),3));
    CHECK(near(canvas->scene().viewport.pan().x,pan.x) && near(canvas->scene().viewport.pan().y,pan.y));
    CHECK(wait([&]{return navigator->completedRenders()>renders;}));
    const auto beforeReset=navigator->completedRenders();
    QTest::mouseClick(navigator->findChild<QToolButton*>("NavigatorResetView"),Qt::LeftButton);
    c::ViewportState expected;expected.fit({1200,800},{double(canvas->width()),double(canvas->height())},52);
    CHECK(near(canvas->zoom(),expected.zoom()));
    CHECK(near(canvas->scene().viewport.pan().x,0) && near(canvas->scene().viewport.pan().y,0));
    QTest::qWait(100);CHECK(navigator->completedRenders()==beforeReset);
    CHECK(document->revision()==revision && document->contentState()==saved);
    CHECK(window.editorSession().history().undoDepth()==history);
    // Show an outline crossing the top and bottom of the document in the preview.
    canvas->setZoomAt(1,navigator->viewAnchor());
    workspace->setPanelsSuppressed(true);CHECK(!navigator->isVisible());
    workspace->setPanelsSuppressed(false);CHECK(navigator->isVisible());
    workspace->tabifyPanel(shell,layers);workspace->activatePanel(layers);CHECK(!navigator->isVisible());
    workspace->activatePanel(shell);CHECK(navigator->isVisible());
    workspace->detachPanel(shell);workspace->floatPanel(shell,QRect(430,90,330,220));
    CHECK(!shell->isWindow() && navigator->isVisible());
    workspace->dockPanel(shell,u::OverlayDockWorkspace::PanelDockSide::Left,1);
    workspace->setDockedPanelHeight(shell,220);
    QTest::qWait(500);
    if(!output.isEmpty())CHECK(shell->grab().save(output));
    if(instance){const auto stats=canvas->rendererStats();CHECK(stats.framesSubmitted>0 && stats.compositionError.empty());
        const auto count=navigator->completedRenders();QTest::qWait(500);
        CHECK(canvas->rendererStats().framesSubmitted==stats.framesSubmitted);
        CHECK(navigator->completedRenders()==count);
        std::cout<<"GPU "<<stats.deviceName<<" idle frame/upload delta "
            <<canvas->rendererStats().framesSubmitted-stats.framesSubmitted<<'/'
            <<canvas->rendererStats().uploadedBytes-stats.uploadedBytes<<'\n';}
    CHECK(window.closeDocument(first));
}
void benchmark()
{
    for(auto extent:{c::Extent2u{3840,2160},c::Extent2u{5120,2880}}) {
        c::Document doc({extent,96});
        for(int i=0;i<3;++i) {
            auto layer=raster(extent,{std::uint8_t(50+i*45),70,170,180});
            layer.opacity=.6F;CHECK(doc.insertLayer(doc.layers().size(),layer));
        }
        u::PixelPreview preview(nullptr,u::PixelPreview::Purpose::Navigator);QString error;
        preview.onReady=[&](auto,QString why){error=why;};preview.setEnabled(true);
        QElapsedTimer timer;timer.start();preview.request(doc.snapshot());CHECK(wait([&]{return !preview.busy();},20000));
        CHECK(error.isEmpty());const auto cold=timer.elapsed();const auto profile=preview.lastProfile();
        const auto count=preview.completedRenders();timer.restart();
        for(int i=0;i<1000;++i)preview.request(doc.snapshot());
        const auto warm=timer.elapsed();CHECK(preview.completedRenders()==count && !preview.busy());
        std::cout<<"Navigator "<<extent.width<<'x'<<extent.height<<" three-layer cold_ms="<<cold
            <<" freeze_ms="<<profile.freezeMs<<" sampling_encode_ms="
            <<profile.evaluation.samplingBlendMs+profile.evaluation.encodingMs
            <<" frozen_bytes="<<preview.frozenBytes()<<" output_bytes="<<profile.evaluation.outputBytes
            <<" warm_1000_requests_ms="<<warm<<'\n';
    }
}
int main(int argc,char** argv)
{
    QApplication app(argc,argv);QTemporaryDir config;QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,config.path());
    app.setOrganizationName("NavigatorTest");app.setApplicationName("NavigatorTest");QStandardPaths::setTestModeEnabled(true);
    u::i18n::Localization::instance().initialize();u::applyEditorTheme(app);
    QVulkanInstance vk;QVulkanInstance* instance=nullptr;
    if(app.arguments().contains("--native")){
        vk.setApiVersion(QVersionNumber(1,3));vk.setLayers({"VK_LAYER_KHRONOS_validation"});
        CHECK(vk.create());instance=&vk;
        vk.installDebugOutputFilter([](VkDebugReportFlagsEXT flags,VkDebugReportObjectTypeEXT,uint64_t,size_t,int32_t,const char*,const char* text){
            if(flags&(VK_DEBUG_REPORT_ERROR_BIT_EXT|VK_DEBUG_REPORT_WARNING_BIT_EXT)){++validationMessages;std::cerr<<text<<'\n';}return false;});
    }
    previewEvaluation();widgetMapping();editorIntegration(instance,qEnvironmentVariable("VULKANA_NAVIGATOR_SCREENSHOT"));
    if(app.arguments().contains("--benchmark"))benchmark();
    CHECK(validationMessages==0);
    std::cout<<"Navigator failures "<<failures<<" validation messages "<<validationMessages<<'\n';return failures?1:0;
}
