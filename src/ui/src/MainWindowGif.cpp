#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/GifImport.hpp"
#include "imageeditor/ui/SnapshotTimelinePanel.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QDialog>
#include <QFileInfo>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <future>

namespace imageeditor::ui {
namespace {
class GifProgress final : public QDialog {
public:
    GifProgress(QWidget* parent, std::atomic_bool& cancelled) : QDialog(parent, Qt::Widget), cancelled_(cancelled)
    {
        setObjectName("GifImportProgress");
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(i18n::make<QLabel>(i18n::text("gif.import.preparing"), this));
        progress = new QProgressBar(this);
        progress->setMinimumWidth(300); progress->setRange(0, 0);
        layout->addWidget(progress);
        cancel_ = i18n::make<QPushButton>(i18n::text("main.window.layer.organization.cancel"), this);
        layout->addWidget(cancel_);
        connect(cancel_, &QPushButton::clicked, this, &GifProgress::reject);
    }
    void reject() override { cancelled_.store(true); cancel_->setEnabled(false); }
    QProgressBar* progress{};
private:
    std::atomic_bool& cancelled_;
    QPushButton* cancel_{};
};
}

bool MainWindow::importGifDocument(const QString& path, const QByteArray& data, bool fromClipboard)
try {
    if (fileBusy_ || workspaceDialog_) return false;
    finishSnapshotPreview();
    GifImportResult result;
    {
        const QScopedValueRollback busy(fileBusy_, true);
        WorkspaceDialog host(*workspace_, *this);
        const QScopedValueRollback active(workspaceDialog_, &host);
        std::atomic_bool cancelled{false};
        std::atomic_size_t done{0}, total{0};
        GifProgress dialog(&host, cancelled);
        const QScopedValueRollback<QWidget*> activeProgress(fileProgress_, &dialog);
        const QString frameName = i18n::text("snapshot.timeline.frame");
        auto worker = std::async(std::launch::async, [&, frameName] {
            const auto progress = [&](std::size_t completed, std::size_t count) {
                if (count) { total.store(count); done.store(completed); }
                return !cancelled.load();
            };
            return data.isEmpty() ? loadGifDocument(path, frameName, progress)
                                  : loadGifDocumentFromData(data, frameName, progress);
        });
        QTimer poll;
        connect(&poll, &QTimer::timeout, &dialog, [&] {
            const auto count = total.load(), completed = done.load();
            if (count) {
                dialog.progress->setRange(0, 1000);
                dialog.progress->setValue(int(completed * 1000 / count));
            }
            if (fileInteractions_.progress && !fileInteractions_.progress(completed, count)) dialog.reject();
            if (worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                dialog.done(cancelled.load() ? QDialog::Rejected : QDialog::Accepted);
        });
        poll.start(16);
        const auto accepted = host.exec(dialog) == QDialog::Accepted;
        if (!accepted) cancelled.store(true);
        result = worker.get();
        if (!accepted || result.cancelled) return false;
    }
    if (!result) {
        reportFileError(fromClipboard ? result.error
            : QString(i18n::text("main.window.files.could.not.open.1.the.current.document.was.kept").arg(path, result.error)));
        return false;
    }
    if (fromClipboard) result.document->markUnsaved();
    if (!initializeDocument(std::move(result.document), fromClipboard ? QStringLiteral("Untitled") : QFileInfo(path).fileName(),
        {}, {}, fromClipboard ? QString{} : path)) return false;
    // The importer stores frame 0 at the top of the layer stack.
    session().setActiveLayer(session().document()->layers().back().id);
    fileState().untouched = false;
    if (persistWindowState_ && !fromClipboard) {
        recentFiles_.recordSuccess(path, RecentFileKind::Image);
        rememberOpenedDocument(path);
    }
    snapshotTimelineAction_->setChecked(true);
    synchronizeUi(true, true);
    canvasWindow_->dismissLayerOutlines();
    updateDocumentTitle();
    statusBar()->showMessage(i18n::text("main.window.files.opened.1").arg(fileState().displayName), 3500);
    return true;
} catch (const std::exception& error) {
    reportFileError(i18n::text("main.window.files.could.not.open.1.the.current.document.was.kept")
        .arg(path, QString::fromUtf8(error.what())));
    return false;
}
}
