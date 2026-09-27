#pragma once

#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/core/Layer.hpp"
#include <QJsonObject>
#include <QString>
#include <vector>

namespace imageeditor::ui::detail {
struct AdjustmentMaskPlan {
    std::size_t index;
    QString path;
    core::Extent2u extent;
    core::AffineTransform localToMask;
};
struct AdjustmentPlan {
    std::shared_ptr<core::AdjustmentStack> stack;
    std::vector<AdjustmentMaskPlan> masks;
};
// Descriptor parsing never allocates coverage planes. The project reader
// validates every ZIP entry and the aggregate budget before loading masks.
[[nodiscard]] QString adjustmentMaskPath(core::LayerId, core::AdjustmentType);
[[nodiscard]] QJsonObject encodeAdjustments(const core::AdjustmentStack&, core::LayerId);
[[nodiscard]] AdjustmentPlan decodeAdjustments(const QJsonObject&, core::LayerId);
} // namespace imageeditor::ui::detail
