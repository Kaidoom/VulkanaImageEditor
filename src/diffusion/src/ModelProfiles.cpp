#include "Profiles.h"
#include <QJsonDocument>
#include <imageeditor/diffusion/ModelProfiles.h>
namespace imageeditor::diffusion {
QJsonArray modelProfiles()
{
    auto profiles = QJsonDocument::fromJson(profileJson).array();
#ifdef Q_OS_WIN
    const QString platform = QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    const QString platform = QStringLiteral("linux");
#else
    const QString platform;
#endif
    for (qsizetype i = profiles.size(); i-- > 0;) {
        const auto supported = profiles.at(i).toObject().value("platforms").toArray();
        if (!supported.isEmpty() && !supported.contains(platform)) profiles.removeAt(i);
    }
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
