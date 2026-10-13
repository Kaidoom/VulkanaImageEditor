#include "imageeditor/platform/ProjectIntegration.hpp"
#include "imageeditor/platform/BuildInfo.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QProcess>
#include <windows.h>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && (app.arguments()[1] == "--shell-cache" || app.arguments()[1] == "--shell-bind")) {
        // Explicit integration probe only (not the registry unit test): a
        // bounded real Shell-cache request, with original associations restored
        // and no changes to the application's saved first-run preference.
        QTemporaryDir temporary;
        QSettings preferences(temporary.filePath("registration.ini"), QSettings::IniFormat);
        const auto bin = QCoreApplication::applicationDirPath();
        const auto error = imageeditor::platform::registerProjectFiles(preferences, QDir(bin).filePath("imageeditor.exe"),
            QString::fromLatin1(imageeditor::platform::buildVersion));
        if (!error.isEmpty()) { std::cerr << error.toStdString(); return 1; }
        QProcess probe;
        probe.start(QDir(bin).filePath("imageeditor_windows_thumbnail_tests.exe"), {
            QDir(bin).filePath("vulkana-thumbnail-provider.dll"), app.arguments()[2], app.arguments()[1]});
        const bool completed = probe.waitForStarted() && probe.waitForFinished(20000);
        if (!completed) { probe.kill(); probe.waitForFinished(); }
        std::cout << probe.readAllStandardOutput().constData();
        std::cerr << probe.readAllStandardError().constData();
        const auto removed = imageeditor::platform::unregisterProjectFiles(preferences);
        if (!removed.isEmpty()) std::cerr << removed.toStdString();
        return completed && probe.exitCode() == 0 && removed.isEmpty() ? 0 : 1;
    }
    int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; } } while (false)
    // Redirect the process's entire HKCU view to a disposable subtree. Never
    // touch the developer's actual Classes or protected UserChoice settings.
    const auto name = QStringLiteral("Software\\VulkanaRegistrationTest-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    HKEY sandbox {};
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, name.toStdWString().c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr) == ERROR_SUCCESS);
    if (!sandbox) return 1;
    const auto redirected = RegOverridePredefKey(HKEY_CURRENT_USER, sandbox);
    CHECK(redirected == ERROR_SUCCESS);
    if (redirected != ERROR_SUCCESS) { RegCloseKey(sandbox); return 1; }
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const auto bin = dir.filePath(QString::fromUtf8("portable café"));
        CHECK(QDir().mkpath(bin));
        for (const auto& name : {"imageeditor.exe", "vulkana-thumbnail-provider.dll", "vulkana-thumbnail-codec.dll"}) {
            QFile file(QDir(bin).filePath(name)); CHECK(file.open(QIODevice::WriteOnly)); file.write("test fixture");
        }
        QSettings settings(dir.filePath("preferences.ini"), QSettings::IniFormat);
        QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
        namespace p = imageeditor::platform;
        const auto executable = QDir(bin).filePath("imageeditor.exe");
        CHECK(p::projectIntegrationAvailable(executable));
        CHECK(!p::projectIntegrationAvailable(dir.filePath("absent.exe")));
        const auto thumb = QStringLiteral(".vulkana/shellex/{E357FCCD-A995-4576-B01F-234630154E96}/Default");
        const auto command = QStringLiteral("Vulkana.Project/shell/open/command/Default");
        registry.setValue(".vulkana/Default", "Other.App");
        registry.setValue(thumb, "Previous.Thumbnail"); registry.sync();
        CHECK(p::registerProjectFiles(settings, executable, "0.3.2").isEmpty()); registry.sync();
        CHECK(registry.value(".vulkana/Default").toString() == "Other.App");
        CHECK(registry.value(command).toString() == '"' + QDir::toNativeSeparators(executable) + "\" \"%1\"");
        CHECK(settings.value(p::projectIntegrationChoiceKey) == "registered");
        CHECK(registry.value(thumb).toString() == "{4EE78B14-4465-40F5-BC5E-C4F096A51B7F}");
        CHECK(p::registerProjectFiles(settings, executable, "0.3.2").isEmpty()); // Repeated setup keeps the original backup.
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(registry.value(".vulkana/Default") == "Other.App");
        CHECK(registry.value(thumb) == "Previous.Thumbnail");
        CHECK(!registry.contains(command));
        CHECK(settings.value(p::projectIntegrationChoiceKey) == "declined");
        registry.remove(".vulkana/Default"); registry.sync();
        CHECK(p::registerProjectFiles(settings, executable, "0.3.2").isEmpty()); registry.sync();
        CHECK(registry.value(".vulkana/Default") == "Vulkana.Project");
        registry.setValue(thumb, "Later.Thumbnail"); registry.sync();
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(registry.value(thumb) == "Later.Thumbnail");
        CHECK(!registry.contains(".vulkana/Default"));
        CHECK(p::unregisterProjectFiles(settings).isEmpty());

        const auto newerBin = dir.filePath(QString::fromUtf8("new portable 日本語"));
        CHECK(QDir().mkpath(newerBin));
        for (const auto& name : {"imageeditor.exe", "vulkana-thumbnail-provider.dll", "vulkana-thumbnail-codec.dll"}) {
            QFile file(QDir(newerBin).filePath(name)); CHECK(file.open(QIODevice::WriteOnly)); file.write("new fixture");
        }
        const auto newerExe = QDir(newerBin).filePath("imageeditor.exe");
        const auto icon = QStringLiteral("Vulkana.Project/DefaultIcon/Default");
        const auto dll = QStringLiteral("CLSID/{4EE78B14-4465-40F5-BC5E-C4F096A51B7F}/InprocServer32/Default");
        const auto versionKey = QStringLiteral("integration/windows/projectFilesRegistrationVersion");
        const auto journalKey = QStringLiteral("integration/windows/projectFilesRegistration");
        auto contents = [](QSettings& source) {
            source.sync();
            QVariantMap values;
            for (const auto& key : source.allKeys()) values.insert(key, source.value(key));
            return values;
        };
        auto unchanged = [&](const QString& path, const QString& version) {
            const auto beforeRegistry = contents(registry), beforeSettings = contents(settings);
            CHECK(p::refreshProjectFiles(settings, path, version).isEmpty());
            CHECK(contents(registry) == beforeRegistry);
            CHECK(contents(settings) == beforeSettings);
        };
        auto checkPaths = [&](const QString& path) {
            registry.sync();
            const auto native = QDir::toNativeSeparators(path);
            CHECK(registry.value(command) == '"' + native + "\" \"%1\"");
            CHECK(registry.value(icon) == '"' + native + "\",0");
            CHECK(registry.value(dll) == QDir::toNativeSeparators(QFileInfo(path).absoluteDir().filePath("vulkana-thumbnail-provider.dll")));
        };
        auto reset = [&] {
            // Clear disposable values, not the open Classes root itself.
            for (const auto& key : registry.allKeys()) registry.remove(key);
            registry.sync(); CHECK(registry.allKeys().isEmpty());
            settings.clear(); settings.sync();
        };
        reset();
        unchanged(newerExe, "0.3.3"); // Never opted in.
        settings.setValue(p::projectIntegrationChoiceKey, "declined");
        unchanged(newerExe, "0.3.3");
        settings.setValue(p::projectIntegrationChoiceKey, "registered");
        unchanged(newerExe, "0.3.3"); // No ownership journal.
        CHECK(!p::registerProjectFiles(settings, executable, "not-a-version").isEmpty());
        reset();
        registry.setValue(icon, "Original icon"); registry.setValue(command, "Original command");
        registry.setValue(dll, "Original DLL"); registry.setValue(thumb, "Original thumbnail"); registry.sync();
        // Protected default-app data lives outside Classes, including our
        // sandbox. It must survive both an upgrade and removal byte-for-byte.
        QSettings userChoice(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.vulkana\\UserChoice"), QSettings::NativeFormat);
        userChoice.setValue("ProgId", "Chosen.App"); userChoice.setValue("Hash", "sentinel");
        const auto protectedChoice = contents(userChoice);
        CHECK(p::registerProjectFiles(settings, executable, "0.3.9").isEmpty());
        CHECK(settings.value(versionKey) == "0.3.9");
        unchanged(newerExe, "0.3.8");
        unchanged(newerExe, "0.3.9"); // Equal version does not chase different copies.
        unchanged(newerExe, "0.3.10-beta");
        unchanged(newerExe, "0.3.10junk");
        unchanged(newerExe, "");
        unchanged(dir.filePath("missing.exe"), "0.3.10");
        CHECK(p::refreshProjectFiles(settings, newerExe, "0.3.10").isEmpty()); // Numeric, not lexical order.
        checkPaths(newerExe);
        CHECK(settings.value(versionKey) == "0.3.10");
        {
            QSettings reopened(settings.fileName(), QSettings::IniFormat);
            CHECK(reopened.value(versionKey) == "0.3.10");
            CHECK(p::refreshProjectFiles(reopened, executable, "0.3.9").isEmpty());
            checkPaths(newerExe);
        }
        unchanged(executable, "0.3.9");
        unchanged(executable, "0.3.10");
        unchanged(newerExe, "0.3.10"); // Repeated launches are read-only.
        CHECK(p::refreshProjectFiles(settings, newerExe, "0.4.0").isEmpty()); // In-place upgrade also advances version.
        CHECK(settings.value(versionKey) == "0.4.0");
        unchanged(executable, "0.3.11");
        CHECK(contents(userChoice) == protectedChoice);
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(registry.value(icon) == "Original icon"); CHECK(registry.value(command) == "Original command");
        CHECK(registry.value(dll) == "Original DLL"); CHECK(registry.value(thumb) == "Original thumbnail");
        CHECK(!settings.contains(versionKey)); CHECK(!settings.contains(journalKey));
        CHECK(contents(userChoice) == protectedChoice);
        unchanged(newerExe, "1.0.0"); // Removal stays removed even on upgrade.

        reset();
        CHECK(p::registerProjectFiles(settings, executable, "0.3.2").isEmpty());
        settings.remove(versionKey); // Upgrade an existing 0.3.2 unversioned registration.
        unchanged(newerExe, "0.3.2");
        CHECK(p::refreshProjectFiles(settings, newerExe, "0.3.3").isEmpty());
        checkPaths(newerExe); CHECK(settings.value(versionKey) == "0.3.3");
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(!registry.contains(icon)); CHECK(!registry.contains(command)); CHECK(!registry.contains(dll));

        reset();
        CHECK(p::registerProjectFiles(settings, executable, "0.3.2").isEmpty());
        settings.setValue(versionKey, "broken"); unchanged(newerExe, "0.3.3");
        settings.setValue(versionKey, "0.3.2");
        settings.setValue(p::projectIntegrationChoiceKey, "declined"); unchanged(newerExe, "0.3.3");
        settings.setValue(p::projectIntegrationChoiceKey, "registered");
        registry.setValue(command, "Later command"); registry.remove(icon);
        registry.setValue(thumb, "Later thumbnail"); registry.setValue(".vulkana/Default", "Later.App"); registry.sync();
        CHECK(p::refreshProjectFiles(settings, newerExe, "0.3.3").isEmpty()); registry.sync();
        CHECK(registry.value(command) == "Later command"); CHECK(!registry.contains(icon));
        CHECK(registry.value(thumb) == "Later thumbnail"); CHECK(registry.value(".vulkana/Default") == "Later.App");
        CHECK(registry.value(dll) == QDir::toNativeSeparators(QDir(newerBin).filePath("vulkana-thumbnail-provider.dll")));
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(registry.value(command) == "Later command"); CHECK(!registry.contains(icon));
        CHECK(registry.value(thumb) == "Later thumbnail"); CHECK(registry.value(".vulkana/Default") == "Later.App");
        CHECK(!registry.contains(dll));

        reset();
        CHECK(p::registerProjectFiles(settings, executable, "0.3.2").isEmpty());
        CHECK(QFile::remove(QDir(newerBin).filePath("vulkana-thumbnail-codec.dll")));
        unchanged(newerExe, "0.3.3"); // Incomplete portable must not take ownership.
        QFile codec(QDir(newerBin).filePath("vulkana-thumbnail-codec.dll"));
        CHECK(codec.open(QIODevice::WriteOnly)); codec.write("fixture"); codec.close();
        CHECK(QFile::remove(executable)); // Upgrade works after the old folder is removed.
        CHECK(p::refreshProjectFiles(settings, newerExe, "0.3.3").isEmpty()); checkPaths(newerExe);
        CHECK(p::unregisterProjectFiles(settings).isEmpty());
    }
    CHECK(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS);
    RegCloseKey(sandbox);
    CHECK(RegDeleteTreeW(HKEY_CURRENT_USER, name.toStdWString().c_str()) == ERROR_SUCCESS);
    std::cout << "Windows project registration checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
