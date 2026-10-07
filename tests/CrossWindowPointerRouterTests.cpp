#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ColorPicker.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QMainWindow>
#include <QMimeData>
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWidget>
#include <QWindow>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

class RecordingWidget final : public QWidget {
public:
    using QWidget::QWidget;

    int presses {0};
    int moves {0};
    int releases {0};
    QPointF lastPosition;
    Qt::MouseButtons lastButtons {Qt::NoButton};
    std::function<void()> onMove;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        ++presses;
        lastPosition = event->position();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        ++moves;
        lastPosition = event->position();
        lastButtons = event->buttons();
        if (onMove) {
            auto callback = std::move(onMove);
            callback();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        ++releases;
        lastPosition = event->position();
        event->accept();
    }
};

class RecordingWindow final : public QWindow {
public:
    int presses {0};
    int moves {0};
    int releases {0};
    int ungrabs {0};
    Qt::MouseButtons lastButtons {Qt::NoButton};

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        ++presses;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        ++moves;
        lastButtons = event->buttons();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        ++releases;
        event->accept();
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::UngrabMouse) {
            ++ungrabs;
        }
        return QWindow::event(event);
    }
};

class TestSlider final : public QSlider {
public:
    using QSlider::QSlider;

    [[nodiscard]] QRect handleRect() const
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        return style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    }
};

class ExplicitGrabWidget final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        grabMouse();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        // Deliberately leave the grab active. The central cancellation path is
        // responsible for unwinding a grab a receiver failed to release.
        event->accept();
    }
};

void sendMouse(QWindow* receiver, QEvent::Type type, const QPointF& global,
    Qt::MouseButton button, Qt::MouseButtons buttons)
{
    const auto local = receiver->mapFromGlobal(global);
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(receiver, &event);
}

void sendMouseDirect(QWidget* receiver, QEvent::Type type, const QPointF& global,
    Qt::MouseButton button, Qt::MouseButtons buttons)
{
    const auto local = receiver->mapFromGlobal(global);
    QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(receiver, &event);
}

void prepareWindow(QWidget& root)
{
    root.setGeometry(100, 100, 640, 360);
    root.show();
    QCoreApplication::processEvents();
    CHECK(root.windowHandle() != nullptr);
}

void widgetDragContinuesAcrossNativeWindow()
{
    QWidget root;
    root.setObjectName(QStringLiteral("TestRoot"));
    RecordingWidget control(&root);
    control.setObjectName(QStringLiteral("DragControl"));
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    native.setObjectName(QStringLiteral("NativeTarget"));
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto pressGlobal = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(control.presses == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::Widget);
    CHECK(router.captureOwner() == &control);

    const auto moveGlobal = control.mapToGlobal(QPointF {230.0, 20.0});
    sendMouse(&native, QEvent::MouseMove, moveGlobal,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 1);
    CHECK(control.lastPosition.x() > control.width());
    CHECK(control.lastButtons == Qt::LeftButton);
    CHECK(native.moves == 0);
    CHECK(router.repairedButtonStateCount() == 1);

    sendMouse(&native, QEvent::MouseButtonRelease, moveGlobal,
        Qt::LeftButton, Qt::NoButton);
    CHECK(control.releases == 1);
    CHECK(native.releases == 0);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(router.routedEventCount() == 2);
}

void colorPickerDragRemainsOwnedAcrossNativeCanvas()
{
    QWidget root;
    imageeditor::ui::ColorPicker picker(&root);
    picker.setGeometry(20, 20, 300, 300);
    prepareWindow(root);
    auto* plane = picker.findChild<QWidget*>("ColorSaturationValue");
    CHECK(plane);
    if (!plane) return;
    picker.setColor(QColor(230, 50, 25, 128));
    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto press = plane->mapToGlobal(plane->rect().center());
    const auto outside = plane->mapToGlobal(QPointF(plane->width() + 80, plane->height() + 80));
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    CHECK(router.captureOwner() == plane);
    sendMouse(&native, QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
    sendMouse(&native, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    CHECK(picker.color() == QColor(0, 0, 0, 128));
    CHECK(native.presses == 0 && native.moves == 0 && native.releases == 0);
    CHECK(router.captureDomain() == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    CHECK(picker.color() == QColor(0, 0, 0, 128));
    CHECK(router.captureDomain() == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void widgetDragSurvivesButtonlessReturnToWidgetWindow()
{
    QWidget root;
    root.setObjectName(QStringLiteral("RoundTripRoot"));
    RecordingWidget control(&root);
    control.setObjectName(QStringLiteral("RoundTripControl"));
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    native.setObjectName(QStringLiteral("RoundTripNativeTarget"));
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto pressGlobal = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(router.captureOwner() == &control);

    const auto nativeGlobal = control.mapToGlobal(QPointF {260.0, 20.0});
    sendMouse(&native, QEvent::MouseMove, nativeGlobal,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 1);
    CHECK(control.lastButtons == Qt::LeftButton);

    // A real KDE/Wayland trace showed the next move returning to the original
    // QWidgetWindow with buttons == NoButton before the physical release. It
    // remains part of the same press sequence and must not cancel the control.
    const auto returnedGlobal = control.mapToGlobal(QPointF {240.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseMove, returnedGlobal,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 2);
    CHECK(control.lastButtons == Qt::LeftButton);
    CHECK(router.captureOwner() == &control);
    CHECK(router.repairedButtonStateCount() == 2);

    sendMouse(root.windowHandle(), QEvent::MouseButtonRelease, returnedGlobal,
        Qt::LeftButton, Qt::NoButton);
    CHECK(control.releases == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(router.routedEventCount() == 2);
}

void nativeContainerWidgetWindowRemainsInCaptureScope()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    QWidget nativeContainer(&root);
    nativeContainer.setAttribute(Qt::WA_NativeWindow);
    nativeContainer.setGeometry(240, 20, 300, 220);
    nativeContainer.show();
    prepareWindow(root);
    QCoreApplication::processEvents();
    CHECK(nativeContainer.windowHandle() != nullptr);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(
        &root, &native, &nativeContainer);

    const auto pressGlobal = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    const auto overContainer = nativeContainer.mapToGlobal(QPointF {80.0, 80.0});
    sendMouse(nativeContainer.windowHandle(), QEvent::MouseMove, overContainer,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 1);
    CHECK(control.lastButtons == Qt::LeftButton);
    CHECK(router.captureOwner() == &control);

    sendMouse(nativeContainer.windowHandle(), QEvent::MouseButtonRelease, overContainer,
        Qt::LeftButton, Qt::NoButton);
    CHECK(control.releases == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void nativeCanvasDoesNotPromoteWidgetSiblings()
{
    QWidget root;
    QWidget canvasContainer(&root);
    QWidget ordinaryPanel(&root);
    canvasContainer.setAttribute(Qt::WA_NativeWindow);
    canvasContainer.setGeometry(0, 0, 320, 240);
    ordinaryPanel.setGeometry(320, 0, 160, 240);
    canvasContainer.show();
    ordinaryPanel.show();
    prepareWindow(root);
    QCoreApplication::processEvents();

    CHECK(QCoreApplication::testAttribute(
        Qt::AA_DontCreateNativeWidgetSiblings));
    CHECK(canvasContainer.testAttribute(Qt::WA_NativeWindow));
    CHECK(canvasContainer.internalWinId() != 0);
    CHECK(canvasContainer.windowHandle() != nullptr);
    CHECK(!ordinaryPanel.testAttribute(Qt::WA_NativeWindow));
    CHECK(ordinaryPanel.internalWinId() == 0);
    CHECK(ordinaryPanel.windowHandle() == nullptr);
}

void widgetInsideNativeOverlayRetainsItsDispatchWindow()
{
    QWidget root;
    root.setObjectName(QStringLiteral("OverlayRoot"));
    QWidget overlay(&root);
    overlay.setObjectName(QStringLiteral("NativePanelOverlay"));
    overlay.setAttribute(Qt::WA_NativeWindow);
    overlay.setGeometry(260, 20, 260, 220);
    RecordingWidget control(&overlay);
    control.setObjectName(QStringLiteral("OverlayResizeHandle"));
    control.setGeometry(10, 20, 40, 160);
    control.show();
    overlay.show();
    prepareWindow(root);
    QCoreApplication::processEvents();
    CHECK(overlay.windowHandle() != nullptr);

    RecordingWindow native;
    native.setObjectName(QStringLiteral("OverlayCanvasTarget"));
    imageeditor::ui::CrossWindowPointerRouter router(
        &root, &native, nullptr);

    const auto pressGlobal = control.mapToGlobal(QPointF {20.0, 30.0});
    sendMouse(overlay.windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(control.presses == 1);
    CHECK(router.captureOwner() == &control);

    const auto canvasGlobal = control.mapToGlobal(QPointF {-180.0, 30.0});
    sendMouse(&native, QEvent::MouseMove, canvasGlobal,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 1);
    CHECK(control.lastButtons == Qt::LeftButton);

    sendMouse(&native, QEvent::MouseButtonRelease, canvasGlobal,
        Qt::LeftButton, Qt::NoButton);
    CHECK(control.releases == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void sliderUsesNormalQtDragSemanticsAcrossNativeWindow()
{
    QWidget root;
    TestSlider slider(Qt::Horizontal, &root);
    slider.setObjectName(QStringLiteral("TestSlider"));
    slider.setRange(0, 100);
    slider.setValue(20);
    slider.setGeometry(20, 20, 260, 32);
    slider.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto pressGlobal = slider.mapToGlobal(slider.handleRect().center());
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(slider.isSliderDown());

    const auto farRight = slider.mapToGlobal(QPointF {
        static_cast<double>(slider.width() - 2),
        static_cast<double>(slider.height()) / 2.0,
    });
    sendMouse(&native, QEvent::MouseMove, farRight,
        Qt::NoButton, Qt::LeftButton);
    CHECK(slider.value() >= 95);

    sendMouse(&native, QEvent::MouseButtonRelease, farRight,
        Qt::LeftButton, Qt::NoButton);
    CHECK(!slider.isSliderDown());
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void compactValueDragContinuesAcrossNativeWindow()
{
    QWidget root;
    imageeditor::ui::CompactValueControl control(&root);
    control.setRange(0.0, 100.0);
    control.setDecimals(0);
    control.setGeometry(20, 20, 220, 32);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto field = control.valueFieldRect();
    const auto press = control.mapToGlobal(QPointF {
        static_cast<double>(field.left() + field.width() / 4),
        static_cast<double>(field.center().y()),
    });
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, press,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(control.isSliding());
    CHECK(control.value() >= 20.0 && control.value() <= 30.0);
    CHECK(router.captureOwner() == &control);

    const auto beyond = control.mapToGlobal(QPointF {
        static_cast<double>(control.width() + 200),
        static_cast<double>(field.center().y()),
    });
    sendMouse(&native, QEvent::MouseMove, beyond,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.value() == 100.0);
    CHECK(control.isSliding());
    sendMouse(&native, QEvent::MouseButtonRelease, beyond,
        Qt::LeftButton, Qt::NoButton);
    CHECK(!control.isSliding());
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void dockSeparatorUsesNormalQtDragSemanticsAcrossNativeWindow()
{
    QMainWindow root;
    root.setObjectName(QStringLiteral("DockTestRoot"));
    auto* central = new QWidget;
    central->setMinimumSize(280, 200);
    root.setCentralWidget(central);
    auto* dock = new QDockWidget(QStringLiteral("Test dock"), &root);
    dock->setMinimumWidth(120);
    dock->setWidget(new QWidget);
    root.addDockWidget(Qt::RightDockWidgetArea, dock);
    prepareWindow(root);
    QCoreApplication::processEvents();

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const int widthBefore = dock->width();
    const int separatorX = (central->geometry().right() + dock->geometry().left()) / 2;
    const QPointF pressLocal {
        static_cast<double>(separatorX),
        static_cast<double>(dock->geometry().center().y()),
    };
    const auto pressGlobal = root.mapToGlobal(pressLocal);
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::Widget);

    const auto moveGlobal = pressGlobal - QPointF {90.0, 0.0};
    sendMouse(&native, QEvent::MouseMove, moveGlobal,
        Qt::NoButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    sendMouse(&native, QEvent::MouseButtonRelease, moveGlobal,
        Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();

    CHECK(dock->width() > widthBefore);
    CHECK(router.routedEventCount() == 2);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void floatingDockRemainsInTheSameCaptureDomain()
{
    QMainWindow root;
    root.setCentralWidget(new QWidget);
    auto* dock = new QDockWidget(QStringLiteral("Floating test"), &root);
    auto* slider = new TestSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(20);
    dock->setWidget(slider);
    root.addDockWidget(Qt::RightDockWidgetArea, dock);
    prepareWindow(root);
    dock->setFloating(true);
    dock->resize(300, 100);
    QCoreApplication::processEvents();
    CHECK(dock->windowHandle() != nullptr);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto pressGlobal = slider->mapToGlobal(slider->handleRect().center());
    sendMouse(dock->windowHandle(), QEvent::MouseButtonPress, pressGlobal,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(slider->isSliderDown());
    CHECK(router.captureOwner() == slider);

    const auto target = slider->mapToGlobal(QPointF {
        static_cast<double>(slider->width() - 2),
        static_cast<double>(slider->height()) / 2.0,
    });
    // A floating dock has its own QWidgetWindow. Route through the original
    // dock window even if a compositor reports the move on the main window.
    sendMouse(root.windowHandle(), QEvent::MouseMove, target,
        Qt::NoButton, Qt::LeftButton);
    CHECK(slider->value() >= 95);
    sendMouse(&native, QEvent::MouseMove, target,
        Qt::NoButton, Qt::LeftButton);
    sendMouse(&native, QEvent::MouseButtonRelease, target,
        Qt::LeftButton, Qt::NoButton);
    CHECK(slider->value() >= 95);
    CHECK(!slider->isSliderDown());

    sendMouse(&native, QEvent::MouseButtonPress, target,
        Qt::MiddleButton, Qt::MiddleButton);
    const int nativeMovesBefore = native.moves;
    sendMouse(dock->windowHandle(), QEvent::MouseMove, pressGlobal,
        Qt::NoButton, Qt::MiddleButton);
    sendMouse(dock->windowHandle(), QEvent::MouseButtonRelease, pressGlobal,
        Qt::MiddleButton, Qt::NoButton);
    CHECK(native.moves == nativeMovesBefore + 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);

    // Floating docks are legitimate top-level native windows. Redocking must
    // demote them back into the main QWidget backing store; otherwise their
    // stale Wayland buffer can overhang the application during a later resize.
    dock->setFloating(false);
    QCoreApplication::processEvents();
    CHECK(!dock->isFloating());
    CHECK(!dock->testAttribute(Qt::WA_NativeWindow));
    CHECK(dock->internalWinId() == 0);
    CHECK(dock->windowHandle() == nullptr);
}

void nativeDragContinuesAcrossWidgetWindow()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    native.setObjectName(QStringLiteral("CanvasLikeWindow"));
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto pressGlobal = root.mapToGlobal(QPointF {320.0, 160.0});
    sendMouse(&native, QEvent::MouseButtonPress, pressGlobal,
        Qt::MiddleButton, Qt::MiddleButton);
    CHECK(native.presses == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);

    const auto overControl = control.mapToGlobal(QPointF {30.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseMove, overControl,
        Qt::NoButton, Qt::MiddleButton);
    CHECK(native.moves == 1);
    CHECK(control.moves == 0);

    sendMouse(root.windowHandle(), QEvent::MouseButtonRelease, overControl,
        Qt::MiddleButton, Qt::NoButton);
    CHECK(native.releases == 1);
    CHECK(control.releases == 0);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void nativeDragSurvivesButtonlessReturnToNativeWindow()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    native.setObjectName(QStringLiteral("RoundTripCanvasLikeWindow"));
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto start = root.mapToGlobal(QPointF {320.0, 160.0});
    sendMouse(&native, QEvent::MouseButtonPress, start,
        Qt::MiddleButton, Qt::MiddleButton);
    CHECK(router.captureOwner() == &native);

    const auto overControl = control.mapToGlobal(QPointF {30.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseMove, overControl,
        Qt::NoButton, Qt::NoButton);
    CHECK(native.moves == 1);
    CHECK(native.lastButtons == Qt::MiddleButton);

    // The return event may be delivered directly to the original native
    // window with its button state stripped. Re-dispatching it to that same
    // QWindow must produce one move, not recurse or duplicate delivery.
    const auto returned = root.mapToGlobal(QPointF {340.0, 160.0});
    sendMouse(&native, QEvent::MouseMove, returned,
        Qt::NoButton, Qt::NoButton);
    CHECK(native.moves == 2);
    CHECK(native.lastButtons == Qt::MiddleButton);
    CHECK(router.repairedButtonStateCount() == 2);
    CHECK(router.routedEventCount() == 2);

    // A button-bearing move already delivered to the capture window follows
    // the ordinary path and must likewise be observed exactly once.
    sendMouse(&native, QEvent::MouseMove, returned,
        Qt::NoButton, Qt::MiddleButton);
    CHECK(native.moves == 3);
    CHECK(router.routedEventCount() == 2);

    sendMouse(&native, QEvent::MouseButtonRelease, returned,
        Qt::MiddleButton, Qt::NoButton);
    CHECK(native.releases == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void unrelatedTopLevelMoveIsNotCaptured()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWidget unrelated;
    unrelated.setGeometry(800, 100, 180, 80);
    unrelated.setMouseTracking(true);
    prepareWindow(unrelated);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto start = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, start,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(router.captureOwner() == &control);

    const auto unrelatedPoint = unrelated.mapToGlobal(QPointF {20.0, 20.0});
    sendMouseDirect(&unrelated, QEvent::MouseMove, unrelatedPoint,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 0);
    CHECK(unrelated.moves == 1);
    CHECK(router.repairedButtonStateCount() == 0);
    CHECK(router.routedEventCount() == 0);
    CHECK(router.captureOwner() == &control);

    // The unrelated buttonless move must not clear the capture's press ledger:
    // a subsequent in-scope seam move still needs button-state repair.
    const auto canvasPoint = control.mapToGlobal(QPointF {260.0, 20.0});
    sendMouse(&native, QEvent::MouseMove, canvasPoint,
        Qt::NoButton, Qt::NoButton);
    CHECK(control.moves == 1);
    CHECK(control.lastButtons == Qt::LeftButton);
    CHECK(router.repairedButtonStateCount() == 1);

    sendMouse(&native, QEvent::MouseButtonRelease, canvasPoint,
        Qt::LeftButton, Qt::NoButton);
    CHECK(control.releases == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void platformUngrabAtNativeSeamRetainsWidgetCapture()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto pressPoint = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressPoint,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(router.captureOwner() == &control);

    // KDE/Wayland can report either side of the native-child transition as
    // losing a platform grab. Neither notification is a physical release.
    QEvent widgetUngrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(root.windowHandle(), &widgetUngrab);
    QEvent nativeUngrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&native, &nativeUngrab);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::Widget);
    CHECK(router.captureOwner() == &control);
    CHECK(control.releases == 0);
    CHECK(router.retainedUngrabCount() >= 2);

    const auto outside = control.mapToGlobal(QPointF {260.0, 20.0});
    // The seam can also strip QMouseEvent::buttons from the first native-side
    // move even though no physical release occurred.
    sendMouse(&native, QEvent::MouseMove, outside,
        Qt::NoButton, Qt::NoButton);
    sendMouse(&native, QEvent::MouseButtonRelease, outside,
        Qt::LeftButton, Qt::NoButton);
    CHECK(control.moves == 1);
    CHECK(control.lastButtons == Qt::LeftButton);
    CHECK(control.releases == 1);
    CHECK(router.repairedButtonStateCount() == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void platformUngrabAtNativeSeamKeepsSliderDown()
{
    QWidget root;
    TestSlider slider(Qt::Horizontal, &root);
    slider.setRange(0, 100);
    slider.setValue(20);
    slider.setGeometry(20, 20, 260, 32);
    slider.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto pressPoint = slider.mapToGlobal(slider.handleRect().center());
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, pressPoint,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(slider.isSliderDown());

    QEvent windowUngrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(root.windowHandle(), &windowUngrab);
    QEvent controlUngrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&slider, &controlUngrab);
    CHECK(slider.isSliderDown());

    const auto farRight = slider.mapToGlobal(QPointF {
        static_cast<double>(slider.width() - 2),
        static_cast<double>(slider.height()) / 2.0,
    });
    sendMouse(&native, QEvent::MouseMove, farRight,
        Qt::NoButton, Qt::LeftButton);
    sendMouse(&native, QEvent::MouseButtonRelease, farRight,
        Qt::LeftButton, Qt::NoButton);
    CHECK(slider.value() >= 95);
    CHECK(!slider.isSliderDown());
}

void platformUngrabAtNativeSeamRetainsCanvasCapture()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto start = root.mapToGlobal(QPointF {320.0, 160.0});
    sendMouse(&native, QEvent::MouseButtonPress, start,
        Qt::MiddleButton, Qt::MiddleButton);
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&native, &ungrab);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    CHECK(router.retainedUngrabCount() == 1);
    CHECK(native.ungrabs == 0);

    const auto overControl = control.mapToGlobal(QPointF {30.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseMove, overControl,
        Qt::NoButton, Qt::MiddleButton);
    sendMouse(root.windowHandle(), QEvent::MouseButtonRelease, overControl,
        Qt::MiddleButton, Qt::NoButton);
    CHECK(native.moves == 1);
    CHECK(native.releases == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void qtDragAndDropTakesOverWithoutSyntheticRelease()
{
    QWidget root;
    root.setAcceptDrops(true);
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto point = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, point,
        Qt::LeftButton, Qt::LeftButton);

    // QDrag emits UngrabMouse as it takes over. Preserve capture through that
    // ambiguous event, then relinquish bookkeeping on the first definitive DnD
    // event without manufacturing a MouseButtonRelease into the source widget.
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(root.windowHandle(), &ungrab);
    CHECK(router.captureOwner() == &control);

    QMimeData mimeData;
    QDragEnterEvent enter(QPoint {10, 10}, Qt::MoveAction, &mimeData,
        Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&root, &enter);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(router.dragTakeoverCount() == 1);
    CHECK(control.releases == 0);
}

void qtDragTakeoverIsObservedInsideRoutedMove()
{
    QWidget root;
    root.setAcceptDrops(true);
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto point = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, point,
        Qt::LeftButton, Qt::LeftButton);

    QMimeData mimeData;
    control.onMove = [&root, &mimeData] {
        // This models QAbstractItemView::startDrag() entering QDrag::exec()
        // synchronously from the move handler reached through the seam bridge.
        QEvent ungrab(QEvent::UngrabMouse);
        QCoreApplication::sendEvent(root.windowHandle(), &ungrab);
        QDragEnterEvent enter(QPoint {10, 10}, Qt::MoveAction, &mimeData,
            Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&root, &enter);
    };

    const auto outside = control.mapToGlobal(QPointF {260.0, 20.0});
    sendMouse(&native, QEvent::MouseMove, outside,
        Qt::NoButton, Qt::LeftButton);
    CHECK(control.moves == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(router.dragTakeoverCount() == 1);
    CHECK(control.releases == 0);
}

void focusLossCancelsBothCaptureDomains()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);

    const auto widgetPoint = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, widgetPoint,
        Qt::LeftButton, Qt::LeftButton);
    QFocusEvent widgetFocusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&control, &widgetFocusOut);
    CHECK(control.releases == 1);
    CHECK(!control.rect().contains(control.lastPosition.toPoint()));
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);

    sendMouse(&native, QEvent::MouseButtonPress, widgetPoint,
        Qt::MiddleButton, Qt::MiddleButton);
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&native, &deactivate);
    CHECK(native.ungrabs == 1);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void canvasDeactivationDoesNotCancelPanelCapture()
{
    QWidget root;
    RecordingWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);
    RecordingWindow native;
    // The real canvas shares the host QWindow ancestry. An unparented test
    // window misses this distinction in eventComesFromWidgetSide().
    native.setParent(root.windowHandle());
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto point = control.mapToGlobal(QPointF {20, 20});
    for (const auto terminal : {QEvent::WindowDeactivate, QEvent::ApplicationDeactivate}) {
        sendMouse(root.windowHandle(), QEvent::MouseButtonPress, point,
            Qt::LeftButton, Qt::LeftButton);
        const auto releases = control.releases;
        QEvent canvasDeactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&native, &canvasDeactivate);
        CHECK(router.captureOwner() == &control);
        CHECK(control.releases == releases);
        sendMouse(&native, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
        CHECK(router.captureOwner() == &control);
        QEvent realLoss(terminal);
        QCoreApplication::sendEvent(root.windowHandle(), &realLoss);
        CHECK(router.captureDomain() == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(control.releases == releases + 1);
    }
}

void ownedComboPopupTakesOverLogicalCapture()
{
    QWidget root;
    QComboBox combo(&root);
    combo.setObjectName(QStringLiteral("PopupCombo"));
    combo.addItems({QStringLiteral("Fixed"), QStringLiteral("Direction")});
    combo.setGeometry(20, 20, 180, 32);
    combo.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto comboPoint = combo.mapToGlobal(combo.rect().center());
    sendMouseDirect(&combo, QEvent::MouseButtonPress, comboPoint,
        Qt::LeftButton, Qt::LeftButton);
    QCoreApplication::processEvents();
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(router.popupTakeoverCount() >= 1);
    CHECK(QApplication::activePopupWidget() != nullptr);
    combo.hidePopup();
    QCoreApplication::processEvents();
}

void ownerDestructionClearsCaptureWithoutStaleRouting()
{
    QWidget root;
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    auto* control = new RecordingWidget(&root);
    control->setGeometry(20, 20, 180, 40);
    control->show();

    const auto point = control->mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, point,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(router.captureOwner() == control);
    delete control;
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);

    sendMouse(&native, QEvent::MouseMove, point,
        Qt::NoButton, Qt::NoButton);
    CHECK(native.moves == 1);
}

void cancellationReleasesLingeringExplicitWidgetGrab()
{
    QWidget root;
    ExplicitGrabWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    imageeditor::ui::CrossWindowPointerRouter router(&root, &native, nullptr);
    const auto point = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, point,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(QWidget::mouseGrabber() == &control);

    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(&control, &focusOut);
    CHECK(QWidget::mouseGrabber() == nullptr);
    CHECK(router.captureDomain()
        == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void routerDestructionReleasesLingeringExplicitWidgetGrab()
{
    QWidget root;
    ExplicitGrabWidget control(&root);
    control.setGeometry(20, 20, 180, 40);
    control.show();
    prepareWindow(root);

    RecordingWindow native;
    auto router = std::make_unique<imageeditor::ui::CrossWindowPointerRouter>(
        &root, &native, nullptr);
    const auto point = control.mapToGlobal(QPointF {20.0, 20.0});
    sendMouse(root.windowHandle(), QEvent::MouseButtonPress, point,
        Qt::LeftButton, Qt::LeftButton);
    CHECK(QWidget::mouseGrabber() == &control);

    router.reset();
    CHECK(QWidget::mouseGrabber() == nullptr);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    if (application.arguments().contains("--color-picker-only")) {
        colorPickerDragRemainsOwnedAcrossNativeCanvas();
        if (!failures) std::cout << "Color picker cross-window drag and cancellation passed\n";
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    widgetDragContinuesAcrossNativeWindow();
    colorPickerDragRemainsOwnedAcrossNativeCanvas();
    widgetDragSurvivesButtonlessReturnToWidgetWindow();
    nativeContainerWidgetWindowRemainsInCaptureScope();
    nativeCanvasDoesNotPromoteWidgetSiblings();
    widgetInsideNativeOverlayRetainsItsDispatchWindow();
    sliderUsesNormalQtDragSemanticsAcrossNativeWindow();
    compactValueDragContinuesAcrossNativeWindow();
    dockSeparatorUsesNormalQtDragSemanticsAcrossNativeWindow();
    floatingDockRemainsInTheSameCaptureDomain();
    nativeDragContinuesAcrossWidgetWindow();
    nativeDragSurvivesButtonlessReturnToNativeWindow();
    unrelatedTopLevelMoveIsNotCaptured();
    platformUngrabAtNativeSeamRetainsWidgetCapture();
    platformUngrabAtNativeSeamKeepsSliderDown();
    platformUngrabAtNativeSeamRetainsCanvasCapture();
    qtDragAndDropTakesOverWithoutSyntheticRelease();
    qtDragTakeoverIsObservedInsideRoutedMove();
    ownedComboPopupTakesOverLogicalCapture();
    focusLossCancelsBothCaptureDomains();
    canvasDeactivationDoesNotCancelPanelCapture();
    ownerDestructionClearsCaptureWithoutStaleRouting();
    cancellationReleasesLingeringExplicitWidgetGrab();
    routerDestructionReleasesLingeringExplicitWidgetGrab();

    if (failures != 0) {
        std::cerr << failures << " cross-window pointer assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All cross-window pointer tests passed\n";
    return EXIT_SUCCESS;
}
