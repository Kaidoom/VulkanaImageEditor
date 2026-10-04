#include "imageeditor/ui/UpdateService.hpp"

#include <QFileInfo>
#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVersionNumber>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

namespace imageeditor::ui {
namespace {
constexpr qint64 manifestLimit = 64 * 1024;
constexpr qint64 packageLimit = 1024 * 1024 * 1024;
constexpr qint64 chunkSize = 64 * 1024;
bool validVersion(const QString& version)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\A(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\z"));
    return pattern.match(version).hasMatch();
}
}

QUrl UpdateService::manifestUrl(const platform::ServiceConfig& config) { return config.url(QStringLiteral("/downloads/latest.json")); }
QUrl UpdateService::downloadPageUrl(const platform::ServiceConfig& config) { return config.url(QStringLiteral("/#download")); }

UpdatePackage UpdateService::buildPackage()
{
#if defined(VULKANA_UPDATE_PACKAGE_APPIMAGE)
    return UpdatePackage::AppImage;
#elif defined(VULKANA_UPDATE_PACKAGE_DEB)
    return UpdatePackage::Deb;
#else
    return UpdatePackage::Rpm;
#endif
}

bool UpdateService::isAppImageHeader(const QByteArray& bytes)
{
    return bytes.size() >= 64 && bytes.first(4) == QByteArray("\x7f" "ELF",4)
        && bytes[4] == 2 && bytes[5] == 1 && bytes.mid(8,3) == QByteArray("AI\x02",3)
        && static_cast<unsigned char>(bytes[18]) == 62 && bytes[19] == 0;
}

UpdateService::UpdateService(QString currentVersion, QObject* parent, QNetworkAccessManager* transport,
    UpdatePackage package, QString appImagePath)
    : QObject(parent), currentVersion_(std::move(currentVersion)),
      network_(transport ? transport : new QNetworkAccessManager(this)), package_(package),
      appImagePath_(appImagePath.isEmpty() ? qEnvironmentVariable("APPIMAGE") : std::move(appImagePath))
{
    deadline_.setSingleShot(true);
    deadline_.setTimerType(Qt::PreciseTimer);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        fail(tr("The request timed out. Please try again."));
    });
    const auto pending=QCoreApplication::instance()->property("vulkanaPendingRestart").toString();
    if(package_==UpdatePackage::AppImage&&!pending.isEmpty()&&pending==QFileInfo(appImagePath_).canonicalFilePath()) {
        downloadedPath_=pending;state_=State::RestartReady;
        message_=tr("Update verified and ready. Restart when you're ready.");
    }
}

UpdateService::~UpdateService() { stopTransfer(); }

std::optional<bool> UpdateService::isNewer(const QString& candidate, const QString& installed)
{
    if (!validVersion(candidate) || !validVersion(installed)) return std::nullopt;
    return QVersionNumber::compare(QVersionNumber::fromString(candidate),
        QVersionNumber::fromString(installed)) > 0;
}

bool UpdateService::startupCheckDue(qint64 now, std::optional<qint64> lastAttempt, bool force)
{
    // Check on missing/invalid cache or clock rollback, then replace the future
    // timestamp. Only subtract ordered, nonnegative times (no overflow).
    return force || !lastAttempt || *lastAttempt <= 0 || now < *lastAttempt
        || now - *lastAttempt >= 24 * 60 * 60;
}

std::optional<UpdateRelease> UpdateService::parseManifest(const QByteArray& bytes, QString& error, UpdatePackage package,
    const platform::ServiceConfig& config)
{
    error.clear();
    if (bytes.size() > manifestLimit) {
        error = tr("The update information exceeds the size limit.");
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    const auto key = package==UpdatePackage::AppImage ? QStringLiteral("appimage")
        : package==UpdatePackage::Deb ? QStringLiteral("deb") : QStringLiteral("rpm");
    // Retain legacy RPM-feed compatibility; never substitute another package.
    const auto artifact = !object.contains(key)&&package==UpdatePackage::Rpm ? object : object.value(key).toObject();
    UpdateRelease result {object.value(QStringLiteral("version")).toString(),
        artifact.value(QStringLiteral("sha256")).toString().toLatin1(),
        QUrl(artifact.value(QStringLiteral("url")).toString(), QUrl::StrictMode)};
    static const QRegularExpression digest(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    const QRegularExpression filename(package==UpdatePackage::AppImage
        ? QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._+-]*\\.AppImage\\z")
        : package==UpdatePackage::Deb ? QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._+-]*\\.deb\\z")
        : QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._+-]*\\.rpm\\z"));
    // Configured origin only. No local URLs,
    // credentials, alternate ports or redirects to a less trusted origin.
    const auto& url = result.url;
    const bool safeUrl = !config.origin.isEmpty() && url.isValid() && url.scheme() == QStringLiteral("https")
        && url.host() == config.origin.host() && url.port(443) == 443
        && url.userInfo().isEmpty() && !url.hasFragment() && !url.hasQuery()
        && url.path() == url.adjusted(QUrl::NormalizePathSegments).path()
        && url.adjusted(QUrl::NormalizePathSegments).path().startsWith(QStringLiteral("/downloads/"))
        && filename.match(QFileInfo(url.path()).fileName()).hasMatch();
    if (!document.isObject() || !validVersion(result.version)
        || !digest.match(QString::fromLatin1(result.sha256)).hasMatch() || !safeUrl) {
        error = tr("The update information is invalid. Try again or open the download page.");
        return std::nullopt;
    }
    result.sha256 = result.sha256.toLower();
    return result;
}

void UpdateService::notify() { if (onChanged) onChanged(); }

void UpdateService::stopTransfer()
{
    deadline_.stop();
    if (reply_) {
        auto* reply = reply_.data();
        reply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    // QSaveFile's default atomic path: failed/cancelled transfers never replace
    // an existing file, and temporary output is removed on destruction.
    file_.reset();
    updateLock_.reset();
    manifest_.clear();
}

void UpdateService::fail(const QString& message)
{
    stopTransfer();
    state_ = State::Error;
    message_ = message;
    notify();
}

void UpdateService::check(int timeoutMs)
{
    if (!configured()) {
        fail(tr("Updates are not configured for this build."));
        return;
    }
    if (busy() || state_==State::RestartReady) return;
    checkTimeoutMs_ = std::clamp(timeoutMs, 1, 60000);
    release_.reset();
    downloadedPath_.clear();
    if (!validVersion(currentVersion_)) {
        fail(tr("This build has no comparable release version. Please use the download page."));
        return;
    }
    state_ = State::Checking;
    message_ = tr("Checking for updates…");
    startRequest(manifestUrl(config_));
    notify();
}

void UpdateService::download(const QString& destination)
{
    if(package_==UpdatePackage::AppImage) { fail(tr("Use the AppImage update action to replace the running image."));return; }
    beginDownload(destination);
}

void UpdateService::downloadAppImage()
{
    if(package_!=UpdatePackage::AppImage||busy()||!release_||state_==State::RestartReady)return;
    const QFileInfo original(appImagePath_);
    // Do not guess the mount/extracted path or overwrite a different program.
    if(!original.isAbsolute()||!original.isFile()||original.isSymLink()
        ||original.ownerId()!=static_cast<uint>(::getuid())) {
        fail(tr("Run a user-owned AppImage from a writable folder to update it. Extracted builds can use the download page."));return;
    }
    const auto path=original.canonicalFilePath();
    updateLock_=std::make_unique<QLockFile>(path+QStringLiteral(".update.lock"));
    if(!updateLock_->tryLock(0)) {
        fail(tr("Cannot lock the AppImage for update. Check folder permissions or finish the other update."));return;
    }
    struct stat st {};
    QFile old(path);
    if(::stat(QFile::encodeName(path).constData(),&st)!=0||!old.open(QIODevice::ReadOnly)||!isAppImageHeader(old.read(64))) {
        fail(tr("The running AppImage file could not be verified. Use the download page."));return;
    }
    originalDevice_=st.st_dev;originalInode_=st.st_ino;originalSize_=st.st_size;
    originalMtime_=st.st_mtim.tv_sec;originalMtimeNsec_=st.st_mtim.tv_nsec;
    beginDownload(path); // QSaveFile stages beside it; never truncates the mounted inode.
}

void UpdateService::beginDownload(const QString& destination)
{
    if (busy() || !release_ || destination.isEmpty()) return;
    downloadedPath_.clear();
    file_ = std::make_unique<QSaveFile>(destination);
    file_->setDirectWriteFallback(false);
    if (!file_->open(QIODevice::WriteOnly)) {
        fail(tr("Failed to write the download. Check the folder permissions and free space."));
        return;
    }
    destination_ = QFileInfo(destination).absoluteFilePath();
    hash_.reset();
    header_.clear();
    state_ = State::Downloading;
    message_ = tr("Downloading Vulkana %1…").arg(release_->version);
    startRequest(release_->url);
    notify();
}

void UpdateService::cancel()
{
    if (!busy()) return;
    stopTransfer();
    state_ = release_ ? State::Available : State::Idle;
    message_ = tr("Cancelled. No download was saved.");
    notify();
}

void UpdateService::startRequest(const QUrl& url)
{
    manifest_.clear();
    received_ = 0;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("User-Agent", "Vulkana/" + currentVersion_.toUtf8());
    request.setRawHeader("Accept", state_ == State::Checking ? "application/json" : "application/octet-stream");
    request.setTransferTimeout(state_ == State::Checking ? std::min(30000, checkTimeoutMs_) : 30000);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(chunkSize);
    connect(reply_, &QNetworkReply::readyRead, this, &UpdateService::readAvailable);
    connect(reply_, &QNetworkReply::finished, this, &UpdateService::finish);
    connect(reply_, &QNetworkReply::metaDataChanged, this, [this] {
        if (!reply_) return;
        const auto http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http >= 400) {
            fail(tr("The server returned HTTP %1. Please try again.").arg(http));
            return;
        }
        const auto limit = state_ == State::Checking ? manifestLimit : packageLimit;
        if (reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit)
            fail(tr("The server response exceeds the download size limit."));
    });
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (state_ == State::Downloading && onProgress) onProgress(received, total);
    });
    deadline_.start(state_ == State::Checking ? checkTimeoutMs_ : 30 * 60 * 1000);
}

void UpdateService::readAvailable()
{
    // Never write an error or redirect body into the package stream.
    if (!reply_ || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
    while (reply_ && reply_->bytesAvailable() > 0) {
        const auto bytes = reply_->read(chunkSize);
        if (bytes.isEmpty()) break;
        received_ += bytes.size();
        if (received_ > (state_ == State::Checking ? manifestLimit : packageLimit)) {
            fail(tr("The server response exceeds the download size limit."));
            return;
        }
        if (state_ == State::Checking) manifest_ += bytes;
        else if (state_ == State::Downloading) {
            if (file_->write(bytes) != bytes.size()) {
                fail(tr("Failed to write the download. Check the folder permissions and free space."));
                return;
            }
            hash_.addData(bytes);
            if(header_.size()<72)header_+=bytes.first(qMin<qsizetype>(bytes.size(),72-header_.size()));
        }
    }
}

void UpdateService::finish()
{
    readAvailable();
    if (!reply_) return; // A streaming error already completed the operation.
    if (reply_->error() != QNetworkReply::NoError) {
        fail(tr("Could not complete the request: %1").arg(reply_->errorString()));
        return;
    }
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http != 200) {
        fail(tr("The server returned HTTP %1. Please try again.").arg(http));
        return;
    }
    if (state_ == State::Checking) {
        QString error;
        auto release = parseManifest(manifest_, error, package_, config_);
        if (!release) { fail(error); return; }
        stopTransfer();
        if (*isNewer(release->version, currentVersion_)) {
            release_ = std::move(release);
            state_ = State::Available;
            message_ = tr("Vulkana %1 is available.").arg(release_->version);
        } else {
            state_ = State::Current;
            message_ = tr("You have the latest version.");
        }
    } else if (state_ == State::Downloading) {
        if (received_ == 0 || hash_.result().toHex() != release_->sha256) {
            fail(tr("Checksum verification failed. No download was saved. Please try again."));
            return;
        }
        if (package_ == UpdatePackage::Deb) {
            // dpkg's ar format starts with the debian-binary member and 2.0\n.
            // Check its bounded header in addition to the whole-file SHA-256.
            const auto member = header_.mid(8, 16).trimmed();
            if (header_.size() < 72 || header_.first(8) != "!<arch>\n"
                || (member != "debian-binary" && member != "debian-binary/")
                || header_.mid(56, 10).trimmed() != "4"
                || header_.mid(66, 6) != "`\n2.0\n") {
                fail(tr("The verified download is not a supported DEB package. No download was saved."));
                return;
            }
        }
        if(package_==UpdatePackage::AppImage) {
            struct stat st {};
            if(!isAppImageHeader(header_)) {fail(tr("The verified download is not an x86-64 Type 2 AppImage. The current image is unchanged."));return;}
            if(::lstat(QFile::encodeName(destination_).constData(),&st)!=0||!S_ISREG(st.st_mode)
                ||st.st_dev!=originalDevice_||st.st_ino!=originalInode_||st.st_size!=originalSize_
                ||st.st_mtim.tv_sec!=originalMtime_||st.st_mtim.tv_nsec!=originalMtimeNsec_) {
                fail(tr("The AppImage changed during download. It was not replaced. Check for updates again."));return;
            }
            if(!file_->setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner
                |QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther)) {
                fail(tr("Failed to make the update executable. The current image is unchanged."));return;
            }
        }
        if (!file_->commit()) {
            fail(tr("Failed to save the download. Check the folder permissions and free space."));
            return;
        }
        downloadedPath_ = destination_;
        stopTransfer();
        state_ = package_==UpdatePackage::AppImage ? State::RestartReady : State::Downloaded;
        message_ = package_==UpdatePackage::AppImage
            ? tr("AppImage replaced and SHA-256 verified. Restart when you're ready.")
            : tr("Download saved and SHA-256 verified. Install the %1 manually when ready; Vulkana will not run it.")
                .arg(package_ == UpdatePackage::Deb ? QStringLiteral("DEB") : QStringLiteral("RPM"));
        if(package_==UpdatePackage::AppImage)
            QCoreApplication::instance()->setProperty("vulkanaPendingRestart",downloadedPath_);
    }
    notify();
}

} // namespace imageeditor::ui
