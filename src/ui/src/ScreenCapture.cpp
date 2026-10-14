#include "imageeditor/ui/ScreenCapture.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QUuid>
#include <QScopeGuard>
#ifdef Q_OS_LINUX
#include <QDBusMessage>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace imageeditor::ui {
ScreenCaptureResult readScreenCapture(const QUrl& uri)
{
    if (!uri.isValid() || !uri.isLocalFile() || uri.hasQuery() || uri.hasFragment()
        || (!uri.host().isEmpty() && uri.host()!=QStringLiteral("localhost")))
        return {{},QStringLiteral("The desktop returned an invalid screenshot location.")};
    const auto path=uri.toLocalFile();
    const QFileInfo info(path);
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null) || !info.isFile()
        || info.size()<=0 || quint64(info.size())>kMaximumRasterPixels*4+1024*1024)
        return {{},QStringLiteral("The screenshot file is unavailable or too large.")};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {{},file.errorString()};
    QImageReader reader(&file);
    const auto size=reader.size();
    if (!size.isValid() || !rasterExtentWithinLimits(std::uint32_t(size.width()),std::uint32_t(size.height())))
        return {{},QStringLiteral("Screenshot exceeds the 16384-pixel / 40-megapixel image limit.")};
    auto image=reader.read();
    if (image.isNull()) return {{},reader.errorString()};
    if (!rasterExtentWithinLimits(std::uint32_t(image.width()),std::uint32_t(image.height())))
        return {{},QStringLiteral("Screenshot dimensions changed while decoding.")};
    image.setDevicePixelRatio(1); // Stored image pixels, never viewport/DPI units.
    return {std::move(image),{}};
}
namespace {
#ifdef Q_OS_WIN
ScreenCaptureResult nativeCapture(ScreenCapture::Target target)
{
    // Scope physical-pixel coordinates to this operation without changing the
    // application's logical widget/canvas coordinates or global DPI policy.
    const auto previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto restoreDpi=qScopeGuard([&]{if(previous)SetThreadDpiAwarenessContext(previous);});
    const RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN)+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    RECT area=desktop;
    if (target==ScreenCapture::Target::ActiveWindow) {
        const auto window=GetForegroundWindow();
        DWORD pid{}; GetWindowThreadProcessId(window,&pid);
        if (!window || pid==GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window)
            || window==GetDesktopWindow() || window==GetShellWindow())
            return {{},QStringLiteral("Choose another application's visible window during the capture delay.")};
        if (FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&area,sizeof(area)))
            && !GetWindowRect(window,&area))
            return {{},QStringLiteral("The active window's bounds are unavailable.")};
        RECT visible{};
        if (!IntersectRect(&visible,&area,&desktop)) return {{},QStringLiteral("The active window is outside the desktop.")};
        area=visible;
    } else if (target!=ScreenCapture::Target::EntireDesktop) {
        return {{},QStringLiteral("Unsupported native screenshot target.")};
    }
    const auto width=area.right-area.left, height=area.bottom-area.top;
    if (width<=0 || height<=0 || !rasterExtentWithinLimits(std::uint32_t(width),std::uint32_t(height)))
        return {{},QStringLiteral("Screenshot exceeds the 16384-pixel / 40-megapixel image limit.")};
    const auto screen=GetDC(nullptr);
    if (!screen) return {{},QStringLiteral("The Windows desktop is unavailable.")};
    const auto releaseScreen=qScopeGuard([&]{ReleaseDC(nullptr,screen);});
    const auto memory=CreateCompatibleDC(screen);
    if (!memory) return {{},QStringLiteral("Cannot create screenshot context.")};
    const auto releaseMemory=qScopeGuard([&]{DeleteDC(memory);});
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels{};
    const auto bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!bitmap || !pixels) { if(bitmap)DeleteObject(bitmap); return {{},QStringLiteral("Not enough memory for the screenshot.")}; }
    const auto releaseBitmap=qScopeGuard([&]{DeleteObject(bitmap);});
    const auto old=SelectObject(memory,bitmap);
    if (!old || old==HGDI_ERROR) return {{},QStringLiteral("Cannot select screenshot bitmap.")};
    const auto restoreBitmap=qScopeGuard([&]{SelectObject(memory,old);});
    if (!BitBlt(memory,0,0,width,height,screen,area.left,area.top,SRCCOPY|CAPTUREBLT|NOMIRRORBITMAP))
        return {{},QStringLiteral("Windows could not capture this desktop (%1).").arg(GetLastError())};
    GdiFlush();
    // GDI produces BGRX, not meaningful alpha. Copy to an owned opaque image
    // before releasing native resources; no file or clipboard is involved.
    auto* rgba=static_cast<quint32*>(pixels);
    for (qint64 i=0;i<qint64(width)*height;++i) rgba[i]|=0xff000000U;
    auto image=QImage(static_cast<const uchar*>(pixels),int(width),int(height),int(width)*4,QImage::Format_RGB32).copy();
    if(image.isNull())return {{},QStringLiteral("Not enough memory for the screenshot.")};
    return {std::move(image),{}};
}
#endif
#ifdef Q_OS_LINUX
void closeRequest(const QString& path)
{
    if (!path.startsWith(QStringLiteral("/org/freedesktop/portal/desktop/request/"))) return;
    auto close=QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",path,"org.freedesktop.portal.Request","Close");
    QDBusConnection::sessionBus().asyncCall(close,1000);
}
#endif
}
ScreenCapture::ScreenCapture(QObject* parent):QObject(parent)
{
    deadline_.setSingleShot(true);
    connect(&deadline_,&QTimer::timeout,this,[this]{
        const auto id=request_; cancel();
        emit finished(id,{},QStringLiteral("The screenshot request timed out. Please try again."),false);
    });
}
ScreenCapture::~ScreenCapture(){cancel();}
bool ScreenCapture::usesDesktopChooser() const
{
#ifdef Q_OS_WIN
    return false;
#else
    return true;
#endif
}
void ScreenCapture::cancel()
{
    ++generation_; active_=false; deadline_.stop();
#ifdef Q_OS_LINUX
    if(!path_.isEmpty()){unsubscribe();const auto old=path_;path_.clear();closeRequest(old);}
#endif
}
void ScreenCapture::complete(ScreenCaptureResult result)
{
    if(!active_)return;
    const auto id=request_; active_=false; deadline_.stop();
#ifdef Q_OS_LINUX
    unsubscribe();path_.clear();
#endif
    emit finished(id,std::move(result.image),std::move(result.error),result.cancelled);
}
void ScreenCapture::start(quint64 request,Target target)
{
    cancel(); request_=request;active_=true; const auto generation=generation_;
#ifdef Q_OS_LINUX
    if(target!=Target::DesktopChooser){complete({{},QStringLiteral("Use the desktop screenshot chooser on Linux.")});return;}
    const auto bus=QDBusConnection::sessionBus();
    auto sender=bus.baseService();sender.remove(0,1);sender.replace('.','_');
    const auto token="vulkana_capture_"+QUuid::createUuid().toString(QUuid::Id128);
    path_="/org/freedesktop/portal/desktop/request/"+sender+'/'+token;
    if(!bus.isConnected()||!subscribe()){complete({{},QStringLiteral("The desktop screenshot chooser is unavailable.")});return;}
    auto message=QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop","/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Screenshot","Screenshot");
    // Empty parent is allowed: a Wayland winId is not an exported portal token.
    // v2 interactive mode delegates capabilities/permission to the compositor.
    message<<QString{}<<QVariantMap{{"handle_token",token},{"interactive",true},{"modal",true}};
    deadline_.start(180000);
    auto* watcher=new QDBusPendingCallWatcher(bus.asyncCall(message,5000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,generation](QDBusPendingCallWatcher* watcher){
        const QDBusPendingReply<QDBusObjectPath> reply=*watcher;watcher->deleteLater();
        if(generation_!=generation||!active_){if(!reply.isError())closeRequest(reply.value().path());return;}
        if(reply.isError()){complete({{},QStringLiteral("Desktop screenshot request failed: %1").arg(reply.error().message())});return;}
        const auto returned=reply.value().path();
        if(!returned.startsWith(QStringLiteral("/org/freedesktop/portal/desktop/request/"))){complete({{},QStringLiteral("Invalid desktop screenshot request handle.")});return;}
        if(returned!=path_){unsubscribe();path_=returned;if(!subscribe())complete({{},QStringLiteral("Cannot receive the screenshot response.")});}
    });
#elif defined(Q_OS_WIN)
    QTimer::singleShot(0,this,[this,generation,target]{
        if(active_&&generation_==generation)complete(nativeCapture(target));
    });
#else
    Q_UNUSED(target);Q_UNUSED(generation);
    complete({{},QStringLiteral("Screenshot capture is unavailable on this platform.")});
#endif
}
#ifdef Q_OS_LINUX
bool ScreenCapture::subscribe()
{
    return QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop",path_,"org.freedesktop.portal.Request",
        "Response",this,SLOT(response(uint,QVariantMap,QDBusMessage)));
}
void ScreenCapture::unsubscribe()
{
    if(!path_.isEmpty())QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop",path_,"org.freedesktop.portal.Request",
        "Response",this,SLOT(response(uint,QVariantMap,QDBusMessage)));
}
void ScreenCapture::response(uint code,const QVariantMap& values,const QDBusMessage& message)
{
    if(!active_||path_.isEmpty()||message.path()!=path_)return;
    if(code==1){complete({{},{},true});return;}
    if(code!=0 || values.value("uri").metaType()!=QMetaType::fromType<QString>()) {
        complete({{},QStringLiteral("The desktop did not return a screenshot.")});return;
    }
    complete(readScreenCapture(QUrl(values.value("uri").toString(),QUrl::StrictMode)));
}
#endif
}
