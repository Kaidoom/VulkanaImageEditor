#pragma once

#include "imageeditor/core/ProjectLayer.hpp"
#include "imageeditor/project/ProjectArchive.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

namespace imageeditor::ui::detail {

inline QString projectAppearancePrefix(core::Extent2u extent)
{
    return QStringLiteral("project-layers/%1x%2-").arg(extent.width).arg(extent.height);
}

inline QString projectAppearancePath(core::Extent2u extent, const QByteArray& digest)
{
    return projectAppearancePrefix(extent) + QString::fromLatin1(digest.toHex()) + QStringLiteral(".rgba");
}

inline bool validProjectAppearancePath(QStringView path, core::Extent2u extent)
{
    const QString prefix = projectAppearancePrefix(extent);
    if (!path.startsWith(prefix) || !path.endsWith(u".rgba") || path.size() != prefix.size() + 64 + 5)
        return false;
    return std::ranges::all_of(path.sliced(prefix.size(), 64), [](QChar c) {
        return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f');
    });
}

// Shared placements pay for one hash per distinct in-memory surface. Separate
// renders of identical pixels also collapse to the same immutable ZIP member.
inline QString projectAppearancePath(const core::RasterSurface& surface)
{
    const auto extent = surface.extent();
    project::require(!extent.empty() && quint64(extent.width) * extent.height <= project::pixelLimit
            && surface.dataKind() == core::RasterDataKind::Color, "Invalid project layer appearance");
    const size_t stride = size_t(extent.width) * 4;
    const auto rows = std::min<size_t>(extent.height, std::max<size_t>(1, project::chunkSize / stride));
    std::vector<std::byte> pixels(rows * stride);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (uint32_t y = 0; y < extent.height;) {
        const auto h = uint32_t(std::min<size_t>(rows, extent.height - y));
        surface.copyRgba8({0, int(y), int(extent.width), int(h)}, pixels, stride);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(pixels.data()), qsizetype(h * stride)));
        y += h;
    }
    return projectAppearancePath(extent, hash.result());
}

inline bool validProjectLinkPath(const QString& path)
{
    return !path.isEmpty() && path.size() <= 32768 && !path.contains(QChar::Null)
        && path.endsWith(QStringLiteral(".vulkana"), Qt::CaseInsensitive);
}

inline QJsonObject encodeProjectSource(const core::ProjectLayerSource& source, const QFileInfo& parent)
{
    const auto dir = parent.absoluteDir();
    const auto relative = [&](const std::string& utf8) {
        const auto path = QString::fromStdString(utf8);
        project::require(validProjectLinkPath(path) && QDir::isAbsolutePath(path), "Invalid linked project path");
        const auto absolute = QDir::cleanPath(path);
        // QFileInfo equality can consider two missing paths equal. Use it
        // only for existing aliases, alongside a lexical destination check.
        const QFileInfo sourceFile(absolute);
        project::require(absolute != parent.absoluteFilePath()
                && !(parent.exists() && sourceFile.exists() && sourceFile == parent),
            "Saving here would create a project layer cycle");
        return dir.relativeFilePath(absolute);
    };
    project::require(source.dependencies.size() <= core::LayerTree::maxItems, "Too many linked project dependencies");
    QJsonArray dependencies;
    for (const auto& path : source.dependencies) dependencies.append(relative(path));
    return QJsonObject{{"version", 1}, {"path", relative(source.path)}, {"dependencies", dependencies}};
}

inline std::shared_ptr<const core::ProjectLayerSource> decodeProjectSource(const QJsonValue& value,
    const QDir& parentDirectory, quint64& pathBytes)
{
    const auto object = value.toObject();
    project::require(object["version"] == 1 && object["dependencies"].isArray(),
        "Invalid project layer source descriptor");
    const auto absolute = [&](const QJsonValue& entry) {
        const auto path = entry.toString();
        project::require(validProjectLinkPath(path), "Invalid linked project path");
        // Resolve lexically, including when the project tree is offline or has
        // moved. External paths are never opened by native project loading.
        const auto resolved = QDir::cleanPath(parentDirectory.absoluteFilePath(path)).toStdString();
        pathBytes = project::checkedBytes(pathBytes, resolved.size() + sizeof(std::string));
        project::require(pathBytes <= 2 * project::manifestBudget, "Linked project paths exceed metadata budget");
        return resolved;
    };
    auto source = std::make_shared<core::ProjectLayerSource>();
    source->path = absolute(object["path"]);
    const auto dependencies = object["dependencies"].toArray();
    project::require(size_t(dependencies.size()) <= core::LayerTree::maxItems, "Too many linked project dependencies");
    for (const auto& dependency : dependencies) source->dependencies.push_back(absolute(dependency));
    return source;
}
} // namespace imageeditor::ui::detail
