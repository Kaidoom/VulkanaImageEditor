#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QRegion>
#include "imageeditor/ui/CompactValueControl.hpp"
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVulkanInstance>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cstdlib>

namespace {

class PointerEventLogger final : public QObject {
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event && (event->type() == QEvent::MouseButtonPress
                || event->type() == QEvent::MouseButtonRelease)) {
            const auto* mouse = static_cast<const QMouseEvent*>(event);
            qInfo() << "POINTER_PROBE_EVENT" << event->type()
                    << watched->metaObject()->className()
                    << watched->objectName()
                    << mouse->globalPosition() << mouse->buttons();
        }
        return false;
    }
};

QRect globalRect(const QWidget& widget)
{
    return {widget.mapToGlobal(QPoint {}), widget.size()};
}

QRect globalRect(const QWindow& window)
{
    return {window.mapToGlobal(QPoint {}), window.size()};
}

QRect sliderGrooveRect(const imageeditor::ui::CompactValueControl& slider)
{
    return slider.progressTrackRect();
}

QWindow* nearestNativeDispatchWindow(QWidget& widget)
{
    for (QWidget* current = &widget; current != nullptr; current = current->parentWidget()) {
        if (current->internalWinId() != 0 && current->windowHandle() != nullptr) {
            return current->windowHandle();
        }
    }
    return widget.window() != nullptr ? widget.window()->windowHandle() : nullptr;
}

bool stableSwapchainStats(
    const imageeditor::render::RendererStats& current,
    const imageeditor::render::RendererStats& baseline)
{
    return current.swapchainGeneration == baseline.swapchainGeneration
        && current.swapchainWidth == baseline.swapchainWidth
        && current.swapchainHeight == baseline.swapchainHeight
        && current.deferredResizeEvents == baseline.deferredResizeEvents
        && current.resizeCommits == baseline.resizeCommits
        && current.resizePresentationSuspends == baseline.resizePresentationSuspends
        && current.resizePresentationResumes == baseline.resizePresentationResumes;
}

struct ProbeBaseline {
    QRect workspace;
    QRect container;
    QRect canvas;
    QRect overlay;
    QRect nativeContainer;
    QRect nativeOverlay;
    QRect resizeHandle;
    QSize canvasSize;
    int panelWidth {0};
    imageeditor::render::RendererStats stats;
};

class PointerProbeController final : public QObject {
public:
    PointerProbeController(
        QApplication& application,
        imageeditor::ui::MainWindow& window,
        imageeditor::ui::OverlayDockWorkspace& workspace,
        imageeditor::ui::CompactValueControl& slider,
        QWidget& canvasContainer,
        QWidget& panelOverlay,
        QWidget& panelResizeHandle,
        imageeditor::render::CanvasWindow& canvasWindow,
        imageeditor::ui::CrossWindowPointerRouter& router)
        : QObject(&window)
        , application_(application)
        , window_(window)
        , workspace_(workspace)
        , slider_(slider)
        , canvasContainer_(canvasContainer)
        , panelOverlay_(panelOverlay)
        , panelResizeHandle_(panelResizeHandle)
        , canvasWindow_(canvasWindow)
        , router_(router)
        , baseline_(captureBaseline())
        , stageTimer_(this)
    {
        routedBefore_ = router_.routedEventCount();
        retainedBefore_ = router_.retainedUngrabCount();

        const auto onStart = slider_.onInteractionStarted;
        slider_.onInteractionStarted = [this, onStart] {
            if (onStart) onStart();
            sliderPressed_ = true;
        };
        const auto onFinish = slider_.onInteractionFinished;
        slider_.onInteractionFinished = [this, onFinish] {
            if (onFinish) onFinish();
            sliderReleased_ = true;
            if (sliderStageReady_) {
                QTimer::singleShot(250, this, [this] { finish(); });
            }
        };
        connect(&stageTimer_, &QTimer::timeout, this, [this] {
            publishSliderStageWhenReady();
        });
    }

    void start()
    {
        const QPoint panelStart = panelResizeHandle_.mapToGlobal(
            panelResizeHandle_.rect().center());
        const QPoint panelEnd = panelStart - QPoint {180, 0};

        const QWindow* overlaySurface = panelOverlay_.windowHandle();
        dispatchSurfaceMatches_ = overlaySurface != nullptr
            && panelOverlay_.internalWinId() != 0
            && nearestNativeDispatchWindow(panelResizeHandle_) == overlaySurface
            && nearestNativeDispatchWindow(slider_) == overlaySurface;

        const QRect overlayRect = globalRect(panelOverlay_);
        const QPoint handleCenterLocal = panelResizeHandle_.mapTo(
            &panelOverlay_, panelResizeHandle_.rect().center());
        resizeHandleInsideOverlay_ = overlayRect.contains(panelStart)
            && panelOverlay_.mask().isEmpty()
            && overlaySurface != nullptr
            && overlaySurface->mask().contains(handleCenterLocal);

        qInfo().noquote()
            << QStringLiteral(
                   "POINTER_PROBE_READY panel=%1,%2:%3,%4 dock=%1,%2:%3,%4 "
                   "dispatchSurface=PanelOverlaySurface dispatchWinId=%5")
                   .arg(panelStart.x()).arg(panelStart.y())
                   .arg(panelEnd.x()).arg(panelEnd.y())
                   .arg(QString::number(
                       static_cast<qulonglong>(panelOverlay_.internalWinId()), 16));

        stageTimer_.start(40);
        QTimer::singleShot(120000, this, [this] { finish(); });
    }

private:
    [[nodiscard]] ProbeBaseline captureBaseline() const
    {
        const QWindow* containerSurface = canvasContainer_.windowHandle();
        const QWindow* overlaySurface = panelOverlay_.windowHandle();
        return {
            globalRect(workspace_),
            globalRect(canvasContainer_),
            globalRect(canvasWindow_),
            globalRect(panelOverlay_),
            containerSurface != nullptr ? globalRect(*containerSurface) : QRect {},
            overlaySurface != nullptr ? globalRect(*overlaySurface) : QRect {},
            globalRect(panelResizeHandle_),
            canvasWindow_.size(),
            workspace_.panelWidth(),
            canvasWindow_.rendererStats(),
        };
    }

    [[nodiscard]] QPoint uncoveredCanvasTarget()
    {
        const QRect canvasRect = globalRect(canvasWindow_);
        const int y = canvasRect.center().y();
        const int rightLimit = canvasRect.right() - 30;
        const QRegion inputRegion = panelOverlay_.windowHandle()
            ? panelOverlay_.windowHandle()->mask() : QRegion {};
        for (int x = canvasRect.center().x(); x >= canvasRect.left() + 30; x -= 30) {
            const QPoint candidate(x, y);
            const QPoint overlayLocal = panelOverlay_.mapFromGlobal(candidate);
            if (!inputRegion.contains(overlayLocal)) {
                targetUncovered_ = true;
                return candidate;
            }
        }
        targetUncovered_ = false;
        return QPoint {std::min(canvasRect.left() + 30, rightLimit), y};
    }

    void publishSliderStageWhenReady()
    {
        if (sliderStageReady_) {
            return;
        }

        const int panelGrowth = workspace_.panelWidth() - baseline_.panelWidth;
        if (panelGrowth < 100
            || router_.captureDomain()
                != imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None) {
            return;
        }

        sliderStageReady_ = true;
        stageTimer_.stop();
        const QRect groove = sliderGrooveRect(slider_);
        const QPoint sliderStart = slider_.mapToGlobal(groove.center());
        const QPoint sliderEnd = uncoveredCanvasTarget();

        qInfo().noquote()
            << QStringLiteral("POINTER_PROBE_SLIDER_READY slider=%1,%2:%3,%4 panelGrowth=%5")
                   .arg(sliderStart.x()).arg(sliderStart.y())
                   .arg(sliderEnd.x()).arg(sliderEnd.y())
                   .arg(panelGrowth);
    }

    void finish()
    {
        if (finished_) {
            return;
        }
        finished_ = true;
        stageTimer_.stop();

        const ProbeBaseline current = captureBaseline();
        const int panelGrowth = workspace_.panelWidth() - baseline_.panelWidth;
        const auto routed = router_.routedEventCount() - routedBefore_;
        const auto retained = router_.retainedUngrabCount() - retainedBefore_;
        const bool baseGeometryStable = current.workspace == baseline_.workspace
            && current.container == baseline_.container
            && current.canvas == baseline_.canvas
            && current.nativeContainer == baseline_.nativeContainer
            && current.overlay == baseline_.overlay
            && current.nativeOverlay == baseline_.nativeOverlay
            && current.canvasSize == baseline_.canvasSize
            && current.workspace == current.container
            && current.workspace == current.canvas
            && current.workspace == current.overlay;
        const bool stationaryOverlayTracksPanel = panelGrowth > 0
            && current.overlay == baseline_.overlay
            && current.nativeOverlay == baseline_.nativeOverlay
            && current.resizeHandle.top() == baseline_.resizeHandle.top()
            && current.resizeHandle.height() == baseline_.resizeHandle.height()
            && baseline_.resizeHandle.x() - current.resizeHandle.x() == panelGrowth
            && current.resizeHandle.left() > current.overlay.left()
            && current.resizeHandle.width() == panelResizeHandle_.width();
        const bool vulkanStable = stableSwapchainStats(current.stats, baseline_.stats);
        const bool captureReleased = router_.captureDomain()
            == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None;
        const bool passed = panelGrowth >= 100
            && sliderStageReady_
            && sliderPressed_
            && sliderReleased_
            && slider_.value() <= 5
            && routed >= 4
            && captureReleased
            && dispatchSurfaceMatches_
            && resizeHandleInsideOverlay_
            && targetUncovered_
            && baseGeometryStable
            && stationaryOverlayTracksPanel
            && vulkanStable;

        qInfo().noquote()
            << QStringLiteral(
                   "POINTER_PROBE_RESULT passed=%1 panelGrowth=%2 slider=%3 "
                   "pressed=%4 released=%5 routed=%6 retainedUngrabs=%7 "
                   "captureReleased=%8 dispatchSurfaceMatches=%9 "
                   "resizeHandleInsideOverlay=%10 targetUncovered=%11 "
                   "baseGeometryStable=%12 stationaryOverlayTracksPanel=%13 "
                   "vulkanStable=%14 swapchainGeneration=%15 "
                   "swapchainExtent=%16x%17")
                   .arg(passed).arg(panelGrowth).arg(slider_.value())
                   .arg(sliderPressed_).arg(sliderReleased_)
                   .arg(routed).arg(retained)
                   .arg(captureReleased).arg(dispatchSurfaceMatches_)
                   .arg(resizeHandleInsideOverlay_).arg(targetUncovered_)
                   .arg(baseGeometryStable).arg(stationaryOverlayTracksPanel)
                   .arg(vulkanStable)
                   .arg(current.stats.swapchainGeneration)
                   .arg(current.stats.swapchainWidth)
                   .arg(current.stats.swapchainHeight);

        window_.close();
        application_.exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    QApplication& application_;
    imageeditor::ui::MainWindow& window_;
    imageeditor::ui::OverlayDockWorkspace& workspace_;
    imageeditor::ui::CompactValueControl& slider_;
    QWidget& canvasContainer_;
    QWidget& panelOverlay_;
    QWidget& panelResizeHandle_;
    imageeditor::render::CanvasWindow& canvasWindow_;
    imageeditor::ui::CrossWindowPointerRouter& router_;
    ProbeBaseline baseline_;
    QTimer stageTimer_;
    std::uint64_t routedBefore_ {0};
    std::uint64_t retainedBefore_ {0};
    bool sliderStageReady_ {false};
    bool sliderPressed_ {false};
    bool sliderReleased_ {false};
    bool dispatchSurfaceMatches_ {false};
    bool resizeHandleInsideOverlay_ {false};
    bool targetUncovered_ {false};
    bool finished_ {false};
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    PointerEventLogger eventLogger;
    application.installEventFilter(&eventLogger);
    imageeditor::ui::applyEditorTheme(application);
    QLoggingCategory::setFilterRules(QStringLiteral(
        "imageeditor.input.capture.debug=true\n"
        "imageeditor.vulkan.debug=false\n"
        "imageeditor.vulkan.info=false\n"
        "imageeditor.vulkan.host.info=false\n"
        "qt.svg.warning=false"));

    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        qCritical() << "POINTER_PROBE requires QT_QPA_PLATFORM=wayland";
        return EXIT_FAILURE;
    }

    QVulkanInstance vulkanInstance;
    vulkanInstance.setApiVersion(QVersionNumber(1, 2, 0));
    if (vulkanInstance.supportedExtensions().contains("VK_EXT_swapchain_colorspace")) {
        vulkanInstance.setExtensions({"VK_EXT_swapchain_colorspace"});
    }
    if (!vulkanInstance.create()) {
        qCritical() << "POINTER_PROBE could not create Vulkan instance";
        return EXIT_FAILURE;
    }

    imageeditor::ui::MainWindow window(&vulkanInstance, false);
    window.setUnsavedPromptEnabled(false);
    window.showMaximized();

    QTimer::singleShot(1200, &window, [&application, &window] {
        auto* slider = dynamic_cast<imageeditor::ui::CompactValueControl*>(
            window.findChild<QDoubleSpinBox*>(QStringLiteral("LayerOpacitySlider")));
        auto* workspace = dynamic_cast<imageeditor::ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        auto* canvasContainer = window.findChild<QWidget*>(QStringLiteral("VulkanCanvasContainer"));
        auto* panelOverlay = window.findChild<QWidget*>(QStringLiteral("PanelOverlaySurface"));
        auto* panelResizeHandle = window.findChild<QWidget*>(
            QStringLiteral("OverlayPanelResizeHandle"));
        auto* router = dynamic_cast<imageeditor::ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));

        imageeditor::render::CanvasWindow* canvasWindow = nullptr;
        for (QWindow* candidate : QGuiApplication::allWindows()) {
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow")) {
                canvasWindow = static_cast<imageeditor::render::CanvasWindow*>(candidate);
                break;
            }
        }

        if (slider == nullptr || workspace == nullptr || canvasContainer == nullptr
            || panelOverlay == nullptr || panelResizeHandle == nullptr
            || router == nullptr || canvasWindow == nullptr) {
            qCritical() << "POINTER_PROBE could not locate required overlay UI objects";
            application.exit(EXIT_FAILURE);
            return;
        }

        auto* controller = new PointerProbeController(
            application, window, *workspace, *slider, *canvasContainer,
            *panelOverlay, *panelResizeHandle, *canvasWindow, *router);
        controller->start();
    });

    return application.exec();
}
