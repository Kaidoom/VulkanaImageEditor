#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/UpdateService.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ProjectIntegrationDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/platform/ProjectIntegration.hpp"
#include "imageeditor/render/CanvasWindow.hpp"

#include <QApplication>
#include <QDialog>
#include <QFrame>
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
    startupNewDocumentPending_ = showNewDocument;
    if (showNewDocument && activeDocument_ && activeDocument_->untouched && !session().document()->isModified()) {
        auto placeholder = activeDocument_;
        showEmptyWorkspace();
        placeholder->closed = true;
        textController_->forgetSession(placeholder->session);
        documents_.clear();
        refreshDocumentTabs(); updateDocumentResources();
    }
    startupUiTimer_ = new QTimer(this);
    startupUiTimer_->setSingleShot(true);
    connect(startupUiTimer_, &QTimer::timeout, this, &MainWindow::presentStartupUi);
    // Startup cards must not wait for the network, including on first launch.
    startupUiTimer_->start(0);

    if (!testUpdate && !platform::ServiceConfig::load().enabled()) {
        return;
    }
    // One bounded, asynchronous request per launch, independent of old versions'
    // saved timestamps. Offline startup remains silent and responsive.
    if (!updateService_) updateService_ = new UpdateService(QCoreApplication::applicationVersion(), this, transport);
    startupUpdateCheckPending_ = true;
    updateService_->onChanged = [this] {
        if (!startupUpdateCheckPending_ || updateService_->busy()) return;
        startupUpdateCheckPending_ = false;
        startupUpdateNoticePending_ = updateService_->state() == UpdateService::State::Available;
        // Results are presentation-only; failure never interrupts the editor.
        startupUiTimer_->start(0);
    };
    if (testUpdate) updateService_->simulateUpdate();
    else updateService_->check(2500);
}

void MainWindow::presentStartupUi()
{
    if (!isVisible()||graphicsClosePending_) return;
    // The compiler is not yet running before the first expose/render event.
    // Wait for a presented frame, not just for "not currently compiling", for
    // Auto as well as explicit GPUs and for every kind of startup UI.
    if(canvasWindow_->vulkanInstance()&&!canvasWindow_->rendererStats().presentQueuedFrames) {
        startupUiTimer_->start(100);return;
    }
    if(canvasWindow_->graphicsInitializing()) {startupUiTimer_->start(100);return;}
    if (startupUpdateNoticePending_) {
        startupUpdateNoticePending_ = false;
        showStartupUpdateNotice();
    }
    if (activeDocument_ && !fileState().untouched) startupNewDocumentPending_ = false;
    const bool chooseLanguage = qApp->property("vulkanaOfferLanguageChoice").toBool()
        && !QSettings().contains(QStringLiteral("preferences/ui/language"));
    const bool offerIntegration = qApp->property("vulkanaOfferProjectIntegration").toBool()
        && platform::projectIntegrationAvailable(QCoreApplication::applicationFilePath())
        && (qApp->property("vulkanaForceProjectIntegrationPrompt").toBool()
            || !QSettings().contains(platform::projectIntegrationChoiceKey));
    if (!startupNewDocumentPending_ && !chooseLanguage && !offerIntegration) return;
    // A replacement GPU's recovery notice precedes the other startup cards.
    if(gpuFallbackPending_) {
        startupUiTimer_->start(200);return;
    }
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
    if (offerIntegration) {
        qApp->setProperty("vulkanaOfferProjectIntegration", false);
        WorkspaceDialog presenter(*workspace_, *this);
        ProjectIntegrationDialog dialog(QSettings().value(platform::projectIntegrationChoiceKey).toString() != "registered", &presenter);
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        presenter.exec(dialog);
    }
    if (startupNewDocumentPending_ && isVisible()) {
        startupNewDocumentPending_ = false;
        showStartupDocument();
    }
}

void MainWindow::showStartupUpdateNotice()
{
    if (startupUpdateNotice_ || !updateService_ || !updateService_->release()
        || updateService_->state() != UpdateService::State::Available) return;
    auto* notice = new QFrame(workspace_->panelOverlay());
    startupUpdateNotice_ = notice;
    notice->setObjectName(QStringLiteral("StartupUpdateNotification"));
    notice->setFocusPolicy(Qt::NoFocus);
    notice->setAttribute(Qt::WA_ShowWithoutActivating);
    auto* layout = new QVBoxLayout(notice);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    auto* header = new QHBoxLayout;
    auto* icon = new QLabel;
    icon->setPixmap(toolGlyph(ToolGlyph::Download, themeColor(ThemeColor::Accent)).pixmap(20, 20));
    header->addWidget(icon);
    auto* title = i18n::make<QLabel>(i18n::text("main.window.updates.update.available"));
    auto font = title->font(); font.setBold(true); title->setFont(font);
    title->setWordWrap(true);
    header->addWidget(title, 1);
    layout->addLayout(header);
    auto* version = i18n::make<QLabel>(i18n::text("main.window.updates.vulkana.1.is.available")
        .arg(i18n::ltrToken(updateService_->release()->version)));
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
    auto* update = i18n::make<QPushButton>(i18n::text("main.window.updates.view.update"));
    update->setObjectName(QStringLiteral("StartupUpdateOpen"));
    buttons->addWidget(dismiss); buttons->addWidget(update);
    layout->addLayout(buttons);
    connect(dismiss, &QPushButton::clicked, this, &MainWindow::dismissStartupUpdateNotice);
    connect(update, &QPushButton::clicked, this, &MainWindow::showAbout, Qt::QueuedConnection);
    workspace_->setNotificationOverlay(notice, true);
}

void MainWindow::dismissStartupUpdateNotice()
{
    startupUpdateNoticePending_ = false;
    if (!startupUpdateNotice_) return;
    workspace_->setNotificationOverlay(nullptr, false);
    startupUpdateNotice_->deleteLater();
    startupUpdateNotice_.clear();
}
} // namespace imageeditor::ui
