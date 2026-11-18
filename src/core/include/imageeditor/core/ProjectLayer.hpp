#pragma once
#include "imageeditor/core/RasterSurface.hpp"
#include <memory>
#include <string>
#include <vector>

namespace imageeditor::core {
struct ProjectLayerSource {
    std::string path; // Resolved local path. Project files may store a portable relative path.
    std::vector<std::string> dependencies;
    std::uint64_t instanceId{}, contentState{}; // Runtime source tab and last rendered content.
};

[[nodiscard]] std::shared_ptr<RasterSurface> readOnlyProjectSurface(std::shared_ptr<const RasterSurface>);
[[nodiscard]] std::shared_ptr<RasterSurface> copyColorRaster(const RasterSurface&);
} // namespace imageeditor::core
