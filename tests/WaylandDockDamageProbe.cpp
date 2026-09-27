#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QPainter>
#include <QProcess>
#include <QRegion>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVulkanInstance>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace {

QRect globalRect(const QWidget& widget)
{
    return {widget.mapToGlobal(QPoint {}), widget.size()};
}

QRect globalRect(const QWindow& window)
{
    return {window.mapToGlobal(QPoint {}), window.size()};
}

QString rectText(const QRect& rect)
{
    return QStringLiteral("%1,%2 %3x%4")
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height());
}

void logNativeStatus(const char* label, QWidget& widget)
{
    const bool native = widget.testAttribute(Qt::WA_NativeWindow) || widget.internalWinId() != 0;
    qInfo().noquote() << "NATIVE_STATUS"
                      << label
                      << "object=" << widget.objectName()
                      << "native=" << native
                      << "winId=" << QString::number(static_cast<qulonglong>(widget.internalWinId()), 16)
                      << "windowHandle=" << static_cast<const void*>(widget.windowHandle());
}

void logNativeTree(QWidget& root, int depth = 0)
{
    const auto children = root.findChildren<QWidget*>(QString {}, Qt::FindDirectChildrenOnly);
    for (QWidget* child : children) {
        const bool native = child->testAttribute(Qt::WA_NativeWindow) || child->internalWinId() != 0;
        qInfo().noquote() << "NATIVE_TREE"
                          << QString(depth * 2, QLatin1Char(' '))
                          << child->metaObject()->className()
                          << "object=" << child->objectName()
                          << "native=" << native
                          << "winId=" << QString::number(static_cast<qulonglong>(child->internalWinId()), 16)
                          << "visible=" << child->isVisible()
                          << "geometry=" << rectText(globalRect(*child));
        logNativeTree(*child, depth + 1);
    }
}

bool hasNativeWidget(QWidget& root)
{
    const auto children = root.findChildren<QWidget*>();
    return std::any_of(children.cbegin(), children.cend(), [](const QWidget* widget) {
        return widget->testAttribute(Qt::WA_NativeWindow) || widget->internalWinId() != 0;
    });
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

QColor averagedPixel(const QImage& image, QPoint center, int radius = 3)
{
    const QRect bounds = image.rect();
    const QRect sampleRect(center.x() - radius, center.y() - radius,
        radius * 2 + 1, radius * 2 + 1);
    const QRect clipped = sampleRect.intersected(bounds);
    if (clipped.isEmpty()) {
        return {};
    }

    quint64 red = 0;
    quint64 green = 0;
    quint64 blue = 0;
    quint64 count = 0;
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            red += static_cast<quint64>(pixel.red());
            green += static_cast<quint64>(pixel.green());
            blue += static_cast<quint64>(pixel.blue());
            ++count;
        }
    }
    return QColor(static_cast<int>(red / count), static_cast<int>(green / count),
        static_cast<int>(blue / count));
}

int maximumRgbDelta(const QColor& first, const QColor& second)
{
    return std::max({std::abs(first.red() - second.red()),
        std::abs(first.green() - second.green()),
        std::abs(first.blue() - second.blue())});
}

bool createReferenceImage(const QString& path)
{
    constexpr int kWidth = 640;
    constexpr int kHeight = 480;
    const std::array<QColor, 4> colors {
        QColor {224, 42, 56},
        QColor {47, 214, 79},
        QColor {52, 88, 229},
        QColor {232, 205, 43},
    };

    QImage image(kWidth, kHeight, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.fillRect(QRect {0, 0, kWidth / 2, kHeight / 2}, colors[0]);
    painter.fillRect(QRect {kWidth / 2, 0, kWidth / 2, kHeight / 2}, colors[1]);
    painter.fillRect(QRect {0, kHeight / 2, kWidth / 2, kHeight / 2}, colors[2]);
    painter.fillRect(QRect {kWidth / 2, kHeight / 2, kWidth / 2, kHeight / 2}, colors[3]);
    painter.end();
    return image.save(path, "PNG");
}

struct GeometrySnapshot {
    QRect workspace;
    QRect container;
    QRect nativeContainer;
    QRect canvas;
    QRect overlay;
    QRect nativeOverlay;
    QSize canvasSize;
    imageeditor::render::RendererStats stats;
};

class GeometrySampler final {
public:
    GeometrySampler(
        imageeditor::ui::MainWindow& window,
        imageeditor::ui::OverlayDockWorkspace& workspace,
        imageeditor::ui::WorkspacePanel& panel,
        QWidget& container,
        QWidget& overlay,
        QWidget& resizeHandle,
        QWidget& panelColumn,
        imageeditor::render::CanvasWindow& canvas)
        : window_(window)
        , workspace_(workspace)
        , panel_(panel)
        , container_(container)
        , overlay_(overlay)
        , resizeHandle_(resizeHandle)
        , panelColumn_(panelColumn)
        , canvas_(canvas)
        , baseline_(snapshot())
    {
    }

    void sample(const char* phase, int requestedPanelWidth)
    {
        const GeometrySnapshot current = snapshot();
        const QRect appRect = globalRect(window_);
        const QRect panelRect = globalRect(panel_);
        const QRect panelColumnRect = globalRect(panelColumn_);

        const bool fullExtent = current.workspace == current.container
            && current.workspace == current.canvas
            && current.workspace == current.overlay;
        const bool nativeMatches = current.nativeContainer == current.container
            && current.nativeOverlay == current.overlay;
        const bool fixedExtent = current.workspace == baseline_.workspace
            && current.container == baseline_.container
            && current.nativeContainer == baseline_.nativeContainer
            && current.canvas == baseline_.canvas
            && current.overlay == baseline_.overlay
            && current.nativeOverlay == baseline_.nativeOverlay
            && current.canvasSize == baseline_.canvasSize;
        const bool canvasInsideApp = appRect.contains(current.canvas);
        const bool canvasInsideScreen = canvas_.screen() == nullptr
            || canvas_.screen()->geometry().contains(current.canvas);
        const bool panelWidthMatches = std::abs(workspace_.panelWidth() - requestedPanelWidth) <= 1;
        const bool vulkanStable = stableSwapchainStats(current.stats, baseline_.stats);

        const QRect handleLocal(
            resizeHandle_.mapTo(&overlay_, QPoint {}), resizeHandle_.size());
        const QRect panelColumnLocal(
            panelColumn_.mapTo(&overlay_, QPoint {}), panelColumn_.size());
        const QRegion inputRegion = overlay_.windowHandle()
            ? overlay_.windowHandle()->mask() : QRegion {};
        const bool stationaryOverlay = current.workspace == current.overlay
            && current.overlay == baseline_.overlay
            && current.nativeOverlay == baseline_.nativeOverlay;
        const bool overlayContentsMatch = overlay_.mask().isEmpty()
            && handleLocal.top() == 10
            && handleLocal.height() == overlay_.height() - 20
            && handleLocal.width() == resizeHandle_.width()
            && panelColumnLocal.center().x() > handleLocal.center().x()
            && inputRegion.contains(handleLocal.center())
            && inputRegion.contains(panelColumnLocal.center())
            && !inputRegion.contains(QPoint(4, overlay_.height() / 2));

        sawGeometryMismatch_ |= !fullExtent || !nativeMatches || !fixedExtent;
        sawOverflow_ |= !canvasInsideApp || !canvasInsideScreen;
        sawPanelWidthMismatch_ |= !panelWidthMatches;
        sawVulkanMutation_ |= !vulkanStable;
        sawOverlayLayoutMismatch_ |= !stationaryOverlay || !overlayContentsMatch;
        maxContainerDelta_ = std::max(
            maxContainerDelta_, std::abs(current.container.width() - baseline_.container.width()));
        maxCanvasDelta_ = std::max(
            maxCanvasDelta_, std::abs(current.canvas.width() - baseline_.canvas.width()));

        ++samples_;
        if (samples_ <= 4 || samples_ % 20 == 0) {
            qInfo().noquote() << "GEOMETRY_SAMPLE"
                              << "phase=" << phase
                              << "requestedPanelWidth=" << requestedPanelWidth
                              << "panelWidth=" << workspace_.panelWidth()
                              << "app=" << rectText(appRect)
                              << "workspace=" << rectText(current.workspace)
                              << "container=" << rectText(current.container)
                              << "canvas=" << rectText(current.canvas)
                              << "overlay=" << rectText(current.overlay)
                              << "panel=" << rectText(panelRect)
                              << "panelColumn=" << rectText(panelColumnRect)
                              << "swapchainGeneration=" << current.stats.swapchainGeneration
                              << "resizeCommits=" << current.stats.resizeCommits
                              << "fullExtent=" << fullExtent
                              << "fixedExtent=" << fixedExtent
                              << "vulkanStable=" << vulkanStable
                              << "stationaryOverlay=" << stationaryOverlay
                              << "overlayContentsMatch=" << overlayContentsMatch;
        }
    }

    [[nodiscard]] bool safe() const
    {
        return !sawGeometryMismatch_ && !sawOverflow_ && !sawPanelWidthMismatch_
            && !sawVulkanMutation_ && !sawOverlayLayoutMismatch_;
    }

    void logSummary() const
    {
        qInfo().noquote() << "GEOMETRY_SUMMARY"
                          << "samples=" << samples_
                          << "maxContainerDelta=" << maxContainerDelta_
                          << "maxCanvasDelta=" << maxCanvasDelta_
                          << "geometryMismatch=" << sawGeometryMismatch_
                          << "overflow=" << sawOverflow_
                          << "panelWidthMismatch=" << sawPanelWidthMismatch_
                          << "vulkanMutation=" << sawVulkanMutation_
                          << "overlayLayoutMismatch=" << sawOverlayLayoutMismatch_;
    }

private:
    [[nodiscard]] GeometrySnapshot snapshot() const
    {
        const QWindow* containerWindow = container_.windowHandle();
        const QWindow* overlayWindow = overlay_.windowHandle();
        return {
            globalRect(workspace_),
            globalRect(container_),
            containerWindow != nullptr ? globalRect(*containerWindow) : QRect {},
            globalRect(canvas_),
            globalRect(overlay_),
            overlayWindow != nullptr ? globalRect(*overlayWindow) : QRect {},
            canvas_.size(),
            canvas_.rendererStats(),
        };
    }

    imageeditor::ui::MainWindow& window_;
    imageeditor::ui::OverlayDockWorkspace& workspace_;
    imageeditor::ui::WorkspacePanel& panel_;
    QWidget& container_;
    QWidget& overlay_;
    QWidget& resizeHandle_;
    QWidget& panelColumn_;
    imageeditor::render::CanvasWindow& canvas_;
    GeometrySnapshot baseline_;
    int samples_ {0};
    int maxContainerDelta_ {0};
    int maxCanvasDelta_ {0};
    bool sawGeometryMismatch_ {false};
    bool sawOverflow_ {false};
    bool sawPanelWidthMismatch_ {false};
    bool sawVulkanMutation_ {false};
    bool sawOverlayLayoutMismatch_ {false};
};

class ProbeController final : public QObject {
public:
    ProbeController(
        QApplication& app,
        imageeditor::ui::MainWindow& window,
        imageeditor::ui::OverlayDockWorkspace& workspace,
        imageeditor::ui::WorkspacePanel& layersPanel,
        imageeditor::ui::WorkspacePanel& propertiesPanel,
        QWidget& container,
        QWidget& overlay,
        QWidget& resizeHandle,
        QWidget& panelColumn,
        imageeditor::render::CanvasWindow& canvas,
        QString artifactDirectory)
        : QObject(&window)
        , app_(app)
        , window_(window)
        , workspace_(workspace)
        , layersPanel_(layersPanel)
        , propertiesPanel_(propertiesPanel)
        , container_(container)
        , overlay_(overlay)
        , resizeHandle_(resizeHandle)
        , panelColumn_(panelColumn)
        , canvas_(canvas)
        , sampler_(window, workspace, layersPanel, container, overlay,
              resizeHandle, panelColumn, canvas)
        , widthTimer_(this)
        , artifactDirectory_(std::move(artifactDirectory))
        , spectacleExecutable_(QStandardPaths::findExecutable(QStringLiteral("spectacle")))
    {
        startWidth_ = workspace_.panelWidth();
        minimumWidth_ = 228;
        expandedWidth_ = std::min(640, startWidth_ + 180);

        containerNative_ = container_.internalWinId() != 0 && container_.windowHandle() != nullptr;
        overlayNative_ = overlay_.internalWinId() != 0 && overlay_.windowHandle() != nullptr;
        overlayIndependentBacking_ = overlay_.isWindow()
            && overlay_.testAttribute(Qt::WA_TranslucentBackground)
            && !overlay_.testAttribute(Qt::WA_OpaquePaintEvent)
            && overlay_.backingStore() != window_.backingStore();
        nativeSiblingTopology_ = containerNative_ && overlayNative_
            && container_.windowHandle()->parent() == overlay_.windowHandle()->parent();
        canvasParentMatches_ = canvas_.parent() == container_.windowHandle();
        panelColumnAlien_ = panelColumn_.internalWinId() == 0
            && !hasNativeWidget(panelColumn_);
        panelsAlien_ = layersPanel_.internalWinId() == 0
            && propertiesPanel_.internalWinId() == 0
            && layersPanel_.windowHandle() == nullptr
            && propertiesPanel_.windowHandle() == nullptr;
        initialDocking_ = workspace_.panelPlacement(&layersPanel_)
                == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedRight
            && workspace_.panelPlacement(&propertiesPanel_)
                == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedRight
            && panelColumn_.isAncestorOf(&layersPanel_)
            && panelColumn_.isAncestorOf(&propertiesPanel_);

        connect(&widthTimer_, &QTimer::timeout, this, [this]() { advanceWidth(); });
    }

    void start()
    {
        logNativeStatus("canvas-container", container_);
        logNativeStatus("panel-overlay", overlay_);
        logNativeStatus("panel-column", panelColumn_);
        logNativeStatus("layers-docked", layersPanel_);
        logNativeStatus("properties-docked", propertiesPanel_);
        logNativeTree(window_);

        qInfo().noquote() << "WAYLAND_DOCK_DAMAGE_PROBE_READY"
                          << "platform=" << QGuiApplication::platformName()
                          << "startPanelWidth=" << startWidth_
                          << "minimumPanelWidth=" << minimumWidth_
                          << "expandedPanelWidth=" << expandedWidth_
                          << "canvas=" << rectText(globalRect(canvas_));

        sampler_.sample("baseline", startWidth_);
        captureCompositedWindow(QStringLiteral("baseline"),
            [this](bool captured, QImage image, const QString& detail) {
                if (captured) {
                    baselineCapture_ = std::move(image);
                } else {
                    pixelFailure_ = detail;
                }
                beginWidthSweep();
            });
    }

private:
    enum class WidthPhase { Shrink, Expand };

    using CaptureCallback = std::function<void(bool, QImage, QString)>;

    void beginWidthSweep()
    {
        fromWidth_ = startWidth_;
        toWidth_ = minimumWidth_;
        widthTimer_.start(5);
    }

    void captureCompositedWindow(
        const QString& phase, CaptureCallback callback, int attempt = 1)
    {
        if (spectacleExecutable_.isEmpty()) {
            callback(false, {}, QStringLiteral("spectacle-not-found"));
            return;
        }

        const QString outputPath = QDir(artifactDirectory_).filePath(
            QStringLiteral("%1-attempt-%2.png").arg(phase).arg(attempt));
        QFile::remove(outputPath);
        window_.activateWindow();

        QTimer::singleShot(80, this,
            [this, phase, attempt, outputPath,
                callback = std::move(callback)]() mutable {
                auto* process = new QProcess(this);
                auto completed = std::make_shared<bool>(false);
                auto completion = std::make_shared<CaptureCallback>(std::move(callback));
                const auto complete = [this, process, completed, completion,
                                          phase, attempt, outputPath](
                                          bool success, const QString& detail) {
                    if (std::exchange(*completed, true)) {
                        return;
                    }
                    QImage capture;
                    if (success) {
                        capture.load(outputPath);
                        success = !capture.isNull();
                    }
                    const QSize expectedCaptureSize {
                        static_cast<int>(std::lround(
                            static_cast<double>(window_.width())
                            * window_.devicePixelRatioF())),
                        static_cast<int>(std::lround(
                            static_cast<double>(window_.height())
                            * window_.devicePixelRatioF())),
                    };
                    if (success && capture.size() != expectedCaptureSize
                        && attempt < 3) {
                        qWarning().noquote()
                            << "COMPOSITOR_CAPTURE_RETRY"
                            << "phase=" << phase
                            << "attempt=" << attempt
                            << "captured=" << capture.size()
                            << "expected=" << expectedCaptureSize
                            << "artifact=" << outputPath;
                        process->deleteLater();
                        QTimer::singleShot(180, this,
                            [this, phase, attempt, completion]() mutable {
                                captureCompositedWindow(
                                    phase, std::move(*completion), attempt + 1);
                            });
                        return;
                    }
                    if (success && capture.size() != expectedCaptureSize) {
                        success = false;
                    }
                    QString resultDetail = success ? outputPath : detail;
                    if (!success && resultDetail.isEmpty()) {
                        resultDetail = QStringLiteral("captured-other-window size=%1x%2 expected=%3x%4")
                                           .arg(capture.width())
                                           .arg(capture.height())
                                           .arg(expectedCaptureSize.width())
                                           .arg(expectedCaptureSize.height());
                    }
                    (*completion)(success, std::move(capture), resultDetail);
                    process->deleteLater();
                };

                connect(process, &QProcess::finished, this,
                    [process, complete](int exitCode, QProcess::ExitStatus exitStatus) mutable {
                        const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;
                        const QString detail = success
                            ? QString {}
                            : QStringLiteral("spectacle-exit-%1: %2")
                                  .arg(exitCode)
                                  .arg(QString::fromUtf8(process->readAllStandardError()).trimmed());
                        complete(success, detail);
                    });
                connect(process, &QProcess::errorOccurred, this,
                    [process, complete](QProcess::ProcessError error) mutable {
                        complete(false, QStringLiteral("spectacle-error-%1: %2")
                                            .arg(static_cast<int>(error))
                                            .arg(process->errorString()));
                    });
                QTimer::singleShot(8000, process, [process, complete]() mutable {
                    if (process->state() != QProcess::NotRunning) {
                        process->kill();
                        complete(false, QStringLiteral("spectacle-timeout"));
                    }
                });

                process->start(spectacleExecutable_, {
                    QStringLiteral("--background"),
                    QStringLiteral("--nonotify"),
                    QStringLiteral("--activewindow"),
                    QStringLiteral("--no-decoration"),
                    QStringLiteral("--no-shadow"),
                    QStringLiteral("--output"),
                    outputPath,
                });
            });
    }

    void advanceWidth()
    {
        constexpr int kSteps = 60;
        widthTimer_.stop();
        ++step_;
        const double t = std::min(1.0, static_cast<double>(step_) / static_cast<double>(kSteps));
        const int requested = static_cast<int>(std::lround(
            static_cast<double>(fromWidth_) + static_cast<double>(toWidth_ - fromWidth_) * t));
        workspace_.setPanelWidth(requested);
        const WidthPhase sampledPhase = phase_;
        QTimer::singleShot(0, this, [this, requested, sampledPhase]() {
            sampler_.sample(sampledPhase == WidthPhase::Shrink ? "shrink" : "expand", requested);
            if (step_ < kSteps) {
                widthTimer_.start(5);
                return;
            }

            if (phase_ == WidthPhase::Shrink) {
                phase_ = WidthPhase::Expand;
                step_ = 0;
                fromWidth_ = minimumWidth_;
                toWidth_ = expandedWidth_;
                QTimer::singleShot(120, this, [this]() { widthTimer_.start(5); });
                return;
            }

            expandedHandleLeft_ = resizeHandle_.mapTo(&window_, QPoint {}).x();
            workspace_.setPanelWidth(startWidth_);
            QTimer::singleShot(140, this, [this]() {
                sampler_.sample("restored", startWidth_);
                captureCompositedWindow(QStringLiteral("restored"),
                    [this](bool captured, QImage image, const QString& detail) {
                        if (captured && !baselineCapture_.isNull()) {
                            validateCompositedPixels(image);
                        } else if (pixelFailure_.isEmpty()) {
                            pixelFailure_ = captured
                                ? QStringLiteral("baseline-capture-unavailable")
                                : detail;
                        }
                        exerciseVisibility();
                    });
            });
        });
    }

    void validateCompositedPixels(const QImage& restoredCapture)
    {
        constexpr int kReferenceColorTolerance = 70;
        constexpr int kChangedPixelTolerance = 20;
        constexpr double kMaximumChangedRatio = 0.002;
        constexpr double kSuspiciousColumnCoverage = 0.08;

        if (restoredCapture.size() != baselineCapture_.size()
            || restoredCapture.width() <= 0 || restoredCapture.height() <= 0
            || window_.width() <= 0 || window_.height() <= 0) {
            pixelFailure_ = QStringLiteral("capture-size-mismatch baseline=%1x%2 restored=%3x%4 window=%5x%6")
                                .arg(baselineCapture_.width())
                                .arg(baselineCapture_.height())
                                .arg(restoredCapture.width())
                                .arg(restoredCapture.height())
                                .arg(window_.width())
                                .arg(window_.height());
            return;
        }

        const double scaleX = static_cast<double>(restoredCapture.width())
            / static_cast<double>(window_.width());
        const double scaleY = static_cast<double>(restoredCapture.height())
            / static_cast<double>(window_.height());
        if (std::abs(scaleX - scaleY) > 0.03 || scaleX < 0.5 || scaleX > 4.0) {
            pixelFailure_ = QStringLiteral("unexpected-capture-scale scaleX=%1 scaleY=%2")
                                .arg(scaleX, 0, 'f', 3)
                                .arg(scaleY, 0, 'f', 3);
            return;
        }

        const QPoint canvasOrigin = canvas_.mapToGlobal(QPoint {})
            - window_.mapToGlobal(QPoint {});
        const auto& scene = canvas_.scene();
        const imageeditor::core::Extent2d documentExtent {
            static_cast<double>(scene.document.canvas.extent.width),
            static_cast<double>(scene.document.canvas.extent.height),
        };
        const std::array<imageeditor::core::Vec2d, 4> documentSamples {{
            {documentExtent.width * 0.25, documentExtent.height * 0.25},
            {documentExtent.width * 0.75, documentExtent.height * 0.25},
            {documentExtent.width * 0.25, documentExtent.height * 0.75},
            {documentExtent.width * 0.75, documentExtent.height * 0.75},
        }};
        const std::array<QColor, 4> expectedColors {
            QColor {224, 42, 56},
            QColor {47, 214, 79},
            QColor {52, 88, 229},
            QColor {232, 205, 43},
        };

        bool referenceVisible = true;
        QStringList sampleDetails;
        for (std::size_t index = 0; index < documentSamples.size(); ++index) {
            const imageeditor::core::Vec2d viewportPoint = scene.viewport.documentToViewport(
                documentSamples[index], documentExtent, scene.logicalViewport);
            const QPoint capturePoint {
                static_cast<int>(std::lround(
                    (static_cast<double>(canvasOrigin.x()) + viewportPoint.x) * scaleX)),
                static_cast<int>(std::lround(
                    (static_cast<double>(canvasOrigin.y()) + viewportPoint.y) * scaleY)),
            };
            const QColor actual = averagedPixel(restoredCapture, capturePoint);
            const int delta = actual.isValid()
                ? maximumRgbDelta(actual, expectedColors[index])
                : 255;
            referenceVisible &= delta <= kReferenceColorTolerance;
            sampleDetails.push_back(QStringLiteral("%1,%2:%3/%4/%5 delta=%6")
                                        .arg(capturePoint.x())
                                        .arg(capturePoint.y())
                                        .arg(actual.red())
                                        .arg(actual.green())
                                        .arg(actual.blue())
                                        .arg(delta));
        }

        const int restoredHandleLeft = resizeHandle_.mapTo(
            &window_, QPoint {}).x();
        const int unscaledLeft = expandedHandleLeft_ + 4;
        const int unscaledRight = restoredHandleLeft - 4;
        const int canvasTop = canvasOrigin.y() + 8;
        const int canvasBottom = canvasOrigin.y() + canvas_.height() - 9;
        QRect sweptBand {
            static_cast<int>(std::ceil(static_cast<double>(unscaledLeft) * scaleX)),
            static_cast<int>(std::ceil(static_cast<double>(canvasTop) * scaleY)),
            static_cast<int>(std::floor(static_cast<double>(unscaledRight) * scaleX))
                - static_cast<int>(std::ceil(static_cast<double>(unscaledLeft) * scaleX)) + 1,
            static_cast<int>(std::floor(static_cast<double>(canvasBottom) * scaleY))
                - static_cast<int>(std::ceil(static_cast<double>(canvasTop) * scaleY)) + 1,
        };
        sweptBand = sweptBand.intersected(restoredCapture.rect());

        quint64 comparedPixels = 0;
        quint64 changedPixels = 0;
        int suspiciousColumns = 0;
        double maximumColumnCoverage = 0.0;
        if (!sweptBand.isEmpty() && expandedHandleLeft_ >= 0
            && restoredHandleLeft > expandedHandleLeft_) {
            for (int x = sweptBand.left(); x <= sweptBand.right(); ++x) {
                int columnChanged = 0;
                for (int y = sweptBand.top(); y <= sweptBand.bottom(); ++y) {
                    const QColor before = baselineCapture_.pixelColor(x, y);
                    const QColor after = restoredCapture.pixelColor(x, y);
                    const bool changed = maximumRgbDelta(before, after)
                        > kChangedPixelTolerance;
                    changedPixels += changed ? 1U : 0U;
                    columnChanged += changed ? 1 : 0;
                    ++comparedPixels;
                }
                const double coverage = static_cast<double>(columnChanged)
                    / static_cast<double>(sweptBand.height());
                maximumColumnCoverage = std::max(maximumColumnCoverage, coverage);
                suspiciousColumns += coverage >= kSuspiciousColumnCoverage ? 1 : 0;
            }
        }

        const double changedRatio = comparedPixels == 0
            ? 1.0
            : static_cast<double>(changedPixels) / static_cast<double>(comparedPixels);
        const bool trailFree = !sweptBand.isEmpty()
            && changedRatio <= kMaximumChangedRatio && suspiciousColumns == 0;
        pixelSafe_ = referenceVisible && trailFree;
        if (!pixelSafe_) {
            pixelFailure_ = QStringLiteral("referenceVisible=%1 trailFree=%2 changedRatio=%3 suspiciousColumns=%4 maxColumnCoverage=%5 band=%6 samples=[%7]")
                                .arg(referenceVisible)
                                .arg(trailFree)
                                .arg(changedRatio, 0, 'f', 6)
                                .arg(suspiciousColumns)
                                .arg(maximumColumnCoverage, 0, 'f', 4)
                                .arg(rectText(sweptBand))
                                .arg(sampleDetails.join(QStringLiteral("; ")));
        }

        qInfo().noquote() << "COMPOSITOR_PIXEL_SUMMARY"
                          << "passed=" << pixelSafe_
                          << "referenceVisible=" << referenceVisible
                          << "trailFree=" << trailFree
                          << "changedRatio=" << QString::number(changedRatio, 'f', 6)
                          << "suspiciousColumns=" << suspiciousColumns
                          << "maxColumnCoverage=" << QString::number(maximumColumnCoverage, 'f', 4)
                          << "sweptBand=" << rectText(sweptBand)
                          << "samples=" << sampleDetails.join(QStringLiteral("; "))
                          << "artifacts=" << artifactDirectory_;
    }

    void exerciseVisibility()
    {
        workspace_.setPanelVisible(&layersPanel_, false);
        QTimer::singleShot(120, this, [this]() {
            sampler_.sample("layers-hidden", startWidth_);
            workspace_.setPanelVisible(&layersPanel_, true);
            workspace_.setPanelVisible(&propertiesPanel_, false);
            QTimer::singleShot(120, this, [this]() {
                sampler_.sample("properties-hidden", startWidth_);
                workspace_.setPanelVisible(&propertiesPanel_, true);
                QTimer::singleShot(120, this, [this]() {
                    sampler_.sample("panels-visible", startWidth_);
                    exerciseFloatingPanel();
                });
            });
        });
    }

    void exerciseFloatingPanel()
    {
        workspace_.floatPanel(
            &propertiesPanel_, QRect {420, 120, 340, 300});
        QTimer::singleShot(180, this, [this]() {
            logNativeStatus("properties-floating", propertiesPanel_);
            floatingPanelInternal_ = !propertiesPanel_.isWindow()
                && propertiesPanel_.internalWinId() == 0
                && propertiesPanel_.windowHandle() == nullptr
                && propertiesPanel_.parentWidget() == workspace_.panelOverlay();
            sampler_.sample("properties-floating", startWidth_);

            workspace_.dockPanel(&propertiesPanel_,
                imageeditor::ui::OverlayDockWorkspace::PanelDockSide::Right);
            QTimer::singleShot(240, this, [this]() {
                logNativeStatus("properties-redocked", propertiesPanel_);
                redockedPanelInternal_ = propertiesPanel_.internalWinId() == 0
                    && propertiesPanel_.windowHandle() == nullptr
                    && panelColumn_.isAncestorOf(&propertiesPanel_)
                    && workspace_.panelPlacement(&propertiesPanel_)
                        == imageeditor::ui::OverlayDockWorkspace::PanelPlacement::DockedRight;
                sampler_.sample("properties-redocked", startWidth_);
                finish();
            });
        });
    }

    void finish()
    {
        sampler_.logSummary();
        const bool topologySafe = containerNative_ && overlayNative_ && nativeSiblingTopology_
            && overlayIndependentBacking_ && canvasParentMatches_
            && panelColumnAlien_ && panelsAlien_ && initialDocking_
            && floatingPanelInternal_ && redockedPanelInternal_;
        const bool passed = sampler_.safe() && topologySafe && pixelSafe_;

        qInfo().noquote() << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT"
                          << "passed=" << passed
                          << "geometrySafe=" << sampler_.safe()
                          << "topologySafe=" << topologySafe
                          << "compositorPixelsSafe=" << pixelSafe_
                          << "pixelFailure=" << (pixelFailure_.isEmpty()
                                  ? QStringLiteral("none") : pixelFailure_)
                          << "artifacts=" << artifactDirectory_
                          << "containerNative=" << containerNative_
                          << "overlayNative=" << overlayNative_
                          << "overlayIndependentBacking=" << overlayIndependentBacking_
                          << "nativeSiblingTopology=" << nativeSiblingTopology_
                          << "canvasParentMatches=" << canvasParentMatches_
                          << "panelColumnAlien=" << panelColumnAlien_
                          << "panelsAlien=" << panelsAlien_
                          << "initialDocking=" << initialDocking_
                          << "floatingPanelInternal=" << floatingPanelInternal_
                          << "redockedPanelInternal=" << redockedPanelInternal_;

        window_.close();
        app_.exit(passed ? 0 : 1);
    }

    QApplication& app_;
    imageeditor::ui::MainWindow& window_;
    imageeditor::ui::OverlayDockWorkspace& workspace_;
    imageeditor::ui::WorkspacePanel& layersPanel_;
    imageeditor::ui::WorkspacePanel& propertiesPanel_;
    QWidget& container_;
    QWidget& overlay_;
    QWidget& resizeHandle_;
    QWidget& panelColumn_;
    imageeditor::render::CanvasWindow& canvas_;
    GeometrySampler sampler_;
    QTimer widthTimer_;
    QString artifactDirectory_;
    QString spectacleExecutable_;
    QImage baselineCapture_;
    int startWidth_ {0};
    int minimumWidth_ {0};
    int expandedWidth_ {0};
    int fromWidth_ {0};
    int toWidth_ {0};
    int step_ {0};
    int expandedHandleLeft_ {-1};
    WidthPhase phase_ {WidthPhase::Shrink};
    bool containerNative_ {false};
    bool overlayNative_ {false};
    bool overlayIndependentBacking_ {false};
    bool nativeSiblingTopology_ {false};
    bool canvasParentMatches_ {false};
    bool panelColumnAlien_ {false};
    bool panelsAlien_ {false};
    bool initialDocking_ {false};
    bool floatingPanelInternal_ {false};
    bool redockedPanelInternal_ {false};
    bool pixelSafe_ {false};
    QString pixelFailure_;
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QLoggingCategory::setFilterRules(QStringLiteral(
        "qt.svg.warning=false\n"
        "imageeditor.vulkan.debug=false\n"));

    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        qCritical().noquote() << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=not-wayland"
                              << "platform=" << QGuiApplication::platformName();
        return 77;
    }

    imageeditor::ui::applyEditorTheme(app);

    QVulkanInstance vulkanInstance;
    vulkanInstance.setLayers(QByteArrayList {QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    if (!vulkanInstance.create()) {
        qCritical().noquote() << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=vulkan-instance"
                              << "errorCode=" << vulkanInstance.errorCode();
        return 78;
    }

    QTemporaryDir artifactDirectory(QDir::tempPath()
        + QStringLiteral("/imageeditor-wayland-dock-damage-XXXXXX"));
    artifactDirectory.setAutoRemove(false);
    if (!artifactDirectory.isValid()) {
        qCritical().noquote()
            << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=artifact-directory";
        return 79;
    }
    const QString referenceImagePath = artifactDirectory.filePath(
        QStringLiteral("four-corner-reference.png"));
    if (!createReferenceImage(referenceImagePath)) {
        qCritical().noquote()
            << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=reference-image"
            << "path=" << referenceImagePath;
        return 80;
    }

    imageeditor::ui::MainWindow window(&vulkanInstance, false);
    window.setUnsavedPromptEnabled(false);
    if (!window.openImageFromPath(referenceImagePath)) {
        qCritical().noquote()
            << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=reference-open"
            << "path=" << referenceImagePath;
        return 81;
    }
    window.resize(1600, 960);

    if (QScreen* primary = QGuiApplication::primaryScreen()) {
        QScreen* target = primary;
        const auto screens = QGuiApplication::screens();
        for (QScreen* screen : screens) {
            if (screen != primary && screen->geometry().x() > primary->geometry().x()) {
                target = screen;
                break;
            }
        }
        const QRect available = target->availableGeometry();
        window.move(available.topLeft() + QPoint {24, 24});
    }

    // Size before mapping so KWin's initial configure and our baseline agree.
    // Normal-window coverage catches panel-induced geometry changes that a
    // maximized shell could otherwise conceal.
    window.show();

    QTimer::singleShot(30000, &window, [&app, &window]() {
        qCritical().noquote() << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=timeout";
        window.close();
        app.exit(3);
    });

    QTimer::singleShot(800, &window, [&app, &window, &artifactDirectory]() {
        auto* layersPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
            window.findChild<QWidget*>(QStringLiteral("LayersPanel")));
        auto* propertiesPanel = dynamic_cast<imageeditor::ui::WorkspacePanel*>(
            window.findChild<QWidget*>(QStringLiteral("PropertiesPanelShell")));
        auto* workspace = dynamic_cast<imageeditor::ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        auto* container = window.findChild<QWidget*>(QStringLiteral("VulkanCanvasContainer"));
        auto* overlay = window.findChild<QWidget*>(QStringLiteral("PanelOverlaySurface"));
        auto* panelColumn = workspace != nullptr
            ? workspace->rightPanelCard() : nullptr;
        auto* resizeHandle = workspace != nullptr
            ? workspace->panelResizeHandle() : nullptr;

        imageeditor::render::CanvasWindow* canvas = nullptr;
        for (QWindow* candidate : QGuiApplication::allWindows()) {
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow")) {
                canvas = static_cast<imageeditor::render::CanvasWindow*>(candidate);
                break;
            }
        }

        if (layersPanel == nullptr || propertiesPanel == nullptr || workspace == nullptr
            || container == nullptr || overlay == nullptr || resizeHandle == nullptr
            || panelColumn == nullptr || canvas == nullptr) {
            qCritical().noquote() << "WAYLAND_DOCK_DAMAGE_PROBE_RESULT passed=false reason=missing-ui";
            window.close();
            app.exit(2);
            return;
        }

        auto* controller = new ProbeController(
            app, window, *workspace, *layersPanel, *propertiesPanel, *container,
            *overlay, *resizeHandle, *panelColumn, *canvas,
            artifactDirectory.path());
        controller->start();
    });

    return app.exec();
}
