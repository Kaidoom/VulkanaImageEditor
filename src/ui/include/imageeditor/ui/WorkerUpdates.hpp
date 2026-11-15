#pragma once
#include "imageeditor/ui/UpdateService.hpp"
#include <QJsonObject>
#include <QNetworkReply>

namespace imageeditor::ui {
struct WorkerRelease {
    QString id;
    UpdateRelease artifact;
    qint64 size{};
    bool pinned{};
};

// One session cache shared by startup, About and the AI panel. Vulkan follows
// the application release; only CUDA is offered as an independent download.
class WorkerUpdates final : public QObject {
    Q_OBJECT
public:
    explicit WorkerUpdates(QString appVersion, QObject* parent = nullptr,
        QNetworkAccessManager* transport = nullptr);
    ~WorkerUpdates() override;
    void check(int timeoutMs = 60000);
    void download(const QString& destination);
    void cancelDownload();
    void stop();
    bool configured() const;
    bool checking() const { return reply_; }
    bool checked() const { return checked_; }
    bool downloading() const;
    const std::optional<WorkerRelease>& release() const { return release_; }
    QString error() const { return error_; }
    qint64 received() const { return received_; }
    qint64 total() const { return total_; }
    QString downloadedPath() const;
    static QString hostPlatform();
    static std::optional<WorkerRelease> parseFeed(const QByteArray&, const QString& appVersion,
        const QString& platformName, const QString& architecture, const platform::ServiceConfig&, QString& error);

signals:
    void changed();
    void downloadFinished(const QString& path);
    void downloadFailed(const QString& message);

private:
    void stopCheck();
    void failCheck(const QString& error);
    void readAvailable();
    void finishCheck();
    const platform::ServiceConfig config_ {platform::ServiceConfig::load()};
    QString appVersion_;
    QNetworkAccessManager* network_{};
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    QByteArray bytes_;
    std::optional<WorkerRelease> release_;
    QString error_;
    bool checked_{};
    UpdateService* transfer_{};
    qint64 received_{}, total_{};
};
} // namespace imageeditor::ui
