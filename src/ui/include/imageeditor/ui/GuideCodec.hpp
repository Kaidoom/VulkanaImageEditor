#pragma once
#include "imageeditor/core/DocumentGuides.hpp"
#include <QJsonObject>
#include <QJsonValue>
namespace imageeditor::ui {
QJsonObject encodeDocumentGuides(const core::DocumentGuides&);
core::DocumentGuides decodeDocumentGuides(const QJsonValue&);
}
