#include "imageeditor/ui/UpdateService.hpp"
#include "imageeditor/ui/AboutDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "TestServiceConfig.hpp"

#include <QApplication>
#include <QAction>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>

namespace u = imageeditor::ui;
using State = u::UpdateService::State;
namespace {
int failures = 0;
void verify(bool ok, const char* expression, int line)
{
    if (!ok) { ++failures; std::cerr << "FAIL " << line << ": " << expression << '\n'; }
}
#define CHECK(x) verify(bool(x), #x, __LINE__)
template<class F> bool waitFor(F condition, int timeout = 3000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) QTest::qWait(2);
    return condition();
}

const QByteArray package = QByteArray::fromHex("edabeedb") + QByteArray(150000, 'r');
QByteArray manifest(QString version = QStringLiteral("0.2.2"), QByteArray payload = package)
{
    return QJsonDocument(QJsonObject {
        {"version", version}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())},
        {"url", "https://updates.example/downloads/vulkana-editor-0.2.2-1.fc44.x86_64.rpm"}
    }).toJson();
}
struct Response {
    QByteArray body;
    int http {200};
    QNetworkReply::NetworkError error {QNetworkReply::NoError};
    bool stall {false};
    qint64 declaredSize {-1};
};

class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request, Response response, QObject* parent)
        : QNetworkReply(parent), response_(std::move(response))
    {
        setRequest(request); setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) return;
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.http);
            if (response_.declaredSize >= 0)
                setHeader(QNetworkRequest::ContentLengthHeader, response_.declaredSize);
            emit metaDataChanged();
            if (!isFinished()) advance();
        });
    }
    void abort() override
    {
        if (isFinished()) return;
        setError(OperationCanceledError, "Cancelled");
        setFinished(true); emit finished();
    }
    qint64 bytesAvailable() const override { return ready_ - read_ + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        const auto count = std::min(maximum, ready_ - read_);
        if (count == 0) return isFinished() ? -1 : 0;
        std::memcpy(data, response_.body.constData() + read_, static_cast<std::size_t>(count));
        read_ += count;
        return count;
    }
private:
    void advance()
    {
        if (isFinished()) return;
        ready_ = std::min(ready_ + 8192, response_.body.size());
        emit readyRead();
        if (isFinished()) return;
        emit downloadProgress(ready_, response_.body.size());
        if (isFinished() || response_.stall) return;
        if (ready_ < response_.body.size()) QTimer::singleShot(0, this, [this] { advance(); });
        else {
            if (response_.error != NoError) setError(response_.error, "Simulated network failure");
            setFinished(true); emit finished();
        }
    }
    Response response_;
    qint64 read_ {}, ready_ {};
};

class Network final : public QNetworkAccessManager {
public:
    std::deque<Response> responses;
    QList<QNetworkRequest> requests;
    QPointer<QNetworkReply> last;
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice*) override
    {
        CHECK(operation == GetOperation);
        requests.push_back(request);
        CHECK(!responses.empty());
        Response response;
        if (!responses.empty()) { response = std::move(responses.front()); responses.pop_front(); }
        else response.error = QNetworkReply::UnknownNetworkError;
        last = new Reply(request, std::move(response), this);
        return last;
    }
};
QByteArray readFile(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void seedFile(const QString& path) { QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write("previous") == 8); }
void available(u::UpdateService& service, Network& network)
{
    network.responses.push_back({manifest()});
    service.check();
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Available);
}

void metadataTests()
{
    CHECK(u::UpdateService::isNewer("0.2.10", "0.2.9") == true);
    CHECK(u::UpdateService::isNewer("1.0.0", "0.99.99") == true);
    CHECK(u::UpdateService::isNewer("0.2.2", "0.2.2") == false);
    CHECK(u::UpdateService::isNewer("0.2.1", "0.2.2") == false);
    CHECK(!u::UpdateService::isNewer("0.2.3evil", "0.2.2"));
    CHECK(!u::UpdateService::isNewer("0.2.3", "development"));
    CHECK(!u::UpdateService::isNewer("1.2.9999999999999999", "0.2.2"));
    QString error;
    CHECK(u::UpdateService::parseManifest(manifest(), error));
    CHECK(error.isEmpty());
    for (const auto& data : {QByteArray("not json"), QByteArray("[]"), QByteArray("{}"), QByteArray(65537, ' ')})
        CHECK(!u::UpdateService::parseManifest(data, error) && !error.isEmpty());
    const auto change = [&](const char* name, const QJsonValue& value) {
        auto object = QJsonDocument::fromJson(manifest()).object(); object.insert(QString::fromLatin1(name), value);
        return QJsonDocument(object).toJson();
    };
    for (const char* url : {"http://updates.example/downloads/a.rpm", "file:///tmp/a.rpm",
            "https://evil.example/downloads/a.rpm", "https://updates.example/downloads/a.zip",
            "https://updates.example/downloads/../a.rpm", "https://user@updates.example/downloads/a.rpm",
            "https://updates.example:1234/downloads/a.rpm"})
        CHECK(!u::UpdateService::parseManifest(change("url", QString::fromLatin1(url)), error));
    CHECK(!u::UpdateService::parseManifest(change("sha256", "short"), error));
    CHECK(!u::UpdateService::parseManifest(change("version", 123), error));
    auto parsed = u::UpdateService::parseManifest(change("sha256", QString(64, 'A')), error);
    CHECK(parsed && parsed->sha256 == QByteArray(64, 'a'));
}

void serviceConfigurationTests()
{
    using imageeditor::platform::ServiceConfig;
    TestServiceConfig file;
    const auto original = ServiceConfig::load();
    CHECK(original.enabled() && original.origin == QUrl("https://updates.example/"));
    CHECK(original.apiKey == "test-client-token");
    CHECK(u::UpdateService::manifestUrl(original) == QUrl("https://updates.example/downloads/latest.json"));
    CHECK(u::UpdateService::downloadPageUrl(original) == QUrl("https://updates.example/#download"));
    for (const auto& bytes : {QByteArray("{"), QByteArray("[]"), QByteArray(65537, ' '),
             QByteArray("{\"version\":2,\"domain\":\"https://updates.example\",\"apiKey\":\"test\"}")}) {
        CHECK(file.writeBytes(bytes)); CHECK(!ServiceConfig::load().enabled());
    }
    for (const auto* domain : {"", "updates.example", "http://updates.example", "file:///tmp",
             "https://user:password@updates.example", "https://updates.example:1234",
             "https://updates.example/api", "https://updates.example/?query", "https://updates.example/#fragment"}) {
        CHECK(file.write({{"version", 1}, {"domain", domain}, {"apiKey", "test"}}));
        CHECK(!ServiceConfig::load().enabled());
    }
    for (const auto& key : {QString{}, QString("bad\r\nheader"), QString(4097, 'a')}) {
        CHECK(file.write({{"version", 1}, {"domain", "https://updates.example"}, {"apiKey", key}}));
        CHECK(!ServiceConfig::load().enabled());
    }
    CHECK(QFile::remove(file.path()));
    CHECK(!ServiceConfig::load().enabled()); // No fallback to private/host config.
    qputenv("VULKANA_SERVICE_CONFIG", "relative.json");
    CHECK(!ServiceConfig::load().enabled());
    qputenv("VULKANA_SERVICE_CONFIG", file.path().toUtf8());

    // No config: neither a manual check nor forced startup can send a request.
    // The normal New Canvas flow still opens, without consuming the throttle.
    for (bool emptyToken : {false, true}) {
        if (emptyToken) CHECK(file.write({{"version", 1}, {"domain", "https://updates.example"}, {"apiKey", ""}}));
        Network network;
        u::UpdateService service("0.2.1", nullptr, &network);
        CHECK(!service.configured()); service.check();
        CHECK(network.requests.empty() && !service.busy());
        QWidget owner;
        u::AboutDialog about(&owner, &network);
        CHECK(!about.findChild<QPushButton*>("AboutCheckUpdate")->isEnabled());
        CHECK(about.findChild<QPushButton*>("AboutDownloadPage")->isEnabled() == emptyToken);
        QSettings().remove("updates/lastStartupAttemptUtcSeconds");
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        CHECK(!window.findChild<QAction*>("FeedbackAction")->isEnabled());
        bool newCanvas = false;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            if (auto* dialog = window.findChild<QDialog*>("NewDocumentDialog"); dialog && dialog->isVisible()) {
                newCanvas = true; dialog->reject();
            }
        });
        responder.start(5);
        QTimer::singleShot(2000, &window, [&] { if (!newCanvas) { CHECK(false); window.close(); } });
        window.startStartupFlow(true, true, &network);
        CHECK(waitFor([&] { return newCanvas; }));
        CHECK(network.requests.empty());
        CHECK(!QSettings().contains("updates/lastStartupAttemptUtcSeconds"));
        window.close();
    }

    // A file change during I/O must not change that service's trusted origin.
    CHECK(file.write({{"version", 1}, {"domain", "https://updates.example"}, {"apiKey", "test-client-token"}}));
    Network network;
    network.responses.push_back({manifest()});
    u::UpdateService service("0.2.1", nullptr, &network);
    CHECK(file.write({{"version", 1}, {"domain", "https://different.example"}, {"apiKey", "new-token"}}));
    service.check();
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Available);
    CHECK(network.requests.front().url() == u::UpdateService::manifestUrl(original));
    CHECK(network.requests.front().rawHeader("X-Vulkana-Token").isEmpty()); // GET needs no token.
    QString error;
    CHECK(!u::UpdateService::parseManifest(manifest(), error)); // New origin rejects old origin.
}

void stateAndDownloadTests()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path = directory.filePath("update.rpm");
    Network network;
    u::UpdateService service("0.2.1", nullptr, &network, u::UpdatePackage::Rpm);
    CHECK(network.requests.isEmpty()); // No startup request.
    available(service, network);
    CHECK(network.requests.back().url() == u::UpdateService::manifestUrl());
    CHECK(network.requests.back().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()
        == QNetworkRequest::SameOriginRedirectPolicy);
    CHECK(network.requests.back().transferTimeout() == 30000);
    seedFile(path);
    network.responses.push_back({package});
    int progressCount = 0;
    service.onProgress = [&](qint64, qint64) { ++progressCount; };
    service.download(path);
    CHECK(service.busy() && service.state() == State::Downloading);
    CHECK(readFile(path) == "previous");
    const auto count = network.requests.size();
    service.check(); service.download(path);
    CHECK(network.requests.size() == count);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Downloaded && readFile(path) == package);
    CHECK(progressCount > 1 && service.downloadedPath() == path);

    seedFile(path);
    network.responses.push_back({"corrupt"});
    service.download(path);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error && service.message().contains("Checksum"));
    CHECK(readFile(path) == "previous");
    network.responses.push_back({package}); // Retry does not need another check.
    service.download(path);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Downloaded && readFile(path) == package);

    seedFile(path);
    network.responses.push_back({package, 200, QNetworkReply::RemoteHostClosedError});
    service.download(path);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error && readFile(path) == "previous");
    network.responses.push_back({package, 200, QNetworkReply::NoError, true});
    service.download(path);
    QTest::qWait(10);
    service.cancel();
    CHECK(service.state() == State::Available && readFile(path) == "previous");
    CHECK(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size() == 1);
    QTest::qWait(10); CHECK(service.state() == State::Available);

    service.download(directory.filePath("missing/folder/update.rpm"));
    CHECK(service.state() == State::Error && service.message().contains("Failed to write"));
    network.responses.push_back({package, 200, QNetworkReply::NoError, false, qint64(1024) * 1024 * 1024 + 1});
    service.download(path);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error && readFile(path) == "previous");
    network.responses.push_back({"error", 503});
    service.download(path);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error && service.message().contains("503") && readFile(path) == "previous");

    for (const auto& version : {QString("0.2.1"), QString("0.2.0")}) {
        network.responses.push_back({manifest(version)});
        service.check();
        CHECK(waitFor([&] { return !service.busy(); }));
        CHECK(service.state() == State::Current && !service.release());
    }
    network.responses.push_back({QByteArray(65537, 'x')});
    service.check();
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error && service.message().contains("size limit"));
    network.responses.push_back({"", 200, QNetworkReply::SslHandshakeFailedError});
    service.check();
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error);
    network.responses.push_back({manifest(), 200, QNetworkReply::NoError, true});
    service.check(); service.check();
    service.cancel();
    CHECK(service.state() == State::Idle);
    QTest::qWait(10); CHECK(service.state() == State::Idle);
    {
        u::UpdateService dying("0.2.1", nullptr, &network, u::UpdatePackage::Rpm);
        available(dying, network);
        network.responses.push_back({package, 200, QNetworkReply::NoError, true});
        dying.download(path); QTest::qWait(10);
    }
    QTest::qWait(10);
    CHECK(readFile(path) == "previous");
    CHECK(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size() == 1);
}

void dialogTests()
{
    QCoreApplication::setApplicationVersion("0.2.1");
    QTemporaryDir files;
    Network network;
    QWidget host; host.resize(700, 800); host.show();
    u::AboutDialog dialog(&host, &network, u::UpdatePackage::Rpm);
    dialog.show();
    auto* check = dialog.findChild<QPushButton*>("AboutCheckUpdate");
    auto* page = dialog.findChild<QPushButton*>("AboutDownloadPage");
    auto* status = dialog.findChild<QLabel*>("AboutUpdateStatus");
    auto* website = dialog.findChild<QLabel*>("AboutVulkanaWebsite");
    auto* progress = dialog.findChild<QProgressBar*>("AboutDownloadProgress");
    CHECK(check && page && status && website && progress);
    if (!check || !page || !status || !website || !progress) return;
    CHECK(!dialog.findChild<QPushButton*>("AboutDownload")); // Exactly one primary action.
    CHECK(website->openExternalLinks() && website->text().contains("https://updates.example/"));
    CHECK(website->text().contains(u::themeColor(u::ThemeColor::Accent).name()));
    CHECK(network.requests.isEmpty() && check->text() == "Check for Updates" && page->isEnabled());
    CHECK(status->textFormat() == Qt::PlainText);
    CHECK(status->parentWidget() == check->parentWidget());
    const auto size = dialog.size();
    const auto actionWidth = check->width();
    network.responses.push_back({manifest()}); check->click(); check->click();
    CHECK(check->isEnabled() && check->text() == "Checking…");
    CHECK(waitFor([&] { return check->text() != "Checking…"; }));
    CHECK(network.requests.size() == 1 && check->text() == "Download RPM" && status->text().contains("0.2.2"));
    CHECK(dialog.width() == size.width() && check->width() == actionWidth);
    bool pickerSeen = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        CHECK(picker);
        if (picker) { pickerSeen = true; picker->reject(); }
    });
    check->click();
    CHECK(pickerSeen && network.requests.size() == 1);
    const auto path = files.filePath("chosen.rpm");
    network.responses.push_back({package});
    QTimer::singleShot(0, &dialog, [&] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        CHECK(picker);
        if (picker) {
            picker->selectFile(path);
            QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
        }
    });
    check->click();
    CHECK(check->text() == "Cancel Download" && check->isEnabled());
    CHECK(progress->isVisible());
    CHECK(waitFor([&] { return check->text() == "Check for Updates"; }));
    CHECK(readFile(path) == package && status->text().contains("verified"));
    CHECK(status->toolTip().contains(path));
    CHECK(dialog.width() == size.width() && check->width() == actionWidth && progress->isHidden());
    network.responses.push_back({manifest(), 200, QNetworkReply::NoError, true});
    check->click();
    dialog.reject();
    CHECK(check->isEnabled());
    QTest::qWait(10);
    dialog.show();
    network.responses.push_back({manifest("0.2.1")});
    check->click();
    CHECK(waitFor([&] { return check->text() != "Checking…"; }));
    CHECK(status->text().contains("latest version") && check->text() == "Check for Updates");
    CHECK(dialog.width() == size.width() && check->width() == actionWidth);
    CHECK(check->geometry().left() > status->geometry().right());
    CHECK(std::abs(check->geometry().center().y() - (status->geometry().center().y()
        + dialog.findChild<QLabel*>("AboutUpdateDetail")->geometry().center().y()) / 2) <= 12);
    CHECK((dialog.geometry().center() - host.rect().center()).manhattanLength() <= 2);
    if (const auto path = qEnvironmentVariable("VULKANA_ABOUT_CAPTURE"); !path.isEmpty())
        CHECK(dialog.grab().save(path));
    network.responses.push_back({manifest()});
    check->click();
    CHECK(waitFor([&] { return check->text() == "Download RPM"; }));
    const auto cancelledPath = files.filePath("cancelled.rpm");
    network.responses.push_back({package, 200, QNetworkReply::NoError, true});
    QTimer::singleShot(0, &dialog, [&] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        CHECK(picker);
        if (picker) {
            picker->selectFile(cancelledPath);
            QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
        }
    });
    check->click();
    CHECK(check->text() == "Cancel Download" && check->isEnabled());
    QTest::qWait(10);
    check->click();
    CHECK(check->text() == "Download RPM" && !QFile::exists(cancelledPath));
    CHECK(progress->isHidden());
}

QByteArray appImageBytes(char body)
{
    QByteArray bytes(180000, body);
    bytes.replace(0, 6, QByteArray("\x7f" "ELF\x02\x01", 6));
    bytes.replace(8, 3, QByteArray("AI\x02", 3));
    bytes[18] = 62; bytes[19] = 0;
    return bytes;
}
QByteArray dualManifest(const QByteArray& payload)
{
    auto object = QJsonDocument::fromJson(manifest()).object();
    QJsonObject rpm=object; rpm.remove("version");
    return QJsonDocument(QJsonObject{{"version", "0.2.2"}, {"rpm",rpm},
        {"appimage",QJsonObject{{"url","https://updates.example/downloads/Vulkana-0.2.2-x86_64.AppImage"},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex())}}}}).toJson();
}
void appImageTests()
{
    const auto old=appImageBytes('a'), replacement=appImageBytes('b');
    const auto metadata=dualManifest(replacement);
    QString error;
    const auto rpm=u::UpdateService::parseManifest(metadata,error,u::UpdatePackage::Rpm);
    const auto image=u::UpdateService::parseManifest(metadata,error,u::UpdatePackage::AppImage);
    CHECK(rpm&&image&&rpm->url!=image->url);
    CHECK(image->url.fileName().endsWith(".AppImage"));
    CHECK(!u::UpdateService::parseManifest(manifest(),error,u::UpdatePackage::AppImage));
    CHECK(!u::UpdateService::isAppImageHeader(package));
    QTemporaryDir files;
    const auto path=files.filePath(QString::fromUtf8("Vulkana 画像.AppImage"));
    auto seed=[&](const QByteArray& bytes) {
        QCoreApplication::instance()->setProperty("vulkanaPendingRestart",QVariant());
        QFile file(path); CHECK(file.open(QIODevice::WriteOnly));CHECK(file.write(bytes)==bytes.size());
    };
    auto ready=[&](u::UpdateService& service,Network& network,const QByteArray& data=QByteArray()) {
        network.responses.push_back({data.isEmpty()?metadata:data});service.check();
        CHECK(waitFor([&]{return !service.busy();}));CHECK(service.state()==State::Available);
    };
    for(int failure=0;failure<5;++failure) {
        seed(old);Network network;
        u::UpdateService service("0.2.1",nullptr,&network,u::UpdatePackage::AppImage,path);
        ready(service,network,failure==3?dualManifest(package):QByteArray());
        network.responses.push_back({failure==0?QByteArray("bad"):(failure==3?package:replacement),200,
            failure==1?QNetworkReply::RemoteHostClosedError:QNetworkReply::NoError,failure==2||failure==4});
        service.downloadAppImage();
        CHECK(readFile(path)==old);
        if(failure==2){QTest::qWait(10);service.cancel();}
        if(failure==4){QTest::qWait(10);seed(replacement);service.cancel();}
        CHECK(waitFor([&]{return !service.busy();}));
        CHECK(service.state()==(failure==2||failure==4?State::Available:State::Error));
        CHECK(readFile(path)==(failure==4?replacement:old));
    }
    seed(old);
    {
        Network network;u::UpdateService service("0.2.1",nullptr,&network,u::UpdatePackage::AppImage,path);
        ready(service,network);
        QLockFile lock(path+".update.lock");CHECK(lock.tryLock());service.downloadAppImage();
        CHECK(service.state()==State::Error&&readFile(path)==old);
    }
    {
        Network network;u::UpdateService service("0.2.1",nullptr,&network,u::UpdatePackage::AppImage,path);
        ready(service,network);network.responses.push_back({replacement});
        // Simulates replacement by another process before the last streamed chunk.
        bool changed=false;
        service.onProgress=[&](qint64,qint64){if(!changed){changed=true;seed(old+"changed");}};
        service.downloadAppImage();CHECK(waitFor([&]{return !service.busy();}));
        CHECK(service.state()==State::Error&&service.message().contains("changed"));
        CHECK(readFile(path)==old+"changed");
    }
    seed(old);
    {
        Network network;u::UpdateService service("0.2.1",nullptr,&network,u::UpdatePackage::AppImage,path);
        ready(service,network);network.responses.push_back({replacement});service.downloadAppImage();
        CHECK(waitFor([&]{return !service.busy();}));
        CHECK(service.state()==State::RestartReady&&readFile(path)==replacement);
        CHECK(QFileInfo(path).isExecutable());
        QWidget host;u::AboutDialog dialog(&host,&network,u::UpdatePackage::AppImage,path);
        auto* button=dialog.findChild<QPushButton*>("AboutCheckUpdate");CHECK(button&&button->text()=="Restart Now");
        bool restart=false;dialog.onRestartRequested=[&](const QString& value){restart=value==path;};
        if(button)button->click();CHECK(restart);
    }
    QCoreApplication::instance()->setProperty("vulkanaPendingRestart",QVariant());
    {
        Network network;u::UpdateService service("0.2.1",nullptr,&network,u::UpdatePackage::AppImage,files.filePath("missing"));
        ready(service,network);service.downloadAppImage();CHECK(service.state()==State::Error);
        CHECK(network.requests.size()==1);
    }
}

#include "StartupUpdateChecks.inc"

void liveCheck(bool download)
{
    QTemporaryDir directory;
    const auto kind = u::UpdateService::buildPackage();
    const auto path = directory.filePath(kind == u::UpdatePackage::AppImage
        ? QStringLiteral("verified-update.AppImage") : QStringLiteral("verified-update.rpm"));
    if (download && kind == u::UpdatePackage::AppImage) {
        // Exercise replacement only on a disposable fixture, never APPIMAGE
        // from the test runner's environment. The downloaded image is not run.
        QFile fixture(path);
        CHECK(fixture.open(QIODevice::WriteOnly));
        fixture.write(appImageBytes('a'));
    }
    u::UpdateService service(download ? QStringLiteral("0.0.0") : QCoreApplication::applicationVersion(),
        nullptr, nullptr, kind, path);
    service.check();
    CHECK(waitFor([&] { return !service.busy(); }, 65000));
    std::cout << service.message().toStdString() << '\n';
    CHECK(service.state() == State::Current || service.state() == State::Available);
    if (download && service.release()) {
        if (kind == u::UpdatePackage::AppImage) service.downloadAppImage();
        else service.download(path);
        CHECK(waitFor([&] { return !service.busy(); }, 90000));
        std::cout << service.message().toStdString() << '\n';
        CHECK(service.state() == (kind == u::UpdatePackage::AppImage ? State::RestartReady : State::Downloaded));
        CHECK(QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256).toHex() == service.release()->sha256);
        std::cout << "Verified temporary download SHA-256: " << service.release()->sha256.constData() << '\n';
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(":/Vulkana512.png")));
    app.setApplicationVersion(QStringLiteral(VULKANA_TEST_VERSION));
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("Updates");
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    if (app.arguments().contains("--live-check") || app.arguments().contains("--live-download"))
        liveCheck(app.arguments().contains("--live-download"));
    else { TestServiceConfig services; serviceConfigurationTests(); metadataTests(); stateAndDownloadTests(); dialogTests(); appImageTests();
        startupPolicyTests(); startupPresentationTests(); startupSilentTests(); startupCloseTests(); }
    std::cout << "Update checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
