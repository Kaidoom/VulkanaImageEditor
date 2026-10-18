#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/NavigatorPanel.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>

namespace imageeditor::ui {
void MainWindow::createMeasureControls()
{
    auto* page = new QWidget;
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0); row->setSpacing(12);
    row->addStretch();
    measureBoundsLabel_ = i18n::make<QLabel>(i18n::text("main.window.measure.bounds.w.h"), page);
    measureBoundsLabel_->setObjectName(QStringLiteral("MeasureBounds"));
    i18n::setToolTip(measureBoundsLabel_, i18n::text("main.window.measure.document.aligned.bounds.of.selected.layers.groups.including.hidden"));
    row->addWidget(measureBoundsLabel_);
    row->addStretch();
    toolOptionsBar_->registerToolPage(core::ToolId::Measure, i18n::text("main.window.measure.measure"), page);
    QVBoxLayout* content {};
    propertiesPanel_->addToolPage(core::ToolId::Measure, i18n::text("main.window.measure.measure"), i18n::text("main.window.measure.drag.a.b.to.measure.document.pixels.including.outside"), content);

}
void MainWindow::refreshMeasureBounds()
{
    measureBoundsRevision_ = session().document() ? session().document()->revision() : 0;
    measureBounds_ = session().document() ? core::selectedLayerBounds(*session().document(), session().selectedLayers())
                                       : std::optional<core::DocumentBounds> {};
    if (measureBoundsLabel_) {
        const auto size = measureBounds_ ? measureBounds_->extent() : core::Extent2d {};
        const auto text = measureBounds_ ? i18n::text("main.window.measure.bounds.w.h.1.2.px")
            .arg(size.width, 0, 'f', 1).arg(size.height, 0, 'f', 1) : i18n::text("main.window.measure.bounds.w.h");
        if (measureBoundsLabel_->text() != text) measureBoundsLabel_->setText(text);
    }
    refreshSelectionStatus();
    refreshRulerView();
}
void MainWindow::refreshSelectionStatus()
{
    if (!selectionStatus_) return;
    QString text;
    const auto* document = session().document();
    const auto& selected = session().selectedLayers();
    if (document && selected.size() > 1) {
        const bool containsContainer = std::ranges::any_of(selected,
            [document](auto id) { return document->tree().container(id) != nullptr; });
        text = (containsContainer ? i18n::text("main.window.measure.1.selected.items") : i18n::text("main.window.measure.1.selected.layers"))
            .arg(selected.size());
    } else if (document && selected.size() == 1) {
        QString name, type;
        if (const auto* layer = document->layer(selected.front())) {
            name = QString::fromStdString(layer->name);
            type = std::holds_alternative<core::RasterLayer>(layer->payload) ? QStringLiteral("Raster")
                : std::holds_alternative<core::TextLayer>(layer->payload) ? QStringLiteral("Text") : QStringLiteral("Shape");
        } else if (const auto* container = document->tree().container(selected.front())) {
            name = QString::fromStdString(container->name);
            type = container->kind == core::ContainerKind::ClippingMaskGroup ? i18n::text("main.window.measure.clipping.mask.group")
                : container->kind == core::ContainerKind::Group ? QStringLiteral("Group") : QStringLiteral("Folder");
        }
        if (!type.isEmpty()) {
            text = QStringLiteral("%1 (%2)").arg(name.simplified(), type);
            if (measureBounds_) {
                const auto size = measureBounds_->extent();
                text += i18n::text("main.window.measure.1.2.px").arg(size.width, 0, 'f', 1).arg(size.height, 0, 'f', 1);
            }
        }
    }
    if (selectionStatus_->text() != text) {
        selectionStatus_->setText(text);
        i18n::setToolTip(selectionStatus_, text.isEmpty() ? QString{} : text
            + i18n::text("main.window.measure.document.aligned.bounds.w.h.including.hidden.group.folder"));
    }
}
void MainWindow::refreshRulerView()
{
    if (!workspace_ || !canvasWindow_) return;
    // Geometry gestures publish live snapshots without rebuilding the Layers
    // UI. Refresh their bounds once per document revision, never per idle frame
    // or paint dab. Other structural/selection edits use updateLayerControls.
    if (session().document() && session().document()->revision() != measureBoundsRevision_
        && (layerTransform_ || layerCrop_ || activeLayerMove_ || shapeResize_)) {
        refreshMeasureBounds(); return;
    }
    const auto& scene = canvasWindow_->scene();
    auto tooltipArea = workspace_->rulerContentRect();
    if (workspace_->rulerVisible(Qt::Horizontal)) {
        const int height = workspace_->rulerStrip(Qt::Horizontal)->height();
        if (workspace_->rulerFarEdge(Qt::Horizontal)) tooltipArea.adjust(0, 0, 0, -height);
        else tooltipArea.adjust(0, height, 0, 0);
    }
    if (workspace_->rulerVisible(Qt::Vertical)) {
        const int width = workspace_->rulerStrip(Qt::Vertical)->width();
        if (workspace_->rulerFarEdge(Qt::Vertical)) tooltipArea.adjust(0, 0, -width, 0);
        else tooltipArea.adjust(width, 0, 0, 0);
    }
    canvasWindow_->setPointerTooltipArea({tooltipArea.x(), tooltipArea.y(), tooltipArea.width(), tooltipArea.height()});
    if(navigatorPanel_)navigatorPanel_->setView(scene.viewport,
        {double(canvasWindow_->width()),double(canvasWindow_->height())},tooltipArea);
    std::optional<core::Vec2d> pointer;
    if (scene.cursorInside) pointer = canvasWindow_->documentPositionForLogical({scene.cursorLogical.x, scene.cursorLogical.y});
    workspace_->setRulerView(scene.viewport,
        {double(scene.document.canvas.extent.width), double(scene.document.canvas.extent.height)},
        scene.logicalViewport, measureBounds_, pointer);
}
void MainWindow::presentTemporaryMeasure(bool active)
{
    auto tool = active ? core::ToolId::Measure : session().activeTool();
    const auto action = toolActionMap_.find(tool);
    if (action != toolActionMap_.end()) action->second->setChecked(true);
    if (tool == core::ToolId::Eraser) tool = core::ToolId::Brush;
    toolOptionsBar_->setActiveTool(tool);
    propertiesPanel_->setActiveTool(tool);
    if (active) refreshMeasureBounds();
}
}
