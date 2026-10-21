#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/GifExportDialog.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/SnapshotTimelinePanel.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <algorithm>

namespace imageeditor::ui
{
void MainWindow::exportGif()
{
    if (fileBusy_ || workspaceDialog_ || !session().document())
        return;
    const auto source = session().document()->snapshotTimeline();
    if (std::ranges::none_of(source.frames, [](const auto &frame) { return bool(frame.image); }))
        return;
    finishSnapshotPreview();
    WorkspaceDialog presenter(*workspace_, *this);
    GifExportDialog dialog(source, &presenter);
    dialog.onExportRequested = [&] {
        QString path;
        if (fileInteractions_.chooseGifExportPath)
            path = fileInteractions_.chooseGifExportPath();
        else {
            QFileDialog chooser(this, i18n::text("gif.export.title"), lastExportDirectory(),
                                i18n::text("gif.export.filter"));
            chooser.setAcceptMode(QFileDialog::AcceptSave);
            chooser.setDefaultSuffix("gif");
            chooser.selectFile("Animation.gif");
            if (chooser.exec() != QDialog::Accepted || chooser.selectedFiles().isEmpty())
                return;
            path = chooser.selectedFiles().front();
        }
        if (path.isEmpty())
            return;
        if (!path.endsWith(".gif", Qt::CaseInsensitive)) {
            path += QStringLiteral(".gif");
            if (QFileInfo::exists(path)) {
                const bool replace =
                    fileInteractions_.confirmReplace
                        ? fileInteractions_.confirmReplace(path)
                        : QMessageBox::question(
                              this, i18n::text("gif.export.title"),
                              i18n::text("main.window.files.replace.1").arg(QFileInfo(path).fileName()),
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) == QMessageBox::Yes;
                if (!replace)
                    return;
            }
        }
        if (const auto error = validateExportDestination(path, projectPath()); !error.isEmpty()) {
            reportFileError(error);
            return;
        }
        dialog.writeTo(path);
    };
    dialog.onExported = [&](const QString &path) {
        rememberExportDirectory(path);
        statusBar()->showMessage(i18n::text("gif.export.saved"), 4000);
    };
    {
        const QScopedValueRollback busy(fileBusy_, true);
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        const QScopedValueRollback<QWidget *> progress(fileProgress_, &dialog);
        snapshotTimeline_->setBusy(true);
        presenter.exec(dialog);
    }
    refreshSnapshotTimeline();
}
} // namespace imageeditor::ui
