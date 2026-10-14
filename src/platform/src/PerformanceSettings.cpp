#include "imageeditor/platform/PerformanceSettings.hpp"
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
namespace imageeditor::platform {
namespace {
std::atomic_bool automaticGraphicsDevice{};
std::atomic_bool rejectPreferredGraphicsDevice{};
std::mutex graphicsDeviceMutex;
std::optional<QString> resolvedGraphicsDevice;
int memoryValue(int value) { return value<=0||value>8192?0:std::max(64,value); }
std::uint64_t bytes(int value,int fallback) { value=memoryValue(value);return std::uint64_t(value?value:fallback)*1024*1024; }
}
std::uint64_t PerformanceSettings::undoBytes() const { return bytes(undoMiB,256); }
std::uint64_t PerformanceSettings::documentCacheBytes() const { return bytes(documentCacheMiB,512); }
std::uint64_t PerformanceSettings::textureCacheBytes() const { return bytes(textureCacheMiB,512); }
PerformanceSettings sanitizePerformanceSettings(PerformanceSettings s)
{
    s.undoMiB=memoryValue(s.undoMiB);s.documentCacheMiB=memoryValue(s.documentCacheMiB);s.textureCacheMiB=memoryValue(s.textureCacheMiB);
    if(s.processingThreads<0||s.processingThreads>8)s.processingThreads=0;
    if(s.objectSelectionThreads<0||s.objectSelectionThreads>6)s.objectSelectionThreads=0;
    static const QRegularExpression identity(QStringLiteral("^gpu:[0-9a-f]{1,8}:[0-9a-f]{1,8}:[0-9a-f]{32}$"));
    if(!s.graphicsDevice.isEmpty()&&!identity.match(s.graphicsDevice).hasMatch())s.graphicsDevice.clear();
    return s;
}
PerformanceSettings loadPerformanceSettings(QSettings& settings)
{
    settings.beginGroup(QStringLiteral("preferences/performance"));
    PerformanceSettings s;
    s.undoMiB=settings.value("undoMiB",0).toInt();s.documentCacheMiB=settings.value("documentCacheMiB",0).toInt();
    s.textureCacheMiB=settings.value("textureCacheMiB",0).toInt();s.processingThreads=settings.value("processingThreads",0).toInt();
    s.objectSelectionThreads=settings.value("objectSelectionThreads",0).toInt();s.graphicsDevice=settings.value("graphicsDevice").toString();
    settings.endGroup();return sanitizePerformanceSettings(std::move(s));
}
bool savePerformanceSettings(QSettings& settings,PerformanceSettings s)
{
    s=sanitizePerformanceSettings(std::move(s));settings.beginGroup(QStringLiteral("preferences/performance"));
    settings.setValue("undoMiB",s.undoMiB);settings.setValue("documentCacheMiB",s.documentCacheMiB);
    settings.setValue("textureCacheMiB",s.textureCacheMiB);settings.setValue("processingThreads",s.processingThreads);
    settings.setValue("objectSelectionThreads",s.objectSelectionThreads);settings.setValue("graphicsDevice",s.graphicsDevice);
    settings.endGroup();settings.sync();return settings.status()==QSettings::NoError;
}
const PerformanceSettings& startupPerformanceSettings()
{
    static const auto value=[] { QSettings settings;return loadPerformanceSettings(settings); }();return value;
}
void useAutomaticGraphicsDeviceForProcess(bool rejectPreferred)
{
    rejectPreferredGraphicsDevice.store(rejectPreferred);automaticGraphicsDevice.store(true);
}
QString startupGraphicsDevice()
{
    const std::lock_guard lock(graphicsDeviceMutex);
    if(resolvedGraphicsDevice)return *resolvedGraphicsDevice;
    return automaticGraphicsDevice.load()?QString{}:startupPerformanceSettings().graphicsDevice;
}
bool acceptGraphicsDeviceFallback(const QString& device)
{
    const std::lock_guard lock(graphicsDeviceMutex);
    if(resolvedGraphicsDevice&&*resolvedGraphicsDevice==device)return false;
    resolvedGraphicsDevice=device;return true;
}
QString rejectedGraphicsDevice()
{
    return rejectPreferredGraphicsDevice.load()?startupPerformanceSettings().graphicsDevice:QString{};
}
unsigned automaticProcessingThreads() { return std::clamp(std::thread::hardware_concurrency(),1U,8U); }
unsigned resolvedObjectSelectionThreads(const PerformanceSettings& s)
{
    const auto hardware=std::max(1U,std::thread::hardware_concurrency());
    return s.objectSelectionThreads>0?std::clamp(unsigned(s.objectSelectionThreads),1U,std::min(hardware,6U))
        :std::clamp(hardware/2,1U,6U);
}
}
