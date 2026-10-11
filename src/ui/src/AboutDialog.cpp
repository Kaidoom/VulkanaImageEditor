#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/AboutDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/UpdateService.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/platform/BuildInfo.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace imageeditor::ui {
AboutDialog::AboutDialog(QWidget* parent, QNetworkAccessManager* transport, UpdatePackage package,
    QString appImagePath, UpdateService* sharedUpdates)
    : QDialog(parent, Qt::SubWindow),
      updates_(sharedUpdates ? sharedUpdates : new UpdateService(QCoreApplication::applicationVersion(), this, transport, package, std::move(appImagePath)))
{
    setObjectName(QStringLiteral("AboutVulkanaDialog"));
    i18n::setWindowTitle(this, i18n::text("about.dialog.about.vulkana"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setSizeGripEnabled(false);
    setFixedWidth(580);
    const auto accent = themeColor(ThemeColor::Accent);
    const auto secondary = themeColor(ThemeColor::SecondaryText);
    const auto textInk = themeColor(ThemeColor::Text);
    const auto surfaceInk = themeColor(ThemeColor::Surface);
    const bool textIsLighter = qGray(textInk.rgb()) > qGray(surfaceInk.rgb());
    const auto primaryInk = qGray(accent.rgb()) > 150
        ? (textIsLighter ? surfaceInk : textInk) : (textIsLighter ? textInk : surfaceInk);
    setStyleSheet(QStringLiteral(
        "QLabel[aboutSecondary=\"true\"] { color: %1; }"
        "QLabel#AboutVulkanaTitle { font-size: 18pt; font-weight: 650; }"
        "QLabel#AboutUpdateStatus { font-weight: 650; }"
        "QFrame#AboutDivider { background: %2; border: none; }"
        "QFrame#AboutUpdateCard { background: %3; border: 1px solid %2; border-radius: 8px; }"
        "QPushButton#AboutCheckUpdate { background: %4; color: %5; border-color: %4; font-weight: 600; }"
        "QPushButton#AboutCheckUpdate:hover { background: %6; }"
        "QPushButton#AboutCheckUpdate:pressed { background: %7; }"
        "QPushButton#AboutCheckUpdate:disabled { background: %3; color: %1; border-color: %2; }"
        "QPushButton#AboutHeaderClose { background: transparent; border: none; padding: 4px; }"
        "QPushButton#AboutHeaderClose:hover { background: %3; }")
        .arg(secondary.name(), themeColor(ThemeColor::Border).name(), themeColor(ThemeColor::Surface).name(),
            accent.name(), primaryInk.name(), accent.lighter(110).name(), accent.darker(110).name()));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    const auto divider = [this] {
        auto* line = new QFrame(this);
        line->setObjectName(QStringLiteral("AboutDivider"));
        line->setFixedHeight(1);
        return line;
    };
    auto* header = new QHBoxLayout;
    header->setContentsMargins(20, 10, 14, 10);
    auto* heading = i18n::make<QLabel>(i18n::text("about.dialog.about.vulkana.image.editor"), this);
    heading->setProperty("aboutSecondary", true);
    header->addWidget(heading);
    header->addStretch();
    auto* headerClose = new QPushButton(toolGlyph(ToolGlyph::Close, secondary), {}, this);
    headerClose->setObjectName(QStringLiteral("AboutHeaderClose"));
    i18n::setAccessibleName(headerClose, i18n::text("about.dialog.close.about"));
    i18n::setToolTip(headerClose, i18n::text("about.dialog.close"));
    headerClose->setAutoDefault(false);
    headerClose->setFixedSize(30, 30);
    headerClose->setIconSize({18, 18});
    connect(headerClose, &QPushButton::clicked, this, &QDialog::reject);
    header->addWidget(headerClose);
    outer->addLayout(header);
    outer->addWidget(divider());
    auto* layout = new QVBoxLayout;
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(10);
    outer->addLayout(layout);
    auto* icon = new QLabel(this);
    icon->setObjectName(QStringLiteral("AboutVulkanaIcon"));
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(QApplication::windowIcon().pixmap(QSize(96, 96), devicePixelRatioF()));
    layout->addWidget(icon);
    auto* title = i18n::make<QLabel>(i18n::text("about.dialog.vulkana.image.editor"), this);
    title->setObjectName(QStringLiteral("AboutVulkanaTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    const auto currentVersion = QCoreApplication::applicationVersion().isEmpty()
        ? QString::fromLatin1(platform::buildVersion) : QCoreApplication::applicationVersion();
    auto* version = i18n::make<QLabel>(i18n::text("about.dialog.version.1.2")
        .arg(i18n::ltrToken(currentVersion), i18n::ltrToken(QString::fromLatin1(platform::releaseChannel))), this);
    version->setObjectName(QStringLiteral("AboutVulkanaVersion"));
    version->setProperty("aboutSecondary", true);
    version->setAlignment(Qt::AlignCenter);
    layout->addWidget(version);
    const auto addPolicyLine = [&](const char* name, const i18n::Message& text, bool subdued) {
        auto* line = i18n::make<QLabel>(text, this);
        line->setObjectName(QString::fromLatin1(name));
        line->setAlignment(Qt::AlignCenter);
        line->setWordWrap(true);
        line->setProperty("aboutSecondary", subdued);
        layout->addWidget(line);
    };
    addPolicyLine("AboutUsage", i18n::text("about.dialog.free.to.use.for.personal.and.commercial.projects"), false);
    addPolicyLine("AboutCopyright", i18n::text("about.dialog.2026.kaidoomdev.licensed.under.the.mit.license"), true);
    addPolicyLine("AboutThirdPartyRights", i18n::text("about.dialog.third.party.components.are.licensed.separately"), true);
    addPolicyLine("AboutBetaNote", i18n::text("about.dialog.actively.developed.beta.please.keep.backups.of.important.projects"), true);
    layout->addSpacing(2);
    layout->addWidget(divider());
    layout->addSpacing(2);
    auto* contactsRow = new QHBoxLayout;
    contactsRow->addStretch();
    auto* contacts = new QGridLayout;
    contacts->setHorizontalSpacing(12);
    contacts->setVerticalSpacing(9);
    contactsRow->addLayout(contacts);
    contactsRow->addStretch();
    layout->addLayout(contactsRow);
    const auto addContact = [&](ToolGlyph glyph, const QString& name, const i18n::Message& text, const i18n::Message& tooltip) {
        const int row = contacts->rowCount();
        auto* symbol = new QLabel(this);
        symbol->setObjectName(name + QStringLiteral("Icon"));
        symbol->setFixedSize(20, 20);
        symbol->setPixmap(toolGlyph(glyph, secondary).pixmap({20, 20}, devicePixelRatioF()));
        contacts->addWidget(symbol, row, 0, Qt::AlignCenter);
        const auto resolve = [text, accent] {
            return text.resolved().replace(QStringLiteral("<a "), QStringLiteral("<a style=\"color:%1;\" ").arg(accent.name()));
        };
        auto* contact = i18n::make<QLabel>(i18n::Message(resolve(), resolve), this);
        contact->setObjectName(name);
        contact->setTextFormat(Qt::RichText);
        contact->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        contact->setTextInteractionFlags(Qt::TextBrowserInteraction);
        contact->setOpenExternalLinks(true);
        auto linkPalette = contact->palette();
        linkPalette.setColor(QPalette::Link, themeColor(ThemeColor::Accent));
        linkPalette.setColor(QPalette::LinkVisited, themeColor(ThemeColor::Accent));
        contact->setPalette(linkPalette);
        i18n::setToolTip(contact, tooltip);
        contacts->addWidget(contact, row, 1);
    };
    addContact(ToolGlyph::Person, QStringLiteral("AboutVulkanaDeveloper"), i18n::text("about.dialog.developed.by.kaidoomdev"), {});
    addContact(ToolGlyph::SocialX, QStringLiteral("AboutVulkanaSocial"),
        QStringLiteral("<a href=\"https://x.com/KaidoomDev\">@KaidoomDev</a>"),
        QStringLiteral("https://x.com/KaidoomDev"));
    addContact(ToolGlyph::Repository, QStringLiteral("AboutVulkanaGitHub"),
        i18n::Message("<a href=\"https://github.com/Kaidoom/VulkanaImageEditor\">%1</a>")
            .arg(i18n::text("about.dialog.view.on.github").toHtmlEscaped()),
        QStringLiteral("https://github.com/Kaidoom/VulkanaImageEditor"));
    addContact(ToolGlyph::Mail, QStringLiteral("AboutVulkanaEmail"),
        QStringLiteral("<a href=\"mailto:kaidoomdev@pm.me\">kaidoomdev@pm.me</a>"),
        i18n::text("about.dialog.send.an.email.to.kaidoomdev.pm.me"));
    const auto services = platform::ServiceConfig::load();
    if (!services.origin.isEmpty())
        addContact(ToolGlyph::Globe, QStringLiteral("AboutVulkanaWebsite"),
            QStringLiteral("<a href=\"%1\">%2</a>").arg(services.origin.toString().toHtmlEscaped(),
                services.origin.host().toHtmlEscaped()), services.origin.toString());
    layout->addSpacing(8);
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("AboutUpdateCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 16, 16, 16);
    cardLayout->setSpacing(10);
    auto* updateRow = new QHBoxLayout;
    updateRow->setSpacing(12);
    auto* stateIcon = new QLabel(card);
    stateIcon->setObjectName(QStringLiteral("AboutUpdateIcon"));
    stateIcon->setFixedSize(32, 32);
    updateRow->addWidget(stateIcon, 0, Qt::AlignVCenter);
    auto* messages = new QVBoxLayout;
    messages->setSpacing(4);
    auto* status = new QLabel(card);
    status->setObjectName(QStringLiteral("AboutUpdateStatus"));
    status->setTextFormat(Qt::PlainText); // Network error strings are not markup.
    status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    status->setWordWrap(true);
    auto messagePolicy = status->sizePolicy();
    messagePolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    status->setSizePolicy(messagePolicy);
    status->setMaximumHeight(status->fontMetrics().lineSpacing() * 2);
    messages->addWidget(status);
    auto* detail = new QLabel(card);
    detail->setObjectName(QStringLiteral("AboutUpdateDetail"));
    detail->setProperty("aboutSecondary", true);
    detail->setTextFormat(Qt::PlainText);
    detail->setWordWrap(true);
    detail->setSizePolicy(messagePolicy);
    detail->setMaximumHeight(detail->fontMetrics().lineSpacing() * 3);
    messages->addWidget(detail);
    updateRow->addLayout(messages, 1);
    auto* progress = new QProgressBar(card);
    progress->setObjectName(QStringLiteral("AboutDownloadProgress"));
    progress->setFixedHeight(18);
    progress->setRange(0, 100);
    progress->hide();
    auto* check = i18n::make<QPushButton>(i18n::text("about.dialog.check.for.updates"), card);
    check->setObjectName(QStringLiteral("AboutCheckUpdate"));
    i18n::setToolTip(check, i18n::text("about.dialog.check.for.a.newer.release"));
    check->setAutoDefault(false);
    check->setFixedWidth(174);
    check->setMinimumHeight(28);
    check->setIconSize({18, 18});
    updateRow->addWidget(check, 0, Qt::AlignVCenter);
    cardLayout->addLayout(updateRow);
    cardLayout->addWidget(progress);
    layout->addWidget(card);
    outer->addWidget(divider());
    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(20, 14, 20, 14);
    auto* notices = i18n::make<QPushButton>(i18n::text("about.dialog.third.party.notices"), this);
    notices->setObjectName(QStringLiteral("AboutThirdPartyNotices"));
    notices->setAutoDefault(false);
    i18n::setToolTip(notices, i18n::text("about.dialog.read.offline.component.licenses.notices.and.release.source.information"));
    connect(notices, &QPushButton::clicked, this, [this] { done(NoticesRequested); });
    footer->addWidget(notices);
    footer->addStretch();
    auto* buttons = new i18n::ButtonBox(QDialogButtonBox::Close, this);
    auto* page = i18n::make<QPushButton>(i18n::text("about.dialog.open.download.page"), buttons);
    buttons->addButton(page, QDialogButtonBox::ActionRole);
    page->setObjectName(QStringLiteral("AboutDownloadPage"));
    page->setAutoDefault(false);
    page->setIcon(toolGlyph(ToolGlyph::ExternalLink));
    const auto downloadPage = UpdateService::downloadPageUrl(services);
    page->setToolTip(downloadPage.toString());
    page->setEnabled(!downloadPage.isEmpty());
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("AboutClose"));
    connect(page, &QPushButton::clicked, this, [this, status, detail, downloadPage] {
        if (!QDesktopServices::openUrl(downloadPage)) {
            i18n::setText(status, i18n::text("about.dialog.could.not.open.your.browser"));
            detail->setText(downloadPage.toDisplayString());
            refreshLayout();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    footer->addWidget(buttons);
    outer->addLayout(footer);
    updates_->onChanged = [this, check, status, detail, progress, stateIcon, primaryInk, accent, currentVersion] {
        using State = UpdateService::State;
        const auto state = updates_->state();
        const bool appImage=updates_->package()==UpdatePackage::AppImage;
        const auto packageName = updates_->package()==UpdatePackage::Deb ? QStringLiteral("DEB") : QStringLiteral("RPM");
        const bool restart=state==State::RestartReady;
        const bool downloading = updates_->state() == UpdateService::State::Downloading;
        const bool checking = updates_->state() == UpdateService::State::Checking;
        const bool canDownload = updates_->release().has_value() && state != State::Downloaded && !restart;
        check->setEnabled(!updates_->simulated() && (updates_->configured() || restart));
        // Keep keyboard focus here while checking. Disabling a focused button
        // makes Qt move focus (and its highlight) to Open Download Page.
        // Repeated checks are ignored below until the reply completes.
        i18n::setText(check, restart ? i18n::text("about.dialog.restart.now") : downloading ? i18n::text("about.dialog.cancel.download") : checking ? i18n::text("about.dialog.checking")
            : canDownload ? (appImage ? i18n::text("about.dialog.update.appimage") : i18n::text("about.dialog.download.1").arg(packageName)) : i18n::text("about.dialog.check.for.updates"));
        check->setIcon(toolGlyph(downloading ? ToolGlyph::Close
            : canDownload ? ToolGlyph::Download : ToolGlyph::Refresh, primaryInk));
        i18n::setToolTip(check, updates_->simulated() ? i18n::text("about.dialog.downloads.are.disabled.for.this.simulated.update")
            : restart ? i18n::text("about.dialog.restart.using.the.verified.update.unsaved.documents.are.checked")
            : downloading ? i18n::text("about.dialog.cancel.the.download.without.saving.a.partial.file")
            : canDownload ? (appImage ? i18n::text("about.dialog.download.verify.and.atomically.replace.this.appimage.restart.is")
                : i18n::text("about.dialog.choose.where.to.save.the.1.it.will.not").arg(packageName))
            : !updates_->configured() ? i18n::text("about.dialog.updates.are.not.configured.for.this.build")
            : i18n::text("about.dialog.check.for.a.newer.release"));
        progress->setVisible(downloading);
        if (downloading) progress->setRange(0, 0);
        i18n::setText(status, !updates_->configured() && !restart && !updates_->simulated() ? i18n::text("about.dialog.updates.are.not.configured")
            : state == State::Idle ? i18n::text("about.dialog.check.for.a.new.release")
            : restart ? i18n::text("about.dialog.update.ready.to.restart") : state == State::Downloaded ? i18n::text("about.dialog.download.saved.and.verified")
            : state == State::Error ? i18n::text("about.dialog.could.not.complete.the.request") : updates_->message());
        i18n::setText(detail, updates_->simulated() ? i18n::text("about.dialog.test.update.only.no.network.request.download.or.installation")
            : state == State::Error ? updates_->message()
            : state == State::Checking ? i18n::text("about.dialog.contacting.the.update.service")
            : state == State::Downloading ? i18n::text("about.dialog.verifying.the.file.before.saving")
            : restart ? i18n::text("about.dialog.your.current.session.stays.open.until.you.restart")
            : state == State::Available ? (appImage ? i18n::text("about.dialog.the.current.appimage.is.kept.until.verification.succeeds") : i18n::text("about.dialog.save.the.1.then.install.it.manually").arg(packageName))
            : state == State::Downloaded ? i18n::text("about.dialog.install.the.1.manually.when.ready").arg(packageName)
            : i18n::text("about.dialog.you.re.running.version.1").arg(i18n::ltrToken(currentVersion)));
        stateIcon->setPixmap(toolGlyph(state == State::Current || state == State::Downloaded || restart
            ? ToolGlyph::CheckCircle : canDownload ? ToolGlyph::Download : ToolGlyph::InfoCircle,
            accent).pixmap({32, 32}, devicePixelRatioF()));
        i18n::setToolTip(status, updates_->downloadedPath().isEmpty() ? updates_->message()
            : i18n::text("about.dialog.saved.to.1").arg(updates_->downloadedPath()));
        i18n::setToolTip(detail, updates_->downloadedPath().isEmpty() ? updates_->message()
            : i18n::text("about.dialog.saved.to.1").arg(updates_->downloadedPath()));
        refreshLayout();
    };
    updates_->onProgress = [progress](qint64 received, qint64 total) {
        if (total <= 0) progress->setRange(0, 0);
        else {
            progress->setRange(0, 100);
            progress->setValue(static_cast<int>(qBound(0.0, 100.0 * static_cast<double>(received)
                / static_cast<double>(total), 100.0)));
        }
    };
    connect(check, &QPushButton::clicked, this, [this] {
        if (updates_->simulated()) return;
        if(updates_->state()==UpdateService::State::RestartReady) {
            if(onRestartRequested) {onRestartRequested(updates_->downloadedPath());accept();}
            return;
        }
        if (updates_->state() == UpdateService::State::Checking) return;
        if (updates_->busy()) { updates_->cancel(); return; }
        if (!updates_->release() || updates_->state() == UpdateService::State::Downloaded) {
            updates_->check();
            return;
        }
        if(updates_->package()==UpdatePackage::AppImage) {updates_->downloadAppImage();return;}
        const auto folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const auto suggested = QDir(folder).filePath(QFileInfo(updates_->release()->url.path()).fileName());
        QString destination = QFileDialog::getSaveFileName(popupTopLevelOwner(this), i18n::text("about.dialog.save.vulkana.update"),
            suggested, updates_->package()==UpdatePackage::Deb ? i18n::text("about.dialog.deb.packages.deb") : i18n::text("about.dialog.rpm.packages.rpm"));
        if (destination.isEmpty()) return;
        updates_->download(destination);
    });
    connect(this, &QDialog::finished, this, [this] { updates_->cancel(); });
    // Rich-text labels invalidate their metrics through queued Qt language/
    // layout events. Measure afterwards, never from the previous script's cache.
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged,
        this, [this] { refreshLayout(); }, Qt::QueuedConnection);
    updates_->onChanged();
}

void AboutDialog::refreshLayout()
{
    // Let wrapped status/detail text determine its true height; there is no
    // reserved blank progress row and no horizontal jump between action roles.
    layout()->invalidate();
    const int preferredHeight = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width())
        : layout()->sizeHint().height();
    setFixedHeight(preferredHeight);
    layout()->activate();
    if (isVisible() && parentWidget() && !isWindow())
        move((parentWidget()->width() - width()) / 2, (parentWidget()->height() - height()) / 2);
}

AboutDialog::~AboutDialog()
{
    updates_->onChanged = {};
    updates_->onProgress = {};
    updates_->cancel();
}
} // namespace imageeditor::ui
