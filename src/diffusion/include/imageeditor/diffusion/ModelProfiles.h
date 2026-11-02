#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
namespace imageeditor::diffusion {
QJsonArray modelProfiles();
QJsonObject modelProfile(const QString& id);
}
