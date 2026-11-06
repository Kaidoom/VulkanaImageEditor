#pragma once
#include <algorithm>
#include <string>
#include <string_view>

namespace imageeditor::diffusion {
// Shared by the editor and native worker so both suggest the same valid sizes.
inline std::string resolutionError(int width, int height, int multiple, std::string_view model)
{
    if (width % multiple == 0 && height % multiple == 0)
        return {};
    const auto size = [](int w, int h) {
        return std::to_string(w) + " × " + std::to_string(h);
    };
    const auto lower = [multiple](int value) {
        return std::clamp(value / multiple * multiple, 32, 8192);
    };
    const auto upper = [multiple](int value) {
        return std::clamp((value + multiple - 1) / multiple * multiple, 32, 8192);
    };
    const auto below = size(lower(width), lower(height));
    const auto above = size(upper(width), upper(height));
    return "Invalid resolution: " + size(width, height) + ". " + std::string(model)
        + " requires dimensions divisible by " + std::to_string(multiple) + ". Try "
        + below + (above == below ? "" : " or " + above) + ".";
}
}
