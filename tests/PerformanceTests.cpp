#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/PerformancePanel.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/core/BoundedParallel.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/render/GraphicsDeviceProbe.hpp"
#include "imageeditor/render/detail/GraphicsProbeCache.hpp"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <atomic>
#include <algorithm>
#include <iostream>
#include <thread>
namespace c=imageeditor::core;namespace u=imageeditor::ui;namespace p=imageeditor::platform;namespace r=imageeditor::render;
namespace {
int failures{};constexpr std::size_t MiB=1024*1024;
#define CHECK(...) do{if(!(__VA_ARGS__)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #__VA_ARGS__ "\n";}}while(false)
template<class F>bool waitFor(F f,int timeout=6000){QElapsedTimer t;t.start();while(!f()&&t.elapsed()<timeout)QTest::qWait(10);return f();}
const QString missingGpu="gpu:ffff:ffff:0123456789abcdef0123456789abcdef";
void choose(QWidget& owner,const char* name,int value){auto* box=owner.findChild<QComboBox*>(name);CHECK(box&&box->findData(value)>=0);if(box)box->setCurrentIndex(box->findData(value));}
struct RetainedCommand final:c::Command {
    std::size_t cost;explicit RetainedCommand(std::size_t n):cost(n){}
    bool apply(c::Document&) override{return true;}bool undo(c::Document&) override{return true;}
    std::string_view label() const noexcept override{return "Retention fixture";}
    std::size_t memoryCost() const noexcept override{return cost;}
};
void historyPolicy()
{
    c::Document document({{1,1},96});c::History history;
    for(int i=0;i<4;++i)CHECK(history.execute(document,std::make_unique<RetainedCommand>(64*MiB)));
    CHECK(history.undo(document));document.markSaved();const auto state=document.contentState(),bytes=history.memoryUsed();
    history.setMemoryBudget(64*MiB);
    CHECK(history.undoDepth()==3&&history.redoDepth()==1&&history.memoryUsed()==bytes);
    CHECK(document.contentState()==state&&!document.isModified());
    CHECK(history.redo(document));CHECK(history.undo(document));CHECK(!document.isModified());
    CHECK(history.execute(document,std::make_unique<RetainedCommand>(128*MiB)));
    CHECK(history.undoDepth()==1&&history.memoryUsed()==128*MiB&&!history.canRedo());
    history.setMemoryBudget(512*MiB);CHECK(history.undoDepth()==1); // Lost history is not resurrected.
}
void compatibilityCachePolicy()
{
    namespace d=r::detail;
    QTemporaryDir files;QSettings settings(files.filePath("compatibility.ini"),QSettings::IniFormat);
    d::GraphicsProbeCache cache(settings);
    const auto key=d::graphicsPresentationCacheKey("gpu-a","driver-1","display-1",true);
    const auto other=d::graphicsPresentationCacheKey("gpu-b","driver-1","display-1",true);
    CHECK(key==d::graphicsPresentationCacheKey("gpu-a","driver-1","display-1",true));
    CHECK(key!=other);
    CHECK(key!=d::graphicsPresentationCacheKey("gpu-a","driver-2","display-1",true));
    CHECK(key!=d::graphicsPresentationCacheKey("gpu-a","driver-1","display-2",true));
    CHECK(key!=d::graphicsPresentationCacheKey("gpu-a","driver-1","display-1",false));
    CHECK(d::graphicsPresentationCacheKey({},"driver-1","display-1",true).isEmpty());
    CHECK(!cache.contains(key));cache.remember(key);cache.remember(other);CHECK(cache.contains(key)&&cache.contains(other));
    {QSettings reopened(settings.fileName(),QSettings::IniFormat);CHECK(d::GraphicsProbeCache(reopened).contains(key));}
    cache.beginStartup(key);cache.completeStartup(key);cache.recoverInterruptedStartup();CHECK(cache.contains(key));
    cache.beginStartup(key);cache.completeStartup(other);cache.recoverInterruptedStartup();
    CHECK(!cache.contains(key)&&cache.contains(other)); // Interrupted startup invalidates only its configuration.
    cache.remember("bad entry");CHECK(!cache.contains("bad entry"));
    for(int i=0;i<24;++i)cache.remember(d::graphicsPresentationCacheKey(QString::number(i),"driver","display",true));
    CHECK(settings.value("graphics/presentation-v1/supported").toStringList().size()==16);
    CHECK(!cache.contains(other));
}
void compatibilityCacheRoundtrip(const QString& settingsRoot)
{
    const auto run=[&](bool rejectHelper,bool changedDisplay,const QStringList& extra=QStringList{}) {
        QProcess process;process.setProcessChannelMode(QProcess::MergedChannels);
        auto env=QProcessEnvironment::systemEnvironment();
        if(rejectHelper)env.insert("VULKANA_TEST_GPU_PROBE_TIMEOUT","1");else env.remove("VULKANA_TEST_GPU_PROBE_TIMEOUT");
        if(changedDisplay)env.insert("KDE_SESSION_VERSION","cache-test-changed-display");
        // Bypassing shader caches is deliberately unrelated to WSI compatibility.
        if(rejectHelper){env.insert("__GL_SHADER_DISK_CACHE","0");env.insert("MESA_SHADER_CACHE_DISABLE","true");}
        process.setProcessEnvironment(env);QElapsedTimer elapsed;elapsed.start();
        QStringList args{"--native","--verify-only","--settings-root",settingsRoot};args.append(extra);
        process.start(QCoreApplication::applicationFilePath(),args);
        CHECK(process.waitForFinished(15000)&&process.exitStatus()==QProcess::NormalExit);
        std::cout<<"Compatibility cache launch: "<<elapsed.elapsed()<<"ms, result "<<process.exitCode()<<'\n';
        return process.exitCode();
    };
    CHECK(run(false,false)==0); // First launch verifies and records success.
    CHECK(run(true,false)==0); // Fresh process must reuse it, even with cold shaders.
    CHECK(run(true,true)==2); // Changed display context requires a real helper.
    CHECK(run(true,false)==0); // Timeout did not overwrite the known-good context.
    CHECK(run(false,false,{"--missing-probe"})==2); // Explicit diagnostic helpers bypass durable cache.
    CHECK(run(true,false,{"--mark-startup-pending"})==0);
    CHECK(run(true,false)==2); // No confirmed/clean completion: discard that cached success.
    CHECK(run(false,false)==0);
    CHECK(run(true,false)==0);
}
void settingsPolicy()
{
    QSettings settings;CHECK(p::loadPerformanceSettings(settings)==p::PerformanceSettings{});
    const p::PerformanceSettings defaults;CHECK(defaults.undoBytes()==256*MiB&&defaults.documentCacheBytes()==512*MiB&&defaults.textureCacheBytes()==512*MiB);
    CHECK(p::startupPerformanceSettings()==defaults);
    p::PerformanceSettings value{128,1024,2048,2,3,missingGpu};CHECK(p::savePerformanceSettings(settings,value));
    CHECK(p::loadPerformanceSettings(settings)==value&&p::startupPerformanceSettings()==defaults);
    value={-1,99999,1,999,-6,"bad device"};value=p::sanitizePerformanceSettings(value);
    CHECK(value.undoMiB==0&&value.documentCacheMiB==0&&value.textureCacheMiB==64&&value.processingThreads==0&&value.objectSelectionThreads==0&&value.graphicsDevice.isEmpty());
    settings.setValue("preferences/performance/undoMiB","invalid");CHECK(p::loadPerformanceSettings(settings).undoMiB==0);
    CHECK(p::savePerformanceSettings(settings,{}));
    std::vector<r::GraphicsDeviceInfo> devices{{"one","Same name",5000},{"two","Same name",10000}};
    CHECK(r::selectGraphicsDevice(devices,"")==1&&r::selectGraphicsDevice(devices,"one")==0);
    CHECK(r::selectGraphicsDevice(devices,"gone")==1);std::reverse(devices.begin(),devices.end());
    CHECK(r::selectGraphicsDevice(devices,"one")==1);CHECK(!r::selectGraphicsDevice({},"one"));
    devices[0].availability=r::GraphicsDeviceAvailability::StartupCheckFailed;
    CHECK(r::selectGraphicsDevice(devices,"")==1&&r::selectGraphicsDevice(devices,"two")==1);
    devices[1].availability=r::GraphicsDeviceAvailability::Unsupported;
    CHECK(!r::selectGraphicsDevice(devices,"")&&!r::selectGraphicsDevice(devices,"one"));
    using Status=r::GraphicsProbeStatus;
    const std::vector<r::GraphicsDeviceInfo> candidates{{"dedicated","Dedicated",10000},{"integrated","Integrated",5000},{"software","Software",500}};
    QStringList checks;
    auto result=r::chooseVerifiedGraphicsDevice(candidates,"dedicated",[&](const QString& id){
        checks.append(id);return r::GraphicsProbeResult{id=="software"?Status::Supported:Status::Rejected,{}};
    });
    CHECK(result.device=="software"&&result.error.isEmpty());
    CHECK(checks==QStringList({"dedicated","integrated","software"})); // Every fallback checked.
    checks.clear();
    result=r::chooseVerifiedGraphicsDevice(candidates,{},[&](const QString& id){
        checks.append(id);return r::GraphicsProbeResult{checks.size()==1?Status::Inconclusive:Status::Supported,{}};
    });
    CHECK(result.device=="dedicated"&&checks==QStringList({"dedicated","dedicated"})); // Slow Auto winner is not rejected.
    checks.clear();
    result=r::chooseVerifiedGraphicsDevice(candidates,"integrated",[&](const QString& id){
        checks.append(id);return r::GraphicsProbeResult{Status::Inconclusive,"timeout"};
    });
    CHECK(result.device.isEmpty()&&!result.error.isEmpty()&&checks==QStringList({"integrated","integrated"}));
    checks.clear();
    result=r::chooseVerifiedGraphicsDevice(candidates,"removed",[&](const QString& id){
        checks.append(id);return r::GraphicsProbeResult{Status::Rejected,{}};
    });
    CHECK(result.device.isEmpty()&&!result.error.isEmpty()&&checks.size()==3);
    CHECK(p::loadPerformanceSettings(settings)==p::PerformanceSettings{}); // No policy writes preferences.
}
void controls(const QString& artifacts)
{
    u::PreferencesDialog dialog({});dialog.show();auto* panel=dialog.performancePanel();
    dialog.findChild<QTabWidget*>()->setCurrentWidget(panel);QApplication::processEvents();
    int applied{},warnings{},clears{},reads{};u::PreferencesState stored;
    dialog.onApply=[&](const auto& value){stored=value;++applied;return true;};
    bool confirm=false;dialog.confirmUndoReduction=[&]{++warnings;return confirm;};
    panel->onClearCaches=[&]{++clears;};panel->readUsage=[&]{++reads;return u::PerformanceUsage{256*MiB,10*MiB,20*MiB,30*MiB,40*MiB,"Fixture GPU"};};
    panel->setGraphicsDevices({{missingGpu.toStdString(),"Integrated fixture GPU",5000,r::GraphicsDeviceAvailability::StartupCheckFailed}});
    auto saved=panel->settings();saved.graphicsDevice=missingGpu;panel->setSettings(saved);
    auto* gpu=dialog.findChild<QComboBox*>("PerformanceGpu");
    CHECK(gpu->currentText().contains("Integrated fixture GPU")&&gpu->currentText().contains("startup check failed"));
    CHECK(!(gpu->model()->flags(gpu->model()->index(gpu->currentIndex(),0))&Qt::ItemIsEnabled));
    CHECK(panel->settings().graphicsDevice==missingGpu); // No silent saved-choice substitution.
    choose(dialog,"PerformanceUndo",64);choose(dialog,"PerformanceThreads",2);
    dialog.findChild<QPushButton*>("PreferencesApply")->click();CHECK(warnings==1&&applied==0);
    confirm=true;dialog.findChild<QPushButton*>("PreferencesApply")->click();CHECK(warnings==2&&applied==1&&stored.performance.undoMiB==64);
    choose(dialog,"PerformanceUndo",128);dialog.findChild<QPushButton*>("PerformanceClearCaches")->click();CHECK(clears==1&&applied==1);
    CHECK(waitFor([&]{return reads>0;}));CHECK(dialog.findChild<QLabel*>("PerformanceUsage")->text().contains("Fixture GPU"));
    CHECK(!dialog.findChild<QLabel*>("PerformanceGpuFallback"));
    dialog.findChild<QPushButton*>("PerformanceClearCaches")->click();
    dialog.findChild<QTabWidget*>()->setCurrentIndex(0);const auto hiddenReads=reads;QTest::qWait(1100);CHECK(reads==hiddenReads);
    dialog.findChild<QTabWidget*>()->setCurrentWidget(panel);
    if(!artifacts.isEmpty()){QDir().mkpath(artifacts);QApplication::processEvents();CHECK(dialog.grab().save(QDir(artifacts).filePath("performance.png")));}
    dialog.findChild<QPushButton*>("PerformanceDefaults")->click();CHECK(panel->settings()==p::PerformanceSettings{}&&applied==1);
    dialog.reject();CHECK(stored.performance.undoMiB==64);const auto oldReads=reads;QTest::qWait(1100);CHECK(reads==oldReads);
}
r::CanvasWindow* canvas(u::MainWindow& window)
{
    for(auto* w:QGuiApplication::allWindows())if(auto* candidate=dynamic_cast<r::CanvasWindow*>(w))
        for(auto* parent=candidate->parent();parent;parent=parent->parent())if(parent==window.windowHandle())return candidate;
    return nullptr;
}
void checkSavedGpuInFreshProcess(const QString& id)
{
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),{"-platform",QGuiApplication::platformName(),"--settings-root",
        qApp->property("performanceTestSettingsRoot").toString(),"--saved-gpu",id});
    const auto finished=child.waitForFinished(15000);
    CHECK(finished&&child.exitCode()==0);
    if(!finished||child.exitCode()!=0)std::cerr<<child.readAllStandardError().toStdString();
}
void fallbackPopup(const QString& artifacts)
{
    const QString replacement="gpu:eeee:eeee:0123456789abcdef0123456789abcdef";
    QSettings settings;p::PerformanceSettings before{128,256,1024,2,3,missingGpu};CHECK(p::savePerformanceSettings(settings,before));
    u::MainWindow window(nullptr,false);window.setUnsavedPromptEnabled(false);window.show();auto* view=canvas(window);CHECK(view);if(!view)return;
    int notices{};QTimer watcher;
    QObject::connect(&watcher,&QTimer::timeout,&window,[&] {
        auto* popup=window.findChild<QDialog*>("GpuFallbackNotification");if(!popup||!popup->isVisible())return;
        ++notices;CHECK(!popup->isWindow());
        CHECK(popup->findChild<QLabel*>("GpuFallbackMessage")->text().contains("Selected fixture GPU"));
        CHECK(popup->findChild<QLabel*>("GpuFallbackMessage")->text().contains("Falling back to Working fixture GPU"));
        auto expected=before;expected.graphicsDevice=replacement;CHECK(p::loadPerformanceSettings(settings)==expected);
        CHECK(p::startupGraphicsDevice()==replacement);
        auto* buttons=popup->findChild<QDialogButtonBox*>();CHECK(buttons&&buttons->standardButtons()==QDialogButtonBox::Ok);
        if(!artifacts.isEmpty()){QDir().mkpath(artifacts);CHECK(popup->grab().save(QDir(artifacts).filePath("gpu-fallback-popup.png")));}
        buttons->button(QDialogButtonBox::Ok)->click();
    });watcher.start(10);
    view->onPreferredGpuUnavailable({missingGpu.toStdString(),"Selected fixture GPU",replacement.toStdString(),"Working fixture GPU"});
    CHECK(waitFor([&]{return notices==1;}));
    view->onPreferredGpuUnavailable({missingGpu.toStdString(),"Selected fixture GPU",replacement.toStdString(),"Working fixture GPU"});
    QTest::qWait(250);CHECK(notices==1);checkSavedGpuInFreshProcess(replacement);
    window.close();
}
void integration(QVulkanInstance* instance,const QString& preferred,bool expectFallback)
{
    QTemporaryDir files;QImage image(128,96,QImage::Format_RGBA8888);image.fill(QColor(25,80,160));CHECK(image.save(files.filePath("fixture.png")));
    u::MainWindow window(instance,false);window.setUnsavedPromptEnabled(false);
    int recoveryNotices{};QTimer recoveryWatcher;
    QObject::connect(&recoveryWatcher,&QTimer::timeout,&window,[&] {
        auto* popup=window.findChild<QDialog*>("GpuFallbackNotification");if(!popup||!popup->isVisible())return;
        ++recoveryNotices;auto* view=canvas(window);CHECK(view);if(!view){popup->reject();return;}
        const auto stats=view->rendererStats();CHECK(stats.presentQueuedFrames>0);
        CHECK(popup->findChild<QLabel*>("GpuFallbackMessage")->text().contains(QString::fromStdString(stats.deviceName)));
        QSettings settings;CHECK(p::loadPerformanceSettings(settings).graphicsDevice==QString::fromStdString(stats.deviceId));
        popup->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });recoveryWatcher.start(10);window.show();
    CHECK(window.openImageFromPath(files.filePath("fixture.png")));auto first=window.activeDocumentId();
    auto* view=canvas(window);CHECK(view);if(!view)return;
    if(instance) {
        QElapsedTimer clock;clock.start();qint64 last=0,largestGap=0;int ticks=0;bool cardSeen=false;
        QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,&window,[&] {
            const auto now=clock.elapsed();largestGap=std::max(largestGap,now-last);last=now;++ticks;
            if(auto* card=window.findChild<QDialog*>("GraphicsInitializationCard");card&&card->isVisible()) {
                if(!cardSeen) {
                    CHECK(!card->findChild<QDialogButtonBox*>());
                    QTest::keyClick(card,Qt::Key_Escape);
                    CHECK(card->isVisible());
                }
                cardSeen=true;
                const auto artifacts=qEnvironmentVariable("VULKANA_GRAPHICS_STARTUP_REVIEW");
                if(!artifacts.isEmpty()&&!QFileInfo::exists(artifacts))CHECK(card->grab().save(artifacts));
            }
        });heartbeat.start(10);
        CHECK(waitFor([&]{return view->rendererStats().framesSubmitted>0;},30000));heartbeat.stop();
        CHECK(largestGap<1000); // Cold compilation must not stall the GUI for seconds.
        if(clock.elapsed()>1000)CHECK(cardSeen&&ticks>30);
        if(auto* card=window.findChild<QDialog*>("GraphicsInitializationCard"))CHECK(!card->isVisible());
        std::cout<<"Graphics startup: elapsed="<<clock.elapsed()<<"ms GUI max gap="<<largestGap<<"ms ticks="<<ticks<<" card="<<cardSeen<<'\n';
    }
    if(instance&&(expectFallback||preferred=="missing")) {
        CHECK(waitFor([&]{return recoveryNotices==1;}));
        checkSavedGpuInFreshProcess(QString::fromStdString(view->rendererStats().deviceId));
    }
    CHECK(window.openImageFromPath(files.filePath("fixture.png")));auto second=window.activeDocumentId();
    if(instance)CHECK(waitFor([&]{return view->rendererStats().textureCacheEntries>=2;}));
    auto* inactive=const_cast<u::DocumentContext*>(window.documentContext(first));
    auto* active=const_cast<u::DocumentContext*>(window.documentContext(second));
    CHECK(inactive&&active);if(!inactive||!active)return;
    const auto makeCache=[] {
        auto cache=std::make_shared<c::LayerRenderCache>();cache->surface=std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{128,96},c::Rgba8{1,2,3,255});return cache;
    };
    inactive->session.document()->layer(inactive->session.document()->layers()[0].id)->renderCache=makeCache();
    active->session.document()->layer(active->session.document()->layers()[0].id)->renderCache=makeCache();
    CHECK(active->session.execute(std::make_unique<RetainedCommand>(10*MiB)));CHECK(active->session.undo());active->session.document()->markSaved();
    const auto state=active->session.document()->contentState();const auto firstState=inactive->session.document()->contentState();
    const auto surface=std::get<c::RasterLayer>(active->session.document()->layers()[0].payload).surface;
    const auto beforeView=view->scene().viewport;const auto clearGeneration=view->scene().inactiveCacheClearGeneration;
    u::MainWindow other(nullptr,false);other.setUnsavedPromptEnabled(false); // Also receives applied memory policy.
    bool handled{};QTimer handler;
    std::uint64_t uploadsBeforeClear{};
    QObject::connect(&handler,&QTimer::timeout,&window,[&]{
        if(handled)return;
        auto* dialog=dynamic_cast<u::PreferencesDialog*>(window.findChild<QDialog*>("PreferencesDialog"));if(!dialog)return;
        handled=true;dialog->findChild<QTabWidget*>()->setCurrentWidget(dialog->performancePanel());
        CHECK(!dialog->findChild<QLabel*>("PerformanceGpuFallback"));
        if(expectFallback||preferred=="missing") {
            auto* gpu=dialog->findChild<QComboBox*>("PerformanceGpu");
            CHECK(gpu->currentData().toString()==QString::fromStdString(view->rendererStats().deviceId));
            CHECK(gpu->model()->flags(gpu->model()->index(gpu->currentIndex(),0))&Qt::ItemIsEnabled);
        }
        if(instance) {
            QTest::qWait(200);const auto frames=view->rendererStats().framesSubmitted;
            QTest::qWait(1100);CHECK(view->rendererStats().framesSubmitted==frames);
        }
        dialog->confirmUndoReduction=[] {return true;};choose(*dialog,"PerformanceUndo",128);choose(*dialog,"PerformanceCpuCache",64);choose(*dialog,"PerformanceGpuCache",64);choose(*dialog,"PerformanceThreads",2);
        dialog->findChild<QPushButton*>("PreferencesApply")->click();
        CHECK(active->session.history().memoryBudget()==128*MiB&&inactive->session.history().memoryBudget()==128*MiB);
        CHECK(other.editorSession().history().memoryBudget()==128*MiB&&active->session.history().canRedo());
        CHECK(view->scene().textureCacheBudgetBytes==64*MiB);
        uploadsBeforeClear=view->rendererStats().uploadedBytes;
        dialog->findChild<QPushButton*>("PerformanceClearCaches")->click();
        CHECK(!inactive->session.document()->layers()[0].renderCache&&active->session.document()->layers()[0].renderCache);
        choose(*dialog,"PerformanceUndo",64);choose(*dialog,"PerformanceThreads",1);dialog->reject();
    });handler.start(10);
    QTimer deadline;deadline.setSingleShot(true);QObject::connect(&deadline,&QTimer::timeout,&window,[&]{CHECK(false);if(auto* d=window.findChild<QDialog*>("PreferencesDialog"))d->reject();});deadline.start(10000);
    window.findChild<QAction*>("PreferencesAction")->trigger();handler.stop();deadline.stop();CHECK(handled);
    CHECK(active->session.document()->contentState()==state&&!active->session.document()->isModified()&&active->session.history().canRedo());
    CHECK(inactive->session.document()->contentState()==firstState&&view->scene().inactiveCacheClearGeneration==clearGeneration+1);
    CHECK(view->scene().viewport.zoom()==beforeView.zoom()&&view->scene().viewport.pan()==beforeView.pan());
    CHECK(std::get<c::RasterLayer>(active->session.document()->layers()[0].payload).surface==surface);
    QSettings settings;const auto saved=p::loadPerformanceSettings(settings);CHECK(saved.undoMiB==128&&saved.processingThreads==2);
    CHECK(active->session.history().memoryBudget()==128*MiB);
    if(instance) {
        CHECK(waitFor([&]{return view->rendererStats().textureCacheEntries==1&&view->rendererStats().pendingTextureReleases==0;}));
        CHECK(view->rendererStats().uploadedBytes==uploadsBeforeClear);
        QTest::qWait(200);const auto idleFrames=view->rendererStats().framesSubmitted;
        QTest::qWait(250);CHECK(view->rendererStats().framesSubmitted==idleFrames);
        const auto stats=view->rendererStats();CHECK(stats.compositionError.empty());
        for(const auto& d:view->graphicsDevices())std::cout<<"GPU: "<<d.name<<" | "<<d.id<<'\n';
        std::cout<<"Selected: "<<stats.deviceName<<" | "<<stats.deviceId<<" fallback="<<stats.preferredDeviceUnavailable<<'\n';
        if(preferred=="missing"||expectFallback)CHECK(stats.preferredDeviceUnavailable);
        else if(!preferred.isEmpty())CHECK(stats.deviceId==preferred.toStdString()&&!stats.preferredDeviceUnavailable);
        if(expectFallback&&!QCoreApplication::arguments().contains("--missing-probe")) {
            const auto devices=view->graphicsDevices();const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.id==preferred.toStdString();});
            CHECK(found!=devices.end());if(found!=devices.end())CHECK(!found->name.empty()&&found->availability==r::GraphicsDeviceAvailability::StartupCheckFailed);
        }
    }
    CHECK(window.openImageFromPath(files.filePath("fixture.png")));CHECK(window.editorSession().history().memoryBudget()==128*MiB);
    CHECK(recoveryNotices==((instance&&(expectFallback||preferred=="missing"))?1:0));
    other.close();window.close();
}
void initializationLifetime(QVulkanInstance& instance,bool cancel,bool startupDialog=false)
{
    u::MainWindow window(&instance,false);window.setUnsavedPromptEnabled(false);window.show();
    auto* view=canvas(window);CHECK(view);if(!view)return;
    QElapsedTimer clock;clock.start();qint64 last=0,maxGap=0,closeAt=0;bool acted=false,repeatedClose=false,abortVisible=true;int ticks=0;
    QTimer timer;QObject::connect(&timer,&QTimer::timeout,&window,[&] {
        const auto now=clock.elapsed();maxGap=std::max(maxGap,now-last);last=now;++ticks;
        if(!view->graphicsInitializing())return;
        auto* card=window.findChild<QDialog*>("GraphicsInitializationCard");
        if(acted) {
            if(cancel) {
                auto* title=card?card->findChild<QLabel*>("ToolTitle"):nullptr;
                abortVisible&=window.isVisible()&&card&&card->isVisible()&&title&&title->text()==QStringLiteral("Aborting…");
                if(!repeatedClose&&now-closeAt>100) {
                    repeatedClose=true;
                    QCloseEvent close;QCoreApplication::sendEvent(window.windowHandle(),&close);
                }
            }
            return;
        }
        if(cancel&&(!card||!card->isVisible()))return;
        acted=true;
        if(cancel) {
            // Exercise the window-manager close path after the card is visible,
            // including a startup chooser underneath it, not just early close().
            QElapsedTimer closeTime;closeTime.start();
            QCloseEvent close;QCoreApplication::sendEvent(window.windowHandle(),&close);
            CHECK(closeTime.elapsed()<200);
            CHECK(window.isVisible()&&card->isVisible());
            auto* title=card->findChild<QLabel*>("ToolTitle");CHECK(title&&title->text()==QStringLiteral("Aborting…"));
            closeAt=now;
            const auto artifacts=qEnvironmentVariable("VULKANA_GRAPHICS_STARTUP_REVIEW");
            if(!artifacts.isEmpty())CHECK(card->grab().save(artifacts+".aborting.png"));
        } else {
            // Native surface changes must not free the worker's VkDevice/layout
            // or restart the same cold compilation. The job has no swapchain.
            view->destroy();view->create();window.resize(1420,850);
        }
    });timer.start(10);
    if(startupDialog) {
        window.showStartupDocument();
        for(auto* dialog:window.findChildren<QDialog*>())
            CHECK(!dynamic_cast<u::NewDocumentDialog*>(dialog)); // Eager startup UI cannot bypass the gate.
    }
    CHECK(waitFor([&]{return acted&&(cancel?!window.isVisible():view->rendererStats().presentQueuedFrames>0);},30000));
    timer.stop();CHECK(acted&&ticks>0&&maxGap<1000);
    if(cancel){CHECK(repeatedClose&&abortVisible);CHECK(view->rendererStats().presentQueuedFrames==0);}
    if(!cancel)CHECK(view->rendererStats().resourceGeneration==1);
    std::cout<<"Graphics initialization "<<(cancel?"close":"remap")<<": GUI max gap="<<maxGap<<"ms ticks="<<ticks<<'\n';
    window.close();
}
class StartupOrderWatcher final:public QObject {
public:
    explicit StartupOrderWatcher(r::CanvasWindow& canvas):canvas_(canvas){}
    bool earlyUi{},chooserSeen{},noticeSeen{},languageSeen{},welcomeSeen{};
protected:
    bool eventFilter(QObject* target,QEvent* event) override
    {
        if(event->type()!=QEvent::Show)return false;
        const auto chooser=dynamic_cast<u::NewDocumentDialog*>(target);
        const bool notice=target->objectName()=="StartupUpdateNotification";
        const bool language=target->objectName()=="FirstRunLanguage";
        const bool welcome=target->objectName()=="DocumentWelcome";
        if(!chooser&&!notice&&!language&&!welcome)return false;
        earlyUi|=canvas_.rendererStats().presentQueuedFrames==0||canvas_.graphicsInitializing();
        if(chooser){chooserSeen=true;QTimer::singleShot(0,chooser,&QDialog::reject);}
        if(language){languageSeen=true;if(auto* dialog=dynamic_cast<QDialog*>(target))QTimer::singleShot(0,dialog,&QDialog::reject);}
        noticeSeen|=notice;
        welcomeSeen|=welcome;
        return false;
    }
private:
    r::CanvasWindow& canvas_;
};
void startupGraphicsOrdering(QVulkanInstance& instance)
{
    const auto version=qApp->applicationVersion();qApp->setApplicationVersion("0.3.3");
    u::MainWindow window(&instance,false);window.setUnsavedPromptEnabled(false);
    auto* initialGate=window.findChild<QDialog*>("GraphicsInitializationCard");
    CHECK(initialGate&&!initialGate->isHidden()&&!window.startupGraphicsReady());
    window.show(); // Establish the native parent, without processing exposure yet.
    CHECK(initialGate&&initialGate->isVisible());
    auto* view=canvas(window);CHECK(view);if(!view)return;
    StartupOrderWatcher watcher(*view);qApp->installEventFilter(&watcher);
    const auto languageChoice=qApp->property("vulkanaOfferLanguageChoice");
    qApp->setProperty("vulkanaOfferLanguageChoice",true);
    // Queue startup before the first surface/expose/frame event. Auto must
    // obey the same readiness boundary as an explicitly selected GPU.
    window.startStartupFlow(true,nullptr,true);
    auto* welcome=window.findChild<QWidget*>("DocumentWelcome");CHECK(welcome&&!welcome->isVisible());
    // The preinstalled shield must block shortcuts before compilation begins,
    // not just when the asynchronous worker eventually announces itself.
    QTest::keyClick(&window,Qt::Key_N,Qt::ControlModifier);
    CHECK(!watcher.chooserSeen);
    CHECK(waitFor([&]{return watcher.chooserSeen&&watcher.noticeSeen&&watcher.languageSeen&&watcher.welcomeSeen&&window.startupGraphicsReady();},30000));
    std::cout<<"Startup ordering: chooser="<<watcher.chooserSeen<<" language="<<watcher.languageSeen
        <<" notice="<<watcher.noticeSeen<<" welcome="<<watcher.welcomeSeen<<" premature="<<watcher.earlyUi<<'\n';
    CHECK(!watcher.earlyUi);
    if(auto* card=window.findChild<QDialog*>("GraphicsInitializationCard"))CHECK(!card->isVisible());
    CHECK(welcome&&welcome->isVisible()); // Cancelling New retains the ordinary empty-workspace actions.
    qApp->setProperty("vulkanaOfferLanguageChoice",languageChoice);
    qApp->setApplicationVersion(version);
    qApp->removeEventFilter(&watcher);window.close();
}
void hiddenCompatibilityProbe()
{
    QProcess process;process.setProcessChannelMode(QProcess::MergedChannels);
    auto environment=QProcessEnvironment::systemEnvironment();environment.insert("WAYLAND_DEBUG","client");
    process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationFilePath(),{QString::fromLatin1(r::graphicsDeviceProbeOption),p::startupGraphicsDevice(),"-platform","wayland"});
    CHECK(process.waitForFinished(15000)&&process.exitCode()==0&&process.exitStatus()==QProcess::NormalExit);
    const auto protocol=process.readAll();
    CHECK(protocol.contains(".attach(")&&protocol.contains(".commit(")); // Real buffer import/presentation, not a headless rendering substitute.
    CHECK(!protocol.contains("get_toplevel")&&!protocol.contains("get_xdg_surface")); // No desktop window to flash or steal focus.
}
void closeBeforeGraphicsStarts(QVulkanInstance& instance)
{
    u::MainWindow window(&instance,false);window.setUnsavedPromptEnabled(false);window.show();
    auto* view=canvas(window);CHECK(view);if(!view)return;
    CHECK(!window.startupGraphicsReady()&&!view->graphicsInitializing());
    QElapsedTimer clock;clock.start();QCloseEvent event;
    QCoreApplication::sendEvent(window.windowHandle(),&event);
    CHECK(clock.elapsed()<200&&!window.isVisible());
    QTest::qWait(100);
    CHECK(view->rendererStats().resourceGeneration==0&&view->rendererStats().presentQueuedFrames==0);
}
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    const auto probe=app.arguments().indexOf(QString::fromLatin1(r::graphicsDeviceProbeOption));
    if(probe>=0) {
        if(qEnvironmentVariableIsSet("VULKANA_TEST_GPU_PROBE_TIMEOUT"))return 3;
        return r::runGraphicsDeviceProbe(app,app.arguments().value(probe+1),
            !app.arguments().contains(QString::fromLatin1(r::graphicsDeviceProbeBasicColorOption)));
    }
    QTemporaryDir settings;app.setOrganizationName("VulkanaTests");app.setApplicationName("Performance");
    const auto args=app.arguments();
    if(args.contains("--probe-unavailable")) {
        const auto checked=r::checkGraphicsDevicePresentation(missingGpu);
        CHECK(checked.status==r::GraphicsProbeStatus::Rejected);
        return failures?1:0; // Exact-adapter probe must not silently try Auto.
    }
    const auto settingsRoot=args.contains("--settings-root")?args.value(args.indexOf("--settings-root")+1):settings.path();
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsRoot);
    app.setProperty("performanceTestSettingsRoot",settingsRoot);
    if(args.contains("--probe-cache-roundtrip")) {
        compatibilityCacheRoundtrip(settingsRoot);return failures?1:0;
    }
    if(args.contains("--saved-gpu")) {
        QSettings current;const auto expected=args.value(args.indexOf("--saved-gpu")+1);
        CHECK(p::loadPerformanceSettings(current).graphicsDevice==expected&&p::startupGraphicsDevice()==expected);return failures?1:0;
    }
    if(args.contains("--workers")) {
        const auto limit=args.value(args.indexOf("--workers")+1).toUInt();CHECK(c::configureBoundedParallelLimit(limit));
        const auto count=c::boundedParallelWorkerCount();CHECK(count==std::min(p::automaticProcessingThreads(),limit?limit:8));
        std::atomic<unsigned> mask{};std::atomic<bool> valid{true};
        CHECK(c::boundedParallel(0,[&](unsigned rank,unsigned participants){if(participants!=count)valid=false;mask.fetch_or(1U<<rank);}));
        CHECK(valid&&mask==(1U<<count)-1);CHECK(!c::configureBoundedParallelLimit(limit==1?2:1));return failures?1:0;
    }
    u::applyEditorTheme(app);const bool native=args.contains("--native");
    const auto preferred=args.contains("--gpu")?args.value(args.indexOf("--gpu")+1):QString{};
    if(native){QSettings s;p::PerformanceSettings v;v.graphicsDevice=preferred=="missing"?missingGpu:preferred;CHECK(p::savePerformanceSettings(s,v));
        if(args.contains("--inconclusive-probe"))qputenv("VULKANA_TEST_GPU_PROBE_TIMEOUT","1");
        QString error;
        const auto accepted=r::verifyStartupGraphicsDevice(args.contains("--missing-probe")?QStringLiteral("/missing/vulkana-probe"):QString{},&error);
        CHECK(p::loadPerformanceSettings(s)==v&&p::startupPerformanceSettings()==v);
        if(args.contains("--verify-only")) {
            if(accepted&&args.contains("--mark-startup-pending"))r::beginGraphicsDeviceStartup(p::startupGraphicsDevice(),true);
            return failures?1:(accepted?0:2);
        }
        if((args.contains("--missing-probe")||args.contains("--inconclusive-probe"))&&QGuiApplication::platformName().startsWith("wayland")) {
            CHECK(!accepted&&!error.isEmpty());CHECK(p::startupGraphicsDevice()==v.graphicsDevice);
            if(args.contains("--inconclusive-probe")) {
                qunsetenv("VULKANA_TEST_GPU_PROBE_TIMEOUT");
                CHECK(r::verifyStartupGraphicsDevice()); // Timeout was not cached as GPU rejection.
                CHECK(p::loadPerformanceSettings(s)==v);
            }
            return failures?1:0; // An inconclusive helper never starts an unverified canvas.
        }
        CHECK(accepted);if(!accepted)return 1;
        if(args.contains("--hidden-probe")){hiddenCompatibilityProbe();return failures?1:0;}
    }
    else {
        settingsPolicy();historyPolicy();compatibilityCachePolicy();controls(args.contains("--artifacts")?args.value(args.indexOf("--artifacts")+1):QString{});
        for(const auto* cap:{"0","1","2","8"}){QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--workers",cap});CHECK(child.waitForFinished(15000)&&child.exitCode()==0);}
    }
    QVulkanInstance instance;int validation{};
    if(native){instance.setApiVersion(QVersionNumber(1,2));instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        if(instance.supportedExtensions().contains("VK_EXT_swapchain_colorspace"))instance.setExtensions({"VK_EXT_swapchain_colorspace"});
        instance.installDebugOutputFilter([&](auto flags,auto types,const void* message){
            if(types.testFlag(QVulkanInstance::ValidationMessage)&&(flags&(QVulkanInstance::ErrorSeverity|QVulkanInstance::WarningSeverity))) {
                ++validation;std::cerr<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage<<'\n';}return false;
        });if(!instance.create())return 2;}
    if(native&&args.contains("--graphics-early-close"))closeBeforeGraphicsStarts(instance);
    else if(native&&args.contains("--graphics-startup-order"))startupGraphicsOrdering(instance);
    else if(native&&(args.contains("--graphics-cancel")||args.contains("--graphics-remap")))
        initializationLifetime(instance,args.contains("--graphics-cancel"),args.contains("--startup-dialog"));
    else integration(native?&instance:nullptr,preferred,args.contains("--expect-fallback"));
    CHECK(validation==0);
    r::completeGraphicsDeviceStartup();
    if(!native)fallbackPopup(args.contains("--artifacts")?args.value(args.indexOf("--artifacts")+1):QString{});
    std::cout<<"Performance checks: "<<failures<<" failures\n";return failures?1:0;
}
