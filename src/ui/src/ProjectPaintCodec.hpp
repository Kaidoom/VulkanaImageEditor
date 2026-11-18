#pragma once

#include "imageeditor/core/ProjectPaint.hpp"
#include "imageeditor/project/ProjectArchive.hpp"
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstring>

namespace imageeditor::ui::detail {

// One compressed member per painted placement. Each record is a pair of
// little-endian tile indices followed by straight RGBA8 pixels in row order.
static_assert(core::ProjectPaint::tileSide == 64 && sizeof(core::Rgba8) == 4);
static_assert(sizeof(core::ProjectPaint::Tile) == 64 * 64 * 4);
inline constexpr int paintRecordSize = 4 + int(sizeof(core::ProjectPaint::Tile));
inline constexpr int paintAxisTiles = 32768 / core::ProjectPaint::tileSide;

inline QString projectPaintPath(std::uint64_t id)
{
    return QStringLiteral("project-paint/%1.tiles").arg(id);
}

inline void validatePaintTile(int x, int y, const core::ProjectPaint::Tile& tile)
{
    // A linked source can shrink and subsequently grow. Its current extent
    // does not bound the paint retained in local coordinates.
    project::require(x >= 0 && y >= 0 && x < paintAxisTiles && y < paintAxisTiles,
        "Project paint tile is outside supported layer dimensions");
    project::require(std::ranges::any_of(tile, [](core::Rgba8 p) { return p.alpha != 0; }),
        "Empty tile in sparse project paint");
}

inline void writeProjectPaint(project::Archive& archive, const QString& path,
    const core::ProjectPaint& paint, project::Progress& progress)
{
    project::writeEntry(archive, path, quint64(paint.tiles.size()) * paintRecordSize, [&](const auto& write) {
        std::array<char, paintRecordSize> record;
        for (const auto& [position, tile] : paint.tiles) {
            const auto [x, y] = position;
            project::require(bool(tile), "Missing project paint tile");
            validatePaintTile(x, y, *tile);
            qToLittleEndian<quint16>(quint16(x), record.data());
            qToLittleEndian<quint16>(quint16(y), record.data() + 2);
            std::memcpy(record.data() + 4, tile->data(), sizeof(*tile));
            write(record.data(), paintRecordSize);
        }
    }, progress);
}

inline core::ProjectPaintState readProjectPaint(project::Archive& archive, const project::Entry& entry,
    project::Progress& progress)
{
    auto paint = std::make_shared<core::ProjectPaint>();
    std::array<char, paintRecordSize> record;
    int filled = 0;
    project::readEntry(archive, entry, [&](const char* data, int bytes) {
        while (bytes > 0) {
            const int count = std::min(bytes, paintRecordSize - filled);
            std::memcpy(record.data() + filled, data, size_t(count));
            data += count;
            bytes -= count;
            filled += count;
            if (filled != paintRecordSize) continue;
            const int x = qFromLittleEndian<quint16>(record.data());
            const int y = qFromLittleEndian<quint16>(record.data() + 2);
            auto tile = std::make_shared<core::ProjectPaint::Tile>();
            std::memcpy(tile->data(), record.data() + 4, sizeof(*tile));
            validatePaintTile(x, y, *tile);
            project::require(paint->tiles.emplace(std::pair{x, y}, std::move(tile)).second,
                "Duplicate project paint tile");
            filled = 0;
        }
    }, progress);
    project::require(filled == 0 && !paint->tiles.empty(), "Incomplete project paint payload");
    return paint;
}

} // namespace imageeditor::ui::detail
