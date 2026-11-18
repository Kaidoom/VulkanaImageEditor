#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/BrushOptionsPage.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/BrushSymmetry.hpp"
#include <QApplication>
#include <QStatusBar>
#include <QTimer>

namespace imageeditor::ui {
void MainWindow::createBrushSymmetryControls()
{
    // Native popup focus behavior differs by platform. Remember only this
    // menu's pending placement intent; ordinary focus/deactivation still
    // cancels canvas interactions immediately.
    struct MenuPlacement { DocumentInstanceId document{}; bool placing{}; };
    const auto menuPlacement = std::make_shared<MenuPlacement>();
    brushOptionsPage_->onSymmetryMenuOpened = [this, menuPlacement] {
        *menuPlacement = {activeDocumentId(), canvasWindow_->brushSymmetryPlacementActive()};
    };
    brushOptionsPage_->onSymmetryMenuClosed = [this, menuPlacement] {
        const auto pending = *menuPlacement;
        menuPlacement->placing = false;
        if (!pending.placing) return;
        // Let the selected menu action publish its new axis first. Do not
        // resume after a tab switch, tool switch, dialog, or application exit.
        QTimer::singleShot(0, this, [this, pending] {
            const auto tool = session().activeTool();
            if (activeDocumentId()!=pending.document || !session().document() || !isVisible() || isMinimized()
                || QGuiApplication::applicationState()!=Qt::ApplicationActive
                || QApplication::activeModalWidget() || QApplication::activePopupWidget()
                || fileBusy_ || workspaceDialog_ || refinement_ || backgroundRemoval_ || activeBrushStroke_
                || (tool!=core::ToolId::Brush && tool!=core::ToolId::Eraser)
                || canvasWindow_->pointerGestureActive() || canvasWindow_->brushSymmetryPlacementActive()) return;
            canvasWindow_->beginBrushSymmetryPlacement();
            refreshBrushSymmetryControls();
        });
    };
    brushOptionsPage_->onSymmetryToggled = [this](bool enabled) {
        if (!session().document()) return;
        auto symmetry = session().document()->brushSymmetry();
        if (canvasWindow_->brushSymmetryPlacementActive())
            canvasWindow_->cancelBrushSymmetryPlacement();
        if (enabled && !symmetry.center) {
            canvasWindow_->beginBrushSymmetryPlacement();
            refreshBrushSymmetryControls();
            return;
        }
        symmetry.enabled = enabled;
        changeBrushSymmetry(symmetry);
    };
    brushOptionsPage_->onSymmetryAxisChanged = [this](core::BrushSymmetryAxis axis) {
        if (!session().document()) return;
        const bool placing = canvasWindow_->brushSymmetryPlacementActive();
        canvasWindow_->cancelBrushSymmetryPlacement();
        auto symmetry = session().document()->brushSymmetry();
        symmetry.axis = axis;
        changeBrushSymmetry(symmetry);
        if (placing) canvasWindow_->beginBrushSymmetryPlacement();
        refreshBrushSymmetryControls();
    };
    brushOptionsPage_->onSymmetryReposition = [this] {
        if (!session().document()) return;
        canvasWindow_->beginBrushSymmetryPlacement();
        refreshBrushSymmetryControls();
    };
    canvasWindow_->onBrushSymmetryPlaced = [this](core::BrushSymmetry symmetry) {
        changeBrushSymmetry(symmetry);
    };
    canvasWindow_->onBrushSymmetryPlacementEnded = [this] { refreshBrushSymmetryControls(); };
    canvasWindow_->onBrushSymmetryPlacementBegan = [this] {
        brushSymmetrySnapping_.clear();
        if (const auto* document=session().document())
            brushSymmetrySnapping_.begin(*document, {{0,0},{0,0}}, {});
    };
    canvasWindow_->onBrushSymmetryTargetMoved = [this](core::Vec2d point, Qt::KeyboardModifiers modifiers) {
        if (!session().document()) return core::SnapResult{point,{}};
        auto options=snappingOptions_;
        options.logicalScale=canvasWindow_->zoom();
        options.bypass|=modifiers.testFlag(Qt::ControlModifier);
        return brushSymmetrySnapping_.resolve(*session().document(),point,options);
    };
    refreshBrushSymmetryControls();
}

void MainWindow::changeBrushSymmetry(const core::BrushSymmetry& symmetry)
{
    auto* document = session().document();
    if (!document || fileBusy_ || workspaceDialog_ || refinement_ || backgroundRemoval_ || activeBrushStroke_)
        return;
    try {
        if (executeDocumentCommand(std::make_unique<core::ChangeBrushSymmetryCommand>(document->brushSymmetry(), symmetry))) {
            fileState().untouched = false;
            synchronizeUi(false, false);
        } else refreshBrushSymmetryControls();
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
        refreshBrushSymmetryControls();
    }
}

void MainWindow::refreshBrushSymmetryControls()
{
    if (!brushOptionsPage_ || !canvasWindow_) return;
    canvasWindow_->setBrushSymmetryContextEnabled(!refinement_ && !backgroundRemoval_);
    const auto* document = session().document();
    const auto tool = session().activeTool();
    const bool available = document && (tool == core::ToolId::Brush || tool == core::ToolId::Eraser)
        && !fileBusy_ && !workspaceDialog_ && !refinement_ && !backgroundRemoval_ && !activeBrushStroke_
        && !canvasWindow_->scene().repairProcessing;
    // Stroke completion refreshes the shell before CanvasWindow releases its
    // pointer latch. The authoritative stroke object, above, owns availability;
    // a transient native pointer flag must not leave this control disabled.
    brushOptionsPage_->setSymmetry(document ? document->brushSymmetry() : core::BrushSymmetry{},
        available, canvasWindow_->brushSymmetryPlacementActive());
}
} // namespace imageeditor::ui
