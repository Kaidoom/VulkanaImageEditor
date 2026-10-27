#pragma once
#include <QJsonObject>
#include <QStringList>
namespace imageeditor::mcp {
struct BridgeCommand {
    QStringList command;
    QString error;
    explicit operator bool() const { return error.isEmpty() && !command.isEmpty(); }
};
BridgeCommand discoverBridge(const QString& applicationPath, const QString& appImage = {}, const QString& appDir = {});
BridgeCommand installedBridge();
QJsonObject openCodeConfiguration(const BridgeCommand&, const QString& endpoint);
// Headless stdio entry point shared by the bridge binary and --mcp-bridge.
int runBridge(int argc, char** argv);
}
