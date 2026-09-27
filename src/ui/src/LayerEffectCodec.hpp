#pragma once
#include "imageeditor/core/LayerEffects.hpp"
#include <QJsonObject>
namespace imageeditor::ui::detail {
QJsonObject encodeLayerEffects(const core::LayerEffectStack &);
core::LayerEffectState decodeLayerEffects(const QJsonObject &);
} // namespace imageeditor::ui::detail
