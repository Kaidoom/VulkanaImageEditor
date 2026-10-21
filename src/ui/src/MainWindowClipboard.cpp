#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/ClipboardImage.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/Localization.hpp"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <algorithm>

namespace imageeditor::ui {
bool MainWindow::createDocumentFromClipboard()
{
    if (fileBusy_ || workspaceDialog_) return false;
    try {
        ClipboardGifSource gif;
        auto loaded = [this, &gif] {
            // Native clipboard retrieval may dispatch events while the owner
            // supplies data. Do not allow overlapping document operations.
            const QScopedValueRollback busy(fileBusy_, true);
            const auto* mime = QApplication::clipboard()->mimeData(QClipboard::Clipboard);
            if (mime) {
                gif = clipboardGifSource(*mime);
                if (gif) return RasterLayerLoadResult{};
            }
            return mime ? loadClipboardImage(*mime) : RasterLayerLoadResult{
                .layer = {}, .extent = {}, .error = QStringLiteral("Clipboard has no image.")};
        }();
        if (gif) {
            if (!gif.error.isEmpty()) { reportFileError(gif.error); return false; }
            return importGifDocument(gif.path, gif.data, true);
        }
        if (!loaded) { reportFileError(loaded.error); return false; }
        auto document = std::make_unique<core::Document>(core::CanvasSpec{loaded.extent, 96.0});
        if (!document->insertLayer(0, std::move(*loaded.layer))) {
            reportFileError(QStringLiteral("Cannot create the clipboard image layer."));
            return false;
        }
        document->markUnsaved();
        // A copied path supplies pixels, not a project/source identity: saving
        // this new document must never overwrite or deduplicate the source.
        if (!initializeDocument(std::move(document), QStringLiteral("Untitled"))) return false;
        fileState().untouched = false;
        synchronizeUi(true, true);
        canvasWindow_->dismissLayerOutlines();
        updateDocumentTitle();
        return true;
    } catch (const std::exception& error) {
        reportFileError(QString::fromUtf8(error.what()));
        return false;
    }
}

void MainWindow::copyCanvasToClipboard()
{
    if (fileBusy_ || workspaceDialog_ || !activeDocument_ || !session().document()) return;
    if (smartInteractionActive() || colorSelectionEditing_ || selectionGesture_ || selectionRasterizer_ || shapeResize_) {
        statusBar()->showMessage(i18n::text("clipboard.copy.canvas.finish.edit"), 4000);
        return;
    }
    // Use the same preflight as export, including the refinement discard guard.
    // Complete edits are kept; in-progress pointer strokes are never copied.
    if (!settleForFileOperation()) return;
    const auto owner = activeDocument_;
    const auto extent = owner->session.document()->canvas().extent;
    ExportSettings settings;
    settings.size = QSize(int(extent.width), int(extent.height));
    FlattenedDocumentResult result;
    {
        const QScopedValueRollback busy(fileBusy_, true);
        QProgressDialog progress(i18n::text("clipboard.copy.canvas.preparing"),
            i18n::text("main.window.layer.organization.cancel"), 0, 1000, this);
        progress.setObjectName(QStringLiteral("CopyCanvasProgress"));
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(250);
        progress.setAutoClose(false); progress.setAutoReset(false);
        const QScopedValueRollback<QWidget*> active(fileProgress_, &progress);
        QElapsedTimer cadence; cadence.start();
        result = renderExport(*owner->session.document(), settings, [&](auto done, auto total) {
            if (fileInteractions_.progress && !fileInteractions_.progress(done, total)) return false;
            if (cadence.elapsed() >= 12) {
                progress.setValue(int(done * 1000 / std::max<std::uint64_t>(total, 1)));
                QApplication::processEvents(QEventLoop::AllEvents, 4);
                cadence.restart();
            }
            return !progress.wasCanceled() && !owner->closed && activeDocument_ == owner;
        });
        // A final Cancel event must not replace the clipboard with a completed
        // render after the user has cancelled it.
        if (progress.wasCanceled()) return;
    }
    if (!result) {
        if (!result.cancelled)
            statusBar()->showMessage(i18n::text("clipboard.copy.canvas.failed").arg(result.error), 7000);
        return;
    }
    if (owner->closed || activeDocument_ != owner) return;
    // QClipboard handles the platform's native image representations.
    // The detached sRGB image retains alpha, DPI and native canvas dimensions;
    // no temporary file, export preferences, selection or history is changed.
    if (fileInteractions_.publishClipboardImage) fileInteractions_.publishClipboardImage(result.image);
    else QApplication::clipboard()->setImage(result.image, QClipboard::Clipboard);
    statusBar()->showMessage(i18n::text("clipboard.copy.canvas.done").arg(settings.size.width()).arg(settings.size.height()), 4000);
}
}
