#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QColorDialog>
#include <QListView>
#include <QScopedValueRollback>
#include <QStatusBar>
namespace imageeditor::ui {
void MainWindow::createEffectsPanel() {
  effectsPanel_ = adjustmentsPanel_->effectsPanel();
  effectsPanel_->onInteractionStarted = [this] { return beginEffectEdit(); };
  effectsPanel_->onPreview = [this](core::LayerEffectState s) {
    previewEffectEdit(std::move(s));
  };
  effectsPanel_->onInteractionFinished = [this](bool commit) {
    finishEffectEdit(commit);
  };
  effectsPanel_->onComparison = [this](bool before) {
    canvasWindow_->setEffectBypassLayer(before ? effectsPanel_->target()
                                               : std::nullopt);
    if (pixelPreview_ && canvasWindow_->scene().pixelPreviewEnabled) {
      auto snapshot = canvasWindow_->scene().document;
      if (before)
        for (auto &layer : snapshot.layersBottomToTop)
          if (layer.id == effectsPanel_->target()) {
            layer.effects.reset();
            layer.effectCache.reset();
          }
      pixelPreview_->request(snapshot);
    }
  };
  effectsPanel_->onColorRequested = [this](auto type, bool second) {
    chooseEffectColor(type, second);
  };
}
bool MainWindow::beginEffectEdit() {
  if (fileBusy_ || !session().document() || !session().activeLayer())
    return false;
  if (const auto* layer=session().document()->layer(*session().activeLayer());
      layer && std::holds_alternative<core::AdjustmentLayer>(layer->payload))
    return false;
  if (effectEdit_)
    return effectEdit_->active() &&
           effectEdit_->target() == *session().activeLayer();
  if (canvasWindow_->pointerGestureActive() || activeBrushStroke_ ||
      activeCloneStroke_ || cloneProcessing_ || activeLocalBlurStroke_ ||
      localBlurProcessing_ || activeFill_ || shapeCreation_ || shapeResize_ ||
      selectionGesture_ || selectionRasterizer_ || colorSelectionEditing_ ||
      smartInteractionActive())
    return false;
  finishAdjustmentEdit(true);
  finishFilterEdit(true);
  if (adjustmentEdit_ || filterEdit_)
    return false;
  if (layerTransform_ || layerCrop_ || selectionTransform_) {
    finishCanvasOperation();
    if (layerTransform_ || layerCrop_ || selectionTransform_)
      return false;
  }
  if (textController_ && textController_->active()) {
    textController_->finish();
    if (textController_->active())
      return false;
  }
  finishLayerMove(true);
  if (activeLayerMove_)
    return false;
  cancelPendingEdits();
  auto transaction = std::make_unique<core::EffectEditTransaction>(
      *session().document(), *session().activeLayer());
  if (!transaction->active())
    return false;
  effectEdit_ = std::move(transaction);
  updateActionState();
  return true;
}
void MainWindow::previewEffectEdit(core::LayerEffectState state) {
  if (!effectEdit_ || !session().document() ||
      session().activeLayer() != effectEdit_->target())
    return;
  try {
    auto *layer = session().document()->layer(effectEdit_->target());
    if (!layer) {
      finishEffectEdit(false);
      return;
    }
    // Typed styles operate in canonical local pixels, never a zoom cache.
    auto source = layer->renderCache;
    if (core::hasActiveLayerEffects(state) &&
        !std::holds_alternative<core::RasterLayer>(layer->payload)) {
      auto candidate = *layer;
      candidate.effects = state;
      source = prepareDocumentSampleCache(candidate, 16ULL * 1024 * 1024);
      const auto revision =
          std::holds_alternative<core::ShapeLayer>(layer->payload)
              ? layer->shapeRevision
              : layer->textRevision;
      if (source->contentRevision != revision) {
        auto cache = std::make_shared<core::LayerRenderCache>(*source);
        cache->contentRevision = revision;
        source = std::move(cache);
      }
    }
    if (effectEdit_->update(std::move(state))) {
      layer->renderCache = std::move(source);
      canvasWindow_->setDocument(session().document()->snapshot(), false);
      effectsPanel_->setTarget(layer);
      scheduleFilterPreparation();
      if (layerList_)
        layerList_->viewport()->update();
    } else if (!effectEdit_->active())
      finishEffectEdit(false);
  } catch (const std::exception &e) {
    statusBar()->showMessage(
        tr("Effects preview failed: %1").arg(QString::fromUtf8(e.what())),
        6000);
    finishEffectEdit(false);
  }
}
void MainWindow::finishEffectEdit(bool commit) {
  if (finishingEffectEdit_ || !effectEdit_)
    return;
  const QScopedValueRollback guard(finishingEffectEdit_, true);
  if (effectsPanel_ && effectsPanel_->interactionActive())
    effectsPanel_->finishEditing(commit);
  try {
    if (commit) {
      if (!effectEdit_->commit(session().history()) && effectEdit_->active())
        return;
    } else
      (void)effectEdit_->cancel();
    effectEdit_.reset();
    if (session().document())
      synchronizeUi(false, false);
  } catch (const std::exception &e) {
    statusBar()->showMessage(
        tr("Could not finish effects: %1").arg(QString::fromUtf8(e.what())),
        6000);
  }
}
void MainWindow::chooseEffectColor(core::LayerEffectType type, bool second) {
  if (workspaceDialog_ || !beginEffectEdit())
    return;
  const auto target = effectEdit_->target();
  const auto *layer = session().document()->layer(target);
  const auto original =
      layer->effects ? *layer->effects : core::LayerEffectStack{};
  const auto c = second ? original.items[std::size_t(type)].secondColor
                        : original.items[std::size_t(type)].color;
  WorkspaceDialog presenter(*workspace_, *this);
  QColorDialog dialog(QColor(c.red, c.green, c.blue, c.alpha), &presenter);
  dialog.setOptions(QColorDialog::DontUseNativeDialog |
                    QColorDialog::ShowAlphaChannel);
  dialog.setWindowFlags(Qt::Widget);
  dialog.setWindowTitle(tr("Effect color"));
  connect(&dialog, &QColorDialog::currentColorChanged, this,
          [this, target, type, second, original](QColor color) {
            if (!effectEdit_ || effectEdit_->target() != target)
              return;
            auto state = original;
            auto &e = state.items[std::size_t(type)];
            (second ? e.secondColor : e.color) = core::Rgba8{
                std::uint8_t(color.red()), std::uint8_t(color.green()),
                std::uint8_t(color.blue()), std::uint8_t(color.alpha())};
            previewEffectEdit(std::make_shared<const core::LayerEffectStack>(
                std::move(state)));
          });
  const QScopedValueRollback active(workspaceDialog_, &presenter);
  finishEffectEdit(presenter.exec(dialog) == QDialog::Accepted);
}
} // namespace imageeditor::ui
