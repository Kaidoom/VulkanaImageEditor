#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/UiLayoutConfig.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/platform/BuildInfo.hpp"
#include "imageeditor/platform/PlatformStartup.hpp"
#include "imageeditor/platform/SingleInstance.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QVulkanInstance>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>

int main(int argc, char* argv[])
{
    QStringList startupArguments;
    for (int i = 0; i < argc; ++i) startupArguments.append(QString::fromLocal8Bit(argv[i]));
    const auto startup = imageeditor::platform::inspectPlatformStartup(startupArguments);
    // This survives a Qt connection failure during QApplication construction.
    qInfo().noquote() << "Qt startup: session=" << (startup.sessionType.isEmpty() ? QStringLiteral("<unset>") : startup.sessionType)
        << "selection=" << startup.selectionSource()
        << "requested=" << (startup.explicitArguments.isEmpty() ? startup.environmentOverride : startup.explicitArguments.back())
        << "DISPLAY present=" << startup.displayPresent << "WAYLAND_DISPLAY present=" << startup.waylandDisplayPresent
        << "WAYLAND_SOCKET present=" << startup.waylandSocketPresent;
    // The embedded Vulkan canvas is the only native child in the Widgets
    // shell. Without this opt-out, making its QWindowContainer native causes
    // QWidget to promote sibling docks and toolbars to native Wayland
    // subsurfaces as well. A live dock shrink then moves the dock subsurface
    // before its old-width backing buffer is replaced; Wayland subsurfaces are
    // not parent-clipped, so that stale buffer can flash on an adjacent output.
    // This attribute must be selected before QApplication/native creation.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    // Stable per-user identity, independent of the display/project name.
    // The packaging gate explicitly starts fresh preferences; legacy files
    // stay untouched and custom brush copies are migrated non-destructively.
    // Platform integration (including KDE's KConfig) may initialize inside the
    // QApplication constructor. Give it the same identity as our QSettings,
    // rather than briefly exposing the executable's historical name.
    QCoreApplication::setOrganizationName(QStringLiteral("vulkanaEditor"));
    QCoreApplication::setApplicationName(QStringLiteral("vulkanaEditor"));
    QGuiApplication::setDesktopFileName(QStringLiteral("vulkana-editor"));
#ifdef VULKANA_BUNDLED_QT
    // Only our matching Qt portal plugin, not an arbitrary host KDE/GTK plugin.
    // Select it for QApplication initialization, then restore the environment
    // before any browser/file-manager child is launched.
    const auto previousPlatformTheme = qgetenv("QT_QPA_PLATFORMTHEME");
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
#endif
    QApplication application(argc, argv);
    imageeditor::platform::retainPlatformArguments(startup);
    qInfo().noquote() << "Qt platform:" << QGuiApplication::platformName() << "selection=" << startup.selectionSource();
#ifdef VULKANA_BUNDLED_QT
    if (previousPlatformTheme.isNull()) qunsetenv("QT_QPA_PLATFORMTHEME");
    else qputenv("QT_QPA_PLATFORMTHEME", previousPlatformTheme);
#endif
    application.setApplicationVersion(QString::fromLatin1(imageeditor::platform::buildVersion));
    application.setApplicationDisplayName(QStringLiteral("Vulkana Image Editor"));
    application.setWindowIcon(QIcon(QStringLiteral(":/Vulkana512.png")));
    imageeditor::ui::applyEditorTheme(application);
    {
        QSettings startupPreferences;
        imageeditor::ui::applyEditorTheme(application, imageeditor::ui::loadThemeSettings(startupPreferences));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Vulkana Image Editor — Vulkan raster editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption smokeTestOption(QStringLiteral("smoke-test"),
        QStringLiteral("Exercise the desktop/Vulkan lifecycle and exit automatically."));
    QCommandLineOption fpsOption(QStringLiteral("fps"),
        QStringLiteral("Show the actual Vulkan canvas frame rate in the status bar."));
    QCommandLineOption skipNewOption(QStringLiteral("skip-new-dialog"),
        QStringLiteral("Development: start with the default transparent canvas without prompting."));
    QCommandLineOption forceUpdateOption(QStringLiteral("force-update-check"),
        QStringLiteral("Check for updates at startup (now the default)."));
    QCommandLineOption testUpdateOption(QStringLiteral("test-update"),
        QStringLiteral("Show a simulated newer update without networking, downloads or installation."));
    QCommandLineOption uiConfigOption(QStringLiteral("ui-config"),
        QStringLiteral("Load developer UI layout values from an INI file."),
        QStringLiteral("path"));
    QCommandLineOption panelDefaultOption(QStringLiteral("panel-default"),
        QStringLiteral("Use the default panel layout for this launch without replacing the saved layout."));
    QCommandLineOption restartOption(QString::fromLatin1(imageeditor::platform::singleInstanceRestartOption),
        QStringLiteral("Internal: wait for the previous instance to finish an update restart."));
    restartOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(restartOption);
    parser.addOption(smokeTestOption);
    parser.addOption(fpsOption);
    parser.addOption(skipNewOption);
    parser.addOption(forceUpdateOption);
    parser.addOption(testUpdateOption);
    parser.addOption(uiConfigOption);
    parser.addOption(panelDefaultOption);
    parser.addPositionalArgument(QStringLiteral("document"),
        QStringLiteral("Optional projects or images; each opens in its own tab."), QStringLiteral("[document…]"));
    parser.process(application);
    // Share one user-scoped owner across installed RPMs, AppImage mounts and
    // versions. Explicit integration smoke probes are deliberately isolated.
    imageeditor::platform::SingleInstance instance;
    if (!parser.isSet(smokeTestOption)) {
        const auto result = instance.start(parser.positionalArguments(), parser.isSet(restartOption),
            parser.isSet(restartOption) ? 15000 : 5000);
        if (result == imageeditor::platform::SingleInstance::Result::Forwarded) {
            qInfo() << "Launch forwarded to the running Vulkana instance.";
            if (parser.isSet(panelDefaultOption))
                qInfo() << "--panel-default takes effect at startup. Close Vulkana, then launch with this flag again.";
            if (parser.isSet(testUpdateOption))
                qInfo() << "--test-update takes effect at startup. Close Vulkana, then launch with this flag again.";
            return EXIT_SUCCESS;
        }
        if (result == imageeditor::platform::SingleInstance::Result::Error) {
            qCritical().noquote() << instance.errorString();
            QMessageBox::warning(nullptr, QStringLiteral("Vulkana"), instance.errorString());
            return EXIT_FAILURE;
        }
    }
    for (const auto& diagnostic : imageeditor::platform::migrateLegacyUserPresets())
        qWarning().noquote() << diagnostic;

    const auto platform = QGuiApplication::platformName();
    for (const auto* screen : QGuiApplication::screens()) {
        qInfo() << "Screen:" << screen->name() << screen->geometry()
                << screen->refreshRate() << "Hz DPR" << screen->devicePixelRatio();
    }
    if (platform != QStringLiteral("wayland") && platform != QStringLiteral("xcb")) {
        qWarning() << "Non-desktop Qt platform (explicit headless/test use):" << platform;
    }

    // Developer layout data is a startup snapshot. No settings object, file
    // path, watcher, or reload callback is retained by MainWindow.
    imageeditor::ui::UiLayoutConfig uiLayoutConfig;
    QString uiConfigPath;
    const bool explicitlyConfigured = parser.isSet(uiConfigOption);
    if (explicitlyConfigured) {
        uiConfigPath = parser.value(uiConfigOption);
    } else if (!parser.isSet(smokeTestOption)) {
        const auto environmentPath = qEnvironmentVariable(
            "IMAGEEDITOR_UI_CONFIG");
        if (!environmentPath.isEmpty()) {
            uiConfigPath = environmentPath;
        } else {
            uiConfigPath = imageeditor::platform::defaultUiConfigPath();
        }
    }
    if (!uiConfigPath.isEmpty() && QFileInfo::exists(uiConfigPath)) {
        QStringList diagnostics;
        uiLayoutConfig = imageeditor::ui::UiLayoutConfig::loadFromIni(
            uiConfigPath, &diagnostics);
        qInfo().noquote() << "UI layout config:" << uiConfigPath;
        for (const auto& diagnostic : diagnostics) {
            qWarning().noquote() << "UI layout config:" << diagnostic;
        }
    } else if (explicitlyConfigured) {
        qWarning().noquote() << "UI layout config does not exist:"
                            << uiConfigPath << "- using built-in defaults";
    }

    std::atomic_uint64_t validationWarnings {0};
    std::atomic_uint64_t validationErrors {0};
    QVulkanInstance vulkanInstance;
    vulkanInstance.setApiVersion(QVersionNumber(1, 2, 0));
    if (vulkanInstance.supportedExtensions().contains("VK_EXT_swapchain_colorspace")) {
        vulkanInstance.setExtensions({"VK_EXT_swapchain_colorspace"});
    }
    vulkanInstance.installDebugOutputFilter(
        [&validationWarnings, &validationErrors](
            QVulkanInstance::DebugMessageSeverityFlags severity,
            QVulkanInstance::DebugMessageTypeFlags type,
            const void* message) {
            if (!type.testFlag(QVulkanInstance::ValidationMessage)) {
                return false;
            }

            const auto* callback = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
            if (severity.testFlag(QVulkanInstance::ErrorSeverity)) {
                validationErrors.fetch_add(1, std::memory_order_relaxed);
                qCritical().noquote() << "Vulkan validation error:"
                                      << (callback && callback->pMessage ? callback->pMessage : "unknown");
            } else if (severity.testFlag(QVulkanInstance::WarningSeverity)) {
                validationWarnings.fetch_add(1, std::memory_order_relaxed);
                qWarning().noquote() << "Vulkan validation warning:"
                                     << (callback && callback->pMessage ? callback->pMessage : "unknown");
            }
            return true;
        });
    bool validationRequested = parser.isSet(smokeTestOption);
#if defined(IMAGEEDITOR_ENABLE_VALIDATION)
    validationRequested = true;
#endif
    bool validationEnabled = false;
    if (validationRequested
        && vulkanInstance.supportedLayers().contains("VK_LAYER_KHRONOS_validation")) {
        vulkanInstance.setLayers({"VK_LAYER_KHRONOS_validation"});
        validationEnabled = true;
        qInfo() << "Vulkan validation layer enabled";
    } else if (validationRequested) {
        qWarning() << "Vulkan validation layer is unavailable";
    } else {
        qInfo() << "Vulkan validation: disabled (not requested)";
    }
    if (parser.isSet(smokeTestOption) && !validationEnabled) {
        qCritical() << "The Vulkan smoke test requires VK_LAYER_KHRONOS_validation";
        return EXIT_FAILURE;
    }
    if (!vulkanInstance.create()) {
        QMessageBox::critical(nullptr, QStringLiteral("Vulkan unavailable"),
            QStringLiteral("ImageEditor could not create a Vulkan instance (error %1).")
                .arg(vulkanInstance.errorCode()));
        return EXIT_FAILURE;
    }

    int result = EXIT_SUCCESS;
    {
        imageeditor::ui::MainWindow window(
            &vulkanInstance, !parser.isSet(smokeTestOption),
            parser.isSet(fpsOption), uiLayoutConfig, nullptr,
            parser.isSet(panelDefaultOption)
                ? imageeditor::ui::MainWindow::PanelLayoutMode::SessionDefaults
                : imageeditor::ui::MainWindow::PanelLayoutMode::Saved);
        // Use the embedded artwork even when an older desktop installation is
        // present. Wayland shells may still prefer that installation's icon
        // via the stable desktop-file ID; the next package updates it too.
        window.setWindowIcon(application.windowIcon());
        // On Wayland the compositor owns top-level sizing. Choose the smoke
        // test's initial size before the first map instead of issuing a
        // client-side resize after mapping; a later compositor configure can
        // otherwise legitimately reapply the initial size during the probe.
        if (parser.isSet(smokeTestOption)) {
            window.setUnsavedPromptEnabled(false);
            window.resize(1260, 780);
        }
        window.show();
        const bool opened = !parser.positionalArguments().isEmpty()
            && window.openImageFromPath(parser.positionalArguments().front());
        if (opened) qInfo().noquote() << "Opened document argument:"
            << QFileInfo(parser.positionalArguments().front()).absoluteFilePath();
        if (!parser.isSet(smokeTestOption))
            QTimer::singleShot(0, &window, [&window, opened, &parser, &skipNewOption, &testUpdateOption] {
                window.startStartupFlow(!opened && !parser.isSet(skipNewOption), nullptr, parser.isSet(testUpdateOption));
            });
        // Every document argument opens in the shared workspace, regardless of
        // which package or executable received the desktop launch.
        for (const auto& path : parser.positionalArguments().mid(1)) {
            if (window.openImageFromPath(path))
                qInfo().noquote() << "Opened document argument:" << QFileInfo(path).absoluteFilePath();
        }
        if (parser.isSet(smokeTestOption)) {
            window.runIntegrationSmokeTest();
        }
        instance.setOpenHandler([&window](const QStringList& files) { window.receiveExternalLaunch(files); });
        result = application.exec();
        instance.setOpenHandler({});
        window.logRendererDiagnostics();
        if (parser.isSet(smokeTestOption) && !window.integrationSmokePassed()) {
            result = EXIT_FAILURE;
        }
    }
    qInfo() << "Vulkan validation active:" << validationEnabled;
    qInfo() << "Vulkan validation summary: warnings" << validationWarnings.load()
            << "errors" << validationErrors.load();
    if (parser.isSet(smokeTestOption)
        && (validationWarnings.load() != 0 || validationErrors.load() != 0)) {
        return EXIT_FAILURE;
    }
    return result;
}
