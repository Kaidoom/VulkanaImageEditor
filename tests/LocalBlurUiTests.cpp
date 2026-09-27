#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LocalBlurOptionsPage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <atomic>
#include <iostream>

namespace c=imageeditor::core;namespace u=imageeditor::ui;namespace r=imageeditor::render;
namespace {
int failures=0;
#define CHECK(...) do {if(!(__VA_ARGS__)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #__VA_ARGS__ "\n";}} while(false)
void settle(){QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();}
void options()
{
    u::LocalBlurOptionsPage page;
    auto* size=page.findChild<QDoubleSpinBox*>("LocalBlurSize");
    auto* hardness=page.findChild<QDoubleSpinBox*>("LocalBlurHardness");
    auto* strength=page.findChild<QDoubleSpinBox*>("LocalBlurStrength");
    auto* radius=page.findChild<QDoubleSpinBox*>("LocalBlurRadius");
    CHECK(size&&hardness&&strength&&radius);if(!size||!hardness||!strength||!radius)return;
    c::BrushSettings brush;brush.opacity=.29;brush.flow=.34;brush.foreground={2,7,14,61};brush.tip.aspectRatio=.6;
    brush.tip.angleDegrees=37;brush.deterministicSeed=72;
    page.setBrushSettings(brush);page.setBlurSettings({.65,6.5});
    int brushChanges=0,blurChanges=0;
    page.onBrushSettingsChanged=[&](const auto& value){++brushChanges;auto expected=brush;
        expected.sizePixels=size->value();expected.hardness=hardness->value()/100;CHECK(value==expected);page.setBrushSettings(value);};
    page.onBlurSettingsChanged=[&](const auto& value){++blurChanges;page.setBlurSettings(value);};
    size->setValue(45);CHECK(brushChanges==1);strength->setValue(70);CHECK(blurChanges==1);
    radius->setValue(7.5);CHECK(page.blurSettings()==c::BlurSettings{.7,7.5});CHECK(brushChanges==1&&blurChanges==2);
    page.resize(page.sizeHint());page.show();settle();
    if (QGuiApplication::platformName() == "wayland")
        CHECK(QTest::qWaitForWindowActive(page.windowHandle()));
    auto* compact=dynamic_cast<u::CompactValueControl*>(radius);CHECK(compact);if(compact){compact->setFocus();
        QTest::keyClick(compact,Qt::Key_6);QTest::keyClick(compact,Qt::Key_Period);QTest::keyClick(compact,Qt::Key_5);
        CHECK(compact->value()==6.5);QTest::keyClick(compact,Qt::Key_Return);CHECK(!compact->isManualEntryActive());}
    const auto geometry=size->geometry();page.setBlurSettings({1,0});CHECK(size->geometry()==geometry);page.close();
}
void interaction(QVulkanInstance* vulkan)
{
    QTemporaryDir files;u::MainWindow window(vulkan,false,false);window.setUnsavedPromptEnabled(false);
    window.resize(1600,950);window.show();settle();r::CanvasWindow* canvas=nullptr;
    for(auto* candidate:QGuiApplication::allWindows())if(candidate->objectName()=="VulkanCanvasWindow")canvas=dynamic_cast<r::CanvasWindow*>(candidate);
    CHECK(canvas);if(!canvas)return;
    QImage image(160,120,QImage::Format_RGBA8888);for(int y=0;y<120;++y)for(int x=0;x<160;++x)
        image.setPixelColor(x,y,QColor((x*31+y*19)%256,(x*7+y*47)%256,(x*29+y*13)%256,60+(x+y)%190));
    const auto path=files.filePath("local-blur.png");CHECK(image.save(path));CHECK(window.openImageFromPath(path));
    auto& session=const_cast<c::EditorSession&>(window.editorSession());
    auto* action=window.findChild<QAction*>("ToolAction_local_blur");CHECK(action);if(!action)return;
    action->trigger();settle();CHECK(session.activeTool()==c::ToolId::LocalBlur);CHECK(action->shortcut()==QKeySequence("K"));
    auto* size=window.findChild<QDoubleSpinBox*>("LocalBlurSize");auto* strength=window.findChild<QDoubleSpinBox*>("LocalBlurStrength");
    auto* radius=window.findChild<QDoubleSpinBox*>("LocalBlurRadius");auto* help=window.findChild<QLabel*>("LocalBlurHelp");
    CHECK(size&&strength&&radius&&help);if(!size||!strength||!radius||!help)return;
    CHECK(help->isVisible()&&help->text().contains("Shift"));size->setValue(20);strength->setValue(100);radius->setValue(5);
    if(auto* focused=QApplication::focusWidget())focused->clearFocus();
    canvas->requestActivate();settle();
    // show()/processEvents() is not a Wayland activation barrier. Previously
    // the first stroke was injected with ApplicationInactive and no native
    // focus, racing the preceding options window's terminal notifications.
    if (vulkan) {
        CHECK(QTest::qWaitForWindowActive(window.windowHandle()));
        CHECK(QTest::qWaitFor([&] {
            return QGuiApplication::applicationState() == Qt::ApplicationActive
                && canvas->rendererStats().framesSubmitted > 0;
        }, 3000));
    }
    const auto source=std::get<c::RasterLayer>(session.document()->layer(*session.activeLayer())->payload).surface;
    const auto snapshot=[&]{std::vector<std::byte> values(160*120*4);source->copyRgba8({0,0,160,120},values,160*4);return values;};
    const auto mouse=[&](QEvent::Type type,c::Vec2d point,Qt::KeyboardModifiers mods=Qt::NoModifier,bool process=true){
        const auto p=canvas->scene().viewport.documentToViewport(point,{160,120},canvas->scene().logicalViewport);
        const QPointF local(p.x,p.y),global(canvas->mapToGlobal(local.toPoint()));
        QMouseEvent event(type,local,local,global,type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
            type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,mods);QCoreApplication::sendEvent(canvas,&event);if(process)settle();};
    const auto before=snapshot();const auto history=session.history().undoDepth();const auto color=session.foregroundColor();
    mouse(QEvent::MouseButtonPress,{50.5,50.5});mouse(QEvent::MouseMove,{80.5,50.5});
    CHECK(session.history().undoDepth()==history);CHECK(snapshot()!=before);
    mouse(QEvent::MouseButtonRelease,{80.5,50.5});CHECK(!canvas->pointerGestureActive());
    CHECK(session.history().undoDepth()==history+1);CHECK(session.foregroundColor()==color);
    const auto after=snapshot();for(std::size_t p=3;p<before.size();p+=4)CHECK(before[p]==after[p]);
    CHECK(session.undo());CHECK(snapshot()==before);CHECK(session.redo());CHECK(snapshot()==after);CHECK(session.undo());
    const auto redo=session.history().redoDepth();canvas->setDocument(session.document()->snapshot(),false);
    for(bool focus:{false,true}){mouse(QEvent::MouseButtonPress,{60.5,60.5});mouse(QEvent::MouseMove,{90.5,60.5});
        if(focus){QFocusEvent event(QEvent::FocusOut);QCoreApplication::sendEvent(canvas,&event);}else QTest::keyClick(canvas,Qt::Key_Escape);
        settle();CHECK(!canvas->pointerGestureActive());CHECK(snapshot()==before&&session.history().redoDepth()==redo);}
    radius->setValue(0);const auto revision=source->revision();mouse(QEvent::MouseButtonPress,{50.5,50.5});mouse(QEvent::MouseButtonRelease,{50.5,50.5});
    CHECK(source->revision()==revision&&session.history().redoDepth()==redo);
    // A release delivered while the first large dab is still filtering must
    // replay after capture installation, not disappear in the processing gate.
    size->setValue(180);radius->setValue(64);
    bool released=false;
    QTimer::singleShot(0,&window,[&]{released=true;mouse(QEvent::MouseButtonRelease,{60.5,60.5});});
    mouse(QEvent::MouseButtonPress,{60.5,60.5});settle();
    CHECK(released&&!canvas->pointerGestureActive());CHECK(session.history().undoDepth()==history+1);
    CHECK(session.undo());CHECK(snapshot()==before);canvas->setDocument(session.document()->snapshot(),false);
    const auto queuedRedo=session.history().redoDepth();
    QTimer::singleShot(0,&window,[&]{QTest::keyClick(canvas,Qt::Key_Escape);});
    mouse(QEvent::MouseButtonPress,{60.5,60.5});settle();
    CHECK(!canvas->pointerGestureActive());CHECK(snapshot()==before&&session.history().redoDepth()==queuedRedo);
    // Deliver the same bent trace normally and while the first-dab processing
    // barrier is active. Its initial status signal provides a deterministic
    // injection point, independent of processor speed/convolution duration.
    size->setValue(20);radius->setValue(5);
    const auto bentTrace=[&]{mouse(QEvent::MouseMove,{100.5,30.5},Qt::NoModifier,false);
        mouse(QEvent::MouseMove,{100.5,90.5},Qt::NoModifier,false);
        mouse(QEvent::MouseButtonRelease,{45.5,90.5},Qt::NoModifier,false);};
    mouse(QEvent::MouseButtonPress,{30.5,30.5});bentTrace();settle();
    const auto bentExpected=snapshot();CHECK(bentExpected!=before);
    CHECK(session.undo());CHECK(snapshot()==before);canvas->setDocument(session.document()->snapshot(),false);
    QMetaObject::Connection injection;bool traceInjected=false;
    injection=QObject::connect(window.statusBar(),&QStatusBar::messageChanged,&window,[&](const QString& message){
        if(!message.contains("preparing original source"))return;
        QObject::disconnect(injection);traceInjected=true;bentTrace();});
    mouse(QEvent::MouseButtonPress,{30.5,30.5});settle();
    QObject::disconnect(injection);
    CHECK(traceInjected&&!canvas->pointerGestureActive());CHECK(snapshot()==bentExpected);
    CHECK(session.history().undoDepth()==history+1);CHECK(session.undo());CHECK(snapshot()==before);
    canvas->setDocument(session.document()->snapshot(),false);
    const auto overflowRedo=session.history().redoDepth();bool overflowInjected=false;
    injection=QObject::connect(window.statusBar(),&QStatusBar::messageChanged,&window,[&](const QString& message){
        if(!message.contains("preparing original source"))return;
        QObject::disconnect(injection);overflowInjected=true;
        for(int i=0;i<4097;++i)mouse(QEvent::MouseMove,{45.5+double(i%50),60.5},Qt::NoModifier,false);
        mouse(QEvent::MouseButtonRelease,{80.5,60.5},Qt::NoModifier,false);});
    mouse(QEvent::MouseButtonPress,{30.5,30.5});settle();QObject::disconnect(injection);
    CHECK(overflowInjected&&!canvas->pointerGestureActive());
    CHECK(snapshot()==before&&session.history().redoDepth()==overflowRedo);
    QLineEdit text(&window);text.show();text.setFocus();settle();QTest::keyClick(&text,Qt::Key_K);CHECK(text.text()=="k");text.hide();
    c::TextLayer textData;textData.utf8="Still editable";auto typed=c::Layer::text("Text source",textData);const auto typedId=typed.id;
    CHECK(session.document()->insertLayer(session.document()->layers().size(),std::move(typed)));session.setActiveLayer(typedId);
    const auto typedHistory=session.history().undoDepth();
    mouse(QEvent::MouseButtonPress,{50.5,50.5});mouse(QEvent::MouseButtonRelease,{50.5,50.5});
    CHECK(session.history().undoDepth()==typedHistory&&snapshot()==before);
    CHECK(std::get<c::TextLayer>(session.document()->layer(typedId)->payload).utf8=="Still editable");
    CHECK(!canvas->pointerGestureActive());
    if(vulkan){QTest::qWait(80);CHECK(canvas->rendererStats().framesSubmitted>0);}
    window.close();settle();
}
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setOrganizationName("ImageEditorTests");QCoreApplication::setApplicationName("LocalBlurTests");
    QApplication app(argc,argv);QStandardPaths::setTestModeEnabled(true);QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    u::applyEditorTheme(app);options();
    if(app.arguments().contains("--native")){
        if(QGuiApplication::platformName()!="wayland")return 77;std::atomic_int messages{0};QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1,2));instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        instance.installDebugOutputFilter([&](auto severity,auto type,const void* raw){if(type.testFlag(QVulkanInstance::ValidationMessage)
            &&(severity.testFlag(QVulkanInstance::WarningSeverity)||severity.testFlag(QVulkanInstance::ErrorSeverity))){++messages;
                std::cerr<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(raw)->pMessage<<"\n";}return false;});
        CHECK(instance.create());if(instance.isValid())interaction(&instance);instance.destroy();settle();CHECK(messages==0);
    }else interaction(nullptr);
    if(!failures)std::cout<<"Local Blur UI tests passed\n";return failures?1:0;
}
