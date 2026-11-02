#include "Profiles.h"
#include <QJsonDocument>
#include <imageeditor/diffusion/ModelProfiles.h>
namespace imageeditor::diffusion {
QJsonArray modelProfiles()
{
    return QJsonDocument::fromJson(profileJson).array();
}
QJsonObject modelProfile(const QString& id)
{
    for (const auto& value : modelProfiles())
        if (value.toObject().value("id").toString() == id)
            return value.toObject();
    return { };
}
}
