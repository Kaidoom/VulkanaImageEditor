#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/UpdateService.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/TextController.hpp"

#include <QApplication>
#include <QDialog>
#include <QComboBox>
#include <QSettings>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVBoxLayout>

namespace imageeditor::ui {
void MainWindow::startStartupFlow(bool showNewDocument, QNetworkAccessManager* transport, bool testUpdate)
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

    if (!testUpdate && !platform::ServiceConfig::load().enabled()) {
        startupUiTimer_->start(0);
        return;
    }
    // One bounded, asynchronous request per launch, independent of old versions'
    // saved timestamps. Offline startup remains silent and responsive.
    if (!updateService_) updateService_ = new UpdateService(QCoreApplication::applicationVersion(), this, transport);
    updateService_->onChanged = [this] {
        if (updateService_->busy()) return;
        startupUpdateNoticePending_ = updateService_->state() == UpdateService::State::Available;
        // Queue presentation after the network callback; no modal nested event
        // loop inside UpdateService::finish(). Failure is deliberately silent.
        startupUiTimer_->start(0);
    };
    if (testUpdate) updateService_->simulateUpdate();
    else updateService_->check(2500);
}

void MainWindow::presentStartupUi()
{
    if (!isVisible()) return;
    if (activeDocument_ && !fileState().untouched) startupNewDocumentPending_ = false;
    const bool chooseLanguage = qApp->property("vulkanaOfferLanguageChoice").toBool()
        && !QSettings().contains(QStringLiteral("preferences/ui/language"));
    if (!startupNewDocumentPending_ && !startupUpdateNoticePending_ && !chooseLanguage) return;
    if (isMinimized() || fileBusy_ || workspaceDialog_ || QApplication::activeModalWidget()
        || QApplication::activePopupWidget() || !shortcutGestureIdle() || editorTextInputActive()) {
        // Never interrupt an edit, native popup or another workspace dialog.
        // This timer exists only while startup presentation is pending.
        startupUiTimer_->start(200);
        return;
    }
    if (chooseLanguage) {
        qApp->setProperty("vulkanaOfferLanguageChoice", false);
        WorkspaceDialog presenter(*workspace_, *this);
        QDialog dialog(&presenter, Qt::SubWindow);
        dialog.setObjectName(QStringLiteral("FirstRunLanguage"));
        i18n::setWindowTitle(&dialog, i18n::text("language.first.title"));
        dialog.setFixedWidth(370);
        auto* layout = new QVBoxLayout(&dialog); layout->setContentsMargins(20,18,20,18);
        layout->addWidget(i18n::make<QLabel>(i18n::text("language.first.title")));
        auto* choice = new QComboBox(&dialog); choice->setObjectName(QStringLiteral("FirstRunLanguageCombo"));
        i18n::addItem(choice, i18n::text("language.system"), QStringLiteral("system"));
        for (const auto& language : i18n::Localization::instance().languages()) choice->addItem(language.nativeName, language.id);
        // System Default resolves to the system match and stays that preference.
        layout->addWidget(choice);
        auto* description = i18n::make<QLabel>(i18n::text("language.first.description"));
        description->setWordWrap(true); layout->addWidget(description);
        auto* proceed = i18n::make<QPushButton>(i18n::text("language.continue")); layout->addWidget(proceed);
        connect(proceed, &QPushButton::clicked, &dialog, &QDialog::accept);
        const auto previous = i18n::Localization::instance().preference();
        connect(choice, &QComboBox::currentIndexChanged, &dialog, [choice] {
            i18n::Localization::instance().requestLanguage(choice->currentData().toString());
        });
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        if (presenter.exec(dialog) == QDialog::Accepted)
            QSettings().setValue(QStringLiteral("preferences/ui/language"), choice->currentData());
        else i18n::Localization::instance().requestLanguage(previous);
    }
    if (startupUpdateNoticePending_) {
        startupUpdateNoticePending_ = false;
        bool openAbout = false;
        {
            WorkspaceDialog presenter(*workspace_, *this);
            QDialog notification(&presenter, Qt::SubWindow);
            notification.setObjectName(QStringLiteral("StartupUpdateNotification"));
            i18n::setWindowTitle(&notification, i18n::text("main.window.updates.update.available"));
            notification.setFixedWidth(390);
            auto* layout = new QVBoxLayout(&notification);
            layout->setContentsMargins(20, 18, 20, 18);
            layout->setSpacing(14);
            auto* title = i18n::make<QLabel>(i18n::text("main.window.updates.update.available"));
            auto font = title->font(); font.setBold(true); title->setFont(font);
            auto ink = title->palette(); ink.setColor(QPalette::WindowText, themeColor(ThemeColor::Accent)); title->setPalette(ink);
            layout->addWidget(title);
            auto* version = i18n::make<QLabel>(i18n::text("main.window.updates.vulkana.1.is.available").arg(updateService_->release()->version));
            version->setObjectName(QStringLiteral("StartupUpdateVersion"));
            version->setWordWrap(true);
            layout->addWidget(version);
            if (updateService_->simulated()) {
                auto* note = i18n::make<QLabel>(i18n::text("main.window.updates.test.update.only.downloads.and.installation.are.disabled"));
                note->setObjectName(QStringLiteral("StartupUpdateTestNote"));
                note->setWordWrap(true);
                layout->addWidget(note);
            }
            auto* buttons = new QHBoxLayout;
            buttons->addStretch();
            auto* dismiss = i18n::make<QPushButton>(i18n::text("main.window.updates.dismiss"));
            dismiss->setObjectName(QStringLiteral("StartupUpdateDismiss"));
            auto* view = i18n::make<QPushButton>(i18n::text("main.window.updates.view.update"));
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
