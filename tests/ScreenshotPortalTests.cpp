#include "imageeditor/ui/ScreenCapture.hpp"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>

// Isolated dbus-run-session fixture: never invokes the desktop's real chooser.
class Request final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    int closed{};
public slots:
    void Close(){++closed;}
};
class Portal final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
public:
    QString path,parent; QVariantMap options; Request request; int calls{};
    bool alternate{},early{};
    QString uri;
    void respond(uint code,QVariantMap values={},QString responsePath={}) {
        auto message=QDBusMessage::createSignal(responsePath.isEmpty()?path:responsePath,
            "org.freedesktop.portal.Request","Response");
        message<<code<<values;QDBusConnection::sessionBus().send(message);
    }
public slots:
    QDBusObjectPath Screenshot(const QString& window,const QVariantMap& values,const QDBusMessage& message) {
        parent=window;options=values;
        auto sender=message.service();sender.remove(0,1);sender.replace('.','_');
        path="/org/freedesktop/portal/desktop/request/"+sender+'/'+options.value("handle_token").toString();
        if(alternate)path+="_alternate";
        QDBusConnection::sessionBus().registerObject(path,&request,QDBusConnection::ExportAllSlots);
        ++calls;
        if(early)respond(0,{{"uri",uri}});
        return QDBusObjectPath(path);
    }
};
namespace {
int failures{};
#define CHECK(...) do{if(!(__VA_ARGS__)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #__VA_ARGS__ "\n";}}while(false)
template<class F>bool waitFor(F f){QElapsedTimer timer;timer.start();while(!f()&&timer.elapsed()<1500)QTest::qWait(5);return f();}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    QTemporaryDir files;QImage original(39,27,QImage::Format_RGBA8888);original.fill(QColor(23,68,201,255));
    const auto filename=files.filePath("image with spaces.png");CHECK(original.save(filename));
    const auto url=QUrl::fromLocalFile(filename);
    using imageeditor::ui::readScreenCapture;
    CHECK(readScreenCapture(url).image.convertToFormat(original.format())==original);
    for(const auto& invalid:{QUrl("https://example.invalid/test.png"),QUrl("file://remote/test.png"),
        QUrl("file:relative.png"),QUrl(url.toString()+"?query"),QUrl(url.toString()+"#fragment"),
        QUrl::fromLocalFile(files.path()),QUrl::fromLocalFile(files.filePath("missing.png"))}) {
        const auto result=readScreenCapture(invalid);CHECK(result.image.isNull()&&!result.error.isEmpty());
    }
    QFile corrupt(files.filePath("broken.png"));CHECK(corrupt.open(QIODevice::WriteOnly));corrupt.write("not an image");corrupt.close();
    CHECK(!readScreenCapture(QUrl::fromLocalFile(corrupt.fileName())).error.isEmpty());
    // A tiny header must be rejected before allocating an oversized raster.
    QFile huge(files.filePath("oversized.ppm"));CHECK(huge.open(QIODevice::WriteOnly));huge.write("P6\n20000 20000\n255\n");huge.close();
    CHECK(!readScreenCapture(QUrl::fromLocalFile(huge.fileName())).error.isEmpty());
    auto bus=QDBusConnection::sessionBus();Portal portal;portal.uri=url.toString(QUrl::FullyEncoded);
    if(!bus.registerService("org.freedesktop.portal.Desktop")||!bus.registerObject("/org/freedesktop/portal/desktop",&portal,QDBusConnection::ExportAllSlots))return 2;
    imageeditor::ui::ScreenCapture capture;
    int completions{};quint64 received{},id{};QImage image;QString error;bool cancelled{};
    QObject::connect(&capture,&imageeditor::ui::ScreenCapture::finished,&app,[&](quint64 request,QImage pixels,QString message,bool cancel){
        ++completions;received=request;image=pixels.convertToFormat(original.format());error=message;cancelled=cancel;
    });
    const auto start=[&]{const int before=portal.calls;capture.start(++id,imageeditor::ui::ScreenCapture::Target::DesktopChooser);
        CHECK(waitFor([&]{return portal.calls==before+1;}));CHECK(portal.parent.isEmpty());
        CHECK(portal.options.value("interactive").toBool()&&portal.options.value("modal").toBool());
        QTest::qWait(10);
    };
    start();portal.respond(0,{{"uri",portal.uri}});CHECK(waitFor([&]{return received==id;}));
    CHECK(image==original&&error.isEmpty()&&!cancelled&&QFile::exists(filename));
    start();portal.respond(1);CHECK(waitFor([&]{return received==id;}));CHECK(cancelled&&image.isNull()&&error.isEmpty());
    for(const auto& values:{QVariantMap{},QVariantMap{{"uri",42}},QVariantMap{{"uri","https://example.invalid/test.png"}}}) {
        start();portal.respond(0,values);CHECK(waitFor([&]{return received==id;}));CHECK(!error.isEmpty()&&image.isNull()&&!cancelled);
    }
    start();const auto obsolete=portal.path;const auto before=completions;const auto closed=portal.request.closed;capture.cancel();
    CHECK(waitFor([&]{return portal.request.closed==closed+1;}));portal.respond(0,{{"uri",portal.uri}},obsolete);QTest::qWait(30);CHECK(completions==before);
    start();portal.respond(0,{{"uri",portal.uri}},obsolete);QTest::qWait(30);CHECK(completions==before);
    portal.respond(0,{{"uri",portal.uri}});CHECK(waitFor([&]{return received==id;}));CHECK(image==original);
    portal.alternate=true;start();portal.respond(0,{{"uri",portal.uri}});CHECK(waitFor([&]{return received==id;}));CHECK(image==original);portal.alternate=false;
    portal.early=true;start();CHECK(waitFor([&]{return received==id;}));CHECK(image==original);portal.early=false;
    // Closing before the method reply still closes its late returned handle.
    const auto lateClosed=portal.request.closed;capture.start(++id,imageeditor::ui::ScreenCapture::Target::DesktopChooser);capture.cancel();
    CHECK(waitFor([&]{return portal.request.closed>lateClosed;}));
    const auto lateCount=completions;portal.respond(0,{{"uri",portal.uri}});QTest::qWait(30);CHECK(completions==lateCount);
    std::cout<<"Screenshot portal checks: "<<failures<<" failures\n";return failures?1:0;
}
#include "ScreenshotPortalTests.moc"
