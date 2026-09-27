#include "imageeditor/render/CanvasWindow.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QTimer>
#include <QWindow>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void sendResize(imageeditor::render::CanvasWindow& window, int width)
{
    const QSize oldSize {width - 1, 500};
    const QSize newSize {width, 500};
    QResizeEvent event(newSize, oldSize);
    QCoreApplication::sendEvent(&window, &event);
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    imageeditor::render::CanvasWindow window;

    const auto baseline = window.rendererStats();
    for (int width = 600; width < 700; ++width) {
        sendResize(window, width);
    }

    const auto duringBurst = window.rendererStats();
    require(duringBurst.deferredResizeEvents == baseline.deferredResizeEvents + 100,
        "every resize event must be recorded as deferred");
    require(duringBurst.resizeCommits == baseline.resizeCommits,
        "a resize burst must not synchronously commit Vulkan resources");

    // A late event inside the settle interval must restart it rather than
    // allowing an intermediate Vulkan rebuild.
    QTimer::singleShot(25, &window, [&window] { sendResize(window, 701); });
    QEventLoop settleLoop;
    QTimer::singleShot(140, &settleLoop, &QEventLoop::quit);
    settleLoop.exec();

    const auto afterBurst = window.rendererStats();
    require(afterBurst.deferredResizeEvents == baseline.deferredResizeEvents + 101,
        "late resize event was not observed");
    require(afterBurst.resizeCommits == baseline.resizeCommits + 1,
        "one resize burst must produce exactly one final commit");

    sendResize(window, 720);
    QEventLoop secondSettleLoop;
    QTimer::singleShot(100, &secondSettleLoop, &QEventLoop::quit);
    secondSettleLoop.exec();
    require(window.rendererStats().resizeCommits == baseline.resizeCommits + 2,
        "a later independent resize must produce its own final commit");

    QWindow nativeHost;
    nativeHost.resize(800, 600);
    nativeHost.show();
    auto* hostedCanvas = new imageeditor::render::CanvasWindow;
    hostedCanvas->setParent(&nativeHost);
    hostedCanvas->resize(800, 600);
    hostedCanvas->show();
    QCoreApplication::processEvents();
    const auto hostedBaseline = hostedCanvas->rendererStats();

    hostedCanvas->prepareForHostResize();
    require(hostedCanvas->presentationSuppressedForResize(),
        "host resize must suppress the old canvas buffer");
    nativeHost.hide();
    QEventLoop hiddenSettleLoop;
    QTimer::singleShot(80, &hiddenSettleLoop, &QEventLoop::quit);
    hiddenSettleLoop.exec();
    require(hostedCanvas->presentationSuppressedForResize(),
        "a hidden parent must defer presentation recovery");

    nativeHost.show();
    QCoreApplication::processEvents();
    hostedCanvas->setHostPresentationAvailable(true);
    require(!hostedCanvas->presentationSuppressedForResize(),
        "an exposed parent must complete presentation recovery");
    require(hostedCanvas->isVisible(),
        "canvas must be visible after its host returns");
    const auto hostedRestored = hostedCanvas->rendererStats();
    require(hostedRestored.resizePresentationSuspends
            == hostedBaseline.resizePresentationSuspends + 1,
        "hidden resize must record one presentation suspension");
    require(hostedRestored.resizePresentationResumes
            == hostedBaseline.resizePresentationResumes + 1,
        "host restoration must record one presentation resume");

    std::cout << "Canvas resize coalescing tests passed\n";
    return EXIT_SUCCESS;
}
