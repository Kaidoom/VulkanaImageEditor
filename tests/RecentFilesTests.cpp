#include "imageeditor/ui/RecentFiles.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

QString settingsPath(const QTemporaryDir& temporary)
{
    if (!temporary.isValid())
        throw std::runtime_error("Cannot create temporary recent-files fixture");
    return temporary.filePath(QStringLiteral("preferences.ini"));
}

struct Fixture {
    QTemporaryDir temporary;
    QSettings settings { settingsPath(temporary), QSettings::IniFormat };
    u::RecentFiles recent { settings };

    QString path(const QString& name) const { return temporary.filePath(name); }
    QString file(const QString& name)
    {
        const auto target = path(name);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            throw std::runtime_error("Cannot create fixture parent directory");
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot create fixture file");
        file.close();
        return target;
    }
};

void normalizedPathsAndDistinctNames()
{
    Fixture f;
    const auto one = f.file(QStringLiteral("one/same.png"));
    const auto two = f.file(QStringLiteral("two/same.png"));
    f.recent.recordSuccess(one, u::RecentFileKind::Image);
    f.recent.recordSuccess(two, u::RecentFileKind::Image);
    CHECK(f.recent.entries().size() == 2);
    CHECK(f.recent.entries()[0].path == QFileInfo(two).canonicalFilePath());
    CHECK(f.recent.entries()[1].path == QFileInfo(one).canonicalFilePath());

    const auto relative = QDir::current().relativeFilePath(one);
    f.recent.recordSuccess(relative, u::RecentFileKind::Project);
    CHECK(f.recent.entries().size() == 2);
    CHECK(f.recent.entries()[0].path == QFileInfo(one).canonicalFilePath());
    CHECK(f.recent.entries()[0].kind == u::RecentFileKind::Project);
    f.recent.recordSuccess(f.path(QStringLiteral("one/../one/same.png")), u::RecentFileKind::Image);
    CHECK(f.recent.entries().size() == 2);
    CHECK(f.recent.entries()[0].kind == u::RecentFileKind::Image);

    const auto alias = f.path(QStringLiteral("alias.png"));
    CHECK(QFile::link(one, alias));
    f.recent.recordSuccess(alias, u::RecentFileKind::Image);
    CHECK(f.recent.entries().size() == 2);
    CHECK(f.recent.entries()[0].path == QFileInfo(one).canonicalFilePath());
    CHECK(f.recent.remove(alias));
    CHECK(f.recent.entries().size() == 1);
    CHECK(f.recent.entries()[0].path == QFileInfo(two).canonicalFilePath());
    CHECK(!f.recent.remove(alias));
    CHECK(f.settings.status() == QSettings::NoError);
}

void orderLimitAndRemoval()
{
    Fixture f;
    for (int i = 0; i < 18; ++i)
        f.recent.recordSuccess(f.path(QStringLiteral("image-%1.png").arg(i)),
            i % 2 ? u::RecentFileKind::Image : u::RecentFileKind::Project);
    CHECK(f.recent.entries().size() == u::RecentFiles::maximumEntries);
    for (qsizetype i = 0; i < f.recent.entries().size(); ++i) {
        const int number = 17 - int(i);
        CHECK(f.recent.entries()[i].path == f.path(QStringLiteral("image-%1.png").arg(number)));
        CHECK(f.recent.entries()[i].kind
            == (number % 2 ? u::RecentFileKind::Image : u::RecentFileKind::Project));
    }
    const auto refreshed = f.path(QStringLiteral("image-10.png"));
    f.recent.recordSuccess(refreshed, u::RecentFileKind::Image);
    CHECK(f.recent.entries().size() == u::RecentFiles::maximumEntries);
    CHECK(f.recent.entries()[0].path == refreshed);
    CHECK(f.recent.entries()[1].path == f.path(QStringLiteral("image-17.png")));
    CHECK(f.recent.remove(refreshed));
    CHECK(f.recent.entries().size() == u::RecentFiles::maximumEntries - 1);
    CHECK(f.recent.entries()[0].path == f.path(QStringLiteral("image-17.png")));
    CHECK(!f.recent.remove(f.path(QStringLiteral("never-recorded.png"))));
    CHECK(!f.recent.remove({ }));
    CHECK(f.settings.status() == QSettings::NoError);
}

void persistenceMissingAndUnicode()
{
    Fixture f;
    const auto missing = f.path(QStringLiteral("未存在/Édition Ω/../图像 🙂.png"));
    const auto unicode = f.file(QStringLiteral("café/画面 Ω.ieproj"));
    CHECK(!QFileInfo::exists(missing));
    f.recent.recordSuccess(missing, u::RecentFileKind::Image);
    f.recent.recordSuccess(unicode, u::RecentFileKind::Project);
    const auto expected = f.recent.entries();
    CHECK(expected[1].path == QDir::cleanPath(missing));
    CHECK(QFileInfo(expected[1].path).isAbsolute());
    CHECK(f.settings.value(QStringLiteral("recentFiles/version")).toInt() == 1);
    CHECK(f.settings.value(QStringLiteral("recentFiles/items/size")).toInt() == 2);

    QSettings reopened(settingsPath(f.temporary), QSettings::IniFormat);
    u::RecentFiles loaded(reopened);
    CHECK(loaded.entries() == expected);
    CHECK(!QFileInfo::exists(loaded.entries()[1].path));
    CHECK(loaded.remove(missing));
    QSettings reopenedAgain(settingsPath(f.temporary), QSettings::IniFormat);
    u::RecentFiles removed(reopenedAgain);
    CHECK(removed.entries().size() == 1);
    CHECK(removed.entries()[0].path == QFileInfo(unicode).canonicalFilePath());
}

void loadingPreviousLargerCapacity()
{
    Fixture f;
    f.settings.setValue(QStringLiteral("recentFiles/version"), 1);
    f.settings.beginWriteArray(QStringLiteral("recentFiles/items"), 12);
    for (int i = 0; i < 12; ++i) {
        f.settings.setArrayIndex(i);
        f.settings.setValue(QStringLiteral("path"), f.path(QStringLiteral("previous-%1.png").arg(i)));
        f.settings.setValue(QStringLiteral("kind"), QStringLiteral("image"));
    }
    f.settings.endArray();
    f.settings.sync();

    u::RecentFiles loaded(f.settings);
    CHECK(u::RecentFiles::maximumEntries == 10);
    CHECK(loaded.entries().size() == 10);
    for (qsizetype i = 0; i < loaded.entries().size(); ++i)
        CHECK(loaded.entries()[i].path == f.path(QStringLiteral("previous-%1.png").arg(i)));

    // Refreshing an existing entry preserves the other entries' MRU order.
    const auto refreshed = f.path(QStringLiteral("previous-7.png"));
    loaded.recordSuccess(refreshed, u::RecentFileKind::Project);
    CHECK(loaded.entries().size() == 10);
    CHECK(loaded.entries()[0].path == refreshed);
    CHECK(loaded.entries()[0].kind == u::RecentFileKind::Project);
    CHECK(loaded.entries()[1].path == f.path(QStringLiteral("previous-0.png")));
    CHECK(loaded.entries().last().path == f.path(QStringLiteral("previous-9.png")));
    CHECK(f.settings.value(QStringLiteral("recentFiles/items/size")).toInt() == 10);

    loaded.recordSuccess(f.path(QStringLiteral("new.png")), u::RecentFileKind::Image);
    CHECK(loaded.entries().size() == 10);
    CHECK(loaded.entries()[0].path == f.path(QStringLiteral("new.png")));
    CHECK(loaded.entries()[1].path == refreshed);
    CHECK(loaded.entries().last().path == f.path(QStringLiteral("previous-8.png")));
    QSettings reopened(settingsPath(f.temporary), QSettings::IniFormat);
    u::RecentFiles persisted(reopened);
    CHECK(persisted.entries() == loaded.entries());
}

void clearPreservesUnrelatedPreferences()
{
    Fixture f;
    f.settings.setValue(QStringLiteral("window/geometry"), QByteArray("keep-me"));
    for (int i = 0; i < 12; ++i)
        f.recent.recordSuccess(
            f.path(QStringLiteral("old-%1.png").arg(i)), u::RecentFileKind::Image);
    f.recent.clear();
    CHECK(f.recent.entries().isEmpty());
    CHECK(
        f.settings.value(QStringLiteral("window/geometry")).toByteArray() == QByteArray("keep-me"));
    CHECK(f.settings.value(QStringLiteral("recentFiles/version")).toInt() == 1);
    for (const auto& key : f.settings.allKeys()) {
        CHECK(!key.startsWith(QStringLiteral("recentFiles/items/"))
            || key == QStringLiteral("recentFiles/items/size"));
    }
    QSettings reopened(settingsPath(f.temporary), QSettings::IniFormat);
    u::RecentFiles loaded(reopened);
    CHECK(loaded.entries().isEmpty());
    loaded.clear();
    CHECK(reopened.value(QStringLiteral("window/geometry")).toByteArray() == QByteArray("keep-me"));
    CHECK(reopened.status() == QSettings::NoError);
}

void malformedEntriesAndVersions()
{
    Fixture f;
    f.recent.recordSuccess({ }, u::RecentFileKind::Image);
    f.recent.recordSuccess(
        QStringLiteral("bad") + QChar::Null + QStringLiteral("path"), u::RecentFileKind::Image);
    CHECK(f.recent.entries().isEmpty());
    CHECK(!f.settings.contains(QStringLiteral("recentFiles/version")));

    f.settings.setValue(QStringLiteral("recentFiles/version"), 1);
    f.settings.beginWriteArray(QStringLiteral("recentFiles/items"), 20);
    for (int i = 0; i < 20; ++i) {
        f.settings.setArrayIndex(i);
        f.settings.setValue(QStringLiteral("path"), f.path(QStringLiteral("%1.png").arg(i)));
        f.settings.setValue(QStringLiteral("kind"), QStringLiteral("image"));
    }
    f.settings.setArrayIndex(1);
    f.settings.setValue(QStringLiteral("path"), f.path(QStringLiteral("0.png"))); // Duplicate.
    f.settings.setArrayIndex(2);
    f.settings.setValue(QStringLiteral("kind"), QStringLiteral("not-a-kind"));
    f.settings.setArrayIndex(3);
    f.settings.setValue(QStringLiteral("path"), QString { });
    f.settings.endArray();
    f.settings.sync();
    u::RecentFiles sanitized(f.settings);
    CHECK(sanitized.entries().size() == 7); // Read at most the first 10 stored slots.
    CHECK(sanitized.entries()[0].path == f.path(QStringLiteral("0.png")));
    CHECK(sanitized.entries()[1].path == f.path(QStringLiteral("4.png")));
    CHECK(sanitized.entries().last().path == f.path(QStringLiteral("9.png")));

    f.settings.setValue(QStringLiteral("recentFiles/version"), 999);
    u::RecentFiles unsupported(f.settings);
    CHECK(unsupported.entries().isEmpty());
    CHECK(f.settings.value(QStringLiteral("recentFiles/version")).toInt()
        == 999); // Loading is read-only.
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        normalizedPathsAndDistinctNames();
        orderLimitAndRemoval();
        persistenceMissingAndUnicode();
        loadingPreviousLargerCapacity();
        clearPreservesUnrelatedPreferences();
        malformedEntriesAndVersions();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::cout << (failures ? "Recent files checks failed: " : "Recent files checks passed: ")
              << failures << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
