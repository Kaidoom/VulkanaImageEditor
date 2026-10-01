#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QAction>
#include <QPushButton>
#include <QStatusBar>
#include <cmath>

namespace imageeditor::ui {
void MainWindow::createLayerMaskActions() {
  const QStringList labels{
      tr("Add Layer Mask"),    tr("Add Mask from Selection"),
      tr("Delete Layer Mask"), tr("Disable Layer Mask"),
      tr("Apply Layer Mask…"), tr("Add Mask to Selection")};
  const QStringList names{"AddLayerMaskAction",    "MaskFromSelectionAction",
                          "DeleteLayerMaskAction", "EnableLayerMaskAction",
                          "ApplyLayerMaskAction",  "MaskToSelectionAction"};
  for (size_t i = 0; i < maskActions_.size(); ++i) {
    auto *action =
        new QAction(toolGlyph(ToolGlyph::LayerMask), labels[int(i)], this);
    action->setObjectName(names[int(i)]);
    maskActions_[i] = action;
    registerEditorWindowAction(action);
  }
  connect(maskActions_[0], &QAction::triggered, this,
          [this] { addLayerMask(); });
  connect(maskActions_[1], &QAction::triggered, this,
          [this] { addLayerMask(true); });
  connect(maskActions_[2], &QAction::triggered, this,
          [this] { changeLayerMask(true); });
  connect(maskActions_[3], &QAction::triggered, this,
          [this] { changeLayerMask(false); });
  connect(maskActions_[4], &QAction::triggered, this,
          &MainWindow::applyLayerMask);
  connect(maskActions_[5], &QAction::triggered, this,
          &MainWindow::layerMaskToSelection);
}
void MainWindow::refreshLayerMaskActions() {
  const auto *layer =
      session().document() && session().activeLayer()
          ? session().document()->layer(*session().activeLayer())
          : nullptr;
  const bool available = layer && !fileBusy_ && !activeFill_ &&
                         !activeBrushStroke_ && !layerTransform_ &&
                         !selectionTransform_ && !layerCrop_;
  maskActions_[0]->setEnabled(available && !layer->mask);
  maskActions_[1]->setEnabled(
      available && !layer->mask && session().document()->selection() &&
      !session().document()->selection()->bounds().empty());
  for (size_t i = 2; i < maskActions_.size(); ++i)
    maskActions_[i]->setEnabled(available && layer->mask);
  maskActions_[3]->setText(layer && layer->mask && !layer->mask->enabled
                               ? tr("Enable Layer Mask")
                               : tr("Disable Layer Mask"));
  maskActions_[4]->setEnabled(available && layer->mask && layer->mask->enabled);
  if (addMaskButton_)
    addMaskButton_->setEnabled(maskActions_[0]->isEnabled());
}
void MainWindow::setLayerEditingTarget(int row, bool mask) {
  const auto id = layerModel_->layerIdAt(row);
  if (!id || fileBusy_ || updatingUi_)
    return;
  // Follow ordinary row selection. File-operation settling cancels pointer
  // capture and synthesizes a release in the middle of this thumbnail press.
  selectLayerFromRow(row, Qt::NoModifier);
  if (session().activeLayer() != id)
    return;
  session().setEditingLayerMask(mask);
  synchronizeUi(false, false);
  statusBar()->showMessage(session().editingLayerMask()
                               ? tr("Editing layer mask · White reveals, black "
                                    "hides · Brush, Eraser and Fill")
                               : tr("Editing layer content"),
                           4000);
}
void MainWindow::addLayerMask(bool fromSelection) {
  if (fileBusy_ || !session().document() || !session().activeLayer() ||
      !settleForFileOperation())
    return;
  auto &doc = *session().document();
  const auto *layer = doc.layer(*session().activeLayer());
  if (!layer || layer->mask)
    return;
  try {
    // Masks retain a stable layer-local pixel frame, independent of view zoom
    // and of the geometry of later text/shape edits. New content outside it is
    // revealed.
    // Cropping is reversible: allocate the full local source/style frame so
    // removing a crop does not expose areas that the mask cannot edit.
    auto bounds = core::hasActiveLayerEffects(layer->effects) &&
                          core::layerEffectCacheValid(*layer) && layer->effectCache
                      ? layer->effectCache->visualBounds
                      : core::layerSourceBounds(*layer);
    if (bounds.empty()) {
      // A hidden or newly created typed layer may not have a display cache.
      // Its model still supplies the local frame without rasterizing content.
      if (const auto *shape = std::get_if<core::ShapeLayer>(&layer->payload))
        bounds = QtShapeRenderService().documentBounds(*shape, {});
      else if (const auto *text = std::get_if<core::TextLayer>(&layer->payload))
        bounds = QtTextLayout(*text).documentBounds({});
      else
        bounds = core::layerSourceBounds(*layer);
    }
    if (bounds.empty())
      throw std::runtime_error("Layer has no renderable bounds");
    const auto x = std::floor(bounds.x), y = std::floor(bounds.y);
    const auto w = std::ceil(bounds.right()) - x,
               h = std::ceil(bounds.bottom()) - y;
    if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 32768 ||
        h > 32768 || w * h > 64.0 * 1024 * 1024)
      throw std::runtime_error("Layer mask exceeds the 64 megapixel limit");
    const core::Extent2u extent{uint32_t(w), uint32_t(h)};
    auto mask = std::make_shared<core::LayerMask>();
    mask->localToMask = {1, 0, -x, 0, 1, -y};
    mask->coverage = core::SelectionMask::filled(extent, 255);
    if (fromSelection) {
      if (!doc.selection() || doc.selection()->bounds().empty())
        return;
      std::vector<uint8_t> bytes(size_t(extent.width) * extent.height);
      const auto selection = std::make_shared<core::LayerMask>(
          core::LayerMask{doc.selection(), {}, 0, true});
      for (uint32_t row = 0; row < extent.height; ++row)
        for (uint32_t column = 0; column < extent.width; ++column) {
          const auto p =
              layer->localToDocument.map({x + column + .5, y + row + .5});
          // Same subpixel coverage reconstruction as editable layer masks.
          bytes[size_t(row) * extent.width + column] =
              uint8_t(std::lround(core::layerMaskCoverage(selection, p) * 255));
        }
      mask->coverage = core::SelectionMask::fromR8(extent, bytes, extent.width);
      mask->outside = 0;
    }
    if (session().execute(std::make_unique<core::LayerMaskCommand>(
            layer->id, nullptr, mask, "Add layer mask"))) {
      session().setEditingLayerMask(true);
      fileState().untouched = false;
      synchronizeUi(true, false);
      statusBar()->showMessage(
          tr("Layer mask active · White reveals, black hides · Click the "
             "content thumbnail to edit the layer"),
          5500);
    }
  } catch (const std::exception &e) {
    statusBar()->showMessage(QString::fromUtf8(e.what()), 6000);
  }
}
void MainWindow::changeLayerMask(bool remove) {
  if (fileBusy_ || !session().document() || !session().activeLayer() ||
      !settleForFileOperation())
    return;
  const auto *layer = session().document()->layer(*session().activeLayer());
  if (!layer || !layer->mask)
    return;
  core::LayerMaskState next;
  if (!remove) {
    auto changed = std::make_shared<core::LayerMask>(*layer->mask);
    changed->enabled = !changed->enabled;
    next = changed;
  }
  if (session().execute(std::make_unique<core::LayerMaskCommand>(
          layer->id, layer->mask, next,
          remove          ? "Delete layer mask"
          : next->enabled ? "Enable layer mask"
                          : "Disable layer mask"))) {
    if (remove)
      session().setEditingLayerMask(false);
    fileState().untouched = false;
    synchronizeUi(true, false);
  }
}
void MainWindow::applyLayerMask() {
  if (fileBusy_ || !session().document() || !session().activeLayer() ||
      !settleForFileOperation())
    return;
  const auto id = *session().activeLayer();
  const auto *layer = session().document()->layer(id);
  if (!layer || !layer->mask || !layer->mask->enabled)
    return;
  if (!confirmLayerChange(
          tr("Apply layer mask"),
          tr("Commit this layer’s masked appearance to raster pixels, "
             "including its transforms and effects? Text and shapes will "
             "become pixels. Layer opacity and blend mode are retained. Undo "
             "restores the editable original.")))
    return;
  auto result = prepareRasterizeLayers(
      *session().document(), session().layerSelectionState(),
      session().history().memoryBudget(), {}, id);
  if (!result.error.isEmpty()) {
    statusBar()->showMessage(result.error, 6000);
    return;
  }
  if (result.command && session().execute(std::move(result.command))) {
    session().setEditingLayerMask(false);
    fileState().untouched = false;
    synchronizeUi(true, false);
  }
}
void MainWindow::layerMaskToSelection() {
  if (fileBusy_ || !session().document() || !session().activeLayer() ||
      !settleForFileOperation())
    return;
  auto &doc = *session().document();
  const auto *layer = doc.layer(*session().activeLayer());
  if (!layer || !layer->mask)
    return;
  const auto inverse = layer->localToDocument.inverted();
  if (!inverse)
    return;
  const auto e = doc.canvas().extent;
  if (uint64_t(e.width) * e.height > 64ULL * 1024 * 1024) {
    statusBar()->showMessage(tr("Selection exceeds 64 megapixels"), 4000);
    return;
  }
  auto mask = std::make_shared<core::LayerMask>(*layer->mask);
  mask->enabled = true;
  std::vector<uint8_t> bytes(size_t(e.width) * e.height);
  for (uint32_t y = 0; y < e.height; ++y)
    for (uint32_t x = 0; x < e.width; ++x)
      bytes[size_t(y) * e.width + x] = uint8_t(std::lround(
          core::layerMaskCoverage(mask, inverse->map({x + .5, y + .5})) * 255));
  auto selection = core::combineSelection(
      doc.selection(), core::SelectionMask::fromR8(e, bytes, e.width),
      core::SelectionOperation::Add);
  if (session().execute(std::make_unique<core::SetSelectionCommand>(
          selection, "Add layer mask to selection")))
    synchronizeUi(false, false);
}
} // namespace imageeditor::ui
