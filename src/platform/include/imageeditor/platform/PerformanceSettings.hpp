#pragma once
#include <QString>
#include <cstdint>
class QSettings;
namespace imageeditor::platform {
// Per-user resource policy, never document content. Zero/empty means Auto.
// Auto preserves the established budgets; these are retention targets, not
// whole-process RAM/VRAM ceilings or reservations.
struct PerformanceSettings {
    int undoMiB{}, documentCacheMiB{}, textureCacheMiB{};
    int processingThreads{}, objectSelectionThreads{};
    QString graphicsDevice;
    bool operator==(const PerformanceSettings&) const = default;
    std::uint64_t undoBytes() const;
    std::uint64_t documentCacheBytes() const;
    std::uint64_t textureCacheBytes() const;
};
PerformanceSettings sanitizePerformanceSettings(PerformanceSettings);
PerformanceSettings loadPerformanceSettings(QSettings&);
bool savePerformanceSettings(QSettings&, PerformanceSettings);
// One immutable process-start snapshot for resources that cannot be changed
// safely while jobs/device allocations are live. Call before starting workers.
const PerformanceSettings& startupPerformanceSettings();
// Startup recovery policy. Once a replacement has presented successfully, the
// UI saves it as the new preference and reports the change once per process.
void useAutomaticGraphicsDeviceForProcess(bool rejectPreferred=false);
QString startupGraphicsDevice();
QString rejectedGraphicsDevice();
bool acceptGraphicsDeviceFallback(const QString& device);
unsigned automaticProcessingThreads();
unsigned resolvedObjectSelectionThreads(const PerformanceSettings&);
}
