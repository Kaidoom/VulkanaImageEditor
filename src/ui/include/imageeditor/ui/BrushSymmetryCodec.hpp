#pragma once
#include "imageeditor/core/BrushSymmetry.hpp"
#include <QJsonObject>
#include <QJsonValue>

namespace imageeditor::ui {
QJsonObject encodeBrushSymmetry(const core::BrushSymmetry&);
core::BrushSymmetry decodeBrushSymmetry(const QJsonValue&);
}
