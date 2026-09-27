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
    setWindowTitle(tr("About Vulkana"));
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
    auto* heading = new QLabel(tr("About Vulkana Image Editor"), this);
    heading->setProperty("aboutSecondary", true);
    header->addWidget(heading);
    header->addStretch();
    auto* headerClose = new QPushButton(toolGlyph(ToolGlyph::Close, secondary), {}, this);
    headerClose->setObjectName(QStringLiteral("AboutHeaderClose"));
    headerClose->setAccessibleName(tr("Close About"));
    headerClose->setToolTip(tr("Close"));
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
    auto* title = new QLabel(tr("Vulkana Image Editor"), this);
    title->setObjectName(QStringLiteral("AboutVulkanaTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto* version = new QLabel(QCoreApplication::applicationVersion().isEmpty()
        ? tr("Version %1 · %2").arg(QString::fromLatin1(platform::buildVersion), QString::fromLatin1(platform::releaseChannel))
        : tr("Version %1 · %2").arg(QCoreApplication::applicationVersion(), QString::fromLatin1(platform::releaseChannel)), this);
    version->setObjectName(QStringLiteral("AboutVulkanaVersion"));
    version->setProperty("aboutSecondary", true);
    version->setAlignment(Qt::AlignCenter);
    layout->addWidget(version);
    const auto addPolicyLine = [&](const char* name, const QString& text, bool subdued) {
        auto* line = new QLabel(text, this);
        line->setObjectName(QString::fromLatin1(name));
        line->setAlignment(Qt::AlignCenter);
        line->setWordWrap(true);
        line->setProperty("aboutSecondary", subdued);
        layout->addWidget(line);
    };
    addPolicyLine("AboutUsage", tr("Free to use for personal and commercial projects."), false);
    addPolicyLine("AboutCopyright", tr("© 2026 KaidoomDev. All rights reserved for Vulkana-owned contributions."), true);
    addPolicyLine("AboutThirdPartyRights", tr("Third-party components are licensed separately."), true);
    addPolicyLine("AboutBetaNote", tr("Actively developed beta. Please keep backups of important projects."), true);
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
    const auto addContact = [&](ToolGlyph glyph, const QString& name, const QString& text, const QString& tooltip) {
        const int row = contacts->rowCount();
        auto* symbol = new QLabel(this);
        symbol->setObjectName(name + QStringLiteral("Icon"));
        symbol->setFixedSize(20, 20);
        symbol->setPixmap(toolGlyph(glyph, secondary).pixmap({20, 20}, devicePixelRatioF()));
        contacts->addWidget(symbol, row, 0, Qt::AlignCenter);
        auto contactText = text;
        contactText.replace(QStringLiteral("<a "), QStringLiteral("<a style=\"color:%1;\" ").arg(accent.name()));
        auto* contact = new QLabel(contactText, this);
        contact->setObjectName(name);
        contact->setTextFormat(Qt::RichText);
        contact->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        contact->setTextInteractionFlags(Qt::TextBrowserInteraction);
        contact->setOpenExternalLinks(true);
        auto linkPalette = contact->palette();
        linkPalette.setColor(QPalette::Link, themeColor(ThemeColor::Accent));
        linkPalette.setColor(QPalette::LinkVisited, themeColor(ThemeColor::Accent));
        contact->setPalette(linkPalette);
        contact->setToolTip(tooltip);
        contacts->addWidget(contact, row, 1);
    };
    addContact(ToolGlyph::Person, QStringLiteral("AboutVulkanaDeveloper"), tr("Developed by KaidoomDev"), {});
    addContact(ToolGlyph::SocialX, QStringLiteral("AboutVulkanaSocial"),
        QStringLiteral("<a href=\"https://x.com/KaidoomDev\">@KaidoomDev</a>"),
        QStringLiteral("https://x.com/KaidoomDev"));
    addContact(ToolGlyph::Repository, QStringLiteral("AboutVulkanaGitHub"),
        QStringLiteral("<a href=\"https://github.com/Kaidoom/VulkanaImageEditor\">%1</a>")
            .arg(tr("View on GitHub").toHtmlEscaped()),
        QStringLiteral("https://github.com/Kaidoom/VulkanaImageEditor"));
    addContact(ToolGlyph::Mail, QStringLiteral("AboutVulkanaEmail"),
        QStringLiteral("<a href=\"mailto:kaidoomdev@pm.me\">kaidoomdev@pm.me</a>"),
        tr("Send an email to kaidoomdev@pm.me"));
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
    auto* check = new QPushButton(tr("Check for Updates"), card);
    check->setObjectName(QStringLiteral("AboutCheckUpdate"));
    check->setToolTip(tr("Check for a newer release."));
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
    auto* notices = new QPushButton(tr("Third-party Notices"), this);
    notices->setObjectName(QStringLiteral("AboutThirdPartyNotices"));
    notices->setAutoDefault(false);
    notices->setToolTip(tr("Read offline component licenses, notices and release source information."));
    connect(notices, &QPushButton::clicked, this, [this] { done(NoticesRequested); });
    footer->addWidget(notices);
    footer->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* page = buttons->addButton(tr("Open Download Page"), QDialogButtonBox::ActionRole);
    page->setObjectName(QStringLiteral("AboutDownloadPage"));
    page->setAutoDefault(false);
    page->setIcon(toolGlyph(ToolGlyph::ExternalLink));
    const auto downloadPage = UpdateService::downloadPageUrl(services);
    page->setToolTip(downloadPage.toString());
    page->setEnabled(!downloadPage.isEmpty());
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("AboutClose"));
    connect(page, &QPushButton::clicked, this, [this, status, detail, downloadPage] {
        if (!QDesktopServices::openUrl(downloadPage)) {
            status->setText(tr("Could not open your browser."));
            detail->setText(downloadPage.toDisplayString());
            refreshLayout();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    footer->addWidget(buttons);
    outer->addLayout(footer);
    updates_->onChanged = [this, check, status, detail, progress, stateIcon, primaryInk, accent] {
        using State = UpdateService::State;
        const auto state = updates_->state();
        const bool appImage=updates_->package()==UpdatePackage::AppImage;
        const bool restart=state==State::RestartReady;
        const bool downloading = updates_->state() == UpdateService::State::Downloading;
        const bool checking = updates_->state() == UpdateService::State::Checking;
        const bool canDownload = updates_->release().has_value() && state != State::Downloaded && !restart;
        check->setEnabled(updates_->configured() || restart);
        // Keep keyboard focus here while checking. Disabling a focused button
        // makes Qt move focus (and its highlight) to Open Download Page.
        // Repeated checks are ignored below until the reply completes.
        check->setText(restart ? tr("Restart Now") : downloading ? tr("Cancel Download") : checking ? tr("Checking…")
            : canDownload ? (appImage ? tr("Update AppImage") : tr("Download RPM")) : tr("Check for Updates"));
        check->setIcon(toolGlyph(downloading ? ToolGlyph::Close
            : canDownload ? ToolGlyph::Download : ToolGlyph::Refresh, primaryInk));
        check->setToolTip(restart ? tr("Restart using the verified update. Unsaved documents are checked first.")
            : downloading ? tr("Cancel the download without saving a partial file.")
            : canDownload ? (appImage ? tr("Download, verify and atomically replace this AppImage. Restart is a separate action.")
                : tr("Choose where to save the RPM. It will not be installed or run."))
            : !updates_->configured() ? tr("Updates are not configured for this build.")
            : tr("Check for a newer release."));
        progress->setVisible(downloading);
        if (downloading) progress->setRange(0, 0);
        status->setText(!updates_->configured() && !restart ? tr("Updates are not configured.")
            : state == State::Idle ? tr("Check for a new release.")
            : restart ? tr("Update ready to restart.") : state == State::Downloaded ? tr("Download saved and verified.")
            : state == State::Error ? tr("Could not complete the request.") : updates_->message());
        detail->setText(state == State::Error ? updates_->message()
            : state == State::Checking ? tr("Contacting the update service…")
            : state == State::Downloading ? tr("Verifying the file before saving.")
            : restart ? tr("Your current session stays open until you restart.")
            : state == State::Available ? (appImage ? tr("The current AppImage is kept until verification succeeds.") : tr("Save the RPM, then install it manually."))
            : state == State::Downloaded ? tr("Install the RPM manually when ready.")
            : tr("You're running version %1.").arg(QCoreApplication::applicationVersion()));
        stateIcon->setPixmap(toolGlyph(state == State::Current || state == State::Downloaded || restart
            ? ToolGlyph::CheckCircle : canDownload ? ToolGlyph::Download : ToolGlyph::InfoCircle,
            accent).pixmap({32, 32}, devicePixelRatioF()));
        status->setToolTip(updates_->downloadedPath().isEmpty() ? updates_->message()
            : tr("Saved to %1").arg(updates_->downloadedPath()));
        detail->setToolTip(status->toolTip());
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
        QString destination = QFileDialog::getSaveFileName(popupTopLevelOwner(this), tr("Save Vulkana update"),
            suggested, tr("RPM packages (*.rpm)"));
        if (destination.isEmpty()) return;
        updates_->download(destination);
    });
    connect(this, &QDialog::finished, this, [this] { updates_->cancel(); });
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
