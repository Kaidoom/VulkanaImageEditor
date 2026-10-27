#include "imageeditor/platform/McpSettings.hpp"
#include <QSettings>
namespace imageeditor::platform {
McpSettings loadMcpSettings(QSettings& settings)
{
    return {settings.value("preferences/mcp/autoStart", false).toBool(),
        settings.value("preferences/mcp/customEndpoint").toString()};
}
bool saveMcpSettings(QSettings& settings, const McpSettings& value)
{
    settings.setValue("preferences/mcp/autoStart", value.autoStart);
    settings.setValue("preferences/mcp/customEndpoint", value.customEndpoint);
    settings.sync();
    return settings.status() == QSettings::NoError;
}
bool shouldStartMcp(const McpSettings& settings, bool explicitStart, bool isolatedTest)
{ return explicitStart || (!isolatedTest && settings.autoStart); }
}
