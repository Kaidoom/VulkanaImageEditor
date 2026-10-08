#include "imageeditor/platform/ApplicationPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace paths = imageeditor::platform;
namespace {
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw std::runtime_error("Cannot create temporary fixture directory");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write temporary fixture");
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read temporary fixture");
    return file.readAll();
}

void userLocations(const QString& root)
{
    // Branding/organization changes must not move established writable data.
    QCoreApplication::setOrganizationName(QStringLiteral("Unrelated test organization"));
    QCoreApplication::setApplicationName(QStringLiteral("Unrelated test application"));
    CHECK(paths::userDataDirectory() == root + QStringLiteral("/xdg-data/vulkanaEditor"));
    CHECK(paths::userCacheDirectory() == root + QStringLiteral("/xdg-cache/vulkanaEditor"));
    CHECK(paths::userPresetDirectory()
        == root + QStringLiteral("/xdg-data/vulkanaEditor/brush-presets-v2"));
    QCoreApplication::setOrganizationName(QStringLiteral("vulkanaEditor"));
    QCoreApplication::setApplicationName(QStringLiteral("vulkanaEditor"));
    CHECK(paths::userDataDirectory() == root + QStringLiteral("/xdg-data/vulkanaEditor"));
    CHECK(QSettings().fileName() == root + QStringLiteral("/xdg-config/vulkanaEditor/vulkanaEditor.conf"));
}

void copyOnlyMigration(const QString& root)
{
    const auto legacy = root + QStringLiteral("/migration/legacy");
    const auto destination = root + QStringLiteral("/migration/current");
    writeFile(legacy + QStringLiteral("/one.iebrush"), "legacy one\n");
    writeFile(legacy + QStringLiteral("/two.iebrush"), "legacy two\n");
    writeFile(legacy + QStringLiteral("/unrelated.txt"), "do not import\n");
    writeFile(destination + QStringLiteral("/two.iebrush"), "newer user copy\n");
    CHECK(paths::migrateLegacyUserPresets(legacy, destination).isEmpty());
    CHECK(readFile(destination + QStringLiteral("/one.iebrush")) == "legacy one\n");
    CHECK(readFile(destination + QStringLiteral("/two.iebrush")) == "newer user copy\n");
    CHECK(readFile(legacy + QStringLiteral("/one.iebrush")) == "legacy one\n");
    CHECK(readFile(legacy + QStringLiteral("/two.iebrush")) == "legacy two\n");
    CHECK(!QFileInfo::exists(destination + QStringLiteral("/unrelated.txt")));
    CHECK(QFileInfo::exists(destination + QStringLiteral("/.legacy-migration-v1")));

    // Re-running startup never restores a preset deliberately deleted later.
    CHECK(QFile::remove(destination + QStringLiteral("/one.iebrush")));
    writeFile(legacy + QStringLiteral("/three.iebrush"), "late legacy copy\n");
    CHECK(paths::migrateLegacyUserPresets(legacy, destination).isEmpty());
    CHECK(!QFileInfo::exists(destination + QStringLiteral("/one.iebrush")));
    CHECK(!QFileInfo::exists(destination + QStringLiteral("/three.iebrush")));
    CHECK(readFile(destination + QStringLiteral("/two.iebrush")) == "newer user copy\n");
    CHECK(paths::migrateLegacyUserPresets(root + QStringLiteral("/missing"),
        root + QStringLiteral("/unused")).isEmpty());
    CHECK(!QFileInfo::exists(root + QStringLiteral("/unused")));
}

void failedPartialMigrationCanRetry(const QString& root)
{
    const auto legacy = root + QStringLiteral("/partial/legacy");
    const auto destination = root + QStringLiteral("/partial/current");
    writeFile(legacy + QStringLiteral("/a-valid.iebrush"), "valid original\n");
    const QByteArray oversized(1024 * 1024 + 1, 'x');
    writeFile(legacy + QStringLiteral("/z-oversized.iebrush"), oversized);
    CHECK(!paths::migrateLegacyUserPresets(legacy, destination).isEmpty());
    CHECK(readFile(destination + QStringLiteral("/a-valid.iebrush")) == "valid original\n");
    CHECK(readFile(legacy + QStringLiteral("/a-valid.iebrush")) == "valid original\n");
    CHECK(readFile(legacy + QStringLiteral("/z-oversized.iebrush")) == oversized);
    CHECK(!QFileInfo::exists(destination + QStringLiteral("/z-oversized.iebrush")));
    CHECK(!QFileInfo::exists(destination + QStringLiteral("/.legacy-migration-v1")));

    writeFile(destination + QStringLiteral("/a-valid.iebrush"), "edited after partial migration\n");
    writeFile(legacy + QStringLiteral("/z-oversized.iebrush"), "corrected preset\n");
    CHECK(paths::migrateLegacyUserPresets(legacy, destination).isEmpty());
    CHECK(readFile(destination + QStringLiteral("/a-valid.iebrush"))
        == "edited after partial migration\n");
    CHECK(readFile(destination + QStringLiteral("/z-oversized.iebrush")) == "corrected preset\n");
    CHECK(QFileInfo::exists(destination + QStringLiteral("/.legacy-migration-v1")));

    const auto unavailable = root + QStringLiteral("/not-a-directory");
    writeFile(unavailable, "existing unrelated file\n");
    CHECK(!paths::migrateLegacyUserPresets(legacy, unavailable).isEmpty());
    CHECK(readFile(unavailable) == "existing unrelated file\n");
    CHECK(readFile(legacy + QStringLiteral("/a-valid.iebrush")) == "valid original\n");
}

void defaultLegacyLocation(const QString& root)
{
    const auto legacy = root + QStringLiteral("/xdg-data/ImageEditor/ImageEditor/brush-presets-v2");
    writeFile(legacy + QStringLiteral("/custom.iebrush"), "isolated old user preset\n");
    CHECK(paths::migrateLegacyUserPresets().isEmpty());
    CHECK(readFile(paths::userPresetDirectory() + QStringLiteral("/custom.iebrush"))
        == "isolated old user preset\n");
    CHECK(readFile(legacy + QStringLiteral("/custom.iebrush")) == "isolated old user preset\n");
}

void developmentPaths()
{
#ifdef VULKANA_TEST_EXPECT_DEVELOPMENT
    CHECK(paths::shaderPath(QStringLiteral("layer.frag.spv"))
        == QStringLiteral(VULKANA_TEST_DEVELOPMENT_SHADER_DIR "/layer.frag.spv"));
    CHECK(QFileInfo::exists(paths::shaderPath(QStringLiteral("layer.frag.spv"))));
    CHECK(paths::defaultUiConfigPath() == QStringLiteral(VULKANA_TEST_DEVELOPMENT_UI_CONFIG));
    CHECK(QFileInfo::exists(paths::defaultUiConfigPath()));
    CHECK(QFileInfo::exists(QDir(paths::objectSelectionBundlePath()).filePath("encoder.onnx")));
#endif
}

void relocatedChild(const QString& root)
{
    const auto data = root + QStringLiteral("/share/vulkana-editor");
    CHECK(QCoreApplication::applicationDirPath() == root + QStringLiteral("/bin"));
    CHECK(paths::installedDataPath(QStringLiteral("probe.txt")) == data + QStringLiteral("/probe.txt"));
    CHECK(readFile(paths::installedDataPath(QStringLiteral("probe.txt"))) == "installed resource\n");
    CHECK(paths::shaderPath(QStringLiteral("packaging-path-probe.spv"))
        == data + QStringLiteral("/shaders/packaging-path-probe.spv"));
#ifdef VULKANA_TEST_RELOCATABLE
    CHECK(paths::installedDataPath(QStringLiteral("does-not-exist"))
        == data + QStringLiteral("/does-not-exist"));
#else
    CHECK(paths::installedDataPath(QStringLiteral("does-not-exist"))
        == QDir(QStringLiteral(VULKANA_TEST_INSTALLED_DATA_DIR)).filePath(QStringLiteral("does-not-exist")));
#endif
#ifdef VULKANA_TEST_EXPECT_DEVELOPMENT
    developmentPaths();
#else
    // The package-mode target has no compiled source/build fallback paths.
    CHECK(paths::defaultUiConfigPath() == data + QStringLiteral("/ui-layout.ini"));
    CHECK(paths::shaderPath(QStringLiteral("layer.frag.spv"))
        == data + QStringLiteral("/shaders/layer.frag.spv"));
    CHECK(paths::objectSelectionBundlePath()==data+QStringLiteral("/object-selection/mobilesam-v1"));
#endif
    CHECK(QDir::setCurrent(root));
    CHECK(paths::installedDataPath(QStringLiteral("probe.txt")) == data + QStringLiteral("/probe.txt"));
}

void relocateAndRun(const QString& root)
{
    const auto prefix = root + QStringLiteral("/relocated-prefix");
    const auto executable = prefix + QStringLiteral("/bin/path-probe");
    CHECK(QDir().mkpath(QFileInfo(executable).absolutePath()));
    CHECK(QFile::copy(QCoreApplication::applicationFilePath(), executable));
    CHECK(QFile::setPermissions(executable, QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner));
    const auto data = prefix + QStringLiteral("/share/vulkana-editor");
    writeFile(data + QStringLiteral("/probe.txt"), "installed resource\n");
    writeFile(data + QStringLiteral("/shaders/packaging-path-probe.spv"), "fixture shader\n");
    writeFile(data + QStringLiteral("/shaders/layer.frag.spv"), "fixture layer shader\n");
    writeFile(data + QStringLiteral("/ui-layout.ini"), "fixture UI configuration\n");
    writeFile(data + QStringLiteral("/object-selection/mobilesam-v1/vulkana-mobilesam-v1.json"), "fixture bundle\n");
    const auto unrelated = root + QStringLiteral("/unrelated-working-directory");
    writeFile(unrelated + QStringLiteral("/probe.txt"), "incorrect cwd resource\n");
    writeFile(unrelated + QStringLiteral("/shaders/packaging-path-probe.spv"), "incorrect cwd shader\n");
    QProcess child;
    child.setWorkingDirectory(unrelated);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
    environment.remove(QStringLiteral("QML_IMPORT_PATH"));
    child.setProcessEnvironment(environment);
    child.start(executable, {QStringLiteral("--relocated-fixture"), prefix});
    CHECK(child.waitForStarted(5000));
    CHECK(child.waitForFinished(10000));
    CHECK(child.exitStatus() == QProcess::NormalExit);
    CHECK(child.exitCode() == EXIT_SUCCESS);
    if (child.exitCode() != EXIT_SUCCESS)
        std::cerr << child.readAllStandardError().toStdString();
}
}

int main(int argc, char** argv)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return EXIT_FAILURE;
    const auto root = temporary.path();
    qputenv("XDG_DATA_HOME", (root + QStringLiteral("/xdg-data")).toUtf8());
    qputenv("XDG_CACHE_HOME", (root + QStringLiteral("/xdg-cache")).toUtf8());
    qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/xdg-config")).toUtf8());
    qputenv("HOME", (root + QStringLiteral("/home")).toUtf8());
    QCoreApplication application(argc, argv);
    try {
        if (application.arguments().size() == 3
            && application.arguments()[1] == QStringLiteral("--relocated-fixture")) {
            relocatedChild(application.arguments()[2]);
        } else {
            userLocations(root);
            copyOnlyMigration(root);
            failedPartialMigrationCanRetry(root);
            defaultLegacyLocation(root);
            developmentPaths();
            relocateAndRun(root);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
