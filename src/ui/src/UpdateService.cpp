#include "imageeditor/ui/Localization.hpp"
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
#ifdef Q_OS_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif
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
#if defined(VULKANA_UPDATE_PACKAGE_NONE)
    return UpdatePackage::Unavailable;
#elif defined(VULKANA_UPDATE_PACKAGE_WINDOWSPORTABLE)
    return UpdatePackage::WindowsPortable;
#elif defined(VULKANA_UPDATE_PACKAGE_APPIMAGE)
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
        fail(i18n::text("update.service.the.request.timed.out.please.try.again"));
    });
    const auto pending=QCoreApplication::instance()->property("vulkanaPendingRestart").toString();
    if(package_==UpdatePackage::AppImage&&!pending.isEmpty()&&pending==QFileInfo(appImagePath_).canonicalFilePath()) {
        downloadedPath_=pending;state_=State::RestartReady;
        message_=i18n::text("update.service.update.verified.and.ready.restart.when.you.re.ready");
    }
}

UpdateService::~UpdateService() { stopTransfer(); }

std::optional<bool> UpdateService::isNewer(const QString& candidate, const QString& installed)
{
    if (!validVersion(candidate) || !validVersion(installed)) return std::nullopt;
    return QVersionNumber::compare(QVersionNumber::fromString(candidate),
        QVersionNumber::fromString(installed)) > 0;
}

void UpdateService::simulateUpdate()
{
    stopTransfer();
    simulated_ = true;
    release_.reset();
    downloadedPath_.clear();
    if (!validVersion(currentVersion_)) {
        fail(i18n::text("update.service.this.build.has.no.comparable.release.version.to.simulate"));
        return;
    }
    auto parts = QVersionNumber::fromString(currentVersion_).segments();
    for (qsizetype index = parts.size(); index-- > 0;) {
        if (parts[index] == 999999999) { parts[index] = 0; continue; }
        ++parts[index];
        release_ = UpdateRelease {QVersionNumber(parts).toString(), {}, {}};
        state_ = State::Available;
        message_ = i18n::text("update.service.vulkana.1.is.available").arg(i18n::ltrToken(release_->version));
        notify();
        return;
    }
    fail(i18n::text("update.service.this.build.s.version.cannot.be.incremented.for.the"));
}

std::optional<UpdateRelease> UpdateService::parseManifest(const QByteArray& bytes, QString& error, UpdatePackage package,
    const platform::ServiceConfig& config)
{
    error.clear();
    if (package == UpdatePackage::Unavailable) {
        error = i18n::text("update.service.updates.are.not.configured.for.this.build");
        return std::nullopt;
    }
    if (bytes.size() > manifestLimit) {
        error = i18n::text("update.service.the.update.information.exceeds.the.size.limit");
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto object = document.object();
    const auto key = package==UpdatePackage::AppImage ? QStringLiteral("appimage")
        : package==UpdatePackage::WindowsPortable ? QStringLiteral("windowsportable")
        : package==UpdatePackage::Deb ? QStringLiteral("deb") : QStringLiteral("rpm");
    // Retain legacy RPM-feed compatibility; never substitute another package.
    const auto artifact = !object.contains(key)&&package==UpdatePackage::Rpm ? object : object.value(key).toObject();
    UpdateRelease result {object.value(QStringLiteral("version")).toString(),
        artifact.value(QStringLiteral("sha256")).toString().toLatin1(),
        QUrl(artifact.value(QStringLiteral("url")).toString(), QUrl::StrictMode)};
    static const QRegularExpression digest(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    const QRegularExpression filename(package==UpdatePackage::AppImage
        ? QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._+-]*\\.AppImage\\z")
        : package==UpdatePackage::WindowsPortable ? QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._+-]*\\.zip\\z")
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
        error = i18n::text("update.service.the.update.information.is.invalid.try.again.or.open");
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

void UpdateService::fail(const i18n::Message& message)
{
    stopTransfer();
    state_ = State::Error;
    message_ = message;
    notify();
}

void UpdateService::check(int timeoutMs)
{
    if (simulated_) { simulateUpdate(); return; }
    if (!configured()) {
        fail(i18n::text("update.service.updates.are.not.configured.for.this.build"));
        return;
    }
    if (busy() || state_==State::RestartReady) return;
    checkTimeoutMs_ = std::clamp(timeoutMs, 1, 60000);
    release_.reset();
    downloadedPath_.clear();
    if (!validVersion(currentVersion_)) {
        fail(i18n::text("update.service.this.build.has.no.comparable.release.version.please.use"));
        return;
    }
    state_ = State::Checking;
    message_ = i18n::text("update.service.checking.for.updates");
    startRequest(manifestUrl(config_));
    notify();
}

void UpdateService::download(const QString& destination)
{
    if (simulated_) return;
    if(package_==UpdatePackage::AppImage) { fail(i18n::text("update.service.use.the.appimage.update.action.to.replace.the.running"));return; }
    beginDownload(destination);
}

void UpdateService::downloadAppImage()
{
    if (simulated_ || package_ != UpdatePackage::AppImage) return;
#ifdef Q_OS_LINUX
    if(package_!=UpdatePackage::AppImage||busy()||!release_||state_==State::RestartReady)return;
    const QFileInfo original(appImagePath_);
    // Do not guess the mount/extracted path or overwrite a different program.
    if(!original.isAbsolute()||!original.isFile()||original.isSymLink()
        ||original.ownerId()!=static_cast<uint>(::getuid())) {
        fail(i18n::text("update.service.run.a.user.owned.appimage.from.a.writable.folder"));return;
    }
    const auto path=original.canonicalFilePath();
    updateLock_=std::make_unique<QLockFile>(path+QStringLiteral(".update.lock"));
    if(!updateLock_->tryLock(0)) {
        fail(i18n::text("update.service.cannot.lock.the.appimage.for.update.check.folder.permissions"));return;
    }
    struct stat st {};
    QFile old(path);
    if(::stat(QFile::encodeName(path).constData(),&st)!=0||!old.open(QIODevice::ReadOnly)||!isAppImageHeader(old.read(64))) {
        fail(i18n::text("update.service.the.running.appimage.file.could.not.be.verified.use"));return;
    }
    originalDevice_=st.st_dev;originalInode_=st.st_ino;originalSize_=st.st_size;
    originalMtime_=st.st_mtim.tv_sec;originalMtimeNsec_=st.st_mtim.tv_nsec;
    beginDownload(path); // QSaveFile stages beside it; never truncates the mounted inode.
#else
    fail(i18n::text("update.service.updates.are.not.configured.for.this.build"));
#endif
}

void UpdateService::beginDownload(const QString& destination)
{
    if (package_ == UpdatePackage::Unavailable) return;
    if (busy() || !release_ || destination.isEmpty()) return;
    downloadedPath_.clear();
    file_ = std::make_unique<QSaveFile>(destination);
    file_->setDirectWriteFallback(false);
    if (!file_->open(QIODevice::WriteOnly)) {
        fail(i18n::text("update.service.failed.to.write.the.download.check.the.folder.permissions"));
        return;
    }
    destination_ = QFileInfo(destination).absoluteFilePath();
    hash_.reset();
    header_.clear();
    state_ = State::Downloading;
    message_ = i18n::text("update.service.downloading.vulkana.1").arg(i18n::ltrToken(release_->version));
    startRequest(release_->url);
    notify();
}

void UpdateService::cancel()
{
    if (!busy()) return;
    stopTransfer();
    state_ = release_ ? State::Available : State::Idle;
    message_ = i18n::text("update.service.cancelled.no.download.was.saved");
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
            fail(i18n::text("update.service.the.server.returned.http.1.please.try.again").arg(http));
            return;
        }
        const auto limit = state_ == State::Checking ? manifestLimit : packageLimit;
        if (reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit)
            fail(i18n::text("update.service.the.server.response.exceeds.the.download.size.limit"));
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
            fail(i18n::text("update.service.the.server.response.exceeds.the.download.size.limit"));
            return;
        }
        if (state_ == State::Checking) manifest_ += bytes;
        else if (state_ == State::Downloading) {
            if (file_->write(bytes) != bytes.size()) {
                fail(i18n::text("update.service.failed.to.write.the.download.check.the.folder.permissions"));
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
        fail(i18n::text("update.service.could.not.complete.the.request.1").arg(reply_->errorString()));
        return;
    }
    const int http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http != 200) {
        fail(i18n::text("update.service.the.server.returned.http.1.please.try.again").arg(http));
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
            message_ = i18n::text("update.service.vulkana.1.is.available").arg(i18n::ltrToken(release_->version));
        } else {
            state_ = State::Current;
            message_ = i18n::text("update.service.you.have.the.latest.version");
        }
    } else if (state_ == State::Downloading) {
        if (received_ == 0 || hash_.result().toHex() != release_->sha256) {
            fail(i18n::text("update.service.checksum.verification.failed.no.download.was.saved.please.try"));
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
                fail(i18n::text("update.service.the.verified.download.is.not.a.supported.deb.package"));
                return;
            }
        }
        // Verify the bounded ZIP signature only. Archive entries are never opened
        // or extracted by the updater; SHA-256 covers the complete downloaded file.
        if (package_ == UpdatePackage::WindowsPortable && header_.first(4) != QByteArray("PK\x03\x04", 4)) {
            fail(i18n::text("update.service.invalid.portable.zip"));
            return;
        }
        if(package_==UpdatePackage::AppImage) {
#ifdef Q_OS_LINUX
            struct stat st {};
            if(!isAppImageHeader(header_)) {fail(i18n::text("update.service.the.verified.download.is.not.an.x86.64.type"));return;}
            if(::lstat(QFile::encodeName(destination_).constData(),&st)!=0||!S_ISREG(st.st_mode)
                ||st.st_dev!=originalDevice_||st.st_ino!=originalInode_||st.st_size!=originalSize_
                ||st.st_mtim.tv_sec!=originalMtime_||st.st_mtim.tv_nsec!=originalMtimeNsec_) {
                fail(i18n::text("update.service.the.appimage.changed.during.download.it.was.not.replaced"));return;
            }
            if(!file_->setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner
                |QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther)) {
                fail(i18n::text("update.service.failed.to.make.the.update.executable.the.current.image"));return;
            }
#else
            fail(i18n::text("update.service.updates.are.not.configured.for.this.build"));return;
#endif
        }
        if (!file_->commit()) {
            fail(i18n::text("update.service.failed.to.save.the.download.check.the.folder.permissions"));
            return;
        }
        downloadedPath_ = destination_;
        stopTransfer();
        state_ = package_==UpdatePackage::AppImage ? State::RestartReady : State::Downloaded;
        message_ = package_==UpdatePackage::AppImage
            ? i18n::text("update.service.appimage.replaced.and.sha.256.verified.restart.when.you")
            : package_==UpdatePackage::WindowsPortable
                ? i18n::text("update.service.portable.zip.saved")
            : i18n::text("update.service.download.saved.and.sha.256.verified.install.the.1")
                .arg(package_ == UpdatePackage::Deb ? QStringLiteral("DEB") : QStringLiteral("RPM"));
        if(package_==UpdatePackage::AppImage)
            QCoreApplication::instance()->setProperty("vulkanaPendingRestart",downloadedPath_);
    }
    notify();
}

} // namespace imageeditor::ui
