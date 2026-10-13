#pragma once
#include "imageeditor/project/ProjectArchive.hpp"
#include <QImage>
#include <QJsonObject>

namespace imageeditor::project {
inline constexpr int previewSide = 512;
inline constexpr qsizetype previewPngBudget = 2 * 1024 * 1024;

// Invalidates previews preserved by older writers that change content. This is
// a freshness check over metadata and payload CRCs, not an authenticity claim.
[[nodiscard]] QByteArray previewFingerprint(QJsonObject manifest, const Entries&);
[[nodiscard]] QJsonObject encodePreview(const QImage&, const QJsonObject&, const Entries&);
[[nodiscard]] QImage decodePreview(const QJsonObject&, const Entries&, int maximumSide = previewSide);
// Reads only the archive directory and bounded manifest/PNG. No layer decoding,
// renderer, Vulkan, display server, source extraction or network access.
[[nodiscard]] QImage readPreview(const QString& path, int maximumSide = previewSide);
// Non-owning seekable stream entry point for isolated desktop thumbnail hosts.
[[nodiscard]] QImage readPreview(QIODevice& source, int maximumSide = previewSide);
}
