#include "imageeditor/ui/ThirdPartyNoticesDialog.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/platform/BuildInfo.hpp"

#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

namespace imageeditor::ui {
QString ThirdPartyNoticesDialog::defaultManifestPath()
{
#ifdef VULKANA_DEVELOPMENT_NOTICES
    return QStringLiteral(VULKANA_DEVELOPMENT_NOTICES);
#else
    return platform::installedDataPath(QStringLiteral("notices/notices.json"));
#endif
}

ThirdPartyNoticesDialog::ThirdPartyNoticesDialog(QWidget* parent, const QString& manifestPath)
    : QDialog(parent, Qt::SubWindow)
{
    setObjectName(QStringLiteral("ThirdPartyNoticesDialog"));
    setWindowTitle(tr("Third-party Notices"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    resize(720, 540);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);
    auto* header = new QHBoxLayout;
    header->addWidget(new QLabel(tr("Third-party Notices"), this));
    header->addStretch();
    auto* close = new QPushButton(tr("Close"), this);
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    header->addWidget(close);
    layout->addLayout(header);
    auto* info = new QLabel(this);
    info->setObjectName(QStringLiteral("NoticesRelease"));
    info->setTextFormat(Qt::PlainText);
    info->setWordWrap(true);
    layout->addWidget(info);
    auto* search = new QLineEdit(this);
    search->setObjectName(QStringLiteral("NoticesSearch"));
    search->setPlaceholderText(tr("Search components or license text…"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    pages_ = new QStackedWidget(this);
    auto* compact = new QWidget(pages_);
    auto* compactLayout = new QVBoxLayout(compact);
    compactLayout->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget(compact);
    list_->setObjectName(QStringLiteral("NoticesComponents"));
    list_->setTextElideMode(Qt::ElideRight);
    compactLayout->addWidget(list_, 1);
    summary_ = new QTextBrowser(compact);
    summary_->setObjectName(QStringLiteral("NoticesSummary"));
    summary_->setOpenLinks(false);
    summary_->setOpenExternalLinks(false);
    summary_->setMaximumHeight(140);
    compactLayout->addWidget(summary_);
    pages_->addWidget(compact);
    body_ = new QTextBrowser(pages_);
    body_->setObjectName(QStringLiteral("NoticesText"));
    body_->setOpenLinks(false);
    body_->setOpenExternalLinks(false);
    pages_->addWidget(body_);
    layout->addWidget(pages_, 1);
    auto* footer = new QHBoxLayout;
    auto* details = new QPushButton(tr("Open full local notices"), this);
    details->setObjectName(QStringLiteral("NoticesDetails"));
    details->setAutoDefault(false);
    details->setCheckable(true);
    connect(details, &QPushButton::toggled, this, [this, details](bool on) {
        pages_->setCurrentIndex(on ? 1 : 0);
        details->setText(on ? tr("Back to components") : tr("Open full local notices"));
    });
    footer->addWidget(details);
    footer->addStretch();
    auto* online = new QPushButton(tr("Sources && details online"), this);
    online->setObjectName(QStringLiteral("NoticesSource"));
    online->setAutoDefault(false);
    footer->addWidget(online);
    layout->addLayout(footer);
    QFile file(manifestPath.isEmpty() ? defaultManifestPath() : manifestPath);
    QJsonParseError error;
    if (file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024 * 1024)
        manifest_ = QJsonDocument::fromJson(file.readAll(), &error).object();
    loaded_ = error.error == QJsonParseError::NoError && manifest_.value("format").toInt() == 1
        && !manifest_.value("components").toArray().isEmpty()
        && manifest_.value("texts").isObject() && manifest_.value("notice_text_keys").isArray();
    if (!loaded_) {
        info->setText(tr("The local notices could not be opened."));
        search->setEnabled(false);
        details->setEnabled(false);
        online->setEnabled(false);
        return;
    }
    info->setText(tr("Vulkana %1 · Qt uses LGPL v3; full license copies are available below.")
        .arg(QString::fromLatin1(platform::buildVersion)));
    info->setProperty("noticeProfile", manifest_.value("profile").toString());
    const QUrl url(manifest_.value("source_url").toString());
    online->setProperty("sourceUrl", url);
    // A staged page is not a working public source offer. Keep the neutral
    // action in place, enabling it only after deployment has been verified.
    online->setEnabled(manifest_.value("source_url_published").toBool()
        && url.scheme() == QStringLiteral("https") && !url.host().isEmpty());
    connect(online, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(url); });
    const auto texts = manifest_.value("texts").toObject();
    QString fullText;
    for (const auto& entry : manifest_.value("acknowledgements").toArray())
        fullText += entry.toString() + QStringLiteral("\n\n");
    // Reader fields are generated on the reviewed components; audit fields,
    // receipts and policy findings are never presentation data.
    const auto noticeKeys = manifest_.value("notice_text_keys").toArray();
    for (const auto& value : manifest_.value("components").toArray()) {
        const auto c = value.toObject();
        const auto n = c.value("notice").toObject();
        if (n.isEmpty()) continue;
        const auto name = n.value("name").toString();
        const auto version = n.value("version").toString();
        const auto title = version.isEmpty() ? name : name + QStringLiteral(" — ") + version;
        const auto summary = title + QStringLiteral("\n") + n.value("scope").toString()
            + QStringLiteral("\nLicense: ") + n.value("license").toString()
            + QStringLiteral("\n\n") + n.value("copyright").toString();
        list_->addItem(title);
        list_->item(list_->count() - 1)->setToolTip(title);
        entrySummaries_.append(summary);
        QString searchable = summary;
        for (const auto& key : c.value("texts").toArray())
            if (noticeKeys.contains(key)) searchable += QStringLiteral("\n") + texts.value(key.toString()).toString();
        entrySearchTexts_.append(searchable);
        fullText += summary + QStringLiteral("\n\n");
    }
    for (const auto& key : noticeKeys)
        fullText += QStringLiteral("\n—— ") + key.toString() + QStringLiteral(" ——\n\n")
            + texts.value(key.toString()).toString() + QStringLiteral("\n");
    body_->setPlainText(fullText);
    connect(list_, &QListWidget::currentRowChanged, this, &ThirdPartyNoticesDialog::showEntry);
    connect(search, &QLineEdit::textChanged, this, [this](const QString& query) {
        list_->blockSignals(true);
        int first = -1;
        for (int row = 0; row < list_->count(); ++row) {
            const bool match = entrySearchTexts_[row].contains(query, Qt::CaseInsensitive);
            list_->item(row)->setHidden(!match);
            if (match && first < 0) first = row;
        }
        list_->setCurrentRow(first);
        list_->blockSignals(false);
        showEntry(first);
        body_->moveCursor(QTextCursor::Start);
        if (!query.isEmpty()) body_->find(query);
    });
    list_->setCurrentRow(0);
}

void ThirdPartyNoticesDialog::showEntry(int row)
{
    summary_->setPlainText(row >= 0 && row < entrySummaries_.size()
        ? entrySummaries_[row] : tr("No matching components."));
}
} // namespace imageeditor::ui
