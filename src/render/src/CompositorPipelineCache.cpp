#include "imageeditor/render/detail/CompositorPipelineCache.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
#include <cstring>

namespace imageeditor::render::detail {
namespace {
constexpr auto magic = "VKNPIPE1";
constexpr qsizetype envelopeBytes = 8 + 32 + 32;
QByteArray digest(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
}
QByteArray compositorPipelineKey(const VkPhysicalDeviceProperties& properties, const QByteArray& spirv)
{
    if (spirv.isEmpty()) return {};
    QByteArray identity(magic, 8);
    // Bump this policy if the compositor's descriptor/push layout contract changes.
    for (quint32 value : {properties.vendorID, properties.deviceID, properties.driverVersion,
             properties.apiVersion, quint32(sizeof(void*)), quint32(1)}) {
        value = qToLittleEndian(value);
        identity.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    identity.append(reinterpret_cast<const char*>(properties.pipelineCacheUUID), VK_UUID_SIZE);
    identity.append(digest(spirv));
    return digest(identity);
}
QByteArray readPipelineCache(const QString& path, const QByteArray& key, const VkPhysicalDeviceProperties& properties)
{
    QFile file(path);
    if (key.size() != 32 || !file.open(QIODevice::ReadOnly)
        || file.size() < envelopeBytes + 32 || file.size() > envelopeBytes + maximumPipelineCacheBytes) return {};
    const auto bytes = file.read(maximumPipelineCacheBytes + envelopeBytes + 1);
    if (bytes.size() < envelopeBytes + 32 || bytes.size() != file.size() || bytes.left(8) != QByteArray(magic, 8)
        || bytes.mid(8, 32) != key) return {};
    const auto data = bytes.mid(envelopeBytes);
    if (digest(data) != bytes.mid(40, 32)) return {};
    // Vulkan's version-one cache header is specified as little-endian bytes.
    const auto* header = reinterpret_cast<const uchar*>(data.constData());
    const auto headerSize = qFromLittleEndian<quint32>(header);
    if (headerSize < 32 || headerSize > quint32(data.size())
        || qFromLittleEndian<quint32>(header + 4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        || qFromLittleEndian<quint32>(header + 8) != properties.vendorID
        || qFromLittleEndian<quint32>(header + 12) != properties.deviceID
        || std::memcmp(header + 16, properties.pipelineCacheUUID, VK_UUID_SIZE)) return {};
    return data;
}
bool writePipelineCache(const QString& path, const QByteArray& key, const QByteArray& data)
{
    if (path.isEmpty() || key.size() != 32 || data.size() < 32 || data.size() > maximumPipelineCacheBytes) return false;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray envelope = QByteArray(magic, 8) + key + digest(data);
    return file.write(envelope) == envelope.size() && file.write(data) == data.size() && file.commit();
}
}
