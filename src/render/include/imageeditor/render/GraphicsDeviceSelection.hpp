#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
namespace imageeditor::render {
enum class GraphicsDeviceAvailability { Available, Unsupported, StartupCheckFailed };
struct GraphicsDeviceFallback {
    std::string requestedId, requestedName, selectedId, selectedName;
};
// Detected devices and their current eligibility. Startup also verifies device
// creation and presentation because hybrid drivers can reject them later.
struct GraphicsDeviceInfo {
    std::string id, name;
    int automaticScore{};
    GraphicsDeviceAvailability availability{GraphicsDeviceAvailability::Available};
};
inline std::optional<std::size_t> selectGraphicsDevice(std::span<const GraphicsDeviceInfo> devices,std::string_view preferred)
{
    std::optional<std::size_t> best;
    for(std::size_t i=0;i<devices.size();++i) {
        if(devices[i].availability!=GraphicsDeviceAvailability::Available)continue;
        if(!preferred.empty()&&!devices[i].id.empty()&&devices[i].id==preferred)return i;
        if(!best||devices[i].automaticScore>devices[*best].automaticScore)best=i;
    }
    return best;
}
}
