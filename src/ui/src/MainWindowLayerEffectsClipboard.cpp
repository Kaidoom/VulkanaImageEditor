#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LayerEffectClipboard.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QStatusBar>

namespace imageeditor::ui {
namespace {
bool acceptsEffects(const core::Layer* layer)
{
    return layer && !std::holds_alternative<core::AdjustmentLayer>(layer->payload)
        && !core::isScalarLayer(*layer) && !core::isPackedDataLayer(*layer);
}
}

void MainWindow::createLayerEffectClipboardActions()
{
    copyLayerEffectsAction_ = i18n::make<QAction>(toolGlyph(ToolGlyph::Copy),
        i18n::text("layer.effects.copy"), this);
    pasteLayerEffectsAction_ = i18n::make<QAction>(toolGlyph(ToolGlyph::Paste),
        i18n::text("layer.effects.paste"), this);
    copyLayerEffectsAction_->setObjectName("CopyLayerEffectsAction");
    pasteLayerEffectsAction_->setObjectName("PasteLayerEffectsAction");
    i18n::setToolTip(pasteLayerEffectsAction_, i18n::text("layer.effects.paste.hint"));
    registerEditorWindowAction(copyLayerEffectsAction_);
    registerEditorWindowAction(pasteLayerEffectsAction_);
    connect(copyLayerEffectsAction_, &QAction::triggered, this, &MainWindow::copyLayerEffects);
    connect(pasteLayerEffectsAction_, &QAction::triggered, this, &MainWindow::pasteLayerEffects);
    const auto refreshClipboard = [this] {
        const auto* mime = QApplication::clipboard()->mimeData();
        layerEffectsClipboardAvailable_ = mime && mime->hasFormat(layerEffectClipboardMime)
            && decodeLayerEffectClipboard(mime->data(layerEffectClipboardMime)).has_value();
        refreshLayerEffectClipboardActions();
    };
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, refreshClipboard);
    refreshClipboard();
}

void MainWindow::refreshLayerEffectClipboardActions()
{
    if (!copyLayerEffectsAction_ || !pasteLayerEffectsAction_) return;
    const auto* document = session().document();
    const auto* layer = document && session().activeLayer() ? document->layer(*session().activeLayer()) : nullptr;
    const bool available = acceptsEffects(layer) && !fileBusy_ && !workspaceDialog_ && !refinement_
        && !activeFill_ && !activeBrushStroke_ && !activeCloneStroke_ && !activeLocalBlurStroke_
        && !activeSpotHealStroke_ && !cloneProcessing_ && !localBlurProcessing_
        && !spotHealBusyForActiveDocument() && !canvasWindow_->pointerGestureActive();
    copyLayerEffectsAction_->setEnabled(available);
    pasteLayerEffectsAction_->setEnabled(available && layerEffectsClipboardAvailable_);
}

void MainWindow::copyLayerEffects()
{
    refreshLayerEffectClipboardActions();
    if (!copyLayerEffectsAction_->isEnabled()) return;
    const auto* layer = session().document()->layer(*session().activeLayer());
    try {
        const auto bytes = encodeLayerEffectClipboard(layer->effects);
        if (bytes.isEmpty()) return;
        auto mime = std::make_unique<QMimeData>();
        mime->setData(layerEffectClipboardMime, bytes);
        QApplication::clipboard()->setMimeData(mime.release());
        statusBar()->showMessage(i18n::text("layer.effects.copy.done"), 3000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::pasteLayerEffects()
{
    refreshLayerEffectClipboardActions();
    if (!pasteLayerEffectsAction_->isEnabled()) return;
    const auto instance = activeDocumentId();
    const auto target = *session().activeLayer();
    try {
        const auto* mime = QApplication::clipboard()->mimeData();
        const auto copied = mime ? decodeLayerEffectClipboard(mime->data(layerEffectClipboardMime)) : std::nullopt;
        if (!copied) {
            statusBar()->showMessage(i18n::text("layer.effects.paste.invalid"), 4000);
            return;
        }
        if (!settleForFileOperation() || activeDocumentId() != instance || session().activeLayer() != target) return;
        const auto* layer = session().document()->layer(target);
        if (!acceptsEffects(layer)) return;
        if (executeDocumentCommand(std::make_unique<core::SetLayerEffectsCommand>(target, layer->effects, *copied, true))) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}
} // namespace imageeditor::ui
