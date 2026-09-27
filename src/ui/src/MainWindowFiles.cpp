#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/platform/PlatformStartup.hpp"
#include "imageeditor/platform/SingleInstance.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QWindow>

namespace imageeditor::ui {
void MainWindow::receiveExternalLaunch(const QStringList& files)
{
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
    if (windowHandle()) windowHandle()->requestActivate(); // compositor controls activation on Wayland
    if (files.isEmpty()) return;
    externalOpenFiles_.append(files);
    startupNewDocumentPending_ = false;
    if (!externalOpenTimer_) {
        externalOpenTimer_ = new QTimer(this);
        externalOpenTimer_->setSingleShot(true);
        connect(externalOpenTimer_, &QTimer::timeout, this, &MainWindow::drainExternalLaunches);
    }
    externalOpenTimer_->start(0);
}

void MainWindow::drainExternalLaunches()
{
    if (externalOpenFiles_.isEmpty() || externalOpenDraining_) return;
    // A file-manager launch can replace the startup chooser, but must never
    // dismiss a save prompt, explicit New dialog, About or an in-progress edit.
    if (workspaceDialog_ && startupDocumentDialog_ && !QApplication::activeModalWidget())
        workspaceDialog_->reject();
    if (fileBusy_ || workspaceDialog_ || QApplication::activeModalWidget()
        || QApplication::activePopupWidget() || !shortcutGestureIdle()) {
        externalOpenTimer_->start(200);
        return;
    }
    const QScopedValueRollback draining(externalOpenDraining_, true);
    const auto path = externalOpenFiles_.takeFirst();
    openImageFromPath(path); // same tabs, path deduplication and error UI as File > Open
    if (!externalOpenFiles_.isEmpty()) externalOpenTimer_->start(0);
}

bool MainWindow::restartAfterUpdate(const QString& path,
    std::function<bool(const QString&, const QStringList&)> launch)
{
    if (path.isEmpty() || !QFileInfo(path).isAbsolute() || !QFileInfo(path).isExecutable()) {
        reportFileError(tr("The updated AppImage is unavailable. Check its location and permissions."));
        return false;
    }
    QList<MainWindow*> windows;
    for (auto* widget : QApplication::topLevelWidgets()) {
        // MainWindow intentionally has no Q_OBJECT.
        if (auto* window = dynamic_cast<MainWindow*>(widget); window && window->isVisible())
            windows.append(window);
    }
    if (!windows.contains(this)) windows.prepend(this);
    QStringList projects;
    for (auto* window : windows) {
        if (window->workspaceDialog_ || !window->guardAllDocuments()) return false;
        for (const auto& context : window->documents_)
            if (!context->projectPath.isEmpty()) projects.append(context->projectPath);
    }
    // Preflight can run nested save dialogs. Recheck busy state before closing;
    // no other document is closed if any guard was cancelled.
    for (auto* window : windows)
        if (!window->settleForFileOperation()) return false;
    const auto quitOnClose = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    for (auto* window : windows) {
        const QScopedValueRollback noSecondPrompt(window->unsavedPromptEnabled_, false);
        if (!window->close()) {
            for (auto* restore : windows) restore->show();
            QApplication::setQuitOnLastWindowClosed(quitOnClose);
            return false;
        }
    }
    QSettings().sync();
    if (!launch) launch = [](const QString& executable, const QStringList& arguments) {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        auto restartArguments = arguments;
        // The pinned runtime names its extract-and-run directory this way.
        // Preserve that mode for systems where FUSE mounting is unavailable.
        if (QFileInfo(environment.value(QStringLiteral("APPDIR"))).fileName().startsWith(QStringLiteral("appimage_extracted_")))
            restartArguments.prepend(QStringLiteral("--appimage-extract-and-run"));
        // The new runtime must discover its own mount; never reuse the old one.
        for (const auto* key : {"APPIMAGE", "APPDIR", "ARGV0", "OWD"}) environment.remove(QString::fromLatin1(key));
        process.setProcessEnvironment(environment);
        process.setProgram(executable);
        process.setArguments(restartArguments);
        process.setWorkingDirectory(QDir::currentPath());
        return process.startDetached();
    };
    // Hold the kernel lock until full shutdown. The child waits for it instead
    // of forwarding back into the instance that is about to exit.
    const bool started = launch(path, platform::platformRestartArguments()
        + QStringList{QStringLiteral("--") + QString::fromLatin1(platform::singleInstanceRestartOption)} + projects);
    QApplication::setQuitOnLastWindowClosed(quitOnClose);
    if (!started) {
        for (auto* window : windows) window->show();
        reportFileError(tr("Failed to restart Vulkana. Your open documents are still available."));
        return false;
    }
    QCoreApplication::quit();
    return true;
}

QString MainWindow::documentOpenFilter()
{
    QStringList suffixes;
    for (const auto& format : QImageReader::supportedImageFormats())
        suffixes.append(QStringLiteral("*.%1").arg(QString::fromLatin1(format)));
    return QStringLiteral("Projects and images (*.vulkana %1);;Vulkana project (*.vulkana);;Images (%1)")
        .arg(suffixes.join(' '));
}

void MainWindow::reportFileError(const QString& error)
{
    if (fileInteractions_.reportError)
        fileInteractions_.reportError(error);
    else
        QMessageBox::critical(this, QStringLiteral("Document operation failed"), error);
}

void MainWindow::updateDocumentTitle()
{
    const bool modified = session().document() && session().document()->isModified();
    setWindowTitle(QStringLiteral("%1%2 — Vulkana Image Editor")
            .arg(fileState().displayName, modified ? QStringLiteral(" *") : QString { }));
    refreshDocumentTabs();
}

bool MainWindow::settleForFileOperation()
{
    if (fileBusy_)
        return false;
    finishLayerRename();
    // A held stroke/drag is not a completed user action. Do not serialize its
    // preview or silently cancel it to satisfy a keyboard save request.
    if (shapeCreation_ || activeBrushStroke_ || activeSpotHealStroke_ || spotHealBusyForActiveDocument() || activeCloneStroke_ || cloneProcessing_
        ||activeLocalBlurStroke_||localBlurProcessing_||activeFill_||canvasWindow_->transformDragging()
        || (activeLayerMove_ && activeLayerMove_->dragging())) {
        statusBar()->showMessage(QStringLiteral("Finish the current stroke, fill or drag before continuing."), 4000);
        return false;
    }
    try {
        adjustmentsPanel_->finishEditing();
        finishAdjustmentEdit(true);
        if (adjustmentEdit_) return false;
        filtersPanel_->finishEditing();finishFilterEdit(true);
        if(filterEdit_)return false;
        finishEffectEdit(true);if(effectEdit_)return false;
        cancelFilterPreparation(true); // Release the worker budget before output preparation.
        if (textController_)
            textController_->finish();
        if (textController_ && textController_->active())
            return false;
        moveOptionsPage_->finishNumericInput();
        shapeOptionsPage_->finishNumericInput();
        transformOptionsPage_->finishNumericInput();
        cropOptionsPage_->finishNumericInput();
        finishLayerCrop(true);
        finishLayerMove(true);
        finishLayerTransform(true); // Publish individual completed actions, not a collapsed session.
        finishSelectionTransform(true);
        if (activeLayerMove_ || layerTransform_ || layerCrop_ || selectionTransform_) {
            reportFileError(QStringLiteral("Unable to finish transform history. Your edits are retained; retry or cancel the transform explicitly."));
            return false;
        }
        pointerRouter_->cancelCapture();
        updateDocumentTitle();
        return true;
    } catch (const std::exception& e) {
        reportFileError(QStringLiteral("Cannot finish the current edit: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
}

bool MainWindow::guardUnsavedChanges()
{
    if (!settleForFileOperation())
        return false;
    if (!session().document() || !session().document()->isModified() || !unsavedPromptEnabled_)
        return true;
    UnsavedChoice choice;
    if (fileInteractions_.askUnsaved)
        choice = fileInteractions_.askUnsaved();
    else {
        QMessageBox question(QMessageBox::Question, QStringLiteral("Save changes?"),
            QStringLiteral("Save changes to %1 before continuing?").arg(fileState().displayName),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
        question.setDefaultButton(QMessageBox::Save);
        const int response = question.exec();
        choice = response == QMessageBox::Save ? UnsavedChoice::Save
            : response == QMessageBox::Discard ? UnsavedChoice::Discard
                                               : UnsavedChoice::Cancel;
    }
    if (choice == UnsavedChoice::Cancel)
        return false;
    return choice == UnsavedChoice::Discard || saveDocument();
}

bool MainWindow::saveDocument(bool saveAs)
{
    if (fileBusy_ || !session().document())
        return false;
    // Reject the pending repair before opening a destination dialog, whose
    // nested loop must not mistake a worker result for already-saved content.
    if (activeSpotHealStroke_ || spotHealBusyForActiveDocument()) {
        statusBar()->showMessage(tr("Finish or cancel Spot Heal before saving."), 4000);
        return false;
    }
    QString path = fileState().projectPath;
    const bool chooseDestination = saveAs || path.isEmpty();
    if (chooseDestination) {
        path = fileInteractions_.chooseSavePath ? fileInteractions_.chooseSavePath()
                                                : QFileDialog::getSaveFileName(this, QStringLiteral("Save Vulkana project"),
                                                      fileState().projectPath.isEmpty() ? QFileInfo(fileState().displayName).completeBaseName() + ".vulkana" : fileState().projectPath,
                                                      QStringLiteral("Vulkana project (*.vulkana)"), nullptr, QFileDialog::DontConfirmOverwrite);
        if (path.isEmpty())
            return false;
        if (!path.endsWith(QStringLiteral(".vulkana"), Qt::CaseInsensitive))
            path += QStringLiteral(".vulkana");
        const auto destination = QFileInfo(path).canonicalFilePath().isEmpty() ? QFileInfo(path).absoluteFilePath() : QFileInfo(path).canonicalFilePath();
        for (const auto& other : documents_) {
            if (other == activeDocument_ || other->projectPath.isEmpty()) continue;
            const auto owned = QFileInfo(other->projectPath).canonicalFilePath();
            if (destination == (owned.isEmpty() ? other->projectPath : owned)) {
                reportFileError(tr("That project is already open in another tab. Choose a different Save As destination."));
                return false;
            }
        }
        if (QFileInfo::exists(path)) {
            const bool replace = fileInteractions_.confirmReplace ? fileInteractions_.confirmReplace(path)
                                                                  : QMessageBox::question(this, QStringLiteral("Replace project?"),
                                                                        QStringLiteral("Replace %1?").arg(QFileInfo(path).fileName()),
                                                                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                    == QMessageBox::Yes;
            if (!replace)
                return false;
        }
    }
    if (!settleForFileOperation())
        return false;
    path = QFileInfo(path).absoluteFilePath();
    // Optional project UI metadata, never content/history. Capture only live
    // folder IDs; deleted-folder tombstones remain useful for in-session Undo.
    auto metadata = fileState().metadata;
    auto ui = metadata["ui"].toObject();
    auto layersUi = ui["layers"].toObject();
    QJsonArray collapsed;
    for (auto id : layerModel_->collapsedFolderIds())
        collapsed.append(QString::number(qulonglong(id)));
    layersUi["collapsedFolders"] = collapsed; // Write [] too, clearing old state.
    ui["layers"] = layersUi;
    metadata["ui"] = ui;
    ProjectIoResult result;
    {
        QScopedValueRollback busy(fileBusy_, true);
        QProgressDialog progress(QStringLiteral("Saving and verifying project…"), QStringLiteral("Cancel"), 0, 1000, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(350);
        progress.setAutoClose(false);
        progress.setAutoReset(false);
        QScopedValueRollback progressPointer(fileProgress_, &progress);
        QElapsedTimer cadence;
        cadence.start();
        result = saveProject(path, *session().document(), metadata, [&](quint64 done, quint64 total) {
            if (fileInteractions_.progress && !fileInteractions_.progress(done, total))
                return false;
            if (cadence.elapsed() >= 16) {
                progress.setValue(int(done * 1000 / std::max<quint64>(total, 1)));
                QApplication::processEvents(QEventLoop::AllEvents, 5);
                cadence.restart();
            }
            return !progress.wasCanceled();
        });
    }
    if (!result) {
        refreshMoveControls();
        if (!result.cancelled)
            reportFileError(QStringLiteral("Could not save %1. The previous file was not replaced.\n%2").arg(path, result.error));
        return false;
    }
    fileState().projectPath = path;
    fileState().metadata = std::move(metadata);
    fileState().displayName = QFileInfo(path).fileName();
    session().document()->markSaved();
    refreshMoveControls();
    if (persistWindowState_)
        recentFiles_.recordSuccess(path, RecentFileKind::Project);
    updateDocumentTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(fileState().displayName), 3500);
    return true;
}

bool MainWindow::openDocumentFromPath(const QString& filePath)
{
    if (fileBusy_ || filePath.isEmpty())
        return false;
    const auto path = QFileInfo(filePath).absoluteFilePath();
    const bool project = QFileInfo(path).suffix().compare(QStringLiteral("vulkana"), Qt::CaseInsensitive) == 0;
    if (project) for (const auto& context : documents_) {
        if (!context->projectPath.isEmpty() && QFileInfo(context->projectPath).canonicalFilePath() == QFileInfo(path).canonicalFilePath()
            && !QFileInfo(path).canonicalFilePath().isEmpty()) return activateDocument(context->id);
    }
    std::unique_ptr<core::Document> document;
    QJsonObject metadata;
    QString error;
    QStringList warnings;
    {
        QScopedValueRollback busy(fileBusy_, true);
        if (project) {
            QProgressDialog progress(QStringLiteral("Opening project…"), QStringLiteral("Cancel"), 0, 1000, this);
            progress.setWindowModality(Qt::WindowModal);
            progress.setMinimumDuration(350);
            progress.setAutoClose(false);
            progress.setAutoReset(false);
            QScopedValueRollback progressPointer(fileProgress_, &progress);
            QElapsedTimer cadence;
            cadence.start();
            auto result = loadProject(path, [&](quint64 done, quint64 total) {
                if (fileInteractions_.progress && !fileInteractions_.progress(done, total))
                    return false;
                if (cadence.elapsed() >= 16) {
                    progress.setValue(int(done * 1000 / std::max<quint64>(total, 1)));
                    QApplication::processEvents(QEventLoop::AllEvents, 5);
                    cadence.restart();
                }
                return !progress.wasCanceled();
            });
            if (result.cancelled)
                return false;
            document = std::move(result.document);
            metadata = std::move(result.metadata);
            error = std::move(result.error);
            warnings = std::move(result.warnings);
        } else {
            auto result = loadRasterDocument(path);
            document = std::move(result.document);
            error = result.error;
        }
    }
    if (!document) {
        reportFileError(QStringLiteral("Could not open %1. The current document was kept.\n%2").arg(path, error));
        return false;
    }
    document->markSaved();
    if (!initializeDocument(std::move(document), QFileInfo(path).fileName(), project ? path : QString{}, std::move(metadata), path)) return false;
    fileState().untouched = false;
    if (persistWindowState_) {
        recentFiles_.recordSuccess(path, project ? RecentFileKind::Project : RecentFileKind::Image);
        rememberOpenedDocument(path);
    }
    synchronizeUi(true, true);
    // The initial primary is an editing fallback, not an explicit user pick.
    // Do this after publishing targets (IDs may be reused by another project).
    canvasWindow_->dismissLayerOutlines();
    updateDocumentTitle();
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(fileState().displayName), 3500);
    if (!warnings.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Project font substitutions"), warnings.join('\n'));
    return true;
}

void MainWindow::refreshRecentMenu()
{
    recentMenu_->clear();
    for (const auto& entry : recentFiles_.entries()) {
        const QFileInfo file(entry.path);
        auto* item = recentMenu_->addMenu(QStringLiteral("%1  ·  %2 — %3")
                .arg(file.fileName(), entry.kind == RecentFileKind::Project ? QStringLiteral("Project") : QStringLiteral("Image"), file.absolutePath()));
        item->addAction(QStringLiteral("Open"), this, [this, path = entry.path] { openImageFromPath(path); });
        item->addAction(QStringLiteral("Remove from recent files"), this, [this, path = entry.path] { recentFiles_.remove(path); });
    }
    if (recentFiles_.entries().isEmpty())
        recentMenu_->addAction(QStringLiteral("No recent files"))->setEnabled(false);
    else {
        recentMenu_->addSeparator();
        recentMenu_->addAction(QStringLiteral("Clear recent files"), this, [this] { recentFiles_.clear(); });
    }
}
}
