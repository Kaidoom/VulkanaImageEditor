#include "imageeditor/render/detail/CompositorPipelineCache.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstring>
#include <iostream>

namespace d = imageeditor::render::detail;
int failures{};
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory; CHECK(directory.isValid());
    const auto path = directory.filePath("vulkan/compositor.bin");
    VkPhysicalDeviceProperties gpu{};
    gpu.vendorID = 0x10de; gpu.deviceID = 0x2704; gpu.driverVersion = 1; gpu.apiVersion = VK_API_VERSION_1_2;
    std::memcpy(gpu.pipelineCacheUUID, "0123456789abcdef", VK_UUID_SIZE);
    QByteArray payload(64, 'a');
    auto* header = reinterpret_cast<uchar*>(payload.data());
    qToLittleEndian<quint32>(32, header);
    qToLittleEndian<quint32>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE, header + 4);
    qToLittleEndian<quint32>(gpu.vendorID, header + 8);
    qToLittleEndian<quint32>(gpu.deviceID, header + 12);
    std::memcpy(header + 16, gpu.pipelineCacheUUID, VK_UUID_SIZE);
    const auto key = d::compositorPipelineKey(gpu, "shader-a");
    CHECK(!key.isEmpty()); CHECK(d::compositorPipelineKey(gpu, {}).isEmpty());
    CHECK(d::readPipelineCache(path, key, gpu).isEmpty());
    CHECK(d::writePipelineCache(path, key, payload));
    CHECK(d::readPipelineCache(path, key, gpu) == payload);
    CHECK(d::readPipelineCache(path, d::compositorPipelineKey(gpu, "shader-b"), gpu).isEmpty());
    for (int field = 0; field < 4; ++field) {
        auto other = gpu;
        if (field == 0) ++other.driverVersion;
        if (field == 1) ++other.deviceID;
        if (field == 2) ++other.vendorID;
        if (field == 3) ++other.pipelineCacheUUID[0];
        CHECK(d::compositorPipelineKey(other, "shader-a") != key);
        CHECK(d::readPipelineCache(path, d::compositorPipelineKey(other, "shader-a"), other).isEmpty());
        if (field != 0) CHECK(d::readPipelineCache(path, key, other).isEmpty());
    }
    QFile file(path); CHECK(file.open(QIODevice::ReadOnly));
    const auto good = file.readAll(); file.close();
    const auto replace = [&](const QByteArray& bytes) {
        CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        CHECK(file.write(bytes) == bytes.size()); file.close();
    };
    auto damaged = good; damaged[damaged.size() - 1] ^= 1; replace(damaged);
    CHECK(d::readPipelineCache(path, key, gpu).isEmpty()); // Payload corruption.
    for (qsizetype length : {qsizetype(0), qsizetype(8), qsizetype(72), good.size() - 1}) {
        replace(good.first(length)); CHECK(d::readPipelineCache(path, key, gpu).isEmpty());
    }
    auto invalidHeader = payload; invalidHeader[4] = char(99);
    CHECK(d::writePipelineCache(path, key, invalidHeader));
    CHECK(d::readPipelineCache(path, key, gpu).isEmpty()); // Integrity alone is insufficient.
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.resize(d::maximumPipelineCacheBytes + 73)); file.close();
    CHECK(d::readPipelineCache(path, key, gpu).isEmpty()); // Reject oversized files before allocation.
    CHECK(d::writePipelineCache(path, key, payload));
    CHECK(d::readPipelineCache(path, key, gpu) == payload); // Recover atomically from a bad cache.
    CHECK(!d::writePipelineCache(directory.path(), key, payload));
    CHECK(d::readPipelineCache(path, key, gpu) == payload);
    return failures ? 1 : 0;
}
