#pragma once
#include "AdjustmentCodec.hpp"
#include "imageeditor/core/SpatialFilters.hpp"

namespace imageeditor::ui::detail {
struct SpatialFilterPlan {
    std::shared_ptr<core::SpatialFilterStack> stack;
    std::vector<AdjustmentMaskPlan> masks;
};
[[nodiscard]] QString spatialFilterMaskPath(core::LayerId,core::SpatialFilterType);
[[nodiscard]] QJsonObject encodeSpatialFilters(const core::SpatialFilterStack&,core::LayerId);
[[nodiscard]] SpatialFilterPlan decodeSpatialFilters(const QJsonObject&,core::LayerId);
}
