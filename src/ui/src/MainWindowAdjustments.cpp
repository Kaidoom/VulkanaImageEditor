#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QListView>
#include <QAction>

namespace imageeditor::ui {
void MainWindow::addAdjustmentLayer()
{
    if(fileBusy_ || !session().document() || !settleForFileOperation())return;
    auto& doc=*session().document();
    try {
        auto tree=doc.tree();
        const auto placement=session().activeLayer()?tree.insertionAbove(*session().activeLayer()):core::ItemPlacement{0,tree.roots.size()};
        auto layer=core::Layer::adjustment("Adjustment",placement.parent?core::AdjustmentScope::ThisGroup:core::AdjustmentScope::AllBelow);
        auto mask=std::make_shared<core::LayerMask>();
        mask->coverage=doc.selection()?doc.selection():core::SelectionMask::filled(doc.canvas().extent,255);
        mask->outside=doc.selection()?0:255;
        layer.mask=std::move(mask);
        const auto id=layer.id;
        auto* siblings=tree.children(placement.parent);
        siblings->insert(siblings->begin()+std::ptrdiff_t(placement.index),id);
        if(executeDocumentCommand(std::make_unique<core::LayerStructureCommand>("New adjustment layer",doc,std::move(tree),
            std::vector<core::LayerId>{},std::vector<core::Layer>{std::move(layer)},session().layerSelectionState(),core::LayerSelectionState{{id},id}))) {
            session().setEditingLayerMask(false);
            fileState().untouched=false;
            synchronizeUi(true,false);
            adjustmentsPanelAction_->setChecked(true);
            workspace_->activatePanel(adjustmentsPanelShell_);
        }
    }catch(const std::exception& e){statusBar()->showMessage(QString::fromUtf8(e.what()),6000);}
}
void MainWindow::createAdjustmentsPanel()
{
    adjustmentsPanel_=new AdjustmentsPanel;
    adjustmentsPanelShell_=new WorkspacePanel(QStringLiteral("Adjustments"),adjustmentsPanel_);
    adjustmentsPanelShell_->setObjectName(QStringLiteral("AdjustmentsPanelShell"));
    adjustmentsPanelShell_->setHeightRange(uiLayoutConfig_.adjustments.minimum,uiLayoutConfig_.adjustments.maximum);
    workspace_->addPanel(adjustmentsPanelShell_,OverlayDockWorkspace::PanelDockSide::Right);
    adjustmentsPanel_->onInteractionStarted=[this](core::AdjustmentType type){return beginAdjustmentEdit(type);};
    adjustmentsPanel_->onPreview=[this](core::AdjustmentState state){previewAdjustmentEdit(std::move(state));};
    adjustmentsPanel_->onInteractionFinished=[this](bool commit){finishAdjustmentEdit(commit);};
    adjustmentsPanel_->onCaptureSelection=[this](core::AdjustmentType type){captureAdjustmentSelection(type);};
    adjustmentsPanel_->onComparison=[this](bool enabled){
        const auto target=adjustmentsPanel_->target();
        const auto bypass=enabled&&target&&session().activeLayer()==target?target:std::nullopt;
        canvasWindow_->setAdjustmentBypassLayer(bypass);
        if(pixelPreview_&&session().document()) {
            auto snapshot=session().document()->snapshot();
            for(auto& layer:snapshot.layersBottomToTop)if(layer.id==bypass) {
                layer.adjustments.reset();layer.filters.reset();layer.filterCache.reset();layer.effectCache.reset();
            }
            pixelPreview_->request(snapshot);
        }
    };
    adjustmentsPanel_->onHistogramRequested=[this]{
        const auto* layer=session().document()&&session().activeLayer()?session().document()->layer(*session().activeLayer()):nullptr;
        adjustmentsPanel_->requestHistogram(layer);
    };
    adjustmentsPanel_->onCapturedRegionChanged=[this]{
        if(!capturedFilterRegionActive_||!adjustmentsPanel_->capturedRegionVisible())
            canvasWindow_->setCapturedAdjustmentRegion(adjustmentsPanel_->capturedRegionVisible()
                ? adjustmentsPanel_->target() : std::nullopt, adjustmentsPanel_->currentType());
    };
}
bool MainWindow::beginAdjustmentEdit(core::AdjustmentType)
{
    if(fileBusy_||!session().document()||!session().activeLayer())return false;
    if(adjustmentEdit_)return adjustmentEdit_->active()&&adjustmentEdit_->target()==*session().activeLayer();
    // Never mutate history beneath an explicit transform/text/stroke session.
    // Completed transforms are applied using their normal command boundary;
    // an unfinished pointer gesture must finish/cancel before panel editing.
    if(canvasWindow_->pointerGestureActive()||activeBrushStroke_||activeCloneStroke_||cloneProcessing_
        ||activeLocalBlurStroke_||localBlurProcessing_||activeFill_||shapeCreation_
        ||shapeResize_||selectionGesture_||selectionRasterizer_||colorSelectionEditing_||smartInteractionActive())return false;
    if(layerTransform_||layerCrop_||selectionTransform_) {
        finishCanvasOperation();if(layerTransform_||layerCrop_||selectionTransform_)return false;
    }
    if(textController_&&textController_->active()) {
        textController_->finish();if(textController_->active())return false;
    }
    finishLayerMove(true);
    if(activeLayerMove_)return false;
    finishFilterEdit(true);if(filterEdit_)return false;
    finishEffectEdit(true);if(effectEdit_)return false;
    cancelPendingEdits();
    if(!session().document()->layer(*session().activeLayer()))return false;
    try{
        auto transaction=std::make_unique<core::AdjustmentEditTransaction>(*session().document(),*session().activeLayer());
        if(!transaction->active())return false;
        adjustmentEdit_=std::move(transaction);capturedFilterRegionActive_=false;updateActionState();return true;
    }catch(const std::exception& e){statusBar()->showMessage(QStringLiteral("Could not begin adjustment: %1").arg(QString::fromUtf8(e.what())),5000);return false;}
}
void MainWindow::previewAdjustmentEdit(core::AdjustmentState state)
{
    if(!adjustmentEdit_||!session().document()||session().activeLayer()!=adjustmentEdit_->target())return;
    try{
        if(adjustmentEdit_->update(std::move(state))) {
            // Only adjustment metadata changes. No raster rewrite, dirty upload,
            // text layout, or shape-cache regeneration is requested here.
            canvasWindow_->setDocument(session().document()->snapshot(),false);
            refreshAdjustmentPanel();
            if(layerList_)layerList_->viewport()->update();
        }else if(!adjustmentEdit_->active())finishAdjustmentEdit(false);
    }catch(const std::exception& e){statusBar()->showMessage(QStringLiteral("Adjustment preview failed: %1").arg(QString::fromUtf8(e.what())),5000);finishAdjustmentEdit(false);}
}
void MainWindow::finishAdjustmentEdit(bool commit)
{
    if(finishingAdjustmentEdit_||!adjustmentEdit_)return;
    const QScopedValueRollback guard(finishingAdjustmentEdit_,true);
    if(adjustmentsPanel_->interactionActive())adjustmentsPanel_->finishEditing(commit);
    try{
        if(commit) {
            if(!adjustmentEdit_->commit(session().history())&&adjustmentEdit_->active()){
                statusBar()->showMessage(QStringLiteral("Could not record the adjustment. Preview retained; retry or Escape to cancel."),5000);return;
            }
        }else (void)adjustmentEdit_->cancel();
        adjustmentEdit_.reset();
        if(session().document())synchronizeUi(false,false);
    }catch(const std::exception& e){statusBar()->showMessage(QStringLiteral("Could not finish adjustment: %1").arg(QString::fromUtf8(e.what())),5000);}
}
void MainWindow::captureAdjustmentSelection(core::AdjustmentType type)
{
    if(!session().document()||!session().document()->selection()||!session().activeLayer())return;
    if(!beginAdjustmentEdit(type))return;
    const auto* layer=session().document()->layer(adjustmentEdit_->target());
    if(!layer){finishAdjustmentEdit(false);return;}
    try{
        auto state=layer->adjustments?*layer->adjustments:core::AdjustmentStack{};
        state.items[std::size_t(type)].mask=core::captureAdjustmentMask(session().document()->selection(),layer->localToDocument);
        previewAdjustmentEdit(std::make_shared<const core::AdjustmentStack>(std::move(state)));
        finishAdjustmentEdit(true);
    }catch(const std::exception& e){statusBar()->showMessage(QStringLiteral("Could not capture adjustment mask: %1").arg(QString::fromUtf8(e.what())),5000);finishAdjustmentEdit(false);}
}
void MainWindow::refreshAdjustmentPanel()
{
    if(!adjustmentsPanel_)return;
    const auto* layer=session().document()&&session().activeLayer()?session().document()->layer(*session().activeLayer()):nullptr;
    adjustmentsPanel_->setTarget(layer,session().document()&&bool(session().document()->selection()),session().document(),activeDocumentId());
    adjustmentsPanel_->requestHistogram(layer);
}
} // namespace imageeditor::ui
