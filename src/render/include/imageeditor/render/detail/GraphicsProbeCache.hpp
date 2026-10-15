#pragma once
#include <QByteArray>
#include <QString>
class QSettings;

namespace imageeditor::render::detail {
// Versioned, bounded success cache. Failures and timeouts are never persisted.
QString graphicsPresentationCacheKey(const QString& device, const QByteArray& driver,
    const QByteArray& display, bool extendedColorSpace);
class GraphicsProbeCache final {
public:
    explicit GraphicsProbeCache(QSettings& settings):settings_(settings){}
    bool contains(const QString& key) const;
    void remember(const QString& key);
    void invalidate(const QString& key);
    void recoverInterruptedStartup();
    void beginStartup(const QString& key);
    void completeStartup(const QString& key);
private:
    QSettings& settings_;
};
}
