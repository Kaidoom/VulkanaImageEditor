#include "imageeditor/platform/ProjectIntegration.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
        const auto error = imageeditor::platform::registerProjectFiles(preferences, QDir(bin).filePath("imageeditor.exe"));
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
    CHECK(RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) == ERROR_SUCCESS);
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
        CHECK(p::registerProjectFiles(settings, executable).isEmpty()); registry.sync();
        CHECK(registry.value(".vulkana/Default").toString() == "Other.App");
        CHECK(registry.value(command).toString() == '"' + QDir::toNativeSeparators(executable) + "\" \"%1\"");
        CHECK(settings.value(p::projectIntegrationChoiceKey) == "registered");
        CHECK(registry.value(thumb).toString() == "{4EE78B14-4465-40F5-BC5E-C4F096A51B7F}");
        CHECK(p::registerProjectFiles(settings, executable).isEmpty()); // Repeated setup keeps the original backup.
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(registry.value(".vulkana/Default") == "Other.App");
        CHECK(registry.value(thumb) == "Previous.Thumbnail");
        CHECK(!registry.contains(command));
        CHECK(settings.value(p::projectIntegrationChoiceKey) == "declined");
        registry.remove(".vulkana/Default"); registry.sync();
        CHECK(p::registerProjectFiles(settings, executable).isEmpty()); registry.sync();
        CHECK(registry.value(".vulkana/Default") == "Vulkana.Project");
        registry.setValue(thumb, "Later.Thumbnail"); registry.sync();
        CHECK(p::unregisterProjectFiles(settings).isEmpty()); registry.sync();
        CHECK(registry.value(thumb) == "Later.Thumbnail");
        CHECK(!registry.contains(".vulkana/Default"));
        CHECK(p::unregisterProjectFiles(settings).isEmpty());
    }
    CHECK(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS);
    RegCloseKey(sandbox);
    CHECK(RegDeleteTreeW(HKEY_CURRENT_USER, name.toStdWString().c_str()) == ERROR_SUCCESS);
    return failures ? 1 : 0;
}
