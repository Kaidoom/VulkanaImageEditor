// Shares the offline transport with UpdateServiceTests.cpp; no live HTTP.
const auto startupTimeKey = QStringLiteral("updates/lastStartupAttemptUtcSeconds");

void projectIntegrationPromptTests()
{
    namespace p = imageeditor::platform;
    if (!p::projectIntegrationAvailable(QCoreApplication::applicationFilePath())) return;
    for (const auto& scenario : {QString("first"), QString("declined"), QString("forced"), QString("registered-forced")}) {
        QSettings settings;
        settings.remove(p::projectIntegrationChoiceKey);
        if (scenario != "first") settings.setValue(p::projectIntegrationChoiceKey,
            scenario == "registered-forced" ? "registered" : "declined");
        qApp->setProperty("vulkanaOfferProjectIntegration", true);
        qApp->setProperty("vulkanaForceProjectIntegrationPrompt", scenario.contains("forced"));
        Network network; network.responses.push_back({packageManifest(QCoreApplication::applicationVersion())});
        u::MainWindow window(nullptr, false); window.setUnsavedPromptEnabled(false); window.show();
        const auto document = window.editorSession().document();
        const auto content = document->contentState();
        const auto depth = window.editorSession().history().undoDepth();
        int shown = 0;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            auto* popup = window.findChild<QDialog*>("ProjectIntegrationDialog");
            if (!popup || !popup->isVisible()) return;
            ++shown;
            CHECK(!popup->isWindow() && !popup->windowHandle());
            CHECK(popup->parentWidget()->objectName() == "WorkspaceDialogShield");
            auto* decline = popup->findChild<QPushButton*>("ProjectIntegrationDecline");
            CHECK(decline); if (decline) decline->click();
        });
        responder.start(5);
        QTimer::singleShot(3000, &window, [&] { window.close(); });
        window.startStartupFlow(false, &network);
        CHECK(waitFor([&] { return scenario == "declined" ? !network.responses.size() : shown == 1; }, 3500));
        QTest::qWait(60);
        CHECK(shown == (scenario == "declined" ? 0 : 1));
        CHECK(settings.value(p::projectIntegrationChoiceKey) == (scenario == "registered-forced" ? "registered" : "declined"));
        CHECK(document->contentState() == content && window.editorSession().history().undoDepth() == depth);
        window.close();
    }
    qApp->setProperty("vulkanaOfferProjectIntegration", false);
    qApp->setProperty("vulkanaForceProjectIntegrationPrompt", false);
}

void startupPolicyTests()
{
    Network network;
    network.responses.push_back({{}, 200, QNetworkReply::NoError, true});
    u::UpdateService service("0.2.1", nullptr, &network);
    service.check(25);
    CHECK(network.requests.front().transferTimeout() == 25);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Error);
    CHECK(service.message().contains("timed out"));
    // A short automatic deadline must not carry into a later manual retry.
    network.responses.push_back({packageManifest("9.9.9")});
    service.check();
    CHECK(network.requests.back().transferTimeout() == 30000);
    CHECK(waitFor([&] { return !service.busy(); }));
    CHECK(service.state() == State::Available);
}

void startupPresentationTests()
{
    for (const bool fresh : {false, true}) for (const bool viewUpdate : {false, true}) {
        QSettings().setValue(startupTimeKey, QDateTime::currentSecsSinceEpoch());
        const auto savedTimestamp = QSettings().value(startupTimeKey);
        Network network;
        network.responses.push_back({packageManifest("9.9.9")});
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false);
        window.resize(1200, 850); window.show();
        QTemporaryDir project;
        if (!fresh) {
            const auto path = project.filePath("opened.vulkana");
            CHECK(u::saveProject(path, *window.editorSession().document()));
            CHECK(window.openImageFromPath(path));
        }
        const auto content = window.editorSession().document()->contentState();
        const auto depth = window.editorSession().history().undoDepth();
        int notices = 0, about = 0, canvases = 0;
        bool completed = false;
        QPointer<QWidget> handledNotice;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            if (auto* popup = window.findChild<QWidget*>("StartupUpdateNotification");
                popup && popup->isVisible() && popup != handledNotice) {
                // View Update is queued; polling again before it opens About
                // must not count/click the same notification a second time.
                handledNotice = popup;
                ++notices;
                CHECK(!popup->isWindow() && !popup->windowHandle());
                auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
                CHECK(workspace && popup->parentWidget() == workspace->panelOverlay() && !workspace->hasModalOverlay());
                CHECK(popup->findChild<QLabel*>("StartupUpdateVersion")->text().contains("9.9.9"));
                CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
                auto* button = popup->findChild<QPushButton*>(viewUpdate ? "StartupUpdateOpen" : "StartupUpdateDismiss");
                CHECK(button); if (button) button->click();
                if (!viewUpdate) completed = true;
            }
            if (auto* popup = window.findChild<QDialog*>("AboutVulkanaDialog"); popup && popup->isVisible()) {
                ++about;
                CHECK(!popup->isWindow());
                const auto kind = u::UpdateService::buildPackage();
                CHECK(popup->findChild<QPushButton*>("AboutCheckUpdate")->text()
                    == (kind == u::UpdatePackage::AppImage ? "Update AppImage" : kind == u::UpdatePackage::Deb ? "Download DEB"
                        : kind == u::UpdatePackage::WindowsPortable ? "Download ZIP" : "Download RPM"));
                CHECK(network.requests.size() == 1); // Cached release, not a second check.
                popup->reject(); completed = true;
            }
            if (auto* popup = window.findChild<QDialog*>("NewDocumentDialog"); popup && popup->isVisible()) {
                ++canvases;
                auto* notice = window.findChild<QWidget*>("StartupUpdateNotification");
                CHECK(!notice || !notice->isVisible());
                popup->reject();
            }
        });
        responder.start(5);
        QTimer::singleShot(5000, &window, [&] { if (!completed) { CHECK(false); window.close(); } });
        window.startStartupFlow(fresh, &network);
        window.startStartupFlow(fresh, &network); // Idempotent per launch window.
        CHECK(waitFor([&] { return completed; }, 6000));
        QTest::qWait(30);
        CHECK(notices == 1 && about == int(viewUpdate));
        CHECK(canvases == int(fresh));
        CHECK(network.requests.size() == 1 && network.requests.front().transferTimeout() == 2500);
        if (fresh) CHECK(!window.editorSession().document() && window.documentCount() == 0);
        else CHECK(window.editorSession().document()->contentState() == content);
        CHECK(window.editorSession().history().undoDepth() == depth);
        CHECK(QSettings().value(startupTimeKey) == savedTimestamp);
        window.close();
    }
}

void startupSilentTests()
{
    for (const auto& scenario : {QString("cached"), QString("current"), QString("failure"),
             QString("invalid"), QString("timeout"), QString("rollback")}) {
        QSettings settings;
        settings.remove(startupTimeKey);
        if (scenario == "cached") settings.setValue(startupTimeKey, QDateTime::currentSecsSinceEpoch());
        if (scenario == "rollback") settings.setValue(startupTimeKey, QDateTime::currentSecsSinceEpoch() + 86400);
        const auto savedTimestamp = settings.value(startupTimeKey);
        Network network;
        if (scenario == "failure") network.responses.push_back({{}, 503});
        else if (scenario == "invalid") network.responses.push_back({"invalid JSON"});
        else if (scenario == "timeout") network.responses.push_back({{}, 200, QNetworkReply::NoError, true});
        else network.responses.push_back({packageManifest(QCoreApplication::applicationVersion())});
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        bool completed = false;
        int ticks = 0;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            ++ticks;
            CHECK(!window.findChild<QWidget*>("StartupUpdateNotification"));
            if (auto* popup = window.findChild<QDialog*>("NewDocumentDialog"); popup && popup->isVisible()) {
                popup->reject(); completed = true;
            }
        });
        responder.start(10);
        QTimer::singleShot(5000, &window, [&] { if (!completed) { CHECK(false); window.close(); } });
        QElapsedTimer elapsed; elapsed.start();
        window.startStartupFlow(true, &network);
        CHECK(waitFor([&] { return completed; }, 6000));
        CHECK(network.requests.size() == 1);
        CHECK(QSettings().value(startupTimeKey) == savedTimestamp);
        if (scenario == "timeout") {
            CHECK(ticks > 0);
            CHECK(elapsed.elapsed() < 1000); // New Canvas does not wait for the network deadline.
            CHECK(network.last && !network.last->isFinished());
            CHECK(waitFor([&] { return !network.last || network.last->isFinished(); }));
            CHECK(ticks > 10); // The event loop remains usable through timeout.
            CHECK(!window.findChild<QWidget*>("StartupUpdateNotification"));
            CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
        }
        window.close();
    }
}

void startupCloseTests()
{
    {
        Network network;
        network.responses.push_back({{}, 200, QNetworkReply::NoError, true});
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        window.startStartupFlow(true, &network);
        CHECK(network.last && !network.last->isFinished());
        window.close();
        CHECK(!network.last || network.last->isFinished());
        QTest::qWait(30);
        CHECK(!window.findChild<QWidget*>("StartupUpdateNotification"));
        CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
    }
    // Opening About manually during the startup request takes ownership of it
    // and suppresses the deferred New Canvas even if About is then dismissed.
    {
        Network network;
        network.responses.push_back({{}, 200, QNetworkReply::NoError, true});
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        window.startStartupFlow(true, &network);
        bool sawAbout = false;
        QTimer::singleShot(10, &window, [&] {
            auto* about = window.findChild<QDialog*>("AboutVulkanaDialog");
            CHECK(about && about->isVisible());
            if (about) {
                sawAbout = true;
                CHECK(about->findChild<QPushButton*>("AboutCheckUpdate")->text() == "Checking…");
                about->reject();
            }
        });
        auto* action = window.findChild<QAction*>("AboutVulkanaAction");
        CHECK(action); if (action) action->trigger();
        QTest::qWait(30);
        CHECK(sawAbout && network.requests.size() == 1);
        CHECK(!network.last || network.last->isFinished());
        CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
        CHECK(!window.findChild<QWidget*>("StartupUpdateNotification"));
        window.close();
    }
    // The same shared-service handoff uses AppImage's update action, not RPM.
    {
        Network network;
        network.responses.push_back({QJsonDocument(QJsonObject{
            {"version", "9.9.9"}, {"appimage", QJsonObject{
                {"url", "https://updates.example/downloads/Vulkana-9.9.9-x86_64.AppImage"},
                {"sha256", QString(64, 'a')}}}}).toJson()});
        u::UpdateService service("0.2.6", nullptr, &network, u::UpdatePackage::AppImage);
        service.check(2500);
        CHECK(waitFor([&] { return !service.busy(); }));
        CHECK(service.state() == State::Available);
        QWidget owner;
        u::AboutDialog about(&owner, nullptr, u::UpdatePackage::AppImage, {}, &service);
        CHECK(about.findChild<QPushButton*>("AboutCheckUpdate")->text() == "Update AppImage");
        CHECK(network.requests.size() == 1);
    }
}

void startupNonblockingNoticeTests(QVulkanInstance* instance = nullptr)
{
    Network network;
    Response delayed{packageManifest("9.9.9")}; delayed.delayMs = 350;
    network.responses.push_back(delayed);
    u::MainWindow window(instance, false);
    window.setUnsavedPromptEnabled(false); window.resize(1200, 850); window.show();
    window.activateWindow(); window.setFocus(); QTest::qWait(30);
    if (instance) {
        imageeditor::render::CanvasWindow* canvas = nullptr;
        for (auto* candidate : QGuiApplication::allWindows())
            if (auto* found = dynamic_cast<imageeditor::render::CanvasWindow*>(candidate)) canvas = found;
        CHECK(canvas && waitFor([&] { return canvas->rendererStats().presentQueuedFrames > 0; }, 5000));
        // Wayland activation arrives after exposure; settle it before testing
        // that the asynchronous notification preserves an established focus.
        CHECK(waitFor([&] { return QApplication::focusWidget() == &window; }));
    }
    auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
    CHECK(workspace); if (!workspace) return;
    const auto geometry = workspace->canvasContainer()->geometry();
    const auto content = window.editorSession().document()->contentState();
    window.startStartupFlow(false, &network);
    CHECK(network.last && !network.last->isFinished());
    auto* selectAll = window.findChild<QAction*>("SelectAllAction");
    CHECK(selectAll && selectAll->isEnabled()); if (selectAll) selectAll->trigger();
    CHECK(window.editorSession().document()->selection());
    const auto selection = window.editorSession().document()->selection();
    const auto history = window.editorSession().history().undoDepth();
    auto* focus = QApplication::focusWidget();
    CHECK(waitFor([&] {
        auto* card = window.findChild<QWidget*>("StartupUpdateNotification");
        return card && card->isVisible();
    }));
    auto* card = window.findChild<QWidget*>("StartupUpdateNotification");
    CHECK(card); if (!card) return;
    CHECK(!card->isWindow() && !card->windowHandle() && !workspace->hasModalOverlay());
    CHECK(QApplication::focusWidget() == focus);
    CHECK(workspace->canvasContainer()->geometry() == geometry);
    CHECK(window.editorSession().document()->contentState() == content);
    CHECK(window.editorSession().document()->selection() == selection);
    CHECK(window.editorSession().history().undoDepth() == history);
    CHECK(card->geometry().right() == workspace->panelOverlay()->width() - 13);
    CHECK(card->geometry().bottom() == workspace->panelOverlay()->height() - 13);
    CHECK(card->width() < 500 && card->height() < 200);
    auto* dismiss = card->findChild<QPushButton*>("StartupUpdateDismiss");
    auto* update = card->findChild<QPushButton*>("StartupUpdateOpen");
    CHECK(dismiss && update && update->text() == "Update");
    CHECK(dismiss && dismiss->width() >= dismiss->sizeHint().width());
    CHECK(update && update->width() >= update->sizeHint().width());
    // Normal editing remains available while the notice is visible.
    auto* deselect = window.findChild<QAction*>("DeselectAction");
    CHECK(deselect && deselect->isEnabled()); if (deselect) deselect->trigger();
    CHECK(!window.editorSession().document()->selection());
    window.resize(1020, 700); QTest::qWait(20);
    CHECK(card->geometry().bottomRight() == workspace->panelOverlay()->rect().bottomRight() - QPoint(12,12));
    if (const auto screenshot=qEnvironmentVariable("VULKANA_UPDATE_NOTICE_PREVIEW"); !screenshot.isEmpty())
        CHECK(card->grab().save(screenshot));
    auto& language = u::i18n::Localization::instance();
    for (const auto* locale : {"fr", "de", "ar", "ja", "en"}) {
        CHECK(language.setLanguage(locale)); QTest::qWait(10);
        CHECK(update->text() == u::i18n::text("main.window.updates.view.update"));
        CHECK(update->width() >= update->sizeHint().width() && dismiss->width() >= dismiss->sizeHint().width());
        CHECK(card->geometry().bottomRight() == workspace->panelOverlay()->rect().bottomRight() - QPoint(12,12));
        CHECK(card->findChild<QLabel*>("StartupUpdateVersion")->text()
            == u::i18n::text("main.window.updates.vulkana.1.is.available").arg(u::i18n::ltrToken("9.9.9")));
        CHECK(network.requests.size() == 1);
    }
    if (instance)
        QTest::mouseClick(workspace->panelOverlay()->windowHandle(), Qt::LeftButton, {},
            dismiss->mapTo(workspace->panelOverlay(), dismiss->rect().center()));
    else QTest::mouseClick(dismiss, Qt::LeftButton);
    QTest::qWait(30);
    CHECK(!window.findChild<QWidget*>("StartupUpdateNotification"));
    // Dismiss does not discard the validated update shown by a later About visit.
    QTimer::singleShot(20, &window, [&] {
        auto* about=window.findChild<QDialog*>("AboutVulkanaDialog");
        CHECK(about && about->isVisible());
        if (about) {
            CHECK(about->findChild<QLabel*>("AboutUpdateStatus")->text().contains("9.9.9"));
            about->reject();
        }
    });
    window.findChild<QAction*>("AboutVulkanaAction")->trigger();
    CHECK(network.requests.size() == 1);
    QTest::qWait(20); CHECK(!window.findChild<QWidget*>("StartupUpdateNotification"));
    window.close();
}

void startupSimulationTests()
{
    for (const bool configured : {false, true}) for (const bool view : {false, true}) {
        TestServiceConfig config;
        if (!configured) CHECK(QFile::remove(config.path()));
        QSettings().remove(startupTimeKey);
        Network network; // No responses: even one HTTP request is a test failure.
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        const auto content = window.editorSession().document()->contentState();
        const auto depth = window.editorSession().history().undoDepth();
        int notices = 0, about = 0;
        bool completed = false;
        QPointer<QWidget> handledNotice;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            if (auto* popup = window.findChild<QWidget*>("StartupUpdateNotification");
                popup && popup->isVisible() && popup != handledNotice) {
                handledNotice = popup;
                ++notices;
                CHECK(!popup->isWindow() && !popup->windowHandle());
                auto* workspace = dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
                CHECK(workspace && popup->parentWidget() == workspace->panelOverlay() && !workspace->hasModalOverlay());
                CHECK(popup->findChild<QLabel*>("StartupUpdateTestNote"));
                auto* button = popup->findChild<QPushButton*>(view ? "StartupUpdateOpen" : "StartupUpdateDismiss");
                CHECK(button); if (button) button->click();
                if (!view) completed = true;
            }
            if (auto* popup = window.findChild<QDialog*>("AboutVulkanaDialog"); popup && popup->isVisible()) {
                ++about;
                CHECK(!popup->isWindow());
                auto* button = popup->findChild<QPushButton*>("AboutCheckUpdate");
                CHECK(button && !button->isEnabled());
                CHECK(popup->findChild<QLabel*>("AboutUpdateDetail")->text().contains("Test update only"));
                CHECK(popup->findChild<QLabel*>("AboutUpdateStatus")->text().contains("is available"));
                popup->reject(); completed = true;
            }
        });
        responder.start(5);
        QTimer::singleShot(3000, &window, [&] { if (!completed) { CHECK(false); window.close(); } });
        window.startStartupFlow(false, &network, true);
        window.startStartupFlow(false, &network, true);
        CHECK(waitFor([&] { return completed; }, 4000));
        QTest::qWait(30);
        CHECK(notices == 1 && about == int(view));
        CHECK(network.requests.empty());
        CHECK(!QSettings().contains(startupTimeKey));
        CHECK(window.editorSession().document()->contentState() == content);
        CHECK(window.editorSession().history().undoDepth() == depth);
        CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
        window.close();
    }
}
