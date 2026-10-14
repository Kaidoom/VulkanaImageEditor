#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ScreenshotDialog.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QStatusBar>

namespace imageeditor::ui {
void MainWindow::takeScreenshot()
{
    if(fileBusy_||workspaceDialog_)return;
    if(!refinement_&&!shortcutGestureIdle(true)) {
        statusBar()->showMessage(i18n::text("main.window.files.finish.the.current.stroke.fill.or.drag.before.continuing"),4000);return;
    }
    // Capture is independent of the active document. A refinement workspace
    // can resume unchanged after Cancel or remain suspended in its own tab.
    const auto owner=activeDocument_;
    suspendRefinement();const auto restore=qScopeGuard([this]{resumeRefinement();});
    WorkspaceDialog presenter(*workspace_,*this);
    auto* backend=fileInteractions_.createScreenCapture?fileInteractions_.createScreenCapture(&presenter):nullptr;
    ScreenshotDialog dialog(*this,&presenter,backend);
    dialog.setCanAddLayer(owner&&owner->session.document());
    for(;;) {
        const auto response=[&]{const QScopedValueRollback active(workspaceDialog_,&presenter);return presenter.exec(dialog);}();
        if(response!=QDialog::Accepted)return;
        try {
            auto image=dialog.selectedImage();
            auto loaded=rasterLayerFromImage(std::move(image),i18n::text("screenshot.document"));
            if(!loaded){dialog.showError(loaded.error);continue;}
            if(dialog.output()==ScreenshotDialog::Output::Layer) {
                const auto targetCurrent=[&]{return owner&&!owner->closed&&activeDocument_==owner&&owner->session.document();};
                if(!targetCurrent()) {
                    dialog.setCanAddLayer(false);dialog.showError(QStringLiteral("The destination document is no longer available."));continue;
                }
                // A suspended refinement is still owned by the destination tab.
                // Use its normal discard guard before changing that document.
                if(!guardRefinement(owner->id))continue;
                if(!targetCurrent())continue;
                const auto layerId=loaded.layer->id;
                if(!executeDocumentCommand(std::make_unique<core::AddLayerCommand>(std::move(*loaded.layer),
                        owner->session.document()->layers().size(),owner->session.activeLayer()))) {
                    dialog.showError(QStringLiteral("Cannot add the screenshot to this document."));continue;
                }
                session().setActiveLayer(layerId);fileState().untouched=false;
                synchronizeUi(true,false);updateDocumentTitle();return;
            }
            auto document=std::make_unique<core::Document>(core::CanvasSpec{loaded.extent,96.0});
            if(!document->insertLayer(0,std::move(*loaded.layer))){dialog.showError(QStringLiteral("Cannot create the screenshot layer."));continue;}
            document->markUnsaved();
            if(!initializeDocument(std::move(document),i18n::text("screenshot.document")))return;
            fileState().untouched=false;synchronizeUi(true,true);updateDocumentTitle();return;
        }catch(const std::exception& error){dialog.showError(QString::fromUtf8(error.what()));}
    }
}
}
