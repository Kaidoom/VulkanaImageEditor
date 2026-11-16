#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QListView>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolButton>

namespace imageeditor::ui {
void MainWindow::createMirroredLayerActions()
{
    constexpr std::array names{"CreateMirroredLayerAction", "PromoteMirroredLayerAction",
        "SyncMirroredLayerAction", "SelectMirrorSourceAction"};
    constexpr std::array labels{"layers.mirror.create", "layers.mirror.promote",
        "layers.mirror.sync", "layers.mirror.select.source"};
    constexpr std::array icons{ToolGlyph::Link, ToolGlyph::LayerRaster,
        ToolGlyph::Refresh, ToolGlyph::ShowSource};
    for (std::size_t i = 0; i < mirrorActions_.size(); ++i) {
        auto* action = i18n::make<QAction>(toolGlyph(icons[i]), i18n::text(labels[i]), this);
        action->setObjectName(QString::fromLatin1(names[i]));
        connect(action, &QAction::triggered, this, [this, i] { changeMirroredLayer(int(i)); });
        registerEditorWindowAction(action);
        mirrorActions_[i] = action;
    }
}

void MainWindow::refreshMirroredLayerActions()
{
    if (!mirrorActions_[0]) return;
    const auto* doc = session().document();
    const auto* layer = doc && session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
    const bool mirrored = layer && core::isMirroredLayer(*layer);
    const bool available = doc && layer && session().selectedLayers().size() == 1
        && !fileBusy_ && !workspaceDialog_ && !refinement_ && !backgroundRemoval_
        && !activeFill_ && !activeBrushStroke_ && !activeCloneStroke_ && !activeLocalBlurStroke_
        && !activeSpotHealStroke_ && !cloneProcessing_ && !localBlurProcessing_
        && !layerTransform_ && !selectionTransform_ && !layerCrop_ && !canvasResize_
        && !spotHealBusyForActiveDocument() && !canvasWindow_->pointerGestureActive();
    mirrorActions_[0]->setEnabled(available && core::canMirrorLayer(*layer));
    mirrorActions_[1]->setEnabled(available && mirrored);
    mirrorActions_[2]->setEnabled(available && mirrored);
    mirrorActions_[3]->setEnabled(available && mirrored && doc->layer(layer->mirror->source));
    if (opacityLinkButton_) {
        const QSignalBlocker block(opacityLinkButton_);
        opacityLinkButton_->setVisible(mirrored);
        opacityLinkButton_->setEnabled(available && mirrored);
        opacityLinkButton_->setChecked(mirrored && !(pixelOpacityMode_
            ? layer->mirror->fillOpacityOverride : layer->mirror->opacityOverride));
        const auto hint = i18n::text(pixelOpacityMode_
            ? "layers.mirror.link.fill" : "layers.mirror.link.opacity");
        i18n::setToolTip(opacityLinkButton_, hint);
        i18n::setAccessibleName(opacityLinkButton_, hint);
    }
}

void MainWindow::changeMirroredLayer(int operation)
{
    refreshMirroredLayerActions();
    if (operation < 0 || std::size_t(operation) >= mirrorActions_.size()
        || !mirrorActions_[std::size_t(operation)]->isEnabled()) return;
    const auto instance = activeDocumentId();
    const auto selected = session().layerSelectionState();
    if (!settleForFileOperation() || instance != activeDocumentId()
        || selected != session().layerSelectionState()) return;
    try {
        auto& doc = *session().document();
        if (operation == 3) {
            const auto* mirror = doc.layer(*session().activeLayer());
            if (!mirror || !mirror->mirror || !doc.layer(mirror->mirror->source)) return;
            const auto id = mirror->mirror->source;
            const std::array ids{id};
            session().setLayerSelection(ids, id);
            session().setEditingLayerMask(false);
            layerModel_->revealItem(id);
            synchronizeUi(false, false);
            const auto row = layerModel_->rowForLayer(id);
            if (row >= 0) layerList_->scrollTo(layerModel_->index(row), QAbstractItemView::EnsureVisible);
            return;
        }
        std::unique_ptr<core::Command> command;
        if (operation == 0) command = core::createMirroredLayer(doc, selected);
        else if (operation == 1) command = core::promoteMirroredLayer(doc, selected);
        else command = std::make_unique<core::SyncMirroredLayerCommand>(*session().activeLayer(), core::MirrorSync::All);
        if (executeDocumentCommand(std::move(command))) {
            session().setEditingLayerMask(false);
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::syncMirroredOpacity()
{
    opacitySlider_->finishEditing();
    activeOpacityMergeKey_ = 0;
    const auto* layer = session().document() && session().activeLayer()
        ? session().document()->layer(*session().activeLayer()) : nullptr;
    if (!layer || !layer->mirror || fileBusy_ || workspaceDialog_) return;
    try {
        if (executeDocumentCommand(std::make_unique<core::SyncMirroredLayerCommand>(layer->id,
                pixelOpacityMode_ ? core::MirrorSync::FillOpacity : core::MirrorSync::Opacity))) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        } else refreshMirroredLayerActions();
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
        refreshMirroredLayerActions();
    }
}
} // namespace imageeditor::ui
