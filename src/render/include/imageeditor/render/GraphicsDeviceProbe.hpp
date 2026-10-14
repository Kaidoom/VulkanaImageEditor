#pragma once
#include <QString>
class QGuiApplication;
namespace imageeditor::render {
inline constexpr auto graphicsDeviceProbeOption="--internal-gpu-probe";
// Dispatch this internal mode before loading user preferences or opening any
// documents. It renders only an empty canvas using isolated temporary settings.
int runGraphicsDeviceProbe(QGuiApplication&,const QString& device);
// Wayland can terminate a client on failed cross-GPU dmabuf import even after
// Vulkan reports presentation support. Probe an explicit choice in a separate
// process before creating the real canvas. Auto and other platforms skip this.
// A failed check selects Auto for this process without rewriting preferences.
bool verifyStartupGraphicsDevice(const QString& executable={});
}
