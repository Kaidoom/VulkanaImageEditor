#include "imageeditor/mcp/Integration.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
namespace imageeditor::mcp {
BridgeCommand discoverBridge(const QString& applicationPath, const QString& appImage, const QString& appDir)
{
    const QFileInfo application(applicationPath);
    const auto executable = application.absoluteFilePath();
    const auto root = QFileInfo(appDir).canonicalFilePath();
    if (!appImage.isEmpty() && !appDir.isEmpty() && !root.isEmpty()
        && application.canonicalFilePath().startsWith(root + '/')) {
        const QFileInfo image(appImage);
        if (!image.isAbsolute() || !image.isFile() || !image.isExecutable())
            return {{}, "The running AppImage file is unavailable. Restart from its current location before copying a configuration."};
        return {{image.absoluteFilePath(), "--mcp-bridge"}, {}};
    }
#ifdef Q_OS_WIN
    const auto bridge = application.dir().absoluteFilePath("vulkana-mcp.exe");
#else
    const auto bridge = application.dir().absoluteFilePath("vulkana-mcp");
#endif
    if (QFileInfo(bridge).isFile() && QFileInfo(bridge).isExecutable()) return {{bridge}, {}};
#ifndef Q_OS_WIN
    // Works for installed, extracted and development binaries without assuming
    // any source/build-directory layout.
    if (application.isFile() && application.isExecutable()) return {{executable, "--mcp-bridge"}, {}};
#endif
    return {{}, "The installed vulkana-mcp bridge executable could not be found next to Vulkana."};
}
BridgeCommand installedBridge()
{ return discoverBridge(QCoreApplication::applicationFilePath(), qEnvironmentVariable("APPIMAGE"), qEnvironmentVariable("APPDIR")); }
QJsonObject openCodeConfiguration(const BridgeCommand& bridge, const QString& endpoint)
{
    if (!bridge) return {};
    auto command = QJsonArray::fromStringList(bridge.command);
    command.append("--socket"); command.append(endpoint);
    return {{"mcp", QJsonObject{{"servers", QJsonObject{{"vulkana", QJsonObject{{"type", "local"}, {"command", command}}}}}}}};
}
}
