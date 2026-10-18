#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/ui/TextController.hpp"
#include <QStatusBar>
namespace imageeditor::ui {
void MainWindow::formatLayerInfoText(core::LayerId id, core::TextStylePatch patch)
{
    if (fileBusy_ || workspaceDialog_ || !session().document() || session().activeLayer() != id
        || canvasWindow_->pointerGestureActive())
        return;
    const auto documentId = activeDocumentId();
    if (!settleForFileOperation() || documentId != activeDocumentId() || session().activeLayer() != id)
        return;
    const auto* layer = session().document()->layer(id);
    const auto* text = layer ? std::get_if<core::TextLayer>(&layer->payload) : nullptr;
    if (!text)
        return;
    try {
        const auto before = core::normalizedText(*text);
        if (patch.family && before.defaultStyle.font.family == *patch.family
            && std::ranges::all_of(
                before.runs, [&](const auto& run) { return run.style.font.family == *patch.family; }))
            return;
        auto after = core::formatTextRange(before, 0, before.utf8.size(), patch);
        after.defaultStyle = core::patchedTextStyle(after.defaultStyle, patch);
        after = core::normalizedText(std::move(after));
        if (after == before)
            return;
        if (!layer->localToDocument.isAffine())
            (void)QtTextLayout(after).documentBounds(layer->localToDocument);
        if (session().execute(std::make_unique<core::TextEditCommand>(id, before, std::move(after),
                core::TextEditHint { id, 0, 0 }, core::TextEditHint { id, 0, 0 }, 0, "Format text"))) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& e) {
        updateLayerControls();
        statusBar()->showMessage(QString::fromUtf8(e.what()), 5000);
    }
}
void MainWindow::chooseLayerInfoTextColor(core::LayerId id)
{
    if (fileBusy_ || workspaceDialog_ || !session().document() || session().activeLayer() != id
        || canvasWindow_->pointerGestureActive())
        return;
    const auto documentId = activeDocumentId();
    if (!settleForFileOperation())
        return;
    const auto* layer = session().document()->layer(id);
    const auto* text = layer ? std::get_if<core::TextLayer>(&layer->payload) : nullptr;
    if (!text)
        return;
    const auto source = core::textStyleAt(*text, 0).color;
    const auto color = ColorDialog::getColor(QColor(source.red, source.green, source.blue, source.alpha),
        this, i18n::text("properties.info.text.color"), ColorDialog::ShowAlphaChannel);
    if (color.isValid() && activeDocumentId() == documentId && session().activeLayer() == id) {
        core::TextStylePatch patch;
        patch.color = core::Rgba8 { std::uint8_t(color.red()), std::uint8_t(color.green()),
            std::uint8_t(color.blue()), std::uint8_t(color.alpha()) };
        formatLayerInfoText(id, std::move(patch));
    }
}
}
