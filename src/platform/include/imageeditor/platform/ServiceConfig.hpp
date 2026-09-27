#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace imageeditor::platform {
// Runtime distribution configuration, not document data or user preferences.
// Keep a snapshot for the lifetime of each service/request. Never log the token.
struct ServiceConfig {
    QUrl origin;
    QByteArray apiKey;
    [[nodiscard]] bool enabled() const { return !origin.isEmpty() && !apiKey.isEmpty(); }
    [[nodiscard]] QUrl url(const QString& path) const;
    static ServiceConfig load();
    static ServiceConfig fromFile(const QString& path);
};
} // namespace imageeditor::platform
