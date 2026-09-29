#include "imageeditor/ui/RecentFiles.hpp"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <optional>

namespace imageeditor::ui {
namespace {
constexpr int schemaVersion = 1;
const QString versionKey = QStringLiteral("recentFiles/version");
const QString itemsKey = QStringLiteral("recentFiles/items");

QString kindName(RecentFileKind kind)
{
    switch (kind) {
    case RecentFileKind::Project:
        return QStringLiteral("project");
    case RecentFileKind::Image:
        return QStringLiteral("image");
    case RecentFileKind::Pdf:
        return QStringLiteral("pdf");
    }
    return { };
}

std::optional<RecentFileKind> parseKind(const QString& value)
{
    if (value == QStringLiteral("project"))
        return RecentFileKind::Project;
    if (value == QStringLiteral("image"))
        return RecentFileKind::Image;
    if (value == QStringLiteral("pdf"))
        return RecentFileKind::Pdf;
    return std::nullopt;
}
}

RecentFiles::RecentFiles()
    : ownedSettings_(std::make_unique<QSettings>())
    , settings_(ownedSettings_.get())
{
    load();
}

RecentFiles::RecentFiles(QSettings& settings)
    : settings_(&settings)
{
    load();
}

QString RecentFiles::normalizedPath(const QString& path)
{
    if (path.isEmpty() || path.contains(QChar::Null))
        return { };
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    // Missing/moved files stay available for the UI to explain or remove.
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

void RecentFiles::load()
{
    if (settings_->value(versionKey).toInt() != schemaVersion)
        return;
    const int count = settings_->beginReadArray(itemsKey);
    // Bound loading too, even if preferences contain an invalid array size.
    const int limit = std::clamp(count, 0, int(maximumEntries));
    for (int i = 0; i < limit; ++i) {
        settings_->setArrayIndex(i);
        const auto kind = parseKind(settings_->value(QStringLiteral("kind")).toString());
        const auto rawPath = settings_->value(QStringLiteral("path"));
        if (!kind || rawPath.metaType().id() != QMetaType::QString)
            continue;
        const auto path = normalizedPath(rawPath.toString());
        if (path.isEmpty() || std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
                return entry.path == path;
            }))
            continue;
        entries_.push_back({ path, *kind });
    }
    settings_->endArray();
}

void RecentFiles::recordSuccess(const QString& path, RecentFileKind kind)
{
    if (kindName(kind).isEmpty())
        return;
    const auto normalized = normalizedPath(path);
    if (normalized.isEmpty())
        return;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                       [&](const auto& entry) { return entry.path == normalized; }),
        entries_.end());
    entries_.prepend({ normalized, kind });
    if (entries_.size() > maximumEntries)
        entries_.resize(maximumEntries);
    persist();
}

bool RecentFiles::remove(const QString& path)
{
    const auto normalized = normalizedPath(path);
    if (normalized.isEmpty())
        return false;
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                       [&](const auto& entry) { return entry.path == normalized; }),
        entries_.end());
    if (before == entries_.size())
        return false;
    persist();
    return true;
}

void RecentFiles::clear()
{
    entries_.clear();
    persist();
}

void RecentFiles::persist()
{
    // Remove only our array, including stale trailing slots. Never clear other
    // app preferences; Clear Recent Files must also erase old stored paths.
    settings_->remove(itemsKey);
    settings_->setValue(versionKey, schemaVersion);
    settings_->beginWriteArray(itemsKey, int(entries_.size()));
    for (qsizetype i = 0; i < entries_.size(); ++i) {
        settings_->setArrayIndex(int(i));
        settings_->setValue(QStringLiteral("path"), entries_[i].path);
        settings_->setValue(QStringLiteral("kind"), kindName(entries_[i].kind));
    }
    settings_->endArray();
    settings_->sync();
}

} // namespace imageeditor::ui
