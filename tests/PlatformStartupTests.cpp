#include "imageeditor/platform/PlatformStartup.hpp"
#include <QCoreApplication>
#include <iostream>

using namespace imageeditor::platform;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << message << '\n'; }
    };
    QProcessEnvironment env;
    env.insert("XDG_SESSION_TYPE", "x11"); env.insert("DISPLAY", ":1");
    auto startup = inspectPlatformStartup({"vulkana"}, env);
    check(startup.sessionType == "x11" && startup.displayPresent
        && !startup.waylandDisplayPresent && startup.explicitArguments.isEmpty(), "X11 diagnostics");
    retainPlatformArguments(startup);
    check(platformRestartArguments().isEmpty(), "automatic selection is not a restart override");
    env.insert("XDG_SESSION_TYPE", "wayland"); env.insert("WAYLAND_DISPLAY", "wayland-test");
    startup = inspectPlatformStartup({"vulkana"}, env);
    check(startup.displayPresent && startup.waylandDisplayPresent
        && startup.selectionSource().startsWith("automatic"), "XWayland DISPLAY does not imply an override");
    env.insert("QT_QPA_PLATFORM", "xcb");
    startup = inspectPlatformStartup({"vulkana"}, env);
    check(startup.selectionSource().contains("environment"), "explicit environment diagnostic");
    retainPlatformArguments(startup);
    check(platformRestartArguments().isEmpty(), "environment override remains inherited, not rewritten as CLI");
    startup = inspectPlatformStartup({"vulkana", "-platform", "xcb", "--platform", "wayland", "file 空間.vulkana"}, env);
    check(startup.explicitArguments == QStringList{"-platform", "wayland"}
        && startup.selectionSource().contains("command line"), "last CLI platform overrides the environment");
    retainPlatformArguments(startup);
    check(platformRestartArguments() == QStringList{"-platform", "wayland"}, "explicit CLI survives QApplication argument consumption for restart");
    startup = inspectPlatformStartup({"vulkana", "-qwindowtitle", "-platform", "image.png"}, env);
    check(startup.explicitArguments.isEmpty(), "Qt option value is not parsed as a platform flag");
    retainPlatformArguments(startup);
    check(platformRestartArguments().isEmpty(), "no stale automatic restart override");
    check(env.value("QT_QPA_PLATFORM") == "xcb", "diagnostics never mutate the environment");
    std::cout << "Platform startup diagnostics/restart: " << failures << " failures\n";
    return failures ? 1 : 0;
}
