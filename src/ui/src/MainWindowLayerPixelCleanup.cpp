#include "imageeditor/ui/MainWindow.hpp"
#include "LayerCleanupPreview.hpp"
#include "imageeditor/core/LayerPixelCleanup.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QVBoxLayout>

namespace imageeditor::ui {
namespace {
std::optional<int> chooseAlphaThreshold(WorkspaceDialog& presenter, const i18n::Message& title,
    const core::Document& document, const core::LayerSelectionState& selected)
{
    QDialog card(&presenter, Qt::SubWindow | Qt::FramelessWindowHint);
    card.setObjectName("CleanTransparencyDialog");
    i18n::setWindowTitle(&card, title);
    auto* layout = new QVBoxLayout(&card);
    auto* preview = new LayerCleanupPreview(document, selected, &card);
    layout->addWidget(preview);
    auto* actions = new i18n::ButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &card);
    layout->addWidget(actions);
    QObject::connect(actions, &QDialogButtonBox::accepted, &card, &QDialog::accept);
    QObject::connect(actions, &QDialogButtonBox::rejected, &card, &QDialog::reject);
    card.setProperty("workspacePreferredSize", card.sizeHint());
    if (presenter.exec(card) != QDialog::Accepted) return std::nullopt;
    return preview->threshold();
}
}

void MainWindow::createLayerPixelActions(QMenu* menu)
{
    menu->addSeparator();
    cleanTransparencyAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::OpacityMode),
        i18n::text("layers.cleanup.transparency") + QStringLiteral("…"), this,
        [this] { cleanLayerPixels(false); });
    cleanTransparencyAction_->setObjectName("CleanTransparencyAction");
    deleteInvertedAction_
        = i18n::addAction(menu, toolGlyph(ToolGlyph::Invert), i18n::text("layers.cleanup.delete.inverted"), this,
            [this] { startFill(false, true, { }, PixelErase::Inverted); });
    deleteInvertedAction_->setObjectName("DeleteInvertedAction");
    trimToCanvasAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::Crop), i18n::text("layers.cleanup.trim"),
        this, [this] { cleanLayerPixels(true); });
    trimToCanvasAction_->setObjectName("TrimLayersToCanvasAction");
    menu->addSeparator();
    connect(menu, &QMenu::aboutToShow, this, &MainWindow::refreshLayerPixelActions);
    refreshLayerPixelActions();
}

void MainWindow::refreshLayerPixelActions()
{
    if (!trimToCanvasAction_)
        return;
    const auto* doc = session().document();
    const bool ready = doc && !fileBusy_ && !workspaceDialog_ && !backgroundRemoval_ && !layerTransform_
        && !selectionTransform_ && !canvasResize_ && shortcutGestureIdle(true);
    const bool writableRasters
        = ready && !session().editingLayerMask() && core::canCleanLayerPixels(*doc, session().selectedLayers());
    cleanTransparencyAction_->setEnabled(writableRasters);
    trimToCanvasAction_->setEnabled(writableRasters);
    const auto* layer = ready && session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
    const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    deleteInvertedAction_->setEnabled(layer && !layer->mirror && doc->selection()
        && !doc->selection()->bounds().empty()
        && (session().editingLayerMask()
            || (raster && raster->surface && raster->surface->dataKind() != core::RasterDataKind::PackedData)));
}

void MainWindow::cleanLayerPixels(bool trimToCanvas)
try {
    if (fileBusy_ || workspaceDialog_ || !activeDocument_ || !settleForFileOperation())
        return;
    const auto owner = activeDocument_;
    auto& doc = *session().document();
    const auto selected = session().layerSelectionState();
    if (session().editingLayerMask() || !core::canCleanLayerPixels(doc, selected.ids))
        return;
    int threshold = core::defaultCleanupAlpha;
    const auto title
        = trimToCanvas ? i18n::text("layers.cleanup.trim") : i18n::text("layers.cleanup.transparency");
    if (!trimToCanvas) {
        WorkspaceDialog presenter(*workspace_, *this);
        QScopedValueRollback modal(workspaceDialog_, &presenter);
        const auto chosen = chooseAlphaThreshold(presenter, title, doc, selected);
        if (!chosen) return;
        threshold = *chosen;
    }
    if (owner != activeDocument_ || owner->closed)
        return;
    std::unique_ptr<core::Command> command;
    {
        WorkspaceDialog presenter(*workspace_, *this);
        QDialog card(&presenter, Qt::SubWindow | Qt::FramelessWindowHint);
        auto* layout = new QVBoxLayout(&card);
        layout->addWidget(i18n::make<QLabel>(title, &card));
        auto* bar = new QProgressBar(&card);
        bar->setRange(0, 0);
        layout->addWidget(bar);
        auto* buttons = new i18n::ButtonBox(QDialogButtonBox::Cancel, &card);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &card, &QDialog::reject);
        bool cancelled = false;
        connect(&card, &QDialog::rejected, &card, [&] { cancelled = true; });
        QScopedValueRollback busy(fileBusy_, true);
        QScopedValueRollback modal(workspaceDialog_, &presenter);
        QScopedValueRollback<QWidget*> progress(fileProgress_, &card);
        QElapsedTimer elapsed, cadence;
        elapsed.start();
        cadence.start();
        const auto finish = qScopeGuard([&] { presenter.finish(); });
        command = core::makeLayerPixelCleanupCommand(doc, selected,
            trimToCanvas ? core::LayerPixelCleanup::TrimToCanvas : core::LayerPixelCleanup::RemoveLowAlpha,
            std::uint8_t(threshold), [&] {
                if (elapsed.elapsed() >= 250 && cadence.elapsed() >= 16) {
                    if (!presenter.active())
                        presenter.open(card);
                    QApplication::processEvents(QEventLoop::AllEvents, 5);
                    cadence.restart();
                }
                return !cancelled;
            });
        if (cancelled)
            return;
    }
    if (!command) {
        statusBar()->showMessage(i18n::text("layers.cleanup.no.pixels"), 2500);
        return;
    }
    if (!session().execute(std::move(command)))
        throw std::runtime_error("Could not apply layer cleanup");
    fileState().untouched = false;
    synchronizeUi(false, false);
} catch (const std::exception& error) {
    statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
}
} // namespace imageeditor::ui
