#include "imageeditor/ui/AboutDialog.hpp"
#include "imageeditor/ui/ThirdPartyNoticesDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/platform/BuildInfo.hpp"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QIcon>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QTemporaryDir>
#include <QUrl>
#include <QTest>
#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationVersion(QString::fromLatin1(imageeditor::platform::buildVersion));
    app.setWindowIcon(QIcon(QStringLiteral(IMAGEEDITOR_SOURCE_DIR "/assets/Vulkana512.png")));
    imageeditor::ui::applyEditorTheme(app);
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    const auto path = QStringLiteral(IMAGEEDITOR_NOTICES_DIR "/notices.json");
#if IMAGEEDITOR_TEST_DEVELOPMENT_RESOURCES
    check(imageeditor::ui::ThirdPartyNoticesDialog::defaultManifestPath() == path,
        "development viewer must use configured notices, not private package files");
#endif
    QFile manifestFile(path);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        std::cerr << "could not read generated notices\n";
        return 1;
    }
    const auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    const auto profile = manifest.value("profile").toString();
    check(manifest.value("version").toString() == QString::fromLatin1(imageeditor::platform::buildVersion), "notice metadata differs from build");
    const auto texts = manifest.value("texts").toObject();
    QFile plainFile(QStringLiteral(IMAGEEDITOR_NOTICES_DIR "/THIRD-PARTY-NOTICES.txt"));
    check(plainFile.open(QIODevice::ReadOnly), "plain-text notices missing");
    const auto plain = QString::fromUtf8(plainFile.readAll());
    for (const auto& key : manifest.value("notice_text_keys").toArray()) {
        const auto text = texts.value(key.toString()).toString();
        check(!text.trimmed().isEmpty(), "missing offline license text");
        check(plain.contains(text), "plain-text notices must preserve full license copies");
    }
    {
        imageeditor::ui::ThirdPartyNoticesDialog dialog(nullptr, path);
        dialog.show();
        check(dialog.loaded(), "offline profile failed to load");
        auto* search = dialog.findChild<QLineEdit*>("NoticesSearch");
        auto* list = dialog.findChild<QListWidget*>("NoticesComponents");
        auto* body = dialog.findChild<QTextBrowser*>("NoticesText");
        auto* summary = dialog.findChild<QTextBrowser*>("NoticesSummary");
        auto* details = dialog.findChild<QPushButton*>("NoticesDetails");
        check(search && list && body && summary && details, "missing searchable viewer controls");
        check(body->isHidden() && !list->isHidden(), "full text must be collapsed initially");
        const auto readerText = body->toPlainText() + summary->toPlainText()
            + dialog.findChild<QLabel*>("NoticesRelease")->text();
        for (const auto* phrase : {"publication blocked", "Review status:", "permissions unresolved", "publication pending", "Evidence\n", "Actions:", "Build ID:"})
            check(!readerText.contains(QString::fromLatin1(phrase), Qt::CaseInsensitive), "internal findings leaked into reader view");
        search->setFocus();
        QTest::keyClicks(search, "lesser general public");
        check(list->currentRow() >= 0 && body->toPlainText().contains("LESSER GENERAL PUBLIC", Qt::CaseInsensitive), "license text not searchable offline");
        search->setText("no-such-component-91636");
        check(summary->toPlainText().contains("No matching"), "empty search leaves stale component");
        search->clear();
        check(list->currentRow() == 0, "clearing search does not restore list");
        check(!body->openExternalLinks() && !body->openLinks(), "notice text must not execute links");
        details->click();
        check(body->isVisible() && body->toPlainText().contains("GNU LESSER GENERAL PUBLIC LICENSE"), "full offline legal texts unavailable");
        details->click();
        check(body->isHidden(), "details does not return to compact list");
        auto* online = dialog.findChild<QPushButton*>("NoticesSource");
        check(online && online->text() == "Sources && details online", "missing single online action");
        const QUrl sourceUrl(manifest.value("source_url").toString());
        check(online->property("sourceUrl").toUrl() == sourceUrl, "source link must follow manifest");
        check(online->isEnabled() == (manifest.value("source_url_published").toBool()
            && sourceUrl.scheme() == "https" && !sourceUrl.host().isEmpty()), "online action must follow publication status and URL validation");
        check(dialog.findChild<QLabel*>("NoticesRelease")->text().contains(
            QString::fromLatin1(imageeditor::platform::buildVersion)), "notices must display the build version");
        if (profile == "source") {
            check(!readerText.contains("AppImage / libfuse"), "source build falsely claims AppImage runtime");
            check(readerText.contains("System-provided"), "system-library distinction lost");
            check(!online->isEnabled() && sourceUrl.isEmpty(), "source build must not advertise a private release website");
        }
        const auto screenshotDir = qEnvironmentVariable("VULKANA_NOTICES_SCREENSHOT_DIR");
        if (!screenshotDir.isEmpty()) {
            app.processEvents();
            check(dialog.grab().save(QDir(screenshotDir).filePath(profile + ".png")), "could not save notices screenshot");
        }
    }
    // Pure local fixtures exercise publication state without using a real service.
    QTemporaryDir fixtureDir;
    check(fixtureDir.isValid(), "could not create notice fixtures");
    const auto fixturePath = fixtureDir.filePath("notices.json");
    const auto checkLink = [&](const QString& url, bool published, bool enabled) {
        auto fixture = manifest;
        fixture["source_url"] = url;
        fixture["source_url_published"] = published;
        QFile file(fixturePath);
        check(file.open(QIODevice::WriteOnly), "could not write notice fixture");
        file.write(QJsonDocument(fixture).toJson());
        file.close();
        imageeditor::ui::ThirdPartyNoticesDialog dialog(nullptr, fixturePath);
        check(dialog.loaded(), "fixture failed to load");
        auto* button = dialog.findChild<QPushButton*>("NoticesSource");
        check(button->isEnabled() == enabled, "fixture source-link state incorrect");
        check(button->property("sourceUrl").toUrl() == QUrl(url), "fixture source URL changed");
    };
    checkLink("https://licenses.example/releases/test/", true, true);
    checkLink("https://licenses.example/releases/test/", false, false);
    checkLink("file:///tmp/not-a-published-page", true, false);
    checkLink("", true, false);
    imageeditor::ui::ThirdPartyNoticesDialog missing(nullptr, "/nonexistent-vulkana-notices.json");
    check(!missing.loaded(), "missing data not reported");
    imageeditor::ui::AboutDialog about(nullptr);
    check(about.findChild<QLabel*>("AboutVulkanaVersion")->text().contains(QString::fromLatin1(imageeditor::platform::buildVersion)), "About version differs from build");
    check(about.findChild<QLabel*>("AboutVulkanaVersion")->text().contains("Beta"), "missing release channel");
    check(about.findChild<QLabel*>("AboutCopyright")->text().contains("Licensed under the MIT License"), "About must reflect the adopted MIT source license");
    check(about.findChild<QLabel*>("AboutBetaNote")->text().contains("backups"), "missing beta note");
    if (const auto screenshotDir = qEnvironmentVariable("VULKANA_NOTICES_SCREENSHOT_DIR"); !screenshotDir.isEmpty()) {
        about.show(); app.processEvents();
        check(about.grab().save(QDir(screenshotDir).filePath("about.png")), "could not save About screenshot");
    }
    about.findChild<QPushButton*>("AboutThirdPartyNotices")->click();
    check(about.result() == imageeditor::ui::AboutDialog::NoticesRequested, "About action not routed");
    return failures ? 1 : 0;
}
