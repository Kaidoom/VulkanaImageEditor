#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <iostream>
namespace c=imageeditor::core;namespace u=imageeditor::ui;namespace r=imageeditor::render;
int messages=0,failures=0;
bool debug(VkDebugReportFlagsEXT flags,VkDebugReportObjectTypeEXT,std::uint64_t,std::size_t,std::int32_t,const char*,const char* text)
{if(flags&(VK_DEBUG_REPORT_ERROR_BIT_EXT|VK_DEBUG_REPORT_WARNING_BIT_EXT)){++messages;std::cerr<<text<<'\n';}return false;}
#define CHECK(x) do{if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";}}while(false)
bool wait(const std::function<bool()>& predicate,int ms=10000)
{QElapsedTimer t;t.start();while(!predicate()&&t.elapsed()<ms)QTest::qWait(2);return predicate();}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName("ImageEditorTests");app.setApplicationName("DocumentTabBenchmark");
    QStandardPaths::setTestModeEnabled(true);QTemporaryDir files;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,files.path());
    u::applyEditorTheme(app);
    QVulkanInstance instance;instance.setApiVersion(QVersionNumber(1,2));
    instance.setLayers({"VK_LAYER_KHRONOS_validation"});CHECK(instance.create());instance.installDebugOutputFilter(debug);
    if(!instance.isValid())return 1;
    for(const auto extent:{c::Extent2u{3840,2160},c::Extent2u{5120,2880}}) {
        c::Document source({extent,144});
        for(int i=0;i<2;++i){auto pixels=std::make_shared<c::ContiguousRasterSurface>(extent,c::Rgba8{std::uint8_t(50+i*70),95,140,180});
            auto layer=c::Layer::raster("Full-resolution raster",std::move(pixels));layer.localToDocument.m02=i*21.5;
            CHECK(source.insertLayer(source.layers().size(),std::move(layer)));}
        c::TextLayer text;text.utf8="Native document tabs Ω";text.defaultStyle.sizePixels=150;
        auto letters=c::Layer::text("Editable heading",text);letters.localToDocument.m02=500;letters.localToDocument.m12=400;CHECK(source.insertLayer(2,letters));
        c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={500,400};shape.strokeEnabled=true;
        auto ellipse=c::Layer::shape("Editable ellipse",shape);ellipse.localToDocument.m02=900;ellipse.localToDocument.m12=800;CHECK(source.insertLayer(3,ellipse));
        const auto master=files.filePath(QString("master-%1.vulkana").arg(extent.width));CHECK(u::saveProject(master,source));
        for(int count:{1,5,10}) {
            u::MainWindow window(&instance,false);window.setUnsavedPromptEnabled(false);window.resize(1450,900);window.show();
            r::CanvasWindow* canvas=nullptr;for(auto* q:QGuiApplication::allWindows())if(auto* c=dynamic_cast<r::CanvasWindow*>(q))canvas=c;
            CHECK(canvas&&wait([&]{return canvas->rendererStats().presentQueuedFrames>0;}));if(!canvas)return 1;
            std::vector<u::DocumentInstanceId> ids;double openMs=0;
            for(int i=0;i<count;++i){const auto path=files.filePath(QString("%1-%2-%3.vulkana").arg(extent.width).arg(count).arg(i));CHECK(QFile::copy(master,path));
                const auto frame=canvas->rendererStats().presentQueuedFrames;QElapsedTimer timer;timer.start();CHECK(window.openImageFromPath(path));
                CHECK(wait([&]{return canvas->rendererStats().presentQueuedFrames>frame;}));openMs+=double(timer.nsecsElapsed())/1e6;ids.push_back(window.activeDocumentId());}
            QTest::qWait(200);const auto stable=canvas->rendererStats();
            // Cold includes any cache reupload after budget eviction, not file I/O.
            QElapsedTimer timer;timer.start();CHECK(window.activateDocument(ids.front()));canvas->scheduleFrame();
            CHECK(wait([&]{return canvas->rendererStats().presentQueuedFrames>stable.presentQueuedFrames;}));const auto coldMs=double(timer.nsecsElapsed())/1e6;
            // Warm alternate two actual four-layer documents (or same-tab no-op).
            double warmMs=0;std::uint64_t extraUploads=0;
            const auto partner=count>1?ids[1]:ids.front();CHECK(window.activateDocument(partner));QTest::qWait(120);
            for(int n=0;n<6;++n){const auto before=canvas->rendererStats();timer.restart();CHECK(window.activateDocument(n%2?partner:ids.front()));canvas->scheduleFrame();
                CHECK(wait([&]{return canvas->rendererStats().presentQueuedFrames>before.presentQueuedFrames;}));warmMs+=double(timer.nsecsElapsed())/1e6;
                extraUploads+=canvas->rendererStats().uploadedBytes-before.uploadedBytes;}
            QTest::qWait(200);const auto idle=canvas->rendererStats();QTest::qWait(250);
            CHECK(canvas->rendererStats().framesSubmitted==idle.framesSubmitted);
            CHECK(canvas->rendererStats().uploadedBytes==idle.uploadedBytes);
            CHECK(idle.resourceGeneration==stable.resourceGeneration&&idle.swapchainGeneration==stable.swapchainGeneration);
            CHECK(extraUploads==0);
            const auto memory=window.documentMemory();
            std::cout<<"tabs_profile platform="<<app.platformName().toStdString()<<" width="<<extent.width<<" documents="<<count
                <<" load_and_first_present_ms="<<openMs/count<<" cold_switch_present_ms="<<coldMs<<" warm_switch_present_ms="<<warmMs/6
                <<" warm_upload_bytes="<<extraUploads<<" idle_frames="<<canvas->rendererStats().framesSubmitted-idle.framesSubmitted
                <<" source_bytes="<<memory.sourceBytes<<" derived_bytes="<<memory.derivedBytes<<" history_bytes="<<memory.historyBytes
                <<" gpu_texture_bytes="<<idle.textureCacheBytes<<" texture_evictions="<<idle.inactiveTextureEvictions<<std::endl;
            // Closing inactive owners and repeatedly reopening never rebuilds the device.
            for(int i=count-1;i>0;--i)CHECK(window.closeDocument(ids[std::size_t(i)]));
            CHECK(window.documentCount()==1);QTest::qWait(60);CHECK(canvas->rendererStats().resourceGeneration==stable.resourceGeneration);
            window.close();
        }
    }
    std::cout<<"Tab validation warnings/errors="<<messages<<" checks_failed="<<failures<<std::endl;
    return failures||messages?1:0;
}
