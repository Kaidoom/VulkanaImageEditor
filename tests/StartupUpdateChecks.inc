// Shares the offline transport with UpdateServiceTests.cpp; no live HTTP.
const auto startupTimeKey = QStringLiteral("updates/lastStartupAttemptUtcSeconds");

void startupPolicyTests()
{
    constexpr qint64 now = 2'000'000'000, day = 86400;
    CHECK(u::UpdateService::startupCheckDue(now, {}));
    CHECK(u::UpdateService::startupCheckDue(now, 0));
    CHECK(u::UpdateService::startupCheckDue(now, -1));
    CHECK(!u::UpdateService::startupCheckDue(now, now));
    CHECK(!u::UpdateService::startupCheckDue(now, now - day + 1));
    CHECK(u::UpdateService::startupCheckDue(now, now - day));
    CHECK(u::UpdateService::startupCheckDue(now, now + 1));
    CHECK(u::UpdateService::startupCheckDue(now, now, true));
    CHECK(u::UpdateService::startupCheckDue(std::numeric_limits<qint64>::min(), now));
    CHECK(u::UpdateService::startupCheckDue(std::numeric_limits<qint64>::max(), 1));

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
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            if (auto* popup = window.findChild<QDialog*>("StartupUpdateNotification"); popup && popup->isVisible()) {
                ++notices;
                CHECK(!popup->isWindow() && !popup->windowHandle());
                CHECK(popup->parentWidget()->objectName() == "WorkspaceDialogShield");
                CHECK(popup->findChild<QLabel*>("StartupUpdateVersion")->text().contains("9.9.9"));
                CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
                auto* button = popup->findChild<QPushButton*>(viewUpdate ? "StartupUpdateOpen" : "StartupUpdateDismiss");
                CHECK(button); if (button) button->click();
                if (!viewUpdate && !fresh) completed = true;
            }
            if (auto* popup = window.findChild<QDialog*>("AboutVulkanaDialog"); popup && popup->isVisible()) {
                ++about;
                CHECK(!popup->isWindow());
                const auto kind = u::UpdateService::buildPackage();
                CHECK(popup->findChild<QPushButton*>("AboutCheckUpdate")->text()
                    == (kind == u::UpdatePackage::AppImage ? "Update AppImage" : kind == u::UpdatePackage::Deb ? "Download DEB" : "Download RPM"));
                CHECK(network.requests.size() == 1); // Cached release, not a second check.
                popup->reject(); completed = true;
            }
            if (auto* popup = window.findChild<QDialog*>("NewDocumentDialog"); popup && popup->isVisible()) {
                ++canvases;
                CHECK(!window.findChild<QDialog*>("StartupUpdateNotification"));
                popup->reject(); completed = true;
            }
        });
        responder.start(5);
        QTimer::singleShot(5000, &window, [&] { if (!completed) { CHECK(false); window.close(); } });
        window.startStartupFlow(fresh, true, &network);
        window.startStartupFlow(fresh, true, &network); // Idempotent per launch window.
        CHECK(waitFor([&] { return completed; }, 6000));
        QTest::qWait(30);
        CHECK(notices == 1 && about == int(viewUpdate));
        CHECK(canvases == int(fresh && !viewUpdate));
        CHECK(network.requests.size() == 1 && network.requests.front().transferTimeout() == 2500);
        if (fresh) CHECK(!window.editorSession().document() && window.documentCount() == 0);
        else CHECK(window.editorSession().document()->contentState() == content);
        CHECK(window.editorSession().history().undoDepth() == depth);
        CHECK(QSettings().value(startupTimeKey).toLongLong() <= QDateTime::currentSecsSinceEpoch());
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
        Network network;
        if (scenario == "failure") network.responses.push_back({{}, 503});
        else if (scenario == "invalid") network.responses.push_back({"invalid JSON"});
        else if (scenario == "timeout") network.responses.push_back({{}, 200, QNetworkReply::NoError, true});
        else if (scenario != "cached") network.responses.push_back({packageManifest(QCoreApplication::applicationVersion())});
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        bool completed = false;
        int ticks = 0;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            ++ticks;
            CHECK(!window.findChild<QDialog*>("StartupUpdateNotification"));
            if (auto* popup = window.findChild<QDialog*>("NewDocumentDialog"); popup && popup->isVisible()) {
                popup->reject(); completed = true;
            }
        });
        responder.start(10);
        QTimer::singleShot(5000, &window, [&] { if (!completed) { CHECK(false); window.close(); } });
        QElapsedTimer elapsed; elapsed.start();
        window.startStartupFlow(true, false, &network);
        CHECK(waitFor([&] { return completed; }, 6000));
        CHECK(network.requests.size() == (scenario == "cached" ? 0 : 1));
        CHECK(QSettings().value(startupTimeKey).toLongLong() <= QDateTime::currentSecsSinceEpoch());
        if (scenario == "timeout") {
            CHECK(ticks > 20); // The event loop remains responsive while waiting.
            CHECK(elapsed.elapsed() < 4000);
            CHECK(!network.last || network.last->isFinished()); // deleteLater may already have reclaimed it.
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
        window.startStartupFlow(true, true, &network);
        CHECK(network.last && !network.last->isFinished());
        window.close();
        CHECK(!network.last || network.last->isFinished());
        QTest::qWait(30);
        CHECK(!window.findChild<QDialog*>("StartupUpdateNotification"));
        CHECK(!window.findChild<QDialog*>("NewDocumentDialog"));
    }
    // Opening About manually during the startup request takes ownership of it
    // and suppresses the deferred New Canvas even if About is then dismissed.
    {
        Network network;
        network.responses.push_back({{}, 200, QNetworkReply::NoError, true});
        u::MainWindow window(nullptr, false);
        window.setUnsavedPromptEnabled(false); window.show();
        window.startStartupFlow(true, true, &network);
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
        CHECK(!window.findChild<QDialog*>("StartupUpdateNotification"));
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
