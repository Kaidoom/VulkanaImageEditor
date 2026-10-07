#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/RulerStrip.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFocusEvent>
#include <QLabel>
#include <QLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QSplitter>
#include <QTabBar>
#include <QToolBar>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using imageeditor::ui::OverlayDockWorkspace;
using imageeditor::ui::WorkspacePanel;

int failures = 0;

class PaintCounter final : public QObject {
public:
    int count {0};

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched && event && event->type() == QEvent::Paint) {
            ++count;
        }
        return false;
    }
};

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void settleLayout()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

WorkspacePanel* makePanel(const QString& title)
{
    auto* content = new QLabel(title + QStringLiteral(" content"));
    content->setMinimumSize(180, 100);
    return new WorkspacePanel(title, content);
}

void checkCanvasFillsWorkspace(const OverlayDockWorkspace& workspace)
{
    const auto* canvasContainer = workspace.canvasContainer();
    CHECK(canvasContainer != nullptr);
    if (!canvasContainer) {
        return;
    }
    CHECK(canvasContainer->geometry() == workspace.rect());
    CHECK(canvasContainer->size() == workspace.size());
}

void panelGeometryIsIndependentFromCanvas()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* panel = makePanel(QStringLiteral("Layers"));
    workspace.addPanel(panel, OverlayDockWorkspace::PanelDockSide::Right);
    workspace.resize(1200, 720);
    workspace.show();
    settleLayout();

    auto* handle = workspace.panelResizeHandle();
    auto* panelCard = workspace.rightPanelCard();
    auto* panelOverlay = workspace.panelOverlay();
    CHECK(handle != nullptr);
    CHECK(panelCard != nullptr);
    CHECK(panelOverlay != nullptr);
    if (!handle || !panelCard || !panelOverlay) {
        return;
    }

    checkCanvasFillsWorkspace(workspace);
    const QRect fixedCanvasGeometry = workspace.canvasContainer()->geometry();
    QSet<int> observedCardWidths;
    QSet<int> observedCardLeftEdges;

    constexpr std::array requestedWidths {-100, 228, 310, 480, 640, 900};
    for (const int requestedWidth : requestedWidths) {
        workspace.setPanelWidth(requestedWidth);

        // Geometry follows the drag synchronously; waiting for a queued layout
        // is visible as lag at high refresh rates.
        const int expectedWidth = std::clamp(requestedWidth, 228, 640);
        CHECK(panelCard->width() == expectedWidth);
        CHECK(panelOverlay->geometry() == workspace.rect());
        CHECK(panelCard->geometry()
            == QRect(workspace.width() - 10 - expectedWidth, 10,
                     expectedWidth, workspace.height() - 20));
        CHECK(handle->geometry()
            == QRect(panelCard->geometry().left() - 6, 10,
                     12, workspace.height() - 20));
        CHECK(std::abs(handle->mapTo(panelOverlay, handle->rect().center()).x()
                - panelCard->geometry().left()) <= 1);
        CHECK(handle->cursor().shape() == Qt::SizeHorCursor);
        CHECK(panelOverlay->mask().isEmpty());
        CHECK(panelOverlay->isWindow());
        CHECK(panelOverlay->testAttribute(Qt::WA_TranslucentBackground));
        CHECK(!panelOverlay->testAttribute(Qt::WA_OpaquePaintEvent));
        CHECK(panelOverlay->backingStore() != workspace.backingStore());
        CHECK(panelOverlay->windowHandle() != nullptr);
        if (panelOverlay->windowHandle()) {
            const QRegion inputRegion = panelOverlay->windowHandle()->mask();
            CHECK(inputRegion.contains(handle->geometry().center()));
            CHECK(inputRegion.contains(panelCard->geometry().center()));
            CHECK(!inputRegion.contains(QPoint(4, workspace.height() / 2)));
        }
        settleLayout();

        CHECK(workspace.panelWidth() == expectedWidth);
        CHECK(panelCard->width() == expectedWidth);
        CHECK(workspace.canvasContainer()->geometry() == fixedCanvasGeometry);
        checkCanvasFillsWorkspace(workspace);
        observedCardWidths.insert(panelCard->width());
        observedCardLeftEdges.insert(panelCard->geometry().left());
    }

    CHECK(observedCardWidths.size() >= 4);
    CHECK(observedCardLeftEdges.size() >= 4);

    workspace.setPanelVisible(panel, false);
    settleLayout();
    CHECK(!panelOverlay->isVisible());
    checkCanvasFillsWorkspace(workspace);

    workspace.setPanelWidth(480);
    workspace.resize(1440, 820);
    settleLayout();
    CHECK(!panelOverlay->isVisible());
    CHECK(workspace.panelWidth() == 480);
    checkCanvasFillsWorkspace(workspace);

    workspace.setPanelVisible(panel, true);
    settleLayout();
    CHECK(panelOverlay->isVisible());
    CHECK(handle->isVisible());
    CHECK(panelCard->isVisible());
    CHECK(panelCard->width() == 480);
    checkCanvasFillsWorkspace(workspace);

    constexpr std::array workspaceSizes {
        QSize {960, 620},
        QSize {1280, 760},
        QSize {1600, 900},
    };
    for (const auto size : workspaceSizes) {
        workspace.resize(size);
        settleLayout();
        CHECK(panelCard->width() == 480);
        CHECK(panelOverlay->geometry() == workspace.rect());
        CHECK(panelCard->geometry()
            == QRect(workspace.width() - 10 - 480, 10,
                     480, workspace.height() - 20));
        checkCanvasFillsWorkspace(workspace);
    }
}

void passiveNotificationDoesNotCreateAnInputFootprint()
{
    auto* canvas = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvas);
    auto* panel = makePanel(QStringLiteral("Layers"));
    workspace.addPanel(panel);
    auto* rail = new QToolBar;
    rail->addAction(QStringLiteral("Brush"));
    workspace.setToolRail(rail, OverlayDockWorkspace::ToolRailPlacement::Left);
    workspace.setRulerVisible(Qt::Horizontal, true);
    workspace.setRulerVisible(Qt::Vertical, true);
    workspace.resize(1100, 700);
    workspace.show();
    settleLayout();
    auto* plane = workspace.panelOverlay();
    auto* surface = plane->windowHandle();
    CHECK(surface);
    if (!surface) return;
    const auto canvasGeometry = workspace.canvasContainer()->geometry();
    const auto canvasSize = canvas->size();
    const auto pan = canvas->scene().viewport.pan();
    const auto zoom = canvas->zoom();
    const auto inputFootprint = surface->mask();
    auto* notification = new QLabel(QStringLiteral("Operation warning"), plane);
    notification->setGeometry(400, 620, 220, 32);
    workspace.setPassiveOverlay(notification, true);
    settleLayout();
    CHECK(notification->isVisible());
    CHECK(notification->testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(notification->focusPolicy() == Qt::NoFocus);
    CHECK(surface->mask() == inputFootprint);

    workspace.setPanelVisible(panel, false);
    workspace.takeToolRail(rail);
    workspace.setRulerVisible(Qt::Horizontal, false);
    workspace.setRulerVisible(Qt::Vertical, false);
    settleLayout();
    CHECK(plane->isVisible() && notification->isVisible());
    CHECK(surface->mask().isEmpty());
    // Empty masks ordinarily mean full-window input on Wayland. The explicit
    // public native flag is essential for this passive-only visible plane.
    CHECK(surface->flags().testFlag(Qt::WindowTransparentForInput));

    QWidget modal(plane);
    workspace.setModalOverlay(&modal);
    settleLayout();
    CHECK(modal.isVisible() && notification->isHidden());
    CHECK(surface->mask() == QRegion(workspace.rect()));
    CHECK(!surface->flags().testFlag(Qt::WindowTransparentForInput));
    workspace.setModalOverlay(nullptr);
    settleLayout();
    CHECK(modal.isHidden() && notification->isVisible());
    CHECK(surface->mask().isEmpty());
    CHECK(surface->flags().testFlag(Qt::WindowTransparentForInput));

    workspace.setPassiveOverlay(notification, false); // Message cleared/expired.
    settleLayout();
    CHECK(notification->isHidden() && plane->isHidden());
    workspace.setPanelVisible(panel, true);
    settleLayout();
    CHECK(plane->isVisible() && panel->isVisible());
    CHECK(!surface->flags().testFlag(Qt::WindowTransparentForInput));
    CHECK(surface->mask().contains(workspace.rightPanelCard()->geometry().center()));
    CHECK(plane->windowHandle() == surface);
    CHECK(workspace.canvasContainer()->geometry() == canvasGeometry);
    CHECK(canvas->size() == canvasSize && canvas->scene().viewport.pan() == pan && canvas->zoom() == zoom);
}

void internalFloatingPanelsRemainOwnedAndRedockable()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* layers = makePanel(QStringLiteral("Layers"));
    auto* properties = makePanel(QStringLiteral("Properties"));
    workspace.addPanel(layers, OverlayDockWorkspace::PanelDockSide::Right);
    workspace.addPanel(properties, OverlayDockWorkspace::PanelDockSide::Right);
    workspace.resize(1100, 700);
    workspace.show();
    settleLayout();

    workspace.floatPanel(layers, QRect {180, 80, 300, 260});
    workspace.floatPanel(properties, QRect {520, 120, 320, 300});
    settleLayout();

    CHECK(workspace.panelPlacement(layers)
        == OverlayDockWorkspace::PanelPlacement::Floating);
    CHECK(workspace.panelPlacement(properties)
        == OverlayDockWorkspace::PanelPlacement::Floating);
    CHECK(layers->parentWidget() == workspace.panelOverlay());
    CHECK(properties->parentWidget() == workspace.panelOverlay());
    CHECK(!layers->isWindow());
    CHECK(!properties->isWindow());
    CHECK(!layers->testAttribute(Qt::WA_NativeWindow));
    CHECK(!properties->testAttribute(Qt::WA_NativeWindow));
    CHECK(layers->internalWinId() == 0);
    CHECK(properties->internalWinId() == 0);
    CHECK(layers->windowHandle() == nullptr);
    CHECK(properties->windowHandle() == nullptr);

    // Empty adoption shelves are transient drag affordances, not idle chrome.
    CHECK(!workspace.leftPanelCard()->isVisible());
    CHECK(!workspace.rightPanelCard()->isVisible());
    CHECK(!workspace.leftPanelResizeHandle()->isVisible());
    CHECK(!workspace.panelResizeHandle()->isVisible());

    workspace.dockPanel(layers, OverlayDockWorkspace::PanelDockSide::Left);
    workspace.dockPanel(properties, OverlayDockWorkspace::PanelDockSide::Right);
    settleLayout();
    CHECK(workspace.panelPlacement(layers)
        == OverlayDockWorkspace::PanelPlacement::DockedLeft);
    CHECK(workspace.panelPlacement(properties)
        == OverlayDockWorkspace::PanelPlacement::DockedRight);
    CHECK(layers->parentWidget() != workspace.panelOverlay());
    CHECK(properties->parentWidget() != workspace.panelOverlay());
    CHECK(!layers->hasFloatingPresentation());
    CHECK(!properties->hasFloatingPresentation());
    CHECK(workspace.leftPanelResizeHandle()->isVisible());
    CHECK(workspace.panelResizeHandle()->isVisible());
}

void toolRailStaysOverlayHostedAtEveryEdge()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* toolRail = new QToolBar;
    toolRail->setObjectName(QStringLiteral("TestToolRail"));
    toolRail->setOrientation(Qt::Vertical);
    toolRail->setFixedWidth(54);
    toolRail->addAction(QStringLiteral("Move"));
    workspace.setToolRail(
        toolRail, OverlayDockWorkspace::ToolRailPlacement::Right);
    workspace.resize(1000, 640);
    workspace.show();
    settleLayout();

    auto* rightCard = workspace.rightPanelCard();
    auto* leftCard = workspace.leftPanelCard();
    auto* overlay = workspace.panelOverlay();
    CHECK(workspace.hasToolRail(toolRail));
    CHECK(workspace.toolRailPlacement()
        == OverlayDockWorkspace::ToolRailPlacement::Right);
    CHECK(rightCard->isVisible());
    CHECK(rightCard->width() == 54);
    CHECK(toolRail->parentWidget() == rightCard);
    CHECK(toolRail->isVisible());
    CHECK(toolRail->orientation() == Qt::Vertical);
    CHECK(toolRail->height() == rightCard->height());
    CHECK(!workspace.panelResizeHandle()->isVisible());
    const QRect fixedCanvasGeometry = workspace.canvasContainer()->geometry();
    const auto fixedRendererStats = canvasWindow->rendererStats();

    auto* panel = makePanel(QStringLiteral("Layers"));
    workspace.addPanel(panel);
    settleLayout();
    CHECK(workspace.panelResizeHandle()->isVisible());
    CHECK(rightCard->width() == workspace.panelWidth() + toolRail->width());
    CHECK(panel->width() >= 228);
    workspace.setPanelVisible(panel, false);
    settleLayout();
    CHECK(rightCard->width() == 54);

    workspace.setToolRail(
        toolRail, OverlayDockWorkspace::ToolRailPlacement::Left);
    settleLayout();
    CHECK(workspace.toolRailPlacement()
        == OverlayDockWorkspace::ToolRailPlacement::Left);
    CHECK(leftCard->isVisible());
    CHECK(leftCard->width() == 54);
    CHECK(toolRail->parentWidget() == leftCard);
    CHECK(toolRail->height() == leftCard->height());
    CHECK(!rightCard->isVisible());

    toolRail->setMinimumWidth(0);
    toolRail->setMaximumWidth(QWIDGETSIZE_MAX);
    toolRail->setMinimumHeight(0);
    toolRail->setMaximumHeight(QWIDGETSIZE_MAX);
    toolRail->setOrientation(Qt::Horizontal);
    toolRail->setFixedHeight(54);
    workspace.setToolRail(
        toolRail, OverlayDockWorkspace::ToolRailPlacement::Top);
    settleLayout();
    CHECK(toolRail->parentWidget() == overlay);
    CHECK(toolRail->geometry() == QRect(10, 10, 980, 54));

    workspace.setToolRail(
        toolRail, OverlayDockWorkspace::ToolRailPlacement::Bottom);
    settleLayout();
    CHECK(toolRail->parentWidget() == overlay);
    CHECK(toolRail->geometry() == QRect(10, 576, 980, 54));
    CHECK(overlay->windowHandle() != nullptr);
    CHECK(overlay->windowHandle()
        && overlay->windowHandle()->mask().contains(
            toolRail->mapTo(overlay, toolRail->rect().center())));

    CHECK(workspace.canvasContainer()->geometry() == fixedCanvasGeometry);
    const auto afterTransitions = canvasWindow->rendererStats();
    CHECK(afterTransitions.deferredResizeEvents
        == fixedRendererStats.deferredResizeEvents);
    CHECK(afterTransitions.resizeCommits == fixedRendererStats.resizeCommits);
    CHECK(afterTransitions.resizePresentationSuspends
        == fixedRendererStats.resizePresentationSuspends);
    CHECK(afterTransitions.resizePresentationResumes
        == fixedRendererStats.resizePresentationResumes);

    workspace.takeToolRail(toolRail);
    settleLayout();
    CHECK(!workspace.hasToolRail(toolRail));
    CHECK(!overlay->isVisible());
}

void floatingResizeUsesVisibleBottomRightGrip()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* panel = makePanel(QStringLiteral("Properties"));
    panel->setHeightRange(180, 280);
    workspace.addPanel(panel);
    workspace.resize(1000, 640);
    workspace.show();
    settleLayout();
    CHECK(panel->layout()->contentsMargins() == QMargins(4, 4, 4, 4));
    CHECK(!panel->resizeGrip()->isVisible());
    workspace.floatPanel(panel, QRect {220, 90, 300, 240});
    settleLayout();

    auto* grip = panel->resizeGrip();
    CHECK(grip != nullptr);
    CHECK(grip && grip->isVisible());
    CHECK(grip && grip->size() == QSize(24, 24));
    CHECK(grip && grip->cursor().shape() == Qt::SizeFDiagCursor);
    if (!grip) {
        return;
    }
    CHECK(grip->geometry().bottomRight() == panel->rect().bottomRight());

    const QRect initialGeometry = panel->geometry();
    const QPoint pressLocal = grip->rect().center();
    const QPoint pressGlobal = grip->mapToGlobal(pressLocal);
    QMouseEvent press(QEvent::MouseButtonPress,
        QPointF(pressLocal), QPointF(pressLocal), QPointF(pressGlobal),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(grip, &press);

    const QPoint movedGlobal = pressGlobal + QPoint {87, 63};
    const QPoint movedLocal = grip->mapFromGlobal(movedGlobal);
    QMouseEvent move(QEvent::MouseMove,
        QPointF(movedLocal), QPointF(movedLocal), QPointF(movedGlobal),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(grip, &move);
    CHECK(panel->pos() == initialGeometry.topLeft());
    CHECK(panel->width() == initialGeometry.width() + 87);
    CHECK(panel->height() == 280);

    QMouseEvent release(QEvent::MouseButtonRelease,
        QPointF(movedLocal), QPointF(movedLocal), QPointF(movedGlobal),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(grip, &release);
    CHECK(grip->geometry().bottomRight() == panel->rect().bottomRight());
}

void dragShowsExplicitDockTargetAndCommitsOnRelease()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* panel = makePanel(QStringLiteral("Properties"));
    workspace.addPanel(panel);
    workspace.resize(1100, 700);
    workspace.show();
    workspace.floatPanel(panel, QRect {420, 120, 320, 280});
    settleLayout();
    CHECK(!workspace.leftPanelCard()->isVisible());
    CHECK(!workspace.rightPanelCard()->isVisible());

    auto* titleBar = panel->findChild<QWidget*>(
        QStringLiteral("WorkspacePanelTitleBar"));
    auto* indicator = workspace.panelDropIndicator();
    CHECK(titleBar != nullptr);
    CHECK(indicator != nullptr);
    if (!titleBar || !indicator) {
        return;
    }

    const QPoint pressLocal = titleBar->rect().center();
    const QPoint pressGlobal = titleBar->mapToGlobal(pressLocal);
    QMouseEvent press(QEvent::MouseButtonPress,
        QPointF(pressLocal), QPointF(pressLocal), QPointF(pressGlobal),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(titleBar, &press);

    const QPoint targetGlobal = workspace.panelOverlay()->mapToGlobal(
        QPoint {90, workspace.panelOverlay()->height() / 2});
    CHECK(!workspace.leftPanelCard()->geometry().contains(
        workspace.panelOverlay()->mapFromGlobal(targetGlobal)));
    const QPoint targetLocal = titleBar->mapFromGlobal(targetGlobal);
    QMouseEvent move(QEvent::MouseMove,
        QPointF(targetLocal), QPointF(targetLocal), QPointF(targetGlobal),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(titleBar, &move);
    CHECK(workspace.leftPanelCard()->isVisible());
    CHECK(workspace.rightPanelCard()->isVisible());
    CHECK(workspace.leftPanelCard()->width() == 38);
    CHECK(workspace.rightPanelCard()->width() == 38);
    CHECK(!indicator->isHidden());
    CHECK(indicator->geometry().contains(
        workspace.leftPanelCard()->geometry()));
    CHECK(indicator->width() >= 100);

    QMouseEvent release(QEvent::MouseButtonRelease,
        QPointF(targetLocal), QPointF(targetLocal), QPointF(targetGlobal),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(titleBar, &release);
    settleLayout();
    CHECK(indicator->isHidden());
    CHECK(workspace.panelPlacement(panel)
        == OverlayDockWorkspace::PanelPlacement::DockedLeft);
    CHECK(workspace.leftPanelCard()->isVisible());
    CHECK(!workspace.rightPanelCard()->isVisible());
}

void dockedPanelsCanBeInsertedAboveAndBelow()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* first = makePanel(QStringLiteral("First"));
    auto* second = makePanel(QStringLiteral("Second"));
    workspace.addPanel(first);
    workspace.addPanel(second);
    workspace.resize(1100, 700);
    workspace.show();
    settleLayout();
    CHECK(first->y() < second->y());

    const auto dragTo = [](WorkspacePanel* panel, QPoint globalTarget) {
        auto* titleBar = panel->findChild<QWidget*>(
            QStringLiteral("WorkspacePanelTitleBar"));
        CHECK(titleBar != nullptr);
        if (!titleBar) {
            return;
        }
        const QPoint pressLocal = titleBar->rect().center();
        const QPoint pressGlobal = titleBar->mapToGlobal(pressLocal);
        QMouseEvent press(QEvent::MouseButtonPress,
            QPointF(pressLocal), QPointF(pressLocal), QPointF(pressGlobal),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(titleBar, &press);
        const QPoint targetLocal = titleBar->mapFromGlobal(globalTarget);
        QMouseEvent move(QEvent::MouseMove,
            QPointF(targetLocal), QPointF(targetLocal), QPointF(globalTarget),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(titleBar, &move);
        QMouseEvent release(QEvent::MouseButtonRelease,
            QPointF(targetLocal), QPointF(targetLocal), QPointF(globalTarget),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(titleBar, &release);
        settleLayout();
    };

    dragTo(first, first->mapToGlobal(first->rect().center()));
    CHECK(first->y() < second->y());

    dragTo(second, first->mapToGlobal(QPoint {first->width() / 2, 4}));
    CHECK(second->y() < first->y());

    dragTo(second, workspace.rightPanelCard()->mapToGlobal(
        QPoint {workspace.rightPanelCard()->width() / 2,
                workspace.rightPanelCard()->height() - 5}));
    CHECK(first->y() < second->y());
}

void threePanelOrderAndSharedSizingAreStable()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* color = makePanel(QStringLiteral("Color"));
    auto* layers = makePanel(QStringLiteral("Layers"));
    auto* properties = makePanel(QStringLiteral("Properties"));
    color->setHeightRange(120, 480);
    layers->setHeightRange(260, QWIDGETSIZE_MAX);
    properties->setHeightRange(200, QWIDGETSIZE_MAX);
    workspace.addPanel(color);
    workspace.addPanel(layers);
    workspace.addPanel(properties);
    workspace.resize(1100, 1200);
    workspace.show();
    settleLayout();

    CHECK(workspace.dockedPanelIndex(color) == 0);
    CHECK(workspace.dockedPanelIndex(layers) == 1);
    CHECK(workspace.dockedPanelIndex(properties) == 2);
    CHECK(color->minimumHeight() == 120);
    CHECK(color->maximumHeight() == 480);
    CHECK(layers->minimumHeight() == 260);
    CHECK(properties->minimumHeight() == 200);

    auto* splitter = workspace.rightPanelCard()->findChild<QSplitter*>(
        QStringLiteral("RightPanelSplitter"));
    CHECK(splitter != nullptr);
    if (splitter) {
        splitter->setSizes({300, 500, 400});
        settleLayout();
        CHECK(color->height() <= 480);
        CHECK(layers->height() >= 260);
        CHECK(properties->height() >= 200);
        const auto savedSizes = splitter->sizes();
        const auto savedState = workspace.saveDockedPanelSizes(
            OverlayDockWorkspace::PanelDockSide::Right);
        splitter->setSizes({1, 1000, 1});
        settleLayout();
        CHECK(color->height() >= 120);
        CHECK(color->height() < layers->height());
        CHECK(properties->height() >= 200);
        CHECK(workspace.restoreDockedPanelSizes(
            OverlayDockWorkspace::PanelDockSide::Right, savedState));
        settleLayout();
        const auto restoredSizes = splitter->sizes();
        CHECK(restoredSizes.size() == savedSizes.size());
        if (restoredSizes.size() == savedSizes.size()) {
            for (qsizetype index = 0; index < savedSizes.size(); ++index) {
                CHECK(std::abs(restoredSizes[index] - savedSizes[index]) <= 1);
            }
        }
    }

    workspace.dockPanel(properties,
        OverlayDockWorkspace::PanelDockSide::Right, 0);
    settleLayout();
    CHECK(workspace.dockedPanelIndex(properties) == 0);
    CHECK(workspace.dockedPanelIndex(color) == 1);
    CHECK(workspace.dockedPanelIndex(layers) == 2);

    workspace.floatPanel(color, QRect {420, 100, 320, 760});
    settleLayout();
    CHECK(workspace.dockedPanelIndex(color) == -1);
    CHECK(color->minimumHeight() == 120);
    CHECK(color->maximumHeight() == 480);
    CHECK(color->height() == 480);

    workspace.dockPanel(color,
        OverlayDockWorkspace::PanelDockSide::Right, 0);
    settleLayout();
    CHECK(workspace.dockedPanelIndex(color) == 0);
    CHECK(workspace.dockedPanelIndex(properties) == 1);
    CHECK(workspace.dockedPanelIndex(layers) == 2);
    CHECK(color->minimumHeight() == 120);
    CHECK(color->maximumHeight() == 480);

    workspace.setPanelVisible(color, false);
    settleLayout();
    CHECK(color->isHidden());
    workspace.setPanelVisible(color, true);
    settleLayout();
    CHECK(color->isVisible());
    CHECK(color->minimumHeight() == 120);
    CHECK(color->maximumHeight() == 480);
}

void workspaceMinimumContainsNativeChildren()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    workspace.resize(120, 90);
    workspace.show();
    settleLayout();

    CHECK(workspace.width() >= 344);
    CHECK(workspace.height() >= 240);
    checkCanvasFillsWorkspace(workspace);
    CHECK(workspace.rect().contains(workspace.panelOverlay()->geometry()));
}

void tabbedPanelsRetainControlsAndMoveIndependently()
{
    using Side = OverlayDockWorkspace::PanelDockSide;
    using Placement = OverlayDockWorkspace::PanelPlacement;
    auto* canvas = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvas);
    auto* a = makePanel("Layers"); a->setObjectName("LayersPanel");
    auto* b = makePanel("Properties"); b->setObjectName("PropertiesPanelShell");
    auto* c = new WorkspacePanel("Adjustments", new imageeditor::ui::AdjustmentsPanel);
    c->setObjectName("AdjustmentsPanelShell");
    auto* d = makePanel("Color"); d->setObjectName("ColorPanelShell");
    for (auto* panel : {a, b, c, d}) workspace.addPanel(panel);
    workspace.resize(1300, 1000); workspace.show(); settleLayout();
    auto* canvasHandle = canvas->handle();
    auto* plane = workspace.panelOverlay()->windowHandle();
    imageeditor::ui::CrossWindowPointerRouter router(&workspace, canvas, workspace.canvasContainer());
    const auto fixedCanvas = workspace.canvasContainer()->geometry();
    auto* content = c->contentWidget();
    const auto send = [](QWindow* window, QEvent::Type type, QPoint global) {
        const auto local = QPointF(window->mapFromGlobal(global));
        const bool move = type == QEvent::MouseMove;
        QMouseEvent event(type, local, local, QPointF(global), move ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    const auto drag = [&](QWidget* widget, QPoint press, QPoint target, const QString& kind, bool cancel = false) {
        send(plane, QEvent::MouseButtonPress, press);
        CHECK(router.captureOwner() == widget);
        // Move/release arrives through the native canvas, not the tab's plane.
        send(canvas, QEvent::MouseMove, target);
        auto* headerFrame = dynamic_cast<WorkspacePanel*>(widget->parentWidget());
        if (qobject_cast<QTabBar*>(widget) || (headerFrame && !headerFrame->hasFloatingPresentation())) {
            auto* proxy = workspace.panelOverlay()->findChild<QWidget*>("WorkspacePanelDragProxy");
            CHECK(proxy && proxy->isVisible());
            CHECK(proxy && proxy->width() <= 200 && proxy->height() <= 40);
            CHECK(proxy && proxy->testAttribute(Qt::WA_TransparentForMouseEvents));
            const auto prefix = qEnvironmentVariable("VULKANA_PANEL_TEST_CAPTURE");
            if (proxy && !prefix.isEmpty()) CHECK(proxy->grab().save(prefix + "-tab-chip.png"));
        }
        if (!kind.isEmpty()) {
            CHECK(!workspace.panelDropIndicator()->isHidden());
            CHECK(workspace.panelDropIndicator()->property("dropKind").toString() == kind);
        }
        if (cancel) {
            QEvent cancellation(QEvent::TouchCancel);
            QCoreApplication::sendEvent(widget, &cancellation);
        }
        send(canvas, QEvent::MouseButtonRelease, target);
        settleLayout();
        CHECK(router.captureDomain() == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(!canvas->pointerGestureActive());
        CHECK(workspace.panelDropIndicator()->isHidden());
        CHECK(workspace.canvasContainer()->geometry() == fixedCanvas);
        CHECK(canvas->handle() == canvasHandle);
        CHECK(workspace.panelOverlay()->windowHandle() == plane);
    };
    const auto header = [](WorkspacePanel* p) {
        return p->findChild<QWidget*>("WorkspacePanelTitleBar", Qt::FindDirectChildrenOnly);
    };
    // Center docking creates tabs; hover/cancel leaves ownership untouched.
    auto* title = header(b);
    drag(title, title->mapToGlobal(title->rect().center()), a->mapToGlobal(a->rect().center()), "new-tab", true);
    CHECK(workspace.panelTabs(a).size() == 1);
    drag(title, title->mapToGlobal(title->rect().center()), a->mapToGlobal(a->rect().center()), "new-tab");
    CHECK(workspace.panelTabs(a) == std::vector<WorkspacePanel*>({a, b}));
    CHECK(workspace.panelFrame(a) == workspace.panelFrame(b));
    CHECK(b->isVisible() && !a->isVisible());
    workspace.tabifyPanel(c, b);
    CHECK(workspace.panelTabs(a).size() == 3);
    CHECK(c->contentWidget() == content);
    CHECK(c->isVisible());
    workspace.activatePanel(a); settleLayout();
    CHECK(a->isVisible() && !c->isVisible());
    workspace.setPanelVisible(c, true); // Refreshing visibility must not change active tab.
    CHECK(a->isVisible());
    workspace.setPanelVisible(a, false); settleLayout();
    CHECK(!a->isVisible() && workspace.panelVisible(b));
    workspace.activatePanel(a); settleLayout();
    CHECK(a->isVisible());
    // A single tab moves at the indicated gap, even if initially inactive.
    auto* tabs = workspace.panelTabBar(a);
    drag(tabs, tabs->mapToGlobal(tabs->tabRect(2).center()),
        tabs->mapToGlobal(QPoint(2, tabs->height() / 2)), "tab-insertion");
    CHECK(workspace.panelTabs(a) == std::vector<WorkspacePanel*>({c, a, b}));
    // The header moves the complete frame; tabs are not recreated.
    auto* frame = workspace.panelFrame(a);
    title = header(frame);
    drag(title, title->mapToGlobal(title->rect().center()),
        workspace.panelOverlay()->mapToGlobal(QPoint(540, 160)), {});
    CHECK(workspace.panelPlacement(c) == Placement::Floating);
    CHECK(workspace.panelFrame(a) == frame && workspace.panelTabBar(a) == tabs);
    CHECK(!frame->isWindow() && !a->isWindow() && !c->isWindow());
    // Explicitly render the floating surface: margins above the tabs are opaque.
    workspace.activatePanel(c); settleLayout();
    CHECK(frame->grab().toImage().pixelColor(5, 45).alpha() == 255);
    const auto capturePrefix = qEnvironmentVariable("VULKANA_PANEL_TEST_CAPTURE");
    if (!capturePrefix.isEmpty()) CHECK(frame->grab().save(capturePrefix + "-tabs.png"));
    const auto saved = workspace.savePanelTabs();
    const auto floating = workspace.floatingPanelGeometry(a);
    // A tab can float independently; cancelling it first leaves the frame intact.
    drag(tabs, tabs->mapToGlobal(tabs->tabRect(2).center()),
        workspace.panelOverlay()->mapToGlobal(QPoint(650, 750)), {}, true);
    CHECK(workspace.panelTabs(a).size() == 3);
    drag(tabs, tabs->mapToGlobal(tabs->tabRect(2).center()),
        workspace.panelOverlay()->mapToGlobal(QPoint(650, 750)), {});
    CHECK(workspace.panelTabs(b).size() == 1 && workspace.panelTabs(a).size() == 2);
    CHECK(workspace.panelPlacement(b) == Placement::Floating);
    CHECK(header(b)->isVisible());
    // Last-member collapse restores the ordinary panel and its own constraints.
    workspace.detachPanel(a); settleLayout();
    CHECK(workspace.panelTabs(c).size() == 1 && header(c)->isVisible());
    CHECK(c->grab().toImage().pixelColor(5, 55).alpha() == 255);
    if (!capturePrefix.isEmpty()) CHECK(c->grab().save(capturePrefix + "-floating.png"));
    CHECK(workspace.restorePanelTabs(saved)); settleLayout();
    CHECK(workspace.panelTabs(a) == std::vector<WorkspacePanel*>({c, a, b}));
    CHECK(workspace.floatingPanelGeometry(a) == floating);
    CHECK(c->isVisible());
    // Dropping one tab at a panel's edge splits it into the existing column.
    tabs = workspace.panelTabBar(a);
    drag(tabs, tabs->mapToGlobal(tabs->tabRect(1).center()),
        d->mapToGlobal(QPoint(d->width() / 2, 3)), "dock");
    CHECK(workspace.panelPlacement(a) == Placement::DockedRight);
    CHECK(workspace.dockedPanelIndex(a) < workspace.dockedPanelIndex(d));
    CHECK(workspace.panelTabs(a).size() == 1);
    // Merge complete groups without nesting tab containers or losing controls.
    workspace.tabifyPanel(d, a);
    workspace.tabifyPanel(workspace.panelFrame(c), a);
    CHECK(workspace.panelTabs(a).size() == 4);
    CHECK(c->contentWidget() == content);
    workspace.dockPanel(c, Side::Left); settleLayout();
    for (auto* panel : {a, b, c, d}) CHECK(workspace.panelPlacement(panel) == Placement::DockedLeft);
    workspace.floatPanel(c, {420, 160, 360, 420}); settleLayout();
    const auto hiddenGeometry = workspace.floatingPanelGeometry(c);
    workspace.setPanelVisible(a, false); workspace.setPanelVisible(b, false);
    workspace.setPanelVisible(c, false); workspace.setPanelVisible(d, false); settleLayout();
    CHECK(!workspace.panelFrame(c)->isVisible());
    CHECK(workspace.floatingPanelGeometry(c) == hiddenGeometry);
    workspace.activatePanel(c); settleLayout();
    CHECK(c->isVisible() && !a->isVisible());
    CHECK(workspace.canvasContainer()->geometry() == fixedCanvas);
    CHECK(canvas->handle() == canvasHandle);
}

void internallyFloatingPanelsCloseWithTheirWorkspace()
{
    QPointer<WorkspacePanel> guardedPanel;
    {
        auto* canvasWindow = new imageeditor::render::CanvasWindow;
        auto* workspace = new OverlayDockWorkspace(canvasWindow);
        auto* panel = makePanel(QStringLiteral("Layers"));
        guardedPanel = panel;
        workspace->addPanel(panel);
        workspace->resize(900, 600);
        workspace->show();
        workspace->floatPanel(panel, QRect {240, 100, 300, 260});
        settleLayout();
        CHECK(!QApplication::topLevelWidgets().contains(panel));
        CHECK(panel->parentWidget() == workspace->panelOverlay());
        delete workspace;
    }
    CHECK(guardedPanel.isNull());
    CHECK(QWidget::mouseGrabber() == nullptr);
}

void floatingPanelsParkByHeaderAndDockAtBottom()
{
    using Side = OverlayDockWorkspace::PanelDockSide;
    using Placement = OverlayDockWorkspace::PanelPlacement;
    auto* canvas = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvas);
    auto* first = makePanel("First");
    auto* second = makePanel("Second");
    auto* panel = makePanel("Floating");
    for (auto* p : {first, second, panel}) workspace.addPanel(p);
    workspace.resize(1200, 720); workspace.show(); settleLayout();
    workspace.floatPanel(panel, {340, 100, 320, 420}); settleLayout();
    auto* plane = workspace.panelOverlay()->windowHandle();
    auto* header = panel->findChild<QWidget*>("WorkspacePanelTitleBar");
    const auto original = panel->geometry();
    const auto canvasGeometry = workspace.canvasContainer()->geometry();
    imageeditor::ui::CrossWindowPointerRouter router(&workspace, canvas, workspace.canvasContainer());
    const auto send = [](QWindow* window, QEvent::Type type, QPoint global) {
        const auto local = QPointF(window->mapFromGlobal(global));
        QMouseEvent event(type, local, local, QPointF(global),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    const auto begin = [&] {
        send(plane, QEvent::MouseButtonPress, header->mapToGlobal(header->rect().center()));
        CHECK(router.captureOwner() == header);
    };
    const auto checkParked = [&](bool atBottom = true) {
        const int limit = workspace.height() - 10 - panel->headerExtent();
        CHECK(panel->y() <= limit);
        if (atBottom) CHECK(panel->y() == limit);
        CHECK(panel->geometry().bottom() > workspace.height());
        CHECK(workspace.rect().contains(QRect(header->mapTo(workspace.panelOverlay(), QPoint{}), header->size())));
        CHECK(plane->mask().subtracted(QRegion(workspace.rect())).isEmpty());
    };
    const auto low = workspace.panelOverlay()->mapToGlobal(QPoint{500, workspace.height() + 200});
    begin(); send(canvas, QEvent::MouseMove, low); settleLayout(); checkParked();
    CHECK(panel->size() == original.size());
    QEvent cancel(QEvent::TouchCancel); QCoreApplication::sendEvent(header, &cancel);
    send(canvas, QEvent::MouseButtonRelease, low); settleLayout();
    CHECK(panel->geometry() == original);

    begin(); send(canvas, QEvent::MouseMove, low); send(canvas, QEvent::MouseButtonRelease, low);
    settleLayout(); checkParked();
    CHECK(workspace.panelPlacement(panel) == Placement::Floating);
    // A resize keeps the header reachable; it does not bottom-anchor a panel
    // when the window manager subsequently gives the workspace more room.
    workspace.resize(1200, 640); settleLayout(); checkParked(false);
    const auto parked = workspace.floatingPanelGeometry(panel);
    workspace.floatPanel(panel, parked); settleLayout(); // Saved geometry remains reachable.
    checkParked(false);
    begin();
    const auto up = workspace.panelOverlay()->mapToGlobal(QPoint{500, 160});
    send(canvas, QEvent::MouseMove, up); send(canvas, QEvent::MouseButtonRelease, up); settleLayout();
    CHECK(panel->geometry().bottom() < workspace.height());

    begin();
    const auto bottom = workspace.rightPanelCard()->mapToGlobal(QPoint{workspace.rightPanelCard()->width()/2,
        workspace.rightPanelCard()->height()-5});
    send(canvas, QEvent::MouseMove, bottom);
    CHECK(!workspace.panelDropIndicator()->isHidden());
    send(canvas, QEvent::MouseButtonRelease, bottom); settleLayout();
    CHECK(workspace.panelPlacement(panel) == Placement::DockedRight);
    CHECK(workspace.dockedPanelIndex(panel) == 2);
    CHECK(workspace.panelTabs(panel).size() == 1);
    CHECK(router.captureDomain() == imageeditor::ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(!canvas->pointerGestureActive());
    CHECK(workspace.canvasContainer()->geometry().size() == workspace.size());
    CHECK(workspace.canvasContainer()->geometry().topLeft() == canvasGeometry.topLeft());
    CHECK(workspace.panelOverlay()->windowHandle() == plane);

    // The header limit also applies to complete tab groups.
    workspace.tabifyPanel(second, first);
    workspace.floatPanel(first, {340, 10000, 320, 420}); settleLayout();
    auto* group = workspace.panelFrame(first);
    CHECK(group->y() == workspace.height()-10-group->headerExtent());
    CHECK(workspace.panelTabs(first).size() == 2);
    workspace.dockPanel(first, Side::Right); settleLayout();
    CHECK(workspace.panelTabs(first).size() == 2);
}

void dockTargetsUseProportionalThirds()
{
    using Side = OverlayDockWorkspace::PanelDockSide;
    for (const bool grouped : {false, true}) {
        auto* canvas = new imageeditor::render::CanvasWindow;
        OverlayDockWorkspace workspace(canvas);
        auto* source = makePanel("Source");
        auto* target = makePanel("Target");
        workspace.addPanel(source, Side::Left);
        workspace.addPanel(target, Side::Right);
        if (grouped) {
            auto* other = makePanel("Other");
            workspace.addPanel(other, Side::Right);
            workspace.tabifyPanel(other, target);
        }
        workspace.show();
        for (const int height : {440, 1000}) {
            workspace.resize(1200, height); settleLayout();
            // Native window managers acknowledge geometry asynchronously. Drain
            // those configure events before recording drag coordinates.
            if (QApplication::platformName() != "offscreen") {
                QEventLoop configured;
                QTimer::singleShot(80, &configured, &QEventLoop::quit);
                configured.exec(); settleLayout();
            }
            auto* frame = workspace.panelFrame(target);
            auto* header = source->findChild<QWidget*>("WorkspacePanelTitleBar");
            const auto press = header->mapToGlobal(header->rect().center());
            const auto offset = source->mapFromGlobal(press);
            const auto sourceGeometry = source->geometry();
            for (const auto fraction : {0.30, 0.40, 0.50, 0.60, 0.70}) {
                const auto point = frame->mapToGlobal(QPoint(frame->width()/2, int(frame->height()*fraction)));
                source->onDragStarted(source, press, offset);
                source->onDragMoved(source, point);
                auto* proxy = workspace.panelOverlay()->findChild<QWidget*>("WorkspacePanelDragProxy");
                CHECK(proxy && proxy->isVisible() && proxy->width() <= 200 && proxy->height() <= 40);
                const auto* indicator = workspace.panelDropIndicator();
                CHECK(indicator->isVisible());
                CHECK(indicator->property("dropKind").toString()
                    == (fraction > 1.0/3.0 && fraction < 2.0/3.0 ? "new-tab" : "dock"));
                CHECK(source->geometry() == sourceGeometry);
                source->onDragFinished(source, point, true); settleLayout();
                CHECK(workspace.panelTabs(target).size() == (grouped ? 2u : 1u));
                CHECK(workspace.panelPlacement(source) == OverlayDockWorkspace::PanelPlacement::DockedLeft);
            }
            // The lower third must also commit as a separate panel, not a tab.
            const auto point = frame->mapToGlobal(QPoint(frame->width()/2, frame->height()*7/10));
            source->onDragStarted(source, press, offset);
            source->onDragMoved(source, point);
            source->onDragFinished(source, point, false); settleLayout();
            CHECK(workspace.dockedPanelIndex(source) == 1);
            CHECK(workspace.panelTabs(source).size() == 1);
            CHECK(workspace.panelTabs(target).size() == (grouped ? 2u : 1u));
            workspace.dockPanel(source, Side::Left); settleLayout();
        }
    }
}

void dockedFramesUseSquareCorners()
{
    auto* canvas = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvas);
    auto* color = new WorkspacePanel("Color", new imageeditor::ui::ColorPanel);
    auto* swatches = makePanel("Swatches");
    auto* lower = makePanel("Properties");
    workspace.addPanel(color);
    workspace.addPanel(swatches);
    workspace.addPanel(lower);
    workspace.tabifyPanel(swatches, color);
    workspace.activatePanel(color);
    workspace.setPanelWidth(551);
    workspace.resize(1200, 720);
    workspace.show(); settleLayout();
    auto* card = workspace.rightPanelCard();
    auto* frame = workspace.panelFrame(color);
    auto image = card->grab().toImage();
    const auto prefix = qEnvironmentVariable("VULKANA_PANEL_TEST_CAPTURE");
    if (!prefix.isEmpty()) CHECK(image.save(prefix + "-docked-corners.png"));
    // Docked frame corners meet the divider without exposing curved cutouts.
    // Grouped and standalone panels retain their inset border and content.
    const qreal dpr = image.devicePixelRatio();
    const auto at = [&](QPoint p) {
        return image.pixelColor(qFloor((p.x()+0.5)*dpr), qFloor((p.y()+0.5)*dpr));
    };
    const auto checkCorner = [&](WorkspacePanel* panel) {
        const auto corner = panel->mapTo(card, QPoint(0, panel->height()-1));
        const auto border = at(corner + QPoint(panel->width()/2, 0));
        const auto background = imageeditor::ui::themeTone("#151820");
        CHECK(at(corner) != background);
        CHECK(border != background);
        // Fractional-DPI borders can cover the corner and straight edge by
        // different subpixel amounts. Both must still reach the square corner.
        if (dpr == std::floor(dpr)) CHECK(at(corner) == border);
    };
    checkCorner(frame);
    checkCorner(lower);
    CHECK(frame->layout()->contentsMargins() == QMargins(4, 4, 4, 4));
    const auto* grip = frame->findChild<QWidget*>("WorkspacePanelResizeGrip");
    CHECK(grip && !grip->isVisible());
    workspace.floatPanel(color, {200, 90, 551, frame->height()}); settleLayout();
    CHECK(workspace.panelFrame(color) == frame && grip && grip->isVisible());
    const auto floatingImage = frame->grab().toImage();
    CHECK(floatingImage.pixelColor(0, 0) != floatingImage.pixelColor(floatingImage.width()/2, 0));
    if (!prefix.isEmpty()) CHECK(floatingImage.save(prefix + "-floating-corners.png"));
    workspace.dockPanel(color, OverlayDockWorkspace::PanelDockSide::Right, 0); settleLayout();
    image = card->grab().toImage();
    checkCorner(frame);
}

void panelResizeHandleTracksGlobalPointer()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* panel = makePanel(QStringLiteral("Layers"));
    workspace.addPanel(panel);
    workspace.resize(1200, 720);
    workspace.show();
    settleLayout();

    auto* handle = workspace.panelResizeHandle();
    auto* overlay = workspace.panelOverlay();
    CHECK(handle != nullptr);
    CHECK(overlay != nullptr);
    if (!handle || !overlay) {
        return;
    }

    const int initialWidth = workspace.panelWidth();
    const QPoint pressLocal = handle->rect().center();
    const QPoint pressGlobal = handle->mapToGlobal(pressLocal);
    QMouseEvent press(QEvent::MouseButtonPress,
        QPointF(pressLocal), QPointF(pressLocal), QPointF(pressGlobal),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &press);

    const QPoint growGlobal = pressGlobal - QPoint {173, 0};
    const QPoint growLocal = handle->mapFromGlobal(growGlobal);
    QMouseEvent grow(QEvent::MouseMove,
        QPointF(growLocal), QPointF(growLocal), QPointF(growGlobal),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &grow);
    CHECK(workspace.panelWidth() == initialWidth + 173);
    CHECK(std::abs(handle->mapToGlobal(pressLocal).x() - growGlobal.x()) <= 1);
    CHECK(overlay->geometry() == workspace.rect());

    const QPoint shrinkGlobal = pressGlobal + QPoint {119, 0};
    const QPoint shrinkLocal = handle->mapFromGlobal(shrinkGlobal);
    QMouseEvent shrink(QEvent::MouseMove,
        QPointF(shrinkLocal), QPointF(shrinkLocal), QPointF(shrinkGlobal),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &shrink);
    CHECK(workspace.panelWidth() == std::max(228, initialWidth - 119));

    QMouseEvent release(QEvent::MouseButtonRelease,
        QPointF(shrinkLocal), QPointF(shrinkLocal), QPointF(shrinkGlobal),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &release);

    const int widthBeforeCanceledMove = workspace.panelWidth();
    const QPoint secondPressGlobal = handle->mapToGlobal(pressLocal);
    QMouseEvent secondPress(QEvent::MouseButtonPress,
        QPointF(pressLocal), QPointF(pressLocal), QPointF(secondPressGlobal),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &secondPress);
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(handle, &focusOut);
    const QPoint afterCancelGlobal = secondPressGlobal - QPoint {80, 0};
    const QPoint afterCancelLocal = handle->mapFromGlobal(afterCancelGlobal);
    QMouseEvent afterCancelMove(QEvent::MouseMove,
        QPointF(afterCancelLocal), QPointF(afterCancelLocal),
        QPointF(afterCancelGlobal), Qt::NoButton, Qt::LeftButton,
        Qt::NoModifier);
    QCoreApplication::sendEvent(handle, &afterCancelMove);
    CHECK(workspace.panelWidth() == widthBeforeCanceledMove);
    CHECK(QWidget::mouseGrabber() == nullptr);
}

void rapidPanelWidthsCoalesceBackingStorePaints()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* panel = makePanel(QStringLiteral("Layers"));
    workspace.addPanel(panel);
    workspace.resize(1400, 820);
    workspace.show();
    settleLayout();

    auto* overlay = workspace.panelOverlay();
    auto* panelCard = workspace.rightPanelCard();
    CHECK(overlay != nullptr);
    CHECK(panelCard != nullptr);
    if (!overlay || !panelCard) {
        return;
    }

    PaintCounter paintCounter;
    overlay->installEventFilter(&paintCounter);

    constexpr int kLastWidth = 497;
    for (int index = 0; index < 120; ++index) {
        const int width = index == 119
            ? kLastWidth
            : 228 + ((index * 47) % (640 - 228 + 1));
        workspace.setPanelWidth(width);
    }

    CHECK(paintCounter.count == 0);
    CHECK(workspace.panelWidth() == kLastWidth);
    CHECK(panelCard->width() == kLastWidth);

    settleLayout();
    CHECK(paintCounter.count >= 1);
    CHECK(paintCounter.count <= 2);
    overlay->removeEventFilter(&paintCounter);
}

void rulersFollowOccupiedWorkspaceChromeWithoutChangingTheCanvas()
{
    using Side = OverlayDockWorkspace::PanelDockSide;
    using Edge = OverlayDockWorkspace::ToolRailPlacement;
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    auto* left = makePanel(QStringLiteral("Arbitrary left content"));
    auto* right = makePanel(QStringLiteral("Arbitrary right content"));
    workspace.addPanel(left, Side::Left);
    workspace.addPanel(right, Side::Right);
    workspace.resize(1440, 900);
    workspace.show();
    settleLayout();

    imageeditor::core::DocumentSnapshot document;
    document.canvas.extent = {1200, 900};
    canvasWindow->setDocument(document, false);
    const QPointF zoomPoint(700, 400);
    QWheelEvent wheel(zoomPoint, canvasWindow->mapToGlobal(zoomPoint), {}, QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvasWindow, &wheel);
    const auto canvasMouse = [&](QEvent::Type type, QPointF position,
                                 Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, canvasWindow->mapToGlobal(position),
            button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow, &event);
    };
    canvasMouse(QEvent::MouseButtonPress, {620, 320}, Qt::MiddleButton, Qt::MiddleButton);
    canvasMouse(QEvent::MouseMove, {655, 301}, Qt::NoButton, Qt::MiddleButton);
    canvasMouse(QEvent::MouseButtonRelease, {655, 301}, Qt::MiddleButton, Qt::NoButton);
    settleLayout();
    CHECK(canvasWindow->zoom() != 1);
    CHECK(canvasWindow->scene().viewport.pan() != imageeditor::core::Vec2d());
    const auto initialCanvas = workspace.canvasContainer()->geometry();
    const auto initialNativeSize = canvasWindow->size();
    const auto initialZoom = canvasWindow->zoom();
    const auto initialPan = canvasWindow->scene().viewport.pan();
    const auto initialMapped = canvasWindow->documentPositionForLogical({200.5, 375.25});
    const auto initialStats = canvasWindow->rendererStats();
    const auto unchangedCanvas = [&] {
        checkCanvasFillsWorkspace(workspace);
        CHECK(workspace.canvasContainer()->geometry() == initialCanvas);
        CHECK(canvasWindow->size() == initialNativeSize);
        CHECK(canvasWindow->zoom() == initialZoom);
        CHECK(canvasWindow->scene().viewport.pan() == initialPan);
        CHECK(canvasWindow->documentPositionForLogical({200.5, 375.25}) == initialMapped);
        const auto stats = canvasWindow->rendererStats();
        CHECK(stats.swapchainGeneration == initialStats.swapchainGeneration);
        CHECK(stats.resizeCommits == initialStats.resizeCommits);
        CHECK(stats.deferredResizeEvents == initialStats.deferredResizeEvents);
        CHECK(stats.uploadedBytes == initialStats.uploadedBytes);
    };

    workspace.setRulerVisible(Qt::Horizontal, true);
    workspace.setRulerVisible(Qt::Vertical, true);
    settleLayout();
    CHECK(workspace.rulerContentRect().left() == workspace.leftPanelResizeHandle()->geometry().right() + 1);
    CHECK(workspace.rulerContentRect().right() == workspace.panelResizeHandle()->geometry().left() - 1);
    CHECK(workspace.rulerContentRect().top() == workspace.rect().top());
    CHECK(workspace.rulerContentRect().bottom() == workspace.rect().bottom());
    unchangedCanvas();

    workspace.setLeftPanelWidth(400);
    workspace.setPanelWidth(370);
    settleLayout();
    CHECK(workspace.rulerContentRect().left() == workspace.leftPanelResizeHandle()->geometry().right() + 1);
    CHECK(workspace.rulerContentRect().right() == workspace.panelResizeHandle()->geometry().left() - 1);
    unchangedCanvas();
    workspace.setPanelVisible(left, false);
    settleLayout();
    CHECK(workspace.rulerContentRect().left() == workspace.rect().left());
    workspace.floatPanel(right, QRect(420, 180, 320, 280));
    settleLayout();
    CHECK(workspace.rulerContentRect() == workspace.rect());
    const auto emptySides = workspace.rulerContentRect();
    workspace.floatPanel(right, QRect(900, 20, 350, 300));
    settleLayout();
    CHECK(workspace.rulerContentRect() == emptySides);
    unchangedCanvas();

    auto* rail = new QToolBar;
    rail->addAction(QStringLiteral("Move"));
    rail->addAction(QStringLiteral("Measure"));
    for (const auto edge : {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom}) {
        const bool vertical = edge == Edge::Left || edge == Edge::Right;
        rail->setMinimumSize(0, 0);
        rail->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        rail->setOrientation(vertical ? Qt::Vertical : Qt::Horizontal);
        if (vertical) rail->setFixedWidth(54);
        else rail->setFixedHeight(54);
        workspace.setToolRail(rail, edge);
        settleLayout();
        auto expectedArea = workspace.rect();
        if (edge == Edge::Left) expectedArea.setLeft(workspace.leftPanelCard()->geometry().right() + 1);
        if (edge == Edge::Right) expectedArea.setRight(workspace.rightPanelCard()->geometry().left() - 1);
        if (edge == Edge::Top) expectedArea.setTop(rail->geometry().bottom() + 1);
        if (edge == Edge::Bottom) expectedArea.setBottom(rail->geometry().top() - 1);
        CHECK(workspace.rulerContentRect() == expectedArea);
        for (bool bottom : {false, true}) for (bool rightEdge : {false, true}) {
            workspace.setRulerFarEdge(Qt::Horizontal, bottom);
            workspace.setRulerFarEdge(Qt::Vertical, rightEdge);
            settleLayout();
            const auto* horizontal = dynamic_cast<const imageeditor::ui::RulerStrip*>(workspace.rulerStrip(Qt::Horizontal));
            const auto* verticalStrip = dynamic_cast<const imageeditor::ui::RulerStrip*>(workspace.rulerStrip(Qt::Vertical));
            CHECK(horizontal && verticalStrip);
            if (!horizontal || !verticalStrip) continue;
            constexpr int thickness = imageeditor::ui::RulerStrip::thickness;
            CHECK(horizontal->geometry() == QRect(expectedArea.left() + (rightEdge ? 0 : thickness),
                bottom ? expectedArea.bottom() - thickness + 1 : expectedArea.top(),
                expectedArea.width() - thickness, thickness));
            CHECK(verticalStrip->geometry() == QRect(rightEdge ? expectedArea.right() - thickness + 1 : expectedArea.left(),
                expectedArea.top() + (bottom ? 0 : thickness), thickness, expectedArea.height() - thickness));
            CHECK(horizontal->ticksTowardStart() == bottom);
            CHECK(verticalStrip->ticksTowardStart() == rightEdge);
            CHECK(!horizontal->geometry().intersects(verticalStrip->geometry()));
            CHECK(workspace.panelOverlay()->windowHandle());
            if (workspace.panelOverlay()->windowHandle()) {
                const auto footprint = workspace.panelOverlay()->windowHandle()->mask();
                CHECK(footprint.contains(horizontal->geometry().center()));
                CHECK(footprint.contains(verticalStrip->geometry().center()));
            }
            unchangedCanvas();
        }
    }
    workspace.takeToolRail(rail);
    settleLayout();
    CHECK(workspace.rulerContentRect() == workspace.rect());
    for (const auto axis : {Qt::Horizontal, Qt::Vertical}) {
        workspace.setRulerVisible(axis, false);
        settleLayout();
        CHECK(!workspace.rulerStrip(axis)->isVisible());
        unchangedCanvas();
        workspace.setRulerVisible(axis, true);
        settleLayout();
        CHECK(workspace.rulerStrip(axis)->isVisible());
        unchangedCanvas();
    }
}

void rulerGripDockingCommitsOnceAndCancelsWithoutClickThrough()
{
    auto* canvasWindow = new imageeditor::render::CanvasWindow;
    OverlayDockWorkspace workspace(canvasWindow);
    workspace.addPanel(makePanel(QStringLiteral("Arbitrary dock")));
    workspace.resize(1100, 700);
    workspace.show();
    workspace.setRulerVisible(Qt::Horizontal, true);
    workspace.setRulerVisible(Qt::Vertical, true);
    settleLayout();
    const auto fixedCanvas = workspace.canvasContainer()->geometry();
    const auto fixedSize = canvasWindow->size();
    const auto fixedPan = canvasWindow->scene().viewport.pan();
    const auto fixedZoom = canvasWindow->zoom();
    int placementChanges = 0;
    workspace.onRulerPlacementChanged = [&] { ++placementChanges; };
    auto* overlay = workspace.panelOverlay();
    auto* firstHint = overlay->findChild<QWidget*>(QStringLiteral("RulerDockHint0"));
    auto* secondHint = overlay->findChild<QWidget*>(QStringLiteral("RulerDockHint1"));
    CHECK(firstHint && secondHint);
    const auto send = [](QWidget* receiver, QEvent::Type type, QPoint global,
                         Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF local(receiver->mapFromGlobal(global));
        QMouseEvent event(type, local, local, QPointF(global), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(receiver, &event);
    };
    for (const auto axis : {Qt::Horizontal, Qt::Vertical}) {
        auto* strip = dynamic_cast<imageeditor::ui::RulerStrip*>(workspace.rulerStrip(axis));
        CHECK(strip);
        if (!strip) continue;
        for (bool far : {true, false}) {
            const auto area = workspace.rulerContentRect();
            const auto target = overlay->mapToGlobal(axis == Qt::Horizontal
                ? QPoint(area.center().x(), far ? area.bottom() - 4 : area.top() + 4)
                : QPoint(far ? area.right() - 4 : area.left() + 4, area.center().y()));
            const auto press = strip->mapToGlobal(strip->gripRect().center());
            const auto oldEdge = workspace.rulerFarEdge(axis);
            const auto changes = placementChanges;
            send(strip, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
            send(strip, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
            CHECK(strip->isDragging());
            CHECK(workspace.rulerFarEdge(axis) == oldEdge);
            CHECK(firstHint && !firstHint->isHidden());
            CHECK(secondHint && !secondHint->isHidden());
            send(strip, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
            settleLayout();
            CHECK(!strip->isDragging());
            CHECK(workspace.rulerFarEdge(axis) == far);
            CHECK(placementChanges == changes + 1);
            CHECK(firstHint && firstHint->isHidden());
            CHECK(secondHint && secondHint->isHidden());
            send(strip, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
            CHECK(placementChanges == changes + 1);
        }

        const auto area = workspace.rulerContentRect();
        const auto target = overlay->mapToGlobal(area.bottomRight() - QPoint(30, 30));
        const auto press = strip->mapToGlobal(strip->gripRect().center());
        send(strip, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
        send(strip, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
        CHECK(strip->isDragging());
        // This is the same cancellation entry point used by shell Escape.
        CHECK(workspace.cancelRulerDrag());
        CHECK(!strip->isDragging());
        CHECK(!workspace.rulerFarEdge(axis));
        CHECK(!workspace.cancelRulerDrag());
        const auto afterCancel = placementChanges;
        send(strip, QEvent::MouseMove, target, Qt::NoButton, Qt::LeftButton);
        send(strip, QEvent::MouseButtonRelease, target, Qt::LeftButton, Qt::NoButton);
        CHECK(placementChanges == afterCancel);
        CHECK(firstHint && firstHint->isHidden());
        CHECK(secondHint && secondHint->isHidden());
        CHECK(workspace.canvasContainer()->geometry() == fixedCanvas);
        CHECK(canvasWindow->size() == fixedSize);
        CHECK(canvasWindow->scene().viewport.pan() == fixedPan);
        CHECK(canvasWindow->zoom() == fixedZoom);
        CHECK(!canvasWindow->pointerGestureActive());
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    imageeditor::ui::applyEditorTheme(application);

    if (application.arguments().contains("--panel-drag-only")) {
        tabbedPanelsRetainControlsAndMoveIndependently();
        floatingPanelsParkByHeaderAndDockAtBottom();
        dockTargetsUseProportionalThirds();
        std::cout << "Panel dragging: " << (failures ? "FAILED" : "passed")
                  << "; platform=" << application.platformName().toStdString() << '\n';
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (application.arguments().contains("--panel-corners-only")) {
        dockedFramesUseSquareCorners();
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    if (application.arguments().contains("--tab-groups-only")) {
        tabbedPanelsRetainControlsAndMoveIndependently();
        std::cout << "Panel tab groups: " << (failures ? "FAILED" : "passed")
                  << "; platform=" << application.platformName().toStdString() << '\n';
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    panelGeometryIsIndependentFromCanvas();
    passiveNotificationDoesNotCreateAnInputFootprint();
    internalFloatingPanelsRemainOwnedAndRedockable();
    toolRailStaysOverlayHostedAtEveryEdge();
    floatingResizeUsesVisibleBottomRightGrip();
    dragShowsExplicitDockTargetAndCommitsOnRelease();
    dockedPanelsCanBeInsertedAboveAndBelow();
    threePanelOrderAndSharedSizingAreStable();
    workspaceMinimumContainsNativeChildren();
    internallyFloatingPanelsCloseWithTheirWorkspace();
    floatingPanelsParkByHeaderAndDockAtBottom();
    dockTargetsUseProportionalThirds();
    panelResizeHandleTracksGlobalPointer();
    rapidPanelWidthsCoalesceBackingStorePaints();
    rulersFollowOccupiedWorkspaceChromeWithoutChangingTheCanvas();
    rulerGripDockingCommitsOnceAndCancelsWithoutClickThrough();
    tabbedPanelsRetainControlsAndMoveIndependently();

    dockedFramesUseSquareCorners();

    if (failures != 0) {
        std::cerr << failures << " overlay workspace assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All overlay workspace tests passed\n";
    return EXIT_SUCCESS;
}
