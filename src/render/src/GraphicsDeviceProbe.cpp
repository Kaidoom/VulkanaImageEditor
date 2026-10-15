#include "imageeditor/render/GraphicsDeviceProbe.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/render/detail/VulkanEnumeration.hpp"
#include "imageeditor/render/detail/GraphicsProbeCache.hpp"
#include "imageeditor/platform/PerformanceSettings.hpp"
#include <QGuiApplication>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QScopedValueRollback>
#include <QCryptographicHash>
#include <QDataStream>
#include <QScreen>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QVulkanInstance>
#include <algorithm>
#include <map>

namespace imageeditor::render {
namespace detail {
namespace {
constexpr auto supportedKey="graphics/presentation-v1/supported";
constexpr auto pendingKey="graphics/presentation-v1/pending";
bool validCacheKey(const QString& key)
{
    return key.size()==64&&std::all_of(key.begin(),key.end(),[](QChar ch){
        return (ch>='0'&&ch<='9')||(ch>='a'&&ch<='f');
    });
}
}
QString graphicsPresentationCacheKey(const QString& device,const QByteArray& driver,const QByteArray& display,bool extendedColorSpace)
{
    if(device.isEmpty()||driver.isEmpty()||display.isEmpty())return {};
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_0);
    // Bump this policy revision when the probe's WSI/format/synchronization
    // contract changes, not on every artwork-shader or executable rebuild.
    stream<<quint32(2)<<device<<driver<<display<<extendedColorSpace;
    return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
bool GraphicsProbeCache::contains(const QString& key) const
{return validCacheKey(key)&&settings_.value(supportedKey).toStringList().contains(key);}
void GraphicsProbeCache::remember(const QString& key)
{
    if(!validCacheKey(key))return;
    auto values=settings_.value(supportedKey).toStringList();
    values.removeIf([&](const QString& item){return item==key||!validCacheKey(item);});
    values.prepend(key);while(values.size()>16)values.removeLast();
    settings_.setValue(supportedKey,values);settings_.sync();
}
void GraphicsProbeCache::invalidate(const QString& key)
{
    auto values=settings_.value(supportedKey).toStringList();
    if(values.removeAll(key)){settings_.setValue(supportedKey,values);settings_.sync();}
}
void GraphicsProbeCache::recoverInterruptedStartup()
{
    const auto key=settings_.value(pendingKey).toString();
    if(key.isEmpty())return;
    invalidate(key);settings_.remove(pendingKey);settings_.sync();
}
void GraphicsProbeCache::beginStartup(const QString& key)
{if(validCacheKey(key)){settings_.setValue(pendingKey,key);settings_.sync();}}
void GraphicsProbeCache::completeStartup(const QString& key)
{if(!key.isEmpty()&&settings_.value(pendingKey).toString()==key){settings_.remove(pendingKey);settings_.sync();}}
}
namespace {
bool verificationEnabled=false;
bool preflightActive=false;
// GUI-thread state. Rejections live only for this launch; durable successes
// are keyed by GPU/driver inventory and the display/Qt/presentation contract.
std::map<QString,GraphicsProbeResult> checkedDevices;
std::map<QString,QByteArray> driverIdentities;
QByteArray displayIdentity;
QString pendingStartupKey;
std::uint64_t startupAttempt{};
QString probeKey(const QString& device,bool extendedColorSpace,const QString& executable)
{return executable+'|'+QGuiApplication::platformName()+'|'+device+(extendedColorSpace?"|extended":"|basic");}
QString persistentKey(const QString& device,bool extendedColorSpace)
{
    const auto found=driverIdentities.find(device);
    return found==driverIdentities.end()?QString{}:detail::graphicsPresentationCacheKey(device,found->second,displayIdentity,extendedColorSpace);
}
QByteArray currentDisplayIdentity()
{
    QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_0);
    stream<<QGuiApplication::platformName()<<QString::fromLatin1(qVersion())<<QSysInfo::kernelVersion();
    for(const auto* name:{"XDG_CURRENT_DESKTOP","XDG_SESSION_DESKTOP","KDE_SESSION_VERSION","WAYLAND_DISPLAY",
        "VK_ICD_FILENAMES","VK_DRIVER_FILES","VK_ADD_DRIVER_FILES","VK_LOADER_DRIVERS_SELECT","VK_LOADER_DRIVERS_DISABLE",
        "VK_INSTANCE_LAYERS","VK_LAYER_PATH","DRI_PRIME","__NV_PRIME_RENDER_OFFLOAD","__VK_LAYER_NV_optimus","MESA_VK_DEVICE_SELECT"})
        stream<<qEnvironmentVariable(name);
    // Screen order, document state, shader caches, executable location and
    // process/session IDs do not change compatibility. Display configuration
    // and every enumerated driver do, including a hybrid compositor's GPU.
    QList<QByteArray> screens;
    for(auto* screen:QGuiApplication::screens()) {
        QByteArray item;QDataStream out(&item,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_0);
        out<<screen->name()<<screen->manufacturer()<<screen->model()<<screen->serialNumber()
            <<screen->geometry()<<screen->depth()<<screen->devicePixelRatio()<<qRound(screen->refreshRate());
        screens.append(item);
    }
    std::sort(screens.begin(),screens.end());stream<<screens;
    for(const auto& [id,driver]:driverIdentities)stream<<id<<driver;
    return bytes;
}
void configureInstance(QVulkanInstance& instance,bool extendedColorSpace)
{
    instance.setApiVersion(QVersionNumber(1,2));
    if(extendedColorSpace&&instance.supportedExtensions().contains("VK_EXT_swapchain_colorspace"))
        instance.setExtensions({"VK_EXT_swapchain_colorspace"});
}
}
bool graphicsDeviceVerificationEnabled(){return verificationEnabled;}
std::uint64_t beginGraphicsDeviceStartup(const QString& device,bool extendedColorSpace)
{
    const auto key=persistentKey(device,extendedColorSpace);
    if(key.isEmpty())return 0;
    if(key!=pendingStartupKey){pendingStartupKey=key;QSettings settings;detail::GraphicsProbeCache(settings).beginStartup(key);}
    return ++startupAttempt;
}
void completeGraphicsDeviceStartup(std::uint64_t attempt)
{
    if(pendingStartupKey.isEmpty()||(attempt&&attempt!=startupAttempt))return;
    QSettings settings;detail::GraphicsProbeCache(settings).completeStartup(pendingStartupKey);pendingStartupKey.clear();
}
void invalidateGraphicsDevicePresentation(const QString& device,bool extendedColorSpace)
{
    checkedDevices.erase(probeKey(device,extendedColorSpace,QCoreApplication::applicationFilePath()));
    QSettings settings;detail::GraphicsProbeCache(settings).invalidate(persistentKey(device,extendedColorSpace));
}
std::optional<GraphicsProbeStatus> cachedGraphicsDevicePresentationStatus(const QString& device,bool extendedColorSpace)
{
    const auto found=checkedDevices.find(probeKey(device,extendedColorSpace,QCoreApplication::applicationFilePath()));
    return found==checkedDevices.end()?std::nullopt:std::optional(found->second.status);
}
GraphicsDeviceInfo describeGraphicsDevice(VkPhysicalDevice device,const VkPhysicalDeviceProperties& properties)
{
    QString id;
    if(properties.apiVersion>=VK_API_VERSION_1_1) {
        VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};info.pNext=&identity;
        vkGetPhysicalDeviceProperties2(device,&info);
        const QByteArray uuid(reinterpret_cast<const char*>(identity.deviceUUID),VK_UUID_SIZE);
        if(uuid!=QByteArray(VK_UUID_SIZE,'\0'))id=QStringLiteral("gpu:%1:%2:%3")
            .arg(properties.vendorID,0,16).arg(properties.deviceID,0,16).arg(QString::fromLatin1(uuid.toHex()));
    }
    int score=0;
    switch(properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:score=10000;break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:score=5000;break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:score=2500;break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:score=500;break;
    default:break;
    }
    score+=int(std::min<std::uint32_t>(properties.limits.maxImageDimension2D/1024U,1000U));
    return {id.toStdString(),properties.deviceName,score};
}
int runGraphicsDeviceProbe(QGuiApplication& app,const QString& device,bool extendedColorSpace)
{
    platform::PerformanceSettings requested;requested.graphicsDevice=device;
    if(device.isEmpty()||platform::sanitizePerformanceSettings(requested).graphicsDevice!=device){qWarning()<<"Invalid GPU probe identity";return 2;}
    QTemporaryDir temporary;
    if(!temporary.isValid()){qWarning()<<"Cannot create GPU probe settings directory";return 3;}
    app.setOrganizationName(QStringLiteral("VulkanaGpuProbe"));
    app.setApplicationName(QStringLiteral("VulkanaGpuProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,temporary.path());
    QSettings settings;if(!platform::savePerformanceSettings(settings,requested)){qWarning()<<"Cannot save isolated GPU probe settings";return 3;}
    QVulkanInstance instance;configureInstance(instance,extendedColorSpace);
    if(!instance.create()){qWarning()<<"Cannot initialize GPU probe instance"<<instance.errorCode();return 3;}
    CanvasWindow window(true);window.setVulkanInstance(&instance);
    window.setTitle(QStringLiteral("Vulkana GPU compatibility check"));
    window.setFlags(Qt::Tool|Qt::FramelessWindowHint|Qt::WindowDoesNotAcceptFocus);
    // Create the native surface without mapping a desktop window. The buffer
    // must still pass the compositor's import/commit path, but has no visible
    // shell role and cannot flash a small square or take keyboard focus.
    window.resize(32,32);window.create();window.scheduleFrame();
    QTimer poll,deadline;bool presented=false;
    const auto confirmPresentation=[&] {
        if(presented)return true;
        const auto stats=window.rendererStats();
        if(!stats.presentQueuedFrames)return false;
        presented=true;poll.stop();deadline.stop();
        if(stats.deviceId!=device.toStdString()||!stats.compositionError.empty()){app.exit(2);return true;}
        if(window.isVisible()||window.isExposed()){app.exit(3);return true;}
        // A successful queue-present is not enough: let the compositor import
        // the buffer, then round-trip before accepting this exact adapter.
        QTimer::singleShot(300,&app,[&] {QGuiApplication::sync();app.exit(0);});
        return true;
    };
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {confirmPresentation();});
    // Even an overdue timer must observe a frame queued during a slow driver
    // call. It cannot override the successful-present confirmation period.
    QObject::connect(&deadline,&QTimer::timeout,&app,[&] {if(!confirmPresentation())app.exit(3);});
    poll.start(20);deadline.setSingleShot(true);deadline.start(10000);
    return app.exec();
}
GraphicsProbeResult checkGraphicsDevicePresentation(const QString& device,bool extendedColorSpace,const QString& executable,const std::function<bool()>& cancelled)
{
    if(cancelled&&cancelled())return {GraphicsProbeStatus::Inconclusive,QStringLiteral("Graphics initialization cancelled.")};
    if(!QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))return {GraphicsProbeStatus::Supported,{}};
    if(device.isEmpty())return {GraphicsProbeStatus::Inconclusive,QStringLiteral("The graphics device has no stable identity.")};
    const auto program=executable.isEmpty()?QCoreApplication::applicationFilePath():executable;
    const auto key=probeKey(device,extendedColorSpace,program);
    if(const auto found=checkedDevices.find(key);found!=checkedDevices.end())return found->second;
    // An explicit helper override is diagnostic and must not be bypassed by a
    // previous result from the app's own helper.
    const auto savedKey=executable.isEmpty()?persistentKey(device,extendedColorSpace):QString{};
    QSettings settings;detail::GraphicsProbeCache cache(settings);
    if(cache.contains(savedKey)) {
        qInfo().noquote()<<"Using cached GPU presentation compatibility:"<<device;
        return checkedDevices.emplace(key,GraphicsProbeResult{GraphicsProbeStatus::Supported,{}}).first->second;
    }
    // The parent owns this root even if the child dies in Qt/driver code.
    QTemporaryDir temporary;
    if(!temporary.isValid())return {GraphicsProbeStatus::Inconclusive,QStringLiteral("Cannot prepare the graphics compatibility check.")};
    QProcess probe;probe.setProcessChannelMode(QProcess::MergedChannels);
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TMPDIR"),temporary.path());probe.setProcessEnvironment(environment);
    QStringList arguments{QString::fromLatin1(graphicsDeviceProbeOption),device,QStringLiteral("-platform"),QGuiApplication::platformName()};
    if(!extendedColorSpace)arguments.append(QString::fromLatin1(graphicsDeviceProbeBasicColorOption));
    // Keep Qt alive during helper startup/confirmation, including a slow or
    // unavailable driver. No document/canvas exists in this startup phase.
    QEventLoop loop;QTimer deadline,cancellation;bool done=false,finished=false,timedOut=false,wasCancelled=false;
    QObject::connect(&probe,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&loop,[&]{finished=true;done=true;loop.quit();});
    QObject::connect(&probe,&QProcess::errorOccurred,&loop,[&](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){done=true;loop.quit();}
    });
    deadline.setSingleShot(true);deadline.setInterval(15000);
    QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{timedOut=true;done=true;loop.quit();});
    cancellation.setInterval(50);
    QObject::connect(&cancellation,&QTimer::timeout,&loop,[&]{if(cancelled&&cancelled()){wasCancelled=true;done=true;loop.quit();}});
    probe.start(program,arguments);
    if(preflightActive) {
        deadline.start();if(cancelled)cancellation.start();
        if(!done)loop.exec();
    } else {
        // A runtime device-recovery call may originate inside render(). Do not
        // nest the GUI loop there and re-enter native-surface/render teardown.
        finished=probe.waitForFinished(15000);timedOut=!finished&&probe.error()==QProcess::Timedout;
    }
    if(!finished&&probe.state()!=QProcess::NotRunning){probe.kill();probe.waitForFinished(1000);}
    GraphicsProbeResult result;
    if(!timedOut&&!wasCancelled&&finished&&probe.exitStatus()==QProcess::NormalExit&&probe.exitCode()==0)result.status=GraphicsProbeStatus::Supported;
    else {
        // Code 3/timeouts and helper-launch failures say nothing about GPU
        // compatibility. Do not cache them as rejection or switch preference.
        if(!timedOut&&!wasCancelled&&finished&&(probe.exitStatus()==QProcess::CrashExit||probe.exitCode()==2||probe.exitCode()==255))
            result.status=GraphicsProbeStatus::Rejected;
        result.diagnostic=QStringLiteral("GPU presentation check: %1 (exit %2). %3%4")
            .arg(result.status==GraphicsProbeStatus::Rejected?QStringLiteral("rejected"):QStringLiteral("inconclusive"))
            .arg(probe.exitCode()).arg(finished?QString{}:probe.errorString()+QStringLiteral(". "))
            .arg(QString::fromLocal8Bit(probe.readAll().right(8192)));
        qWarning().noquote()<<result.diagnostic;
    }
    if(result.status!=GraphicsProbeStatus::Inconclusive)checkedDevices.emplace(key,result);
    if(result.status==GraphicsProbeStatus::Supported)cache.remember(savedKey);
    return result;
}
GraphicsStartupResult chooseVerifiedGraphicsDevice(std::vector<GraphicsDeviceInfo> devices,const QString& preferred,
    const std::function<GraphicsProbeResult(const QString&)>& probe)
{
    while(const auto chosen=selectGraphicsDevice(devices,preferred.toStdString())) {
        auto& device=devices[*chosen];const auto id=QString::fromStdString(device.id);
        auto result=probe(id);
        if(result.status==GraphicsProbeStatus::Inconclusive)result=probe(id);
        if(result.status==GraphicsProbeStatus::Supported)return {id,{}};
        if(result.status==GraphicsProbeStatus::Inconclusive)return {{},QStringLiteral(
            "The presentation check for %1 could not finish. Your GPU preference has not changed. Please try starting Vulkana again.\n\n%2")
            .arg(QString::fromStdString(device.name),result.diagnostic)};
        device.availability=GraphicsDeviceAvailability::StartupCheckFailed;
    }
    return {{},QStringLiteral("No graphics device passed the display compatibility check. Your GPU preference has not changed.")};
}
bool verifyStartupGraphicsDevice(const QString& executable,QString* error,const std::function<bool()>& cancelled)
{
    if(!QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))return true;
    const QScopedValueRollback preflight(preflightActive,true);
    if(!verificationEnabled){QSettings settings;detail::GraphicsProbeCache(settings).recoverInterruptedStartup();}
    verificationEnabled=true;
    QVulkanInstance instance;configureInstance(instance,true);
    if(!instance.create()){if(error)*error=QStringLiteral("Cannot initialize Vulkan for the graphics compatibility check.");return false;}
    const auto vkInstance=instance.vkInstance();
    const auto enumeration=detail::enumerateVulkanValues<VkPhysicalDevice>([&](auto* count,auto* values){return vkEnumeratePhysicalDevices(vkInstance,count,values);});
    if(enumeration.result!=VK_SUCCESS){if(error)*error=QStringLiteral("Cannot enumerate Vulkan graphics devices.");return false;}
    std::vector<GraphicsDeviceInfo> devices;
    driverIdentities.clear();
    for(auto device:enumeration.values) {
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(device,&properties);
        devices.push_back(describeGraphicsDevice(device,properties));
        QByteArray driver;QDataStream stream(&driver,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_6_0);
        stream<<properties.vendorID<<properties.deviceID<<properties.apiVersion<<properties.driverVersion
            <<QByteArray(reinterpret_cast<const char*>(properties.pipelineCacheUUID),VK_UUID_SIZE);
        if(properties.apiVersion>=VK_API_VERSION_1_1) {
            VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};info.pNext=&identity;
            vkGetPhysicalDeviceProperties2(device,&info);
            stream<<QByteArray(reinterpret_cast<const char*>(identity.driverUUID),VK_UUID_SIZE);
        }
        driverIdentities.emplace(QString::fromStdString(devices.back().id),driver);
    }
    displayIdentity=currentDisplayIdentity();
    const auto result=chooseVerifiedGraphicsDevice(std::move(devices),platform::startupPerformanceSettings().graphicsDevice,
        [&](const QString& id){return checkGraphicsDevicePresentation(id,instance.extensions().contains("VK_EXT_swapchain_colorspace"),executable,cancelled);});
    if(cancelled&&cancelled()){if(error)error->clear();return false;}
    if(error)*error=result.error;
    if(result.device.isEmpty())return false;
    platform::useVerifiedGraphicsDeviceForProcess(result.device);
    return true;
}
}
