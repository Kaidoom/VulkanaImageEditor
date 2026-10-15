#pragma once
#include <vulkan/vulkan.h>
#include "imageeditor/render/GraphicsDeviceSelection.hpp"
#include <QString>
#include <functional>
#include <cstdint>
#include <vector>
class QGuiApplication;
namespace imageeditor::render {
inline constexpr auto graphicsDeviceProbeOption="--internal-gpu-probe";
inline constexpr auto graphicsDeviceProbeBasicColorOption="--internal-gpu-probe-basic-colorspace";
enum class GraphicsProbeStatus { Supported, Rejected, Inconclusive };
struct GraphicsProbeResult {
    GraphicsProbeStatus status{GraphicsProbeStatus::Inconclusive};
    QString diagnostic;
};
// Shared identity/ranking for startup inventory and the actual canvas surface.
GraphicsDeviceInfo describeGraphicsDevice(VkPhysicalDevice,const VkPhysicalDeviceProperties&);
bool graphicsDeviceVerificationEnabled();
// Track actual initialization separately from the shader-free compatibility
// helper. An interrupted startup invalidates only its cached GPU/configuration.
std::uint64_t beginGraphicsDeviceStartup(const QString& device,bool extendedColorSpace);
void completeGraphicsDeviceStartup(std::uint64_t attempt=0);
void invalidateGraphicsDevicePresentation(const QString& device,bool extendedColorSpace);
std::optional<GraphicsProbeStatus> cachedGraphicsDevicePresentationStatus(const QString&,bool extendedColorSpace);
// Dispatch this internal mode before loading user preferences or opening any
// documents. Only clear/present a swapchain image: no artwork or shader pipelines.
int runGraphicsDeviceProbe(QGuiApplication&,const QString& device,bool extendedColorSpace=true);
GraphicsProbeResult checkGraphicsDevicePresentation(const QString& device,bool extendedColorSpace=true,const QString& executable={},
    const std::function<bool()>& cancelled={});
// Ordered, bounded policy shared by real startup and deterministic tests. A
// timeout is retried once, then stops startup without rejecting the adapter or
// changing preferences. Only an explicitly rejected adapter permits fallback.
struct GraphicsStartupResult { QString device, error; };
GraphicsStartupResult chooseVerifiedGraphicsDevice(std::vector<GraphicsDeviceInfo>,const QString& preferred,
    const std::function<GraphicsProbeResult(const QString&)>& probe);
// Wayland can terminate a client on failed cross-GPU dmabuf import even after
// Vulkan reports presentation support. Check both preferred and Auto candidates
// in isolated processes. False means no verified adapter; do not start a canvas.
// No preference is rewritten here; normal successful-presentation recovery owns it.
bool verifyStartupGraphicsDevice(const QString& executable={},QString* error=nullptr,const std::function<bool()>& cancelled={});
}
