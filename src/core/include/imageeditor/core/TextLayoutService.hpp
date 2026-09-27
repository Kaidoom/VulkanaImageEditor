#pragma once

#include "imageeditor/core/Geometry.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RasterSurface.hpp"

#include <memory>
#include <string>

namespace imageeditor::core {

struct TextLayoutRequest {
    TextLayer text;
    double rasterDensity {1.0};
};

struct TextLayoutResult {
    std::string resolvedFontFamily;
    std::shared_ptr<const LayerRenderCache> cache;
};

// The V1 Qt adapter implements this interface with Qt's font discovery and
// text layout facilities. No Qt type crosses this boundary, and the returned
// raster is a disposable cache rather than canonical TextLayer state.
class TextLayoutService {
public:
    virtual ~TextLayoutService() = default;
    [[nodiscard]] virtual TextLayoutResult layout(const TextLayoutRequest& request) = 0;
};

} // namespace imageeditor::core
