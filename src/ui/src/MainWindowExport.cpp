#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <thread>

namespace imageeditor::ui {
namespace {
    bool usesMatte(const ExportSettings& s)
    {
        return s.format == ExportFormat::Jpeg || (s.format == ExportFormat::Png && s.pngMatte);
    }
    bool sameRendering(const ExportSettings& a, const ExportSettings& b)
    {
        return a.size == b.size && usesMatte(a) == usesMatte(b)
            && (!usesMatte(a) || exportMatteColor(a).rgb() == exportMatteColor(b).rgb());
    }
    bool sameEncoding(const ExportSettings& a, const ExportSettings& b)
    {
        if (a.format != b.format || !sameRendering(a, b))
            return false;
        switch (a.format) {
        case ExportFormat::Png:
            return a.pngEffort == b.pngEffort;
        case ExportFormat::Jpeg:
            return a.jpegQuality == b.jpegQuality;
        case ExportFormat::WebP:
            return a.webpLossless == b.webpLossless && a.webpEffort == b.webpEffort
                && (a.webpLossless || a.webpQuality == b.webpQuality);
        }
        return false;
    }
    struct EncodeJob {
        std::atomic_bool cancel { false }, done { false };
        EncodedExport result;
        std::thread worker;
        ~EncodeJob()
        {
            if (worker.joinable())
                worker.join();
        }
    };
    struct WriteJob {
        std::atomic_bool cancel { false }, done { false };
        ExportWriteResult result;
        ExportSettings settings;
        qint64 bytes { 0 };
        std::thread worker;
        ~WriteJob()
        {
            if (worker.joinable())
                worker.join();
        }
    };

    // Only this owner-thread coordinator sees Document. Codec tasks own
    // immutable output images/bytes, never shared mutable RasterSurfaces or UI.
    // At most one encoder runs at once. Stale jobs cannot publish preview or files.
    class ExportCoordinator final : public QObject {
    public:
        ExportCoordinator(const core::Document& document, ExportDialog& dialog, QString projectPath,
            bool persist, std::function<void(const ExportSettings&, qint64)> success)
            : document_(document)
            , dialog_(dialog)
            , projectPath_(std::move(projectPath))
            , persist_(persist)
            , success_(std::move(success))
        {
            debounce_.setSingleShot(true);
            debounce_.setInterval(180);
            poll_.setInterval(20);
            connect(&debounce_, &QTimer::timeout, this, [this] { prepare(); });
            connect(&poll_, &QTimer::timeout, this, [this] { poll(); });
            dialog_.onSettingsChanged = [this] { changed(); };
            dialog_.onExportRequested = [this] { requestWrite(); };
            dialog_.onCancelRequested = [this] { cancel(); };
            debounce_.start();
        }
        ~ExportCoordinator() override
        {
            renderCancel_ = true;
            if (encode_)
                encode_->cancel = true;
            if (write_)
                write_->cancel = true;
            dialog_.onSettingsChanged = { };
            dialog_.onExportRequested = { };
            dialog_.onCancelRequested = { };
        }
        void requestWrite()
        {
            if (write_ || closing_)
                return;
            auto s = dialog_.settings();
            const auto error = validateExport(s, canvasSize());
            if (!error.isEmpty()) {
                dialog_.setError(error);
                return;
            }
            // Refuse project destinations before extension reconciliation too.
            if (const auto destinationError = validateExportDestination(s.destination, projectPath_);
                !destinationError.isEmpty()) {
                dialog_.setError(destinationError);
                return;
            }
            const auto proposed = exportPathForFormat(s.destination, s.format);
            if (proposed != s.destination) {
                if (QMessageBox::question(popupTopLevelOwner(&dialog_), tr("Match image extension?"),
                        tr("The selected encoding is %1. Use the matching filename %2?")
                            .arg(QString::fromLatin1(exportFormatName(s.format)).toUpper(),
                                QFileInfo(proposed).fileName()),
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    != QMessageBox::Yes)
                    return;
                s.destination = proposed;
                dialog_.setDestination(proposed);
            }
            s.destination = QFileInfo(s.destination).absoluteFilePath();
            if (const auto destinationError = validateExportDestination(s.destination, projectPath_);
                !destinationError.isEmpty()) {
                dialog_.setError(destinationError);
                return;
            }
            if (QFileInfo::exists(s.destination)
                && QMessageBox::question(popupTopLevelOwner(&dialog_), tr("Replace exported image?"),
                       tr("Replace %1? The existing file remains intact "
                          "unless export finishes successfully.")
                           .arg(s.destination),
                       QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    != QMessageBox::Yes)
                return;
            pendingWrite_ = s;
            if (ready_ && sameEncoding(readySettings_, s))
                publish();
            else {
                debounce_.stop();
                prepare();
            }
        }

    private:
        QSize canvasSize() const
        {
            const auto e = document_.canvas().extent;
            return { int(e.width), int(e.height) };
        }
        void changed()
        {
            if (closing_ || write_)
                return;
            pendingWrite_.reset(); // An overwrite confirmation never authorizes changed
                                   // settings/path.
            const auto s = dialog_.settings();
            if (ready_ && sameEncoding(readySettings_, s)) {
                showReadyPreview();
                return;
            }
            if (rendering_)
                renderCancel_ = true;
            if (encode_ && !sameEncoding(encodeSettings_, s))
                encode_->cancel = true;
            debounce_.start();
        }
        void prepare()
        {
            if (closing_ || write_)
                return;
            if (rendering_) {
                renderCancel_ = true;
                debounce_.start();
                return;
            }
            const auto s = dialog_.settings();
            if (ready_ && sameEncoding(readySettings_, s)) {
                showReadyPreview();
                if (pendingWrite_)
                    publish();
                return;
            }
            if (encode_) {
                if (!sameEncoding(s, encodeSettings_))
                    encode_->cancel = true;
                poll_.start();
                return; // Coalesce, never launch competing full-image workers.
            }
            if (const auto error = validateExport(s, canvasSize()); !error.isEmpty()) {
                pendingWrite_.reset();
                dialog_.setError(error);
                return;
            }
            if (rendered_.isNull() || !sameRendering(renderSettings_, s)) {
                rendered_ = { };
                ready_ = { };
                dialog_.setProgress(tr("Rendering document…"));
                rendering_ = true;
                renderCancel_ = false;
                QElapsedTimer cadence;
                cadence.start();
                const auto result = renderExport(document_, s, [&](quint64, quint64) {
                    if (cadence.elapsed() >= 8) {
                        QApplication::processEvents(QEventLoop::AllEvents, 4);
                        cadence.restart();
                    }
                    return !renderCancel_ && !closing_;
                });
                rendering_ = false;
                if (closing_) {
                    dialog_.done(QDialog::Rejected);
                    return;
                }
                if (!result) {
                    if (result.cancelled) {
                        debounce_.start();
                        return;
                    }
                    pendingWrite_.reset();
                    dialog_.setError(result.error);
                    return;
                }
                rendered_ = result.image;
                renderSettings_ = s;
                if (!sameEncoding(s, dialog_.settings())) {
                    debounce_.start();
                    return;
                }
            }
            ready_ = { };
            encode_ = std::make_shared<EncodeJob>();
            encodeSettings_ = s;
            auto* job = encode_.get();
            const auto pixels = rendered_;
            dialog_.setProgress(tr("Encoding %1 and decoding preview…")
                    .arg(QString::fromLatin1(exportFormatName(s.format)).toUpper()));
            try {
                job->worker = std::thread([job, pixels, s] {
                    job->result = encodeExport(pixels, s, job->cancel);
                    job->done.store(true, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                encode_.reset();
                pendingWrite_.reset();
                dialog_.setError(QString::fromUtf8(e.what()));
                return;
            }
            poll_.start();
        }
        void poll()
        {
            if (write_) {
                if (!write_->done.load(std::memory_order_acquire))
                    return;
                auto completed = std::move(write_);
                poll_.stop();
                if (completed->result) {
                    if (persist_)
                        saveExportPreferences(completed->settings);
                    dialog_.setExported(
                        completed->settings.destination, completed->settings.size, completed->bytes);
                    success_(completed->settings, completed->bytes);
                } else if (!completed->result.cancelled)
                    dialog_.setError(completed->result.error);
                if (closing_)
                    dialog_.done(QDialog::Rejected);
                return;
            }
            if (!encode_) {
                poll_.stop();
                return;
            }
            if (!encode_->done.load(std::memory_order_acquire))
                return;
            auto job = std::move(encode_);
            poll_.stop();
            if (closing_) {
                dialog_.done(QDialog::Rejected);
                return;
            }
            if (job->cancel || !sameEncoding(encodeSettings_, dialog_.settings())) {
                debounce_.start();
                return;
            }
            if (!job->result) {
                pendingWrite_.reset();
                dialog_.setError(job->result.error);
                return;
            }
            ready_ = std::move(job->result);
            readySettings_ = encodeSettings_;
            showReadyPreview();
            if (pendingWrite_)
                publish();
        }
        void showReadyPreview()
        {
            // Path changes can reuse encoded bytes, but not permission to
            // overwrite the open project (including aliases to its path).
            if (const auto error = validateExportDestination(dialog_.settings().destination, projectPath_);
                !error.isEmpty()) {
                dialog_.setError(error);
                return;
            }
            dialog_.setPreview(ready_.decoded, ready_.bytes.size());
        }
        void publish()
        {
            if (!pendingWrite_ || !ready_ || !sameEncoding(*pendingWrite_, readySettings_))
                return;
            // Bytes shown in the preview are precisely the bytes atomically saved.
            write_ = std::make_shared<WriteJob>();
            write_->settings = *pendingWrite_;
            write_->bytes = ready_.bytes.size();
            pendingWrite_.reset();
            debounce_.stop();
            auto* job = write_.get();
            const auto bytes = ready_.bytes;
            dialog_.setProgress(tr("Writing image…"), true);
            try {
                job->worker = std::thread([job, bytes] {
                    job->result = writeExportAtomically(job->settings.destination, bytes, job->cancel);
                    job->done.store(true, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                write_.reset();
                dialog_.setError(QString::fromUtf8(e.what()));
                return;
            }
            poll_.start();
        }
        void cancel()
        {
            closing_ = true;
            debounce_.stop();
            pendingWrite_.reset();
            renderCancel_ = true;
            if (encode_) {
                encode_->cancel = true;
                dialog_.setProgress(tr("Cancelling encoding…"), true);
                return;
            }
            if (write_) {
                write_->cancel = true;
                dialog_.setProgress(tr("Cancelling write…"), true);
                return;
            }
            if (!rendering_)
                dialog_.done(QDialog::Rejected);
        }
        const core::Document& document_;
        ExportDialog& dialog_;
        QString projectPath_;
        bool persist_, rendering_ { false }, renderCancel_ { false }, closing_ { false };
        std::function<void(const ExportSettings&, qint64)> success_;
        QTimer debounce_, poll_;
        QImage rendered_;
        ExportSettings renderSettings_, encodeSettings_, readySettings_;
        EncodedExport ready_;
        std::optional<ExportSettings> pendingWrite_;
        std::shared_ptr<EncodeJob> encode_;
        std::shared_ptr<WriteJob> write_;
    };
} // namespace

void MainWindow::exportImage(bool again)
{
    if (fileBusy_ || workspaceDialog_ || !session().document())
        return;
    if (smartInteractionActive() || colorSelectionEditing_ || selectionGesture_ || selectionRasterizer_
        || shapeResize_) {
        statusBar()->showMessage(tr("Finish or cancel the current selection/shape "
                                    "interaction before exporting."),
            4000);
        return;
    }
    if (again && !fileState().exportSettings) {
        statusBar()->showMessage(tr("Export an image first to establish Export Again settings."), 4000);
        return;
    }
    if (!settleForFileOperation())
        return;
    const auto extent = session().document()->canvas().extent;
    const QSize canvas(int(extent.width), int(extent.height));
    const auto owner = activeDocument_;
    auto initial = owner->exportSettings.value_or(loadExportPreferences(canvas, fileState().displayName));
    if (!owner->exportSettings) {
        initial.format = ExportFormat::Png;
        initial.destination = exportPathForFormat(initial.destination, initial.format);
    }
    // The workspace input shield plus fileBusy excludes all editor mutations
    // during owner-thread source evaluation. Only immutable rendered buffers
    // leave this thread; neither a document snapshot nor raster refs do.
    const QScopedValueRollback busy(fileBusy_, true);
    WorkspaceDialog presenter(*workspace_, *this);
    ExportDialog dialog(initial, canvas, &presenter);
    ExportCoordinator coordinator(*session().document(), dialog, fileState().projectPath, persistWindowState_,
        [this, owner](const ExportSettings& s, qint64 bytes) {
            if (owner->closed) return;
            owner->exportSettings = s;
            if (owner != activeDocument_) return;
            statusBar()->showMessage(tr("Exported %1 · %2 × %3 px · %4 bytes")
                                         .arg(QFileInfo(s.destination).fileName())
                                         .arg(s.size.width())
                                         .arg(s.size.height())
                                         .arg(bytes),
                8000);
        });
    const QScopedValueRollback active(workspaceDialog_, &presenter);
    if (again)
        QTimer::singleShot(0, &dialog, [&coordinator] { coordinator.requestWrite(); });
    presenter.exec(dialog);
    if (!owner->closed) owner->exportSettings = dialog.settings();
    // No checkpoint, association, selection, viewport or history changes here.
}
} // namespace imageeditor::ui
