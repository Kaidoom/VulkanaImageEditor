#pragma once

#include <QByteArray>
#include <QString>
#include <vulkan/vulkan.h>

namespace imageeditor::render::detail {
// Driver-owned binary data is disposable. Validate identity and integrity before
// handing disk contents back to Vulkan; cache failures never prevent rendering.
inline constexpr qsizetype maximumPipelineCacheBytes = 64 * 1024 * 1024;
QByteArray compositorPipelineKey(const VkPhysicalDeviceProperties&, const QByteArray& spirv);
QByteArray readPipelineCache(const QString& path, const QByteArray& key, const VkPhysicalDeviceProperties&);
bool writePipelineCache(const QString& path, const QByteArray& key, const QByteArray& data);
}
