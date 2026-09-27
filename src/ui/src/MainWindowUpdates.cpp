#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/UpdateService.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/TextController.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace imageeditor::ui {
void MainWindow::startStartupFlow(bool showNewDocument, bool force, QNetworkAccessManager* transport)
{
    if (startupUiTimer_) return;
    if (!externalOpenFiles_.isEmpty()) showNewDocument = false;
    if (showNewDocument && activeDocument_ && activeDocument_->untouched && !session().document()->isModified()) {
        auto placeholder = activeDocument_;
        showEmptyWorkspace();
        placeholder->closed = true;
        textController_->forgetSession(placeholder->session);
        documents_.clear();
        refreshDocumentTabs(); updateDocumentResources();
    }
    startupNewDocumentPending_ = showNewDocument;
    startupUiTimer_ = new QTimer(this);
    startupUiTimer_->setSingleShot(true);
    connect(startupUiTimer_, &QTimer::timeout, this, &MainWindow::presentStartupUi);

    QSettings settings;
    const auto key = QStringLiteral("updates/lastStartupAttemptUtcSeconds");
    bool valid = false;
    const auto previous = settings.value(key).toLongLong(&valid);
    const auto now = QDateTime::currentSecsSinceEpoch();
    if (!platform::ServiceConfig::load().enabled()
        || !UpdateService::startupCheckDue(now, valid ? std::optional(previous) : std::nullopt, force)) {
        startupUiTimer_->start(0);
        return;
    }
    // Throttle attempts, not just successes: offline launches should not retry
    // repeatedly. Manual checks in About remain independent of this throttle.
    settings.setValue(key, now);
    settings.sync();
    if (!updateService_) updateService_ = new UpdateService(QCoreApplication::applicationVersion(), this, transport);
    updateService_->onChanged = [this] {
        if (updateService_->busy()) return;
        startupUpdateNoticePending_ = updateService_->state() == UpdateService::State::Available;
        // Queue presentation after the network callback; no modal nested event
        // loop inside UpdateService::finish(). Failure is deliberately silent.
        startupUiTimer_->start(0);
    };
    updateService_->check(2500);
}

void MainWindow::presentStartupUi()
{
    if (!isVisible()) return;
    if (activeDocument_ && !fileState().untouched) startupNewDocumentPending_ = false;
    if (!startupNewDocumentPending_ && !startupUpdateNoticePending_) return;
    if (isMinimized() || fileBusy_ || workspaceDialog_ || QApplication::activeModalWidget()
        || QApplication::activePopupWidget() || !shortcutGestureIdle() || editorTextInputActive()) {
        // Never interrupt an edit, native popup or another workspace dialog.
        // This timer exists only while startup presentation is pending.
        startupUiTimer_->start(200);
        return;
    }
    if (startupUpdateNoticePending_) {
        startupUpdateNoticePending_ = false;
        bool openAbout = false;
        {
            WorkspaceDialog presenter(*workspace_, *this);
            QDialog notification(&presenter, Qt::SubWindow);
            notification.setObjectName(QStringLiteral("StartupUpdateNotification"));
            notification.setWindowTitle(tr("Update available"));
            notification.setFixedWidth(390);
            auto* layout = new QVBoxLayout(&notification);
            layout->setContentsMargins(20, 18, 20, 18);
            layout->setSpacing(14);
            auto* title = new QLabel(tr("Update available"));
            auto font = title->font(); font.setBold(true); title->setFont(font);
            auto ink = title->palette(); ink.setColor(QPalette::WindowText, themeColor(ThemeColor::Accent)); title->setPalette(ink);
            layout->addWidget(title);
            auto* version = new QLabel(tr("Vulkana %1 is available.").arg(updateService_->release()->version));
            version->setObjectName(QStringLiteral("StartupUpdateVersion"));
            version->setWordWrap(true);
            layout->addWidget(version);
            auto* buttons = new QHBoxLayout;
            buttons->addStretch();
            auto* dismiss = new QPushButton(tr("Dismiss"));
            dismiss->setObjectName(QStringLiteral("StartupUpdateDismiss"));
            auto* view = new QPushButton(tr("View Update"));
            view->setObjectName(QStringLiteral("StartupUpdateOpen"));
            buttons->addWidget(dismiss); buttons->addWidget(view);
            layout->addLayout(buttons);
            connect(dismiss, &QPushButton::clicked, &notification, &QDialog::reject);
            connect(view, &QPushButton::clicked, &notification, &QDialog::accept);
            const QScopedValueRollback active(workspaceDialog_, &presenter);
            openAbout = presenter.exec(notification) == QDialog::Accepted;
        }
        if (openAbout) {
            startupNewDocumentPending_ = false;
            showAbout(); // Reuses the validated release; no second HTTP request.
            return;
        }
    }
    if (startupNewDocumentPending_ && isVisible()) {
        startupNewDocumentPending_ = false;
        showStartupDocument();
    }
}
} // namespace imageeditor::ui
