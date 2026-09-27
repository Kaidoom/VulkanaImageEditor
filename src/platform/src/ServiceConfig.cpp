#include "imageeditor/platform/ServiceConfig.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace imageeditor::platform {
QUrl ServiceConfig::url(const QString& path) const
{
    return origin.isEmpty() ? QUrl{} : origin.resolved(QUrl(path));
}

ServiceConfig ServiceConfig::fromFile(const QString& path)
{
    QFile file(path);
    constexpr qint64 limit = 64 * 1024;
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError) return {};
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    if (!document.isObject() || object.value("version").toDouble(-1) != 1) return {};
    QUrl origin(object.value("domain").toString(), QUrl::StrictMode);
    // An HTTPS origin only. Keep update validation and feedback on this origin;
    // never accept URL credentials, query strings or a path disguised as a host.
    if (!origin.isValid() || origin.scheme() != "https" || origin.host().isEmpty()
        || origin.port(443) != 443 || !origin.userInfo().isEmpty()
        || origin.hasQuery() || origin.hasFragment()
        || (!origin.path().isEmpty() && origin.path() != "/")) return {};
    origin.setPath("/");
    const auto token = object.value("apiKey").toString().toUtf8();
    const bool validToken = !token.isEmpty() && token.size() <= 4096
        && std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
    return {origin, validToken ? token : QByteArray{}};
}

ServiceConfig ServiceConfig::load()
{
    // An explicit override is authoritative even when absent/invalid: never
    // fall through to an official endpoint after a developer disables it.
    if (qEnvironmentVariableIsSet("VULKANA_SERVICE_CONFIG")) {
        const auto path = qEnvironmentVariable("VULKANA_SERVICE_CONFIG");
        return QFileInfo(path).isAbsolute() ? fromFile(path) : ServiceConfig{};
    }
#ifdef VULKANA_DEVELOPMENT_SERVICE_CONFIG
    // A source build without private configuration stays offline, even if an
    // official RPM happens to be installed on the development machine.
    return fromFile(QStringLiteral(VULKANA_DEVELOPMENT_SERVICE_CONFIG));
#else
    return fromFile(installedDataPath(QStringLiteral("services.json")));
#endif
}
} // namespace imageeditor::platform
