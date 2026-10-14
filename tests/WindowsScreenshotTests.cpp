#include "imageeditor/ui/ScreenCapture.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <windows.h>
#include <iostream>

// Opt-in interactive desktop test. Capture only the fixture's own visible
// window, never the rest of the user's desktop, clipboard or other windows.
int main(int argc,char** argv)
{
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    const auto args=app.arguments();
    if(args.size()>2&&args[1]=="--fixture") {
        QWidget view(nullptr,Qt::Window|Qt::FramelessWindowHint);
        view.setWindowTitle(args[2]);view.setGeometry(100,100,360,240);
        view.setStyleSheet("background: rgb(35,69,201);");view.show();view.raise();view.activateWindow();
        QTimer::singleShot(15000,&app,&QApplication::quit);return app.exec();
    }
    if(!args.contains("--native")){std::cerr<<"Use --native on an interactive Windows desktop.\n";return 2;}
    const auto title=QStringLiteral("Vulkana capture fixture %1").arg(QCoreApplication::applicationPid());
    QProcess fixture;fixture.start(QCoreApplication::applicationFilePath(),{"--fixture",title});
    if(!fixture.waitForStarted(3000))return 3;
    const auto stop=qScopeGuard([&]{fixture.kill();fixture.waitForFinished(3000);});
    QElapsedTimer wait;wait.start();HWND window{};
    while(!window&&wait.elapsed()<5000){window=FindWindowW(nullptr,reinterpret_cast<const wchar_t*>(title.utf16()));QTest::qWait(25);}
    if(!window){std::cerr<<"Fixture window unavailable\n";return 4;}
    ShowWindow(window,SW_SHOW);SetForegroundWindow(window);QTest::qWait(500);
    if(GetForegroundWindow()!=window){std::cerr<<"Fixture could not become foreground; manual test needed\n";return 5;}
    imageeditor::ui::ScreenCapture capture;bool done{};QImage image;QString error;
    QObject::connect(&capture,&imageeditor::ui::ScreenCapture::finished,&app,[&](quint64 id,QImage pixels,QString message,bool cancelled){
        done=id==1&&!cancelled;image=pixels;error=message;
    });
    capture.start(1,imageeditor::ui::ScreenCapture::Target::ActiveWindow);
    wait.restart();while(!done&&wait.elapsed()<5000)QTest::qWait(5);
    if(!done||image.isNull()){std::cerr<<"Capture failed: "<<error.toStdString()<<'\n';return 6;}
    RECT rect{};GetWindowRect(window,&rect);
    if(image.size()!=QSize(rect.right-rect.left,rect.bottom-rect.top)){std::cerr<<"Physical dimensions differ\n";return 7;}
    // Stay clear of possible platform decorations and antialiased corners.
    for(int y=image.height()/4;y<image.height()*3/4;++y)for(int x=image.width()/4;x<image.width()*3/4;++x)
        if(image.pixelColor(x,y)!=QColor(35,69,201,255)){std::cerr<<"Unexpected visible fixture pixels\n";return 8;}
    std::cout<<"Windows native capture: physical bounds, RGB and opaque alpha passed ("<<image.width()<<'x'<<image.height()<<")\n";
    return 0;
}
