#include "imageeditor/platform/PlatformStartup.hpp"
#include <QCoreApplication>
#include <QVariant>

namespace imageeditor::platform {
namespace {
constexpr auto restartProperty = "vulkanaExplicitPlatformArguments";
}
QString PlatformStartup::selectionSource() const
{
    if (!explicitArguments.isEmpty()) return QStringLiteral("explicit command line (-platform)");
    if (!environmentOverride.isEmpty()) return QStringLiteral("explicit environment (QT_QPA_PLATFORM)");
    return QStringLiteral("automatic (Qt session selection)");
}
PlatformStartup inspectPlatformStartup(const QStringList& arguments, const QProcessEnvironment& environment)
{
    PlatformStartup result;
    result.sessionType = environment.value(QStringLiteral("XDG_SESSION_TYPE"));
    result.environmentOverride = environment.value(QStringLiteral("QT_QPA_PLATFORM"));
    result.displayPresent = environment.contains(QStringLiteral("DISPLAY"));
    result.waylandDisplayPresent = environment.contains(QStringLiteral("WAYLAND_DISPLAY"));
    result.waylandSocketPresent = environment.contains(QStringLiteral("WAYLAND_SOCKET"));
    // Match Qt 6's documented -platform/--platform <value>, including last
    // occurrence winning. Skip values of Qt options so a title is not a flag.
    const QStringList valueOptions {QStringLiteral("platformpluginpath"), QStringLiteral("platformtheme"),
        QStringLiteral("qwindowgeometry"), QStringLiteral("qwindowtitle"), QStringLiteral("qwindowicon"),
        QStringLiteral("geometry"), QStringLiteral("title"), QStringLiteral("icon")};
    for (int i = 1; i < arguments.size(); ++i) {
        auto option = arguments[i];
        if (!option.startsWith(QLatin1Char('-'))) continue;
        option.remove(0, option.startsWith(QStringLiteral("--")) ? 2 : 1);
        if (option == QStringLiteral("platform") && i + 1 < arguments.size())
            result.explicitArguments = {QStringLiteral("-platform"), arguments[++i]};
        else if (valueOptions.contains(option)) ++i;
    }
    return result;
}
void retainPlatformArguments(const PlatformStartup& startup)
{
    if (auto* app = QCoreApplication::instance()) app->setProperty(restartProperty, startup.explicitArguments);
}
QStringList platformRestartArguments()
{
    const auto* app = QCoreApplication::instance();
    return app ? app->property(restartProperty).toStringList() : QStringList{};
}
} // namespace imageeditor::platform
