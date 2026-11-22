#include "Profiles.h"
#include <QJsonDocument>
#include <imageeditor/diffusion/ModelProfiles.h>
namespace imageeditor::diffusion {
QJsonArray modelProfiles()
{
    auto profiles = QJsonDocument::fromJson(profileJson).array();
#ifndef Q_OS_LINUX
    for (qsizetype i = profiles.size(); i-- > 0;)
        if (profiles.at(i).toObject().value("platform") == "linux") profiles.removeAt(i);
#endif
    return profiles;
}
QJsonObject modelProfile(const QString& id)
{
    for (const auto& value : modelProfiles())
        if (value.toObject().value("id").toString() == id)
            return value.toObject();
    return { };
}
}
