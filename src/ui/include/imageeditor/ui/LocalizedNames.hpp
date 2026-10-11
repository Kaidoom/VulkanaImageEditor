#pragma once
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/SpatialFilters.hpp"
#include "imageeditor/core/BlendMode.hpp"
namespace imageeditor::ui::i18n {
inline Message named(const char* category, std::string_view identifier) {
    const auto id = QByteArray(category) + '.' + QByteArray(identifier.data(), qsizetype(identifier.size()));
    return text(id.constData());
}
inline Message effectName(core::LayerEffectType type) { return named("effect", core::layerEffectIdentifier(type)); }
inline Message adjustmentName(core::AdjustmentType type) { return named("adjustment", core::adjustmentIdentifier(type)); }
inline Message filterName(core::SpatialFilterType type) { return named("filter", core::spatialFilterIdentifier(type)); }
inline Message blendName(core::BlendMode type) { return named("blend", core::blendModeId(type)); }
}
