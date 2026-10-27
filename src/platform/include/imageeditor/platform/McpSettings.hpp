#pragma once
#include <QString>
class QSettings;
namespace imageeditor::platform {
struct McpSettings {
    bool autoStart {false};
    QString customEndpoint; // Empty means the secure per-user automatic endpoint.
    bool operator==(const McpSettings&) const = default;
};
McpSettings loadMcpSettings(QSettings&);
bool saveMcpSettings(QSettings&, const McpSettings&);
// Command-line opt-ins affect this launch, never the saved preference.
bool shouldStartMcp(const McpSettings&, bool explicitStart, bool isolatedTest = false);
}
