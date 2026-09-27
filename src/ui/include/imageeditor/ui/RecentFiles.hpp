#pragma once

#include <QSettings>
#include <QString>
#include <QVector>
#include <memory>

namespace imageeditor::ui {

enum class RecentFileKind { Project, Image };

struct RecentFileEntry {
    QString path;
    RecentFileKind kind { RecentFileKind::Image };
    friend bool operator==(const RecentFileEntry&, const RecentFileEntry&) = default;
};

// Application preferences, not document state or history. The caller records
// only successfully opened/saved files; this utility never opens a document.
class RecentFiles final {
public:
    static constexpr qsizetype maximumEntries = 10;

    // Uses the application's existing QSettings identity without changing it.
    RecentFiles();
    // The supplied settings must outlive this object. Useful for scoped tests.
    explicit RecentFiles(QSettings& settings);

    [[nodiscard]] const QVector<RecentFileEntry>& entries() const noexcept { return entries_; }
    void recordSuccess(const QString& path, RecentFileKind kind);
    bool remove(const QString& path);
    void clear();

private:
    static QString normalizedPath(const QString& path);
    void load();
    void persist();

    std::unique_ptr<QSettings> ownedSettings_;
    QSettings* settings_;
    QVector<RecentFileEntry> entries_;
};

} // namespace imageeditor::ui
