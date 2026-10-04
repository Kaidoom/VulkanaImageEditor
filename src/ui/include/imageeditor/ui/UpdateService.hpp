#pragma once

#include "imageeditor/platform/ServiceConfig.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QLockFile;

namespace imageeditor::ui {

struct UpdateRelease {
    QString version;
    QByteArray sha256;
    QUrl url;
};
enum class UpdatePackage { Rpm, AppImage, Deb };

// Checks are asynchronous. Downloads are user-initiated: RPMs/DEBs are saved, never
// installed. AppImages are replaced atomically only after verification;
// restart is a separate unsaved-edit guard.
// An injected transport lets focused tests exercise failures without networking.
class UpdateService final : public QObject {
public:
    enum class State { Idle, Checking, Current, Available, Downloading, Downloaded, RestartReady, Error };
    static UpdatePackage buildPackage();
    explicit UpdateService(QString currentVersion, QObject* parent = nullptr,
        QNetworkAccessManager* transport = nullptr, UpdatePackage package = buildPackage(),
        QString appImagePath = {});
    ~UpdateService() override;

    void check(int timeoutMs = 60000);
    void download(const QString& destination);
    void downloadAppImage();
    void cancel();
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool configured() const { return config_.enabled(); }
    [[nodiscard]] bool busy() const { return state_ == State::Checking || state_ == State::Downloading; }
    [[nodiscard]] const QString& message() const { return message_; }
    [[nodiscard]] const std::optional<UpdateRelease>& release() const { return release_; }
    [[nodiscard]] const QString& downloadedPath() const { return downloadedPath_; }
    [[nodiscard]] UpdatePackage package() const { return package_; }
    std::function<void()> onChanged;
    std::function<void(qint64 received, qint64 total)> onProgress;

    static QUrl manifestUrl(const platform::ServiceConfig& = platform::ServiceConfig::load());
    static QUrl downloadPageUrl(const platform::ServiceConfig& = platform::ServiceConfig::load());
    static std::optional<UpdateRelease> parseManifest(const QByteArray&, QString& error,
        UpdatePackage package = UpdatePackage::Rpm,
        const platform::ServiceConfig& = platform::ServiceConfig::load());
    static bool isAppImageHeader(const QByteArray&);
    // Only published three-component numeric versions are compared. Malformed
    // values never silently turn into version zero or a lexical comparison.
    static std::optional<bool> isNewer(const QString& candidate, const QString& installed);
    static bool startupCheckDue(qint64 nowUtcSeconds, std::optional<qint64> lastAttempt,
        bool force = false);

private:
    void startRequest(const QUrl&);
    void readAvailable();
    void finish();
    void stopTransfer();
    void fail(const QString&);
    void notify();
    void beginDownload(const QString&);

    const platform::ServiceConfig config_ {platform::ServiceConfig::load()};
    QString currentVersion_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    int checkTimeoutMs_ {60000};
    State state_ {State::Idle};
    QString message_;
    std::optional<UpdateRelease> release_;
    QByteArray manifest_;
    std::unique_ptr<QSaveFile> file_;
    QCryptographicHash hash_ {QCryptographicHash::Sha256};
    qint64 received_ {};
    QString destination_;
    QString downloadedPath_;
    UpdatePackage package_;
    QString appImagePath_;
    QByteArray header_;
    std::unique_ptr<QLockFile> updateLock_;
    quint64 originalDevice_ {}, originalInode_ {};
    qint64 originalSize_ {}, originalMtime_ {}, originalMtimeNsec_ {};
};

} // namespace imageeditor::ui
