#include "imageeditor/core/Layer.hpp"
#include "imageeditor/core/RichText.hpp"

#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
namespace {
LayerId nextLayerId {1};
std::mutex layerIdMutex;
std::set<LayerId> importedFutureIds;
}

LayerId makeLayerId()
{
    const std::lock_guard lock(layerIdMutex);
    while (nextLayerId <= LayerId(std::numeric_limits<int64_t>::max())) {
        const auto candidate = nextLayerId++;
        if (!importedFutureIds.erase(candidate)) return candidate;
    }
    throw std::length_error("Layer ID space exhausted");
}
void reserveLayerId(LayerId id)
{
    if (!id || id > LayerId(std::numeric_limits<int64_t>::max()))
        throw std::invalid_argument("Layer ID exhausts the supported range");
    const std::lock_guard lock(layerIdMutex);
    // A project with a very high ID must not push the allocator to exhaustion.
    // Previously generated IDs are already behind our monotonic counter.
    if (id >= nextLayerId) importedFutureIds.insert(id);
}

Layer Layer::raster(std::string name, std::shared_ptr<RasterSurface> surface)
{
    if (!surface) {
        throw std::invalid_argument("Raster layer requires a surface");
    }
    return Layer {
        .id = makeLayerId(),
        .name = std::move(name),
        .visible = true,
        .opacity = 1.0F,
        .adjustments = {},
        .filters = {},
        .filterCache = {},
        .effects = {},
        .effectCache = {},
        .localToDocument = {},
        .rasterEffectFrame = {},
        .crop = {},
        .mask = {},
        .payload = RasterLayer {.surface = std::move(surface)},
        .renderCache = {},
    };
}

Layer Layer::adjustment(std::string name, AdjustmentScope scope)
{
    Layer layer;
    layer.id = makeLayerId();
    layer.name = std::move(name);
    layer.payload = AdjustmentLayer{scope};
    return layer;
}

Layer Layer::text(std::string name, TextLayer textData)
{
    return Layer {
        .id = makeLayerId(),
        .name = std::move(name),
        .visible = true,
        .opacity = 1.0F,
        .adjustments = {},
        .filters = {},
        .filterCache = {},
        .effects = {},
        .effectCache = {},
        .localToDocument = {},
        .rasterEffectFrame = {},
        .crop = {},
        .mask = {},
        .payload = normalizedText(std::move(textData)),
        .renderCache = {},
    };
}

Layer Layer::shape(std::string name, ShapeLayer shapeData)
{
    if (!validShape(shapeData))
        throw std::invalid_argument("Shape layer requires valid geometry and style");
    return Layer {
        .id = makeLayerId(),
        .name = std::move(name),
        .visible = true,
        .opacity = 1.0F,
        .adjustments = {},
        .filters = {},
        .filterCache = {},
        .effects = {},
        .effectCache = {},
        .localToDocument = {},
        .rasterEffectFrame = {},
        .crop = {},
        .mask = {},
        .payload = std::move(shapeData),
        .renderCache = {},
    };
}

} // namespace imageeditor::core
