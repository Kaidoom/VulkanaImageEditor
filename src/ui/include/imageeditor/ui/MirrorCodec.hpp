#pragma once
#include "imageeditor/core/Layer.hpp"
#include <QJsonObject>
namespace imageeditor::ui {
QJsonObject encodeLayerMirror(const core::LayerMirror&);
core::LayerMirror decodeLayerMirror(const QJsonValue&);
}
