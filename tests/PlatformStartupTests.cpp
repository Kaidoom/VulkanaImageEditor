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
    check(!startup.coldShaderCache,"shader cache behavior unchanged by default");
    startup=inspectPlatformStartup({"vulkana","--cold-cache","-platform","wayland"},env);
    check(startup.coldShaderCache&&startup.explicitArguments==QStringList{"-platform","wayland"},"cold cache detected before Qt consumes arguments");
    retainPlatformArguments(startup);
    check(platformRestartArguments()==QStringList{"-platform","wayland"},"cold-cache flag is not saved as a restart preference");
    check(!inspectPlatformStartup({"vulkana","--","--cold-cache"},env).coldShaderCache,"positional filename is not a cache flag");
    for(const auto* option:{"-qwindowtitle","--language","--language-pack-dir","--ui-config","-platform","--internal-gpu-probe"})
        check(!inspectPlatformStartup({"vulkana",option,"--cold-cache"},env).coldShaderCache,"option value is not a cache flag");
    check(!env.contains("__GL_SHADER_DISK_CACHE")&&!env.contains("MESA_SHADER_CACHE_DISABLE"),"inspection remains read-only");
    std::cout << "Platform startup diagnostics/restart: " << failures << " failures\n";
    return failures ? 1 : 0;
}
