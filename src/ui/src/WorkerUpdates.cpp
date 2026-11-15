#include "imageeditor/ui/WorkerUpdates.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSysInfo>
#include <QVersionNumber>
#include <algorithm>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
constexpr qint64 feedLimit = 512 * 1024;
bool validVersion(const QString& s) { return UpdateService::isNewer(s, s).has_value(); }
int compare(const QString& a, const QString& b) {
    return QVersionNumber::compare(QVersionNumber::fromString(a), QVersionNumber::fromString(b));
}
void require(bool ok) { if (!ok) throw std::runtime_error("Invalid worker update information"); }
struct Range {
    QString minimum, maximum;
    QStringList excluded;
    bool contains(const QString& v) const {
        return compare(v, minimum) >= 0 && (maximum.isEmpty() || compare(v, maximum) < 0) && !excluded.contains(v);
    }
};
Range range(const QJsonValue& value) {
    require(value.isObject()); const auto obj = value.toObject();
    Range r{obj["min"].toString(), obj["maxExclusive"].toString(), {}};
    require(validVersion(r.minimum) && (obj["maxExclusive"].isUndefined() || obj["maxExclusive"].isNull() ||
        (obj["maxExclusive"].isString() && validVersion(r.maximum) && compare(r.minimum, r.maximum) < 0)));
    require(obj["exclude"].isUndefined() || obj["exclude"].isArray());
    for (const auto v : obj["exclude"].toArray()) { require(v.isString() && validVersion(v.toString())); r.excluded << v.toString(); }
    return r;
}
struct Entry { WorkerRelease release; Range range; QString backend, platform, architecture; int protocol{}; bool enabled{}; };
}

std::optional<WorkerRelease> WorkerUpdates::parseFeed(const QByteArray& bytes, const QString& appVersion,
        const QString& platformName, const QString& architecture, const platform::ServiceConfig& config, QString& error) {
    error.clear();
    try {
        require(bytes.size() <= feedLimit && validVersion(appVersion));
        const auto document = QJsonDocument::fromJson(bytes); const auto root = document.object();
        require(document.isObject() && root["schemaVersion"].toInt() == 1 && root["workers"].isArray() && root["pins"].isArray());
        QMap<QString, Entry> entries;
        const QRegularExpression sha("\\A[0-9a-fA-F]{64}\\z"), identifier("\\A[a-zA-Z0-9_-]{1,64}\\z");
        for (const auto value : root["workers"].toArray()) {
            require(value.isObject()); const auto w = value.toObject(); Entry e;
            e.release.id = w["id"].toString(); e.release.artifact.version = w["version"].toString();
            e.release.artifact.sha256 = w["sha256"].toString().toLatin1().toLower();
            e.release.artifact.url = QUrl(w["url"].toString(), QUrl::StrictMode);
            e.release.size = w["size"].toInteger(-1);
            e.backend = w["backend"].toString(); e.platform = w["platform"].toString(); e.architecture = w["architecture"].toString();
            e.protocol = w["protocol"].toInt(-1); e.enabled = w["enabled"].toBool(); e.range = range(w["vulkana"]);
            require(validVersion(e.release.artifact.version) && (e.backend == "cuda" || e.backend == "vulkan") &&
                identifier.match(e.platform).hasMatch() && identifier.match(e.architecture).hasMatch() &&
                e.release.id == e.backend + '-' + e.release.artifact.version + '-' + e.platform + '-' + e.architecture &&
                !entries.contains(e.release.id) && w["enabled"].isBool() && e.protocol > 0 &&
                w["protocol"].toDouble() == e.protocol && w["size"].isDouble() && w["size"].toDouble() == double(e.release.size) &&
                e.release.size > 0 && e.release.size <= qint64(4) * 1024 * 1024 * 1024 && sha.match(QString::fromLatin1(e.release.artifact.sha256)).hasMatch());
            const auto format = w["archive"].toString();
            require((format == "zip" || format == "tar.xz") &&
                UpdateService::trustedDownloadUrl(e.release.artifact.url, config, '.' + format));
            if (e.platform == "windows") require(format == "zip");
            if (e.platform == "linux") require(format == "tar.xz");
            entries.insert(e.release.id, e);
        }
        const auto host = [&](const Entry& e) { return e.backend == "cuda" && e.platform == platformName && e.architecture == architecture; };
        const auto compatible = [&](const Entry& e) { return host(e) && e.enabled && e.protocol == 1 && e.range.contains(appVersion); };
        std::optional<WorkerRelease> pinned;
        bool hasPin = false;
        for (const auto value : root["pins"].toArray()) {
            require(value.isObject()); const auto pin = value.toObject();
            const auto it = entries.constFind(pin["workerId"].toString()); require(it != entries.cend());
            const auto interval = range(pin["vulkana"]);
            if (host(*it) && interval.contains(appVersion)) {
                require(!hasPin); hasPin = true;
                if (compatible(*it)) { pinned = it->release; pinned->pinned = true; }
            }
        }
        // A pin never silently falls back to a different/newer package.
        if (hasPin) return pinned;
        std::optional<WorkerRelease> result;
        for (const auto& entry : entries) if (compatible(entry) &&
            (!result || compare(entry.release.artifact.version, result->artifact.version) > 0)) result = entry.release;
        return result;
    } catch (const std::exception&) {
        error = i18n::text("diffusion.workers.feed.invalid"); return {};
    }
}

QString WorkerUpdates::hostPlatform() {
#ifdef Q_OS_WIN
    return "windows";
#elif defined(Q_OS_LINUX)
    return "linux";
#else
    return {};
#endif
}
WorkerUpdates::WorkerUpdates(QString appVersion, QObject* parent, QNetworkAccessManager* transport)
    : QObject(parent), appVersion_(std::move(appVersion)), network_(transport ? transport : new QNetworkAccessManager(this)) {
    setObjectName("DiffusionWorkerUpdates");
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] { failCheck(i18n::text("update.service.the.request.timed.out.please.try.again")); });
    transfer_ = new UpdateService(appVersion_, this, network_, UpdatePackage::WindowsPortable);
    transfer_->onProgress = [this](qint64 received, qint64) { received_ = received; emit changed(); };
    transfer_->onChanged = [this] {
        emit changed();
        if (transfer_->state() == UpdateService::State::Downloaded) emit downloadFinished(transfer_->downloadedPath());
        else if (transfer_->state() == UpdateService::State::Error) emit downloadFailed(transfer_->message());
    };
}
WorkerUpdates::~WorkerUpdates() { stopCheck(); transfer_->onChanged = {}; transfer_->onProgress = {}; transfer_->cancel(); }
bool WorkerUpdates::configured() const { return config_.enabled() && !hostPlatform().isEmpty() && validVersion(appVersion_); }
bool WorkerUpdates::downloading() const { return transfer_->state() == UpdateService::State::Downloading; }
QString WorkerUpdates::downloadedPath() const { return transfer_->downloadedPath(); }
void WorkerUpdates::stopCheck() {
    deadline_.stop();
    if (reply_) { auto* reply = reply_.data(); reply_ = nullptr; reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    bytes_.clear();
}
void WorkerUpdates::failCheck(const QString& error) { stopCheck(); checked_ = true; release_.reset(); error_ = error; emit changed(); }
void WorkerUpdates::check(int timeoutMs) {
    if (checking()) return;
    if (!configured()) { failCheck(i18n::text("update.service.updates.are.not.configured.for.this.build")); return; }
    error_.clear(); bytes_.clear();
    QNetworkRequest request(config_.url("/downloads/workers.json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("User-Agent", "Vulkana/" + appVersion_.toUtf8());
    request.setRawHeader("Accept", "application/json");
    timeoutMs = std::clamp(timeoutMs, 1, 60000); request.setTransferTimeout(std::min(timeoutMs, 30000));
    // Public GET, like the application feed: the private token gates access to
    // this feature locally, and is never sent in a URL, header or diagnostic.
    reply_ = network_->get(request); reply_->setReadBufferSize(64 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, &WorkerUpdates::readAvailable);
    connect(reply_, &QNetworkReply::finished, this, &WorkerUpdates::finishCheck);
    connect(reply_, &QNetworkReply::metaDataChanged, this, [this] {
        if (!reply_) return;
        const auto http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http >= 400) { failCheck(i18n::text("update.service.the.server.returned.http.1.please.try.again").arg(http)); return; }
        if (reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong() > feedLimit)
            failCheck(i18n::text("diffusion.workers.feed.invalid"));
    });
    deadline_.start(timeoutMs); emit changed();
}
void WorkerUpdates::readAvailable() {
    if (!reply_ || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
    while (reply_ && reply_->bytesAvailable()) {
        const auto chunk = reply_->read(16 * 1024);
        if (chunk.isEmpty()) break;
        bytes_ += chunk;
        if (bytes_.size() > feedLimit) { failCheck(i18n::text("diffusion.workers.feed.invalid")); return; }
    }
}
void WorkerUpdates::finishCheck() {
    readAvailable(); if (!reply_) return;
    if (reply_->error() != QNetworkReply::NoError) { failCheck(i18n::text("diffusion.workers.check.failed")); return; }
    const auto http = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http != 200) { failCheck(i18n::text("update.service.the.server.returned.http.1.please.try.again").arg(http)); return; }
    QString error;
    auto result = parseFeed(bytes_, appVersion_, hostPlatform(), QSysInfo::buildCpuArchitecture(), config_, error);
    stopCheck(); checked_ = true; release_ = std::move(result); error_ = error; emit changed();
}
void WorkerUpdates::download(const QString& destination) {
    if (!configured() || checking() || downloading() || !release_ || destination.isEmpty()) return;
    received_ = 0; total_ = release_->size;
    transfer_->downloadArchive(release_->artifact, total_, hostPlatform() == "windows" ? UpdateService::ArchiveFormat::Zip : UpdateService::ArchiveFormat::Xz, destination);
}
void WorkerUpdates::cancelDownload() { transfer_->cancel(); }
void WorkerUpdates::stop() { stopCheck(); transfer_->cancel(); }
} // namespace imageeditor::ui
