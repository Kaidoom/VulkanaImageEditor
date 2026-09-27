#include "imageeditor/core/Document.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <utility>
#include <vector>

namespace {
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
constexpr std::size_t layerCount = 3;
constexpr double MiB = 1024.0 * 1024.0;

void require(bool result, const char* message)
{
    if (!result) throw std::runtime_error(message);
}

double elapsedMs(const QElapsedTimer& timer)
{
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

struct MemoryUsage {
    std::uint64_t highWaterKiB {0};
    std::uint64_t residentKiB {0};
};

MemoryUsage memoryUsage()
{
    QFile status(QStringLiteral("/proc/self/status"));
    require(status.open(QIODevice::ReadOnly), "Could not read /proc/self/status");
    MemoryUsage result;
    bool foundHighWater = false;
    bool foundResident = false;
    // VmHWM belongs to this executable's current address space, unlike
    // ru_maxrss which may retain the launching tool worker's pre-exec peak.
    // This is read-only: do not reset kernel accounting via clear_refs.
    for (const auto& line : status.readAll().split('\n')) {
        const bool highWater = line.startsWith("VmHWM:");
        const bool resident = line.startsWith("VmRSS:");
        if (!highWater && !resident) continue;
        const auto fields = line.simplified().split(' ');
        require(fields.size() == 3 && fields[2] == "kB", "Unexpected /proc memory field");
        bool parsed = false;
        const auto value = fields[1].toULongLong(&parsed);
        require(parsed, "Invalid /proc memory field");
        if (highWater) {
            result.highWaterKiB = static_cast<std::uint64_t>(value);
            foundHighWater = true;
        } else {
            result.residentKiB = static_cast<std::uint64_t>(value);
            foundResident = true;
        }
    }
    require(foundHighWater && foundResident, "Missing /proc VmHWM or VmRSS");
    return result;
}

long legacyPeakRssKiB()
{
    rusage usage {};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
    // Diagnostic only: exec does not necessarily reset this inherited value.
    return usage.ru_maxrss;
}

std::uint32_t noise(std::uint32_t x, std::uint32_t y, std::uint32_t seed)
{
    auto value = x * 0x9e3779b9U ^ y * 0x85ebca6bU ^ seed;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    return value ^ (value >> 16U);
}

std::shared_ptr<c::RasterSurface> makeRaster(c::Extent2u extent, std::uint32_t index)
{
    auto surface = std::make_shared<c::ContiguousRasterSurface>(extent);
    const auto stride = static_cast<std::size_t>(extent.width) * 4;
    std::vector<std::byte> row(stride);
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        const auto greenGradient = y * 255U / (extent.height - 1U);
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            const auto redGradient = x * 255U / (extent.width - 1U);
            const auto texture = noise(x, y, 0x31f7a90bU + index * 19U);
            const auto bands = (x / 37U + y / 53U + index * 5U) & 31U;
            const auto offset = static_cast<std::size_t>(x) * 4;
            // Colorful gradients, mild fine grain, and broad bands resemble
            // edited raster content without either zero-filled compression or
            // a deliberately incompressible full-range random-byte fixture.
            row[offset] = std::byte((redGradient + index * 43U + (texture & 7U)) & 255U);
            row[offset + 1] = std::byte((greenGradient + index * 71U + ((texture >> 3U) & 7U)) & 255U);
            row[offset + 2] = std::byte(((redGradient + greenGradient) / 2U + bands * 3U
                + index * 29U + ((texture >> 6U) & 7U)) & 255U);
            const auto coverageBand = (x / 157U + y / 113U + index) % 7U;
            const auto alpha = coverageBand == 0U ? 0U
                : coverageBand == 1U ? 64U + ((texture >> 9U) & 63U)
                                     : 192U + ((texture >> 15U) & 63U);
            // Preserve nonzero hidden RGB too: transparent samples are still
            // authoritative straight-RGBA8 bytes, not disposable display data.
            row[offset + 3] = std::byte(alpha);
        }
        (void)surface->replaceRgba8({0, static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(extent.width), 1}, row, stride);
    }
    return surface;
}

QByteArray rasterHash(const c::RasterSurface& surface)
{
    const auto extent = surface.extent();
    const auto stride = static_cast<std::size_t>(extent.width) * 4;
    const auto rowsPerChunk = std::max<std::size_t>(1, (256 * 1024) / stride);
    std::vector<std::byte> buffer(rowsPerChunk * stride);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (std::uint32_t y = 0; y < extent.height;) {
        const auto height = static_cast<std::uint32_t>(std::min<std::size_t>(rowsPerChunk, extent.height - y));
        surface.copyRgba8({0, static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(height)}, buffer, stride);
        const auto bytes = static_cast<qsizetype>(static_cast<std::size_t>(height) * stride);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(buffer.data()), bytes));
        y += height;
    }
    return hash.result();
}

void run(c::Extent2u extent)
{
    // Both source and loaded documents die on return before the next dimension
    // starts. The allocator/OS may retain pages; VmHWM remains cumulative for
    // this executable's address space rather than resetting between cases.
    const auto memoryBefore = memoryUsage();
    const auto legacyPeakBefore = legacyPeakRssKiB();
    QTemporaryDir temporary;
    require(temporary.isValid(), "Could not create benchmark temporary directory");
    const auto path = temporary.filePath(QStringLiteral("persistence-%1x%2.vulkana")
        .arg(extent.width).arg(extent.height));

    QElapsedTimer timer;
    timer.start();
    c::Document source({extent, 144.0});
    for (std::size_t i = 0; i < layerCount; ++i) {
        auto layer = c::Layer::raster("Procedural gradient " + std::to_string(i + 1),
            makeRaster(extent, static_cast<std::uint32_t>(i)));
        layer.visible = i != 1;
        layer.opacity = i == 2 ? 0.625F : 1.0F;
        if (i == 2) {
            // Nonidentity, off-canvas affine content must remain complete and
            // editable rather than being clipped or flattened on persistence.
            layer.localToDocument = {0.875, -0.375, -static_cast<double>(extent.width) * 0.25,
                0.375, 0.875, static_cast<double>(extent.height) * 0.125};
        }
        require(source.insertLayer(i, std::move(layer)), "Could not insert benchmark layer");
    }
    const auto generateMs = elapsedMs(timer);
    const auto memorySource = memoryUsage();
    const auto sourceBytes = static_cast<std::uint64_t>(extent.width) * extent.height * 4U * layerCount;
    const auto sourceRevision = source.revision();
    std::array<QByteArray, layerCount> sourceHashes;
    std::array<c::Revision, layerCount> surfaceRevisions {};
    timer.restart();
    for (std::size_t i = 0; i < layerCount; ++i) {
        const auto& raster = *std::get<c::RasterLayer>(source.layers()[i].payload).surface;
        sourceHashes[i] = rasterHash(raster);
        surfaceRevisions[i] = raster.revision();
    }
    const auto sourceHashMs = elapsedMs(timer);

    timer.restart();
    const auto saved = u::saveProject(path, source);
    const auto saveMs = elapsedMs(timer);
    if (!saved) throw std::runtime_error("Save failed: " + saved.error.toStdString());
    const auto memorySaved = memoryUsage();
    const auto archiveBytes = QFileInfo(path).size();
    require(archiveBytes > 0, "Saved archive is missing or empty");

    timer.restart();
    auto loaded = u::loadProject(path);
    const auto loadMs = elapsedMs(timer);
    if (!loaded) throw std::runtime_error("Load failed: " + loaded.error.toStdString());
    const auto memoryLoaded = memoryUsage();

    timer.restart();
    require(source.revision() == sourceRevision, "Save changed document revision");
    require(loaded.document->canvas() == source.canvas(), "Canvas failed exact round trip");
    require(loaded.document->layers().size() == layerCount, "Layer count failed round trip");
    for (std::size_t i = 0; i < layerCount; ++i) {
        const auto& original = source.layers()[i];
        const auto& restored = loaded.document->layers()[i];
        require(original.id == restored.id && original.name == restored.name
            && original.visible == restored.visible && original.opacity == restored.opacity
            && original.localToDocument == restored.localToDocument,
            "Layer order, identity, visibility, opacity, or transform failed exact round trip");
        const auto* restoredRaster = std::get_if<c::RasterLayer>(&restored.payload);
        require(restoredRaster && restoredRaster->surface, "Raster payload failed round trip");
        const auto& originalRaster = *std::get<c::RasterLayer>(original.payload).surface;
        require(originalRaster.revision() == surfaceRevisions[i], "Save changed raster revision");
        require(originalRaster.extent() == restoredRaster->surface->extent(), "Raster extent failed round trip");
        require(sourceHashes[i] == rasterHash(*restoredRaster->surface),
            "Full-stream SHA-256 mismatch: raster bytes failed exact round trip");
    }
    const auto verifyMs = elapsedMs(timer);
    const auto memoryFinal = memoryUsage();
    const auto legacyPeakFinal = legacyPeakRssKiB();

    std::cout << extent.width << 'x' << extent.height
              << " layers=" << layerCount << " hidden_layers=1 off_canvas_layers=1"
              << " generate_ms=" << generateMs << " source_hash_ms=" << sourceHashMs
              << " save_including_crc_verification_ms=" << saveMs
              << " load_ms=" << loadMs << " full_round_trip_verify_ms=" << verifyMs
              << " archive_bytes=" << archiveBytes
              << " archive_MiB=" << static_cast<double>(archiveBytes) / MiB
              << " archive_to_source_ratio=" << static_cast<double>(archiveBytes) / static_cast<double>(sourceBytes)
              << " live_source_raster_bytes=" << sourceBytes
              << " live_source_raster_MiB=" << static_cast<double>(sourceBytes) / MiB
              << " source_plus_loaded_raster_MiB=" << static_cast<double>(sourceBytes) * 2.0 / MiB
              << " vm_hwm_before_KiB=" << memoryBefore.highWaterKiB
              << " vm_rss_before_KiB=" << memoryBefore.residentKiB
              << " vm_hwm_source_KiB=" << memorySource.highWaterKiB
              << " vm_rss_source_KiB=" << memorySource.residentKiB
              << " vm_hwm_saved_KiB=" << memorySaved.highWaterKiB
              << " vm_rss_saved_KiB=" << memorySaved.residentKiB
              << " vm_hwm_loaded_KiB=" << memoryLoaded.highWaterKiB
              << " vm_rss_loaded_KiB=" << memoryLoaded.residentKiB
              << " vm_hwm_final_KiB=" << memoryFinal.highWaterKiB
              << " vm_rss_final_KiB=" << memoryFinal.residentKiB
              << " diagnostic_inherited_ru_maxrss_before_KiB=" << legacyPeakBefore
              << " diagnostic_inherited_ru_maxrss_final_KiB=" << legacyPeakFinal
              << " sha256_verified_layers=" << layerCount
              << " sha256_verified_rgba_bytes=" << sourceBytes
              << " warnings=" << loaded.warnings.size() << '\n';
    for (const auto& warning : loaded.warnings)
        std::cerr << "Load warning: " << warning.toStdString() << '\n';
    std::cout.flush();
    // QTemporaryDir removes both the archive and directory normally, including
    // when an exception propagates. This benchmark never touches user files.
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    const auto dimension = arguments.size() > 1 ? arguments[1].toLower() : QStringLiteral("both");
    if (arguments.size() > 2 || (dimension != "4k" && dimension != "5k" && dimension != "both")) {
        std::cerr << "Usage: imageeditor_project_persistence_benchmarks [4k|5k|both]\n";
        return 2;
    }
    try {
        std::cout << std::fixed << std::setprecision(3)
                  << "CPU-only project persistence benchmark; no Vulkan or framebuffer readback.\n"
                  << "Save timing includes the writer's full archive CRC verification, flush/fsync, and atomic replacement.\n"
                  << "vm_hwm fields read Linux /proc/self/status VmHWM: peak RSS for this executable's address space, cumulative across cases; vm_rss is current RSS at each checkpoint (KiB).\n"
                  << "diagnostic_inherited_ru_maxrss may include a tool worker's pre-exec high-water mark; it is NOT a valid new-executable peak measurement. No kernel accounting is reset.\n"
                  << "Source and loaded documents are destroyed between cases; raster byte counts exclude metadata, codec buffers, and allocator overhead.\n"
                  << "Files are temporary; immediate reload uses the normal OS page cache (not a cold-disk benchmark).\n";
        if (dimension == "4k" || dimension == "both") run({3840, 2160});
        if (dimension == "5k" || dimension == "both") run({5120, 2880});
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
