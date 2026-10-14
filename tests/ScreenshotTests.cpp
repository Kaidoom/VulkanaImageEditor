#include "imageeditor/ui/ScreenCapture.hpp"
#include "imageeditor/ui/ScreenshotDialog.hpp"
#include "imageeditor/ui/ScreenshotPreview.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <QVulkanInstance>
#ifdef Q_OS_LINUX
#include <QtGui/qguiapplication_platform.h>
#endif
#include <array>
#include <iostream>

namespace u=imageeditor::ui;
namespace c=imageeditor::core;
namespace {
int failures{};bool nativeInputUnavailable{};
#define CHECK(...) do{if(!(__VA_ARGS__)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #__VA_ARGS__ "\n";}}while(false)
template<class F>bool waitFor(F f){QElapsedTimer timer;timer.start();while(!f()&&timer.elapsed()<2500)QTest::qWait(5);return f();}
QImage fixture(int w=400,int h=200)
{
    QImage image(w,h,QImage::Format_RGBA8888);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)image.setPixelColor(x,y,QColor(x%256,y%256,(x+y)%256,255));
    return image;
}
void mouse(u::ScreenshotPreview& view,QEvent::Type type,QPointF point,Qt::MouseButton button=Qt::LeftButton)
{
    QMouseEvent event(type,point,view.mapToGlobal(point),type==QEvent::MouseMove?Qt::NoButton:button,
        type==QEvent::MouseButtonRelease?Qt::NoButton:button,Qt::NoModifier);
    QApplication::sendEvent(&view,&event);
}
void wheel(u::ScreenshotPreview& view,QPointF point,int delta=120)
{
    QWheelEvent event(point,view.mapToGlobal(point),{},QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(&view,&event);
}
QPointF position(u::ScreenshotPreview& view,double x,double y)
{
    const auto r=view.imageRect();return r.topLeft()+QPointF(x*r.width()/view.image().width(),y*r.height()/view.image().height());
}
class Capture final:public u::ScreenCapture {
public:
    using ScreenCapture::ScreenCapture;
    bool usesDesktopChooser() const override{return chooser;}
    void start(quint64 id,Target value) override{request=id;target=value;++calls;}
    void cancel() override{++cancellations;}
    void deliver(QImage image,QString error={},bool cancelled=false,quint64 id=0){emit finished(id?id:request,image,error,cancelled);}
    quint64 request{};int calls{},cancellations{};Target target{};bool chooser{true};
};
void cropTests()
{
    const auto original=fixture();auto highDpi=original;highDpi.setDevicePixelRatio(2);
    u::ScreenshotPreview view;view.resize(613,367);view.setImage(highDpi);view.show();
    CHECK(view.image().size()==original.size()&&view.image().devicePixelRatio()==1);
    CHECK(view.croppedImage()==original);
    mouse(view,QEvent::MouseButtonPress,position(view,40.5,25.5));
    mouse(view,QEvent::MouseMove,position(view,150.5,85.5));
    mouse(view,QEvent::MouseButtonRelease,position(view,150.5,85.5));
    CHECK(view.crop()==QRect(40,25,111,61));CHECK(view.croppedImage()==original.copy(view.crop()));
    const auto saved=view.crop();
    for(const auto size:{QSize(377,253),QSize(900,700),QSize(250,450)}) {
        view.resize(size);view.setLayoutDirection(Qt::RightToLeft);
        CHECK(view.crop()==saved&&view.croppedImage()==original.copy(saved));
    }
    view.resize(700,500);
    mouse(view,QEvent::MouseButtonPress,position(view,90.5,50.5));
    mouse(view,QEvent::MouseMove,position(view,110.5,60.5));
    mouse(view,QEvent::MouseButtonRelease,position(view,110.5,60.5));
    CHECK(view.crop()==saved.translated(20,10));
    const auto moved=view.crop();
    QTest::keyClick(&view,Qt::Key_Left,Qt::ShiftModifier);CHECK(view.crop()==moved.translated(-10,0));
    view.setCrop(saved);
    mouse(view,QEvent::MouseButtonPress,position(view,40.1,25.1));
    mouse(view,QEvent::MouseMove,position(view,20.5,15.5));
    mouse(view,QEvent::MouseButtonRelease,position(view,20.5,15.5));
    CHECK(view.crop()==QRect(20,15,131,71));
    const auto before=view.crop();
    mouse(view,QEvent::MouseButtonPress,position(view,250.5,150.5));
    mouse(view,QEvent::MouseMove,position(view,300.5,175.5));
    QTest::keyClick(&view,Qt::Key_Escape);CHECK(view.crop()==before);
    mouse(view,QEvent::MouseButtonRelease,position(view,300.5,175.5));CHECK(view.crop()==before);
    view.resetCrop();CHECK(view.croppedImage()==original);CHECK(view.image()==original);
    mouse(view,QEvent::MouseButtonPress,position(view,150.5,85.5));
    mouse(view,QEvent::MouseMove,position(view,40.5,25.5));
    mouse(view,QEvent::MouseButtonRelease,position(view,40.5,25.5));CHECK(view.crop()==saved);
    view.resetCrop();
    mouse(view,QEvent::MouseButtonPress,position(view,399.5,199.5));
    mouse(view,QEvent::MouseButtonRelease,position(view,399.5,199.5));CHECK(view.crop()==QRect(399,199,1,1));
    CHECK(view.croppedImage()==original.copy(399,199,1,1));
    // Zoom/pan are inspection state: cursor anchoring and original crop pixels
    // remain exact, independently of preview DPI, widget size and RTL.
    view.setCrop(saved);const auto fitRect=view.imageRect();const auto fitZoom=view.zoom();
    const auto focal=position(view,250.25,122.75);
    wheel(view,focal);CHECK(std::abs(view.zoom()-fitZoom*1.16)<1e-9);
    CHECK(QLineF(position(view,250.25,122.75),focal).length()<1e-9);
    CHECK(view.crop()==saved&&view.croppedImage()==original.copy(saved)&&!view.viewIsFitted());
    const auto beforePan=view.imageRect();const QPointF panPoint(155,180),delta(13.5,-9.25);
    mouse(view,QEvent::MouseButtonPress,panPoint,Qt::MiddleButton);
    mouse(view,QEvent::MouseMove,panPoint+delta,Qt::MiddleButton);
    const auto panZoom=view.zoom();wheel(view,panPoint);CHECK(view.zoom()==panZoom);
    mouse(view,QEvent::MouseButtonRelease,panPoint+delta,Qt::MiddleButton);
    CHECK(QLineF(view.imageRect().topLeft(),beforePan.topLeft()+delta).length()<1e-9);
    CHECK(view.crop()==saved&&view.croppedImage()==original.copy(saved));
    const auto panned=view.imageRect();
    mouse(view,QEvent::MouseButtonPress,panPoint,Qt::MiddleButton);
    mouse(view,QEvent::MouseMove,panPoint+QPointF(60,40),Qt::MiddleButton);
    QTest::keyClick(&view,Qt::Key_Escape);CHECK(view.imageRect()==panned);
    mouse(view,QEvent::MouseButtonRelease,panPoint+QPointF(60,40),Qt::MiddleButton);
    view.resetView();CHECK(view.viewIsFitted()&&view.imageRect()==fitRect&&view.crop()==saved);
    // Precisely crop at high zoom, preserving inclusive source-pixel endpoints.
    wheel(view,position(view,200,100),120*10);view.resetCrop();
    mouse(view,QEvent::MouseButtonPress,position(view,201.5,102.5));
    mouse(view,QEvent::MouseMove,position(view,198.5,99.5));
    mouse(view,QEvent::MouseButtonRelease,position(view,198.5,99.5));
    CHECK(view.crop()==QRect(198,99,4,4));CHECK(view.croppedImage()==original.copy(view.crop()));
    const auto zoom=view.zoom();view.resize(460,290);CHECK(view.zoom()==zoom&&view.crop()==QRect(198,99,4,4));
    for(int i=0;i<12;++i)wheel(view,view.rect().center(),120*20);
    CHECK(view.zoom()==64);view.resetView();CHECK(view.crop()==QRect(198,99,4,4));
    view.setImage(original);CHECK(view.viewIsFitted()&&view.crop()==original.rect());
}
void dialogTests(const QString& artifacts)
{
    QWidget host;host.resize(900,730);host.show();Capture service;
    u::ScreenshotDialog dialog(host,&host,&service);dialog.show();
    auto* capture=dialog.findChild<QPushButton*>("ScreenshotCapture");
    auto* stop=dialog.findChild<QPushButton*>("ScreenshotStop");
    auto* open=dialog.findChild<QPushButton*>("ScreenshotOpen");
    auto* layer=dialog.findChild<QPushButton*>("ScreenshotAddLayer");
    auto* resetZoom=dialog.findChild<QPushButton*>("ScreenshotResetZoom");
    auto* preview=dynamic_cast<u::ScreenshotPreview*>(dialog.findChild<QWidget*>("ScreenshotPreview"));
    CHECK(!dialog.isWindow()&&!dialog.windowHandle());CHECK(capture&&stop&&open&&preview&&layer&&resetZoom);
    CHECK(!layer->isEnabled());dialog.setCanAddLayer(true);CHECK(!layer->isEnabled());
    const auto original=fixture();
    capture->click();CHECK(!capture->isEnabled()&&stop->isEnabled()&&!open->isEnabled());
    CHECK(!layer->isEnabled()&&!resetZoom->isEnabled());
    CHECK(waitFor([&]{return service.calls==1;}));CHECK(service.target==u::ScreenCapture::Target::DesktopChooser);
    service.deliver(original);CHECK(!host.isMinimized()&&open->isEnabled()&&layer->isEnabled());
    dialog.setCanAddLayer(false);CHECK(!layer->isEnabled()&&open->isEnabled());dialog.setCanAddLayer(true);
    preview->setCrop({20,30,120,100});const auto first=dialog.selectedImage();
    CHECK(first==original.copy(20,30,120,100));
    wheel(*preview,preview->rect().center());CHECK(resetZoom->isEnabled());resetZoom->click();
    CHECK(preview->viewIsFitted()&&!resetZoom->isEnabled()&&dialog.selectedImage()==first);
    if(!artifacts.isEmpty()){QDir().mkpath(artifacts);CHECK(dialog.grab().save(QDir(artifacts).filePath("screenshot-review.png")));}
    capture->click();CHECK(waitFor([&]{return service.calls==2;}));const auto old=service.request;
    stop->click();CHECK(!host.isMinimized());service.deliver(fixture(5,5),{},false,old);
    CHECK(dialog.selectedImage()==first);
    capture->click();CHECK(waitFor([&]{return service.calls==3;}));
    service.deliver(fixture(5,5),{},false,old);CHECK(!open->isEnabled());
    service.deliver({}, {}, true);CHECK(open->isEnabled()&&dialog.selectedImage()==first);
    capture->click();CHECK(waitFor([&]{return service.calls==4;}));service.deliver({},"test error");
    CHECK(open->isEnabled()&&dialog.selectedImage()==first);
    capture->click();dialog.reject();QTest::qWait(350);CHECK(service.calls==4&&!host.isMinimized());
    service.deliver(fixture(5,5));CHECK(!dialog.isVisible()&&dialog.selectedImage()==first);
    Capture nativeService;nativeService.chooser=false;
    u::ScreenshotDialog nativeDialog(host,&host,&nativeService);
    CHECK(nativeDialog.findChild<QDoubleSpinBox*>("ScreenshotDelay")->value()==1);
}
void documentTests(QVulkanInstance* instance)
{
    QTemporaryDir files;const auto image=fixture(64,48);CHECK(image.save(files.filePath("input.png")));
    u::MainWindow window(instance,false);window.setUnsavedPromptEnabled(false);window.show();
    if(instance)CHECK(waitFor([&]{return window.windowHandle()->isExposed();}));
#ifdef Q_OS_LINUX
#if QT_CONFIG(wayland)
    if(instance)if(auto* wayland=qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        wayland&&!wayland->lastInputSeat()) {
        // Like the workspace-dialog fixture, obtain a compositor pointer-enter
        // serial. Synthetic clicks alone cannot authorize Wayland restoration.
        window.showMaximized();
        if(!waitFor([&]{return wayland->lastInputSeat()!=nullptr;})) {
            std::cerr<<"SKIP native document workflow: Wayland requires a real pointer-enter serial for restoration.\n";
            nativeInputUnavailable=true;return;
        }
        window.showNormal();CHECK(waitFor([&]{return window.windowHandle()->isExposed()&&!window.isMaximized();}));
    }
#endif
#endif
    CHECK(window.openImageFromPath(files.filePath("input.png")));
    const auto originalId=window.activeDocumentId();const auto* context=window.documentContext(originalId);
    auto& session=const_cast<c::EditorSession&>(window.editorSession());
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(*session.activeLayer(),.5F)));CHECK(session.undo());
    session.document()->setSelection(c::SelectionMask::rectangle({64,48},{2,3,5,6}));
    const auto state=session.document()->contentState(),depth=session.history().undoDepth();
    const auto selection=session.document()->selection();
    // 0: cancel; 1: add to this document; 2: open a separate document.
    for(int output:{0,1,2}) {
        QPointer<Capture> service;u::MainWindow::FileInteractions hooks;
        hooks.createScreenCapture=[&](QObject* parent){service=new Capture(parent);return service.data();};window.setFileInteractions(hooks);
        int stage=0;QTimer response;
        QObject::connect(&response,&QTimer::timeout,&window,[&]{
            auto* dialog=window.findChild<QDialog*>("ScreenshotDialog");if(!dialog||!dialog->isVisible())return;
            // A real user cannot click a card before the compositor has
            // exposed its host after restoration. QWidget visibility alone
            // changes synchronously, before the Wayland surface round trip.
            if(instance&&!window.windowHandle()->isExposed())return;
            CHECK(!dialog->isWindow()&&dialog->parentWidget()->objectName()=="WorkspaceDialogShield");
            if(stage==0){++stage;dialog->findChild<QPushButton*>("ScreenshotCapture")->click();}
            // The host is minimized while awaiting capture; deliver separately
            // below so this fixture follows the real restore/preview path.
            if(stage==2){++stage;
                if(output)dialog->findChild<QPushButton*>(output==1?"ScreenshotAddLayer":"ScreenshotOpen")->click();
                else dialog->reject();
            }
        });
        QTimer delivery;
        QObject::connect(&delivery,&QTimer::timeout,&window,[&]{if(stage==1&&service&&service->calls){
            service->deliver(image);auto* dialog=window.findChild<QDialog*>("ScreenshotDialog");
            dynamic_cast<u::ScreenshotPreview*>(dialog->findChild<QWidget*>("ScreenshotPreview"))->setCrop({3,4,12,10});stage=2;
        }});
        response.start(10);delivery.start(10);
        QTimer deadline;deadline.setSingleShot(true);QObject::connect(&deadline,&QTimer::timeout,&window,[&]{
            std::cerr<<"Screenshot document timeout: stage="<<stage<<", minimized="<<window.isMinimized()<<", visible="<<window.isVisible()<<", calls="<<(service?service->calls:-1)<<'\n';
            CHECK(false);if(auto* dialog=window.findChild<QDialog*>("ScreenshotDialog"))dialog->reject();
        });deadline.start(4000);
        window.findChild<QAction*>("TakeScreenshotAction")->trigger();response.stop();delivery.stop();deadline.stop();CHECK(stage==3);
        CHECK(context->session.document()->selection()==selection);
        if(output==1) {
            CHECK(window.activeDocumentId()==originalId&&window.documentCount()==1);
            CHECK(session.document()->canvas().extent==c::Extent2u{64,48});
            CHECK(session.document()->layers().size()==2&&session.history().undoDepth()==depth+1&&!session.history().canRedo());
            const auto id=session.activeLayer();CHECK(id&&id==session.document()->layers().back().id);
            const auto& layer=session.document()->layers().back();
            CHECK(layer.localToDocument==c::AffineTransform{});
            const auto surface=std::get<c::RasterLayer>(layer.payload).surface;
            CHECK(surface->extent()==c::Extent2u{12,10});
            std::array<std::byte,4> pixel{};surface->copyRgba8({0,0,1,1},pixel,4);
            CHECK((pixel==std::array<std::byte,4>{std::byte{3},std::byte{4},std::byte{7},std::byte{255}}));
            CHECK(session.undo());CHECK(session.document()->layers().size()==1&&session.document()->contentState()==state);
            CHECK(session.redo());CHECK(session.activeLayer()==id&&session.document()->layers().size()==2);
            CHECK(session.undo());
        }
        CHECK(context->session.document()->contentState()==state);
        CHECK(context->session.history().undoDepth()==depth&&context->session.history().canRedo());
        if(!output){CHECK(window.activeDocumentId()==originalId&&window.documentCount()==1);}
        if(output==2) {
            CHECK(window.activeDocumentId()!=originalId&&window.documentCount()==2);
            CHECK(window.projectPath().isEmpty()&&window.editorSession().document()->isModified());
            CHECK(window.editorSession().document()->canvas().extent==c::Extent2u{12,10});
            const auto surface=std::get<c::RasterLayer>(window.editorSession().document()->layers().front().payload).surface;
            std::array<std::byte,4> pixel{};
            surface->copyRgba8({0,0,1,1},pixel,4);
            CHECK((pixel==std::array<std::byte,4>{std::byte{3},std::byte{4},std::byte{7},std::byte{255}}));
            const auto saved=files.filePath("capture.vulkana");CHECK(u::saveProject(saved,*window.editorSession().document()));
            const auto loaded=u::loadProject(saved);CHECK(loaded);
            if(loaded)CHECK(loaded.document->canvas().extent==c::Extent2u{12,10});
        }
    }
    window.close();
}
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settings;app.setOrganizationName("VulkanaTests");app.setApplicationName("Screenshot");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    u::applyEditorTheme(app);
    const bool native=app.arguments().contains("--native");
    QVulkanInstance instance;int validationMessages{};
    if(native) {
        instance.setApiVersion(QVersionNumber(1,2));instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,QVulkanInstance::DebugMessageTypeFlags types,const void* message){
            if(types.testFlag(QVulkanInstance::ValidationMessage)&&(flags&(QVulkanInstance::ErrorSeverity|QVulkanInstance::WarningSeverity))) {
                ++validationMessages;std::cerr<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage<<'\n';
            }
            return false;
        });
        if(!instance.create())return 2;
    }
    cropTests();dialogTests(native?QString{}:app.arguments().value(1));documentTests(native?&instance:nullptr);
    CHECK(validationMessages==0);
    std::cout<<"Screenshot UI/crop/document checks: "<<failures<<" failures\n";
    return failures?1:nativeInputUnavailable?77:0;
}
