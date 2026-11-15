// Included by UpdateServiceTests.cpp to share its asynchronous network fixture.
namespace {
const QByteArray workerPayload = QByteArray::fromHex("fd377a585a00") + QByteArray(150000, 'w');
QJsonObject workerEntry(const QString& version, const QString& platform = "linux", const QByteArray& payload = workerPayload) {
    const auto suffix = platform == "windows" ? "win64.zip" : "linux-x86_64.tar.xz";
    return {{"id", "cuda-" + version + '-' + platform + "-x86_64"}, {"version", version}, {"backend", "cuda"},
        {"platform", platform}, {"architecture", "x86_64"}, {"protocol", 1}, {"enabled", true},
        {"vulkana", QJsonObject{{"min", "0.3.8"}, {"maxExclusive", QJsonValue::Null}, {"exclude", QJsonArray{}}}},
        {"archive", platform == "windows" ? "zip" : "tar.xz"}, {"size", payload.size()},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())},
        {"url", "https://updates.example/downloads/Vulkana-CUDA-" + version + '-' + suffix}};
}
QByteArray workerFeed(const QJsonArray& workers, const QJsonArray& pins = {}) {
    return QJsonDocument(QJsonObject{{"schemaVersion",1},{"workers",workers},{"pins",pins}}).toJson();
}
QByteArray hostWorkerPayload() {
    return u::WorkerUpdates::hostPlatform() == "windows" ? QByteArray("PK\x03\x04",4) + QByteArray(150000, 'w') : workerPayload;
}
QByteArray hostWorkerFeed(const QString& version = "1.0.0") {
    return workerFeed({workerEntry(version, u::WorkerUpdates::hostPlatform(), hostWorkerPayload())});
}
void workerMetadataTests() {
    QString error;
    const auto config = imageeditor::platform::ServiceConfig::load();
    auto parse = [&](const QByteArray& data, const QString& app = "0.3.8") {
        return u::WorkerUpdates::parseFeed(data, app, "linux", "x86_64", config, error);
    };
    auto first = workerEntry("1.0.0"), newer = workerEntry("1.10.0");
    auto result = parse(workerFeed({first, workerEntry("1.9.0"), newer}));
    CHECK(result && result->artifact.version == "1.10.0");
    CHECK(!parse(workerFeed({first}), "0.3.7") && error.isEmpty());
    QJsonArray pins{QJsonObject{{"workerId", first["id"]}, {"vulkana", QJsonObject{{"min","0.3.8"},{"maxExclusive","0.3.9"}}}}};
    result = parse(workerFeed({first,newer},pins));
    CHECK(result && result->pinned && result->artifact.version == "1.0.0");
    result = parse(workerFeed({first,newer},pins),"0.3.9"); CHECK(result && result->artifact.version == "1.10.0");
    first["enabled"] = false; CHECK(!parse(workerFeed({first,newer},pins)) && error.isEmpty());
    CHECK(!parse(workerFeed({newer},pins)) && !error.isEmpty());
    first["enabled"] = true;
    CHECK(!parse(workerFeed({first,newer},{pins[0],pins[0]})) && !error.isEmpty());
    for (const auto* url : {"file:///tmp/a.tar.xz", "http://updates.example/downloads/a.tar.xz", "https://evil.example/downloads/a.tar.xz",
             "https://updates.example:8443/downloads/a.tar.xz", "https://user@updates.example/downloads/a.tar.xz", "https://updates.example/downloads/../a.tar.xz"}) {
        auto bad = first; bad["url"] = url; CHECK(!parse(workerFeed({bad})) && !error.isEmpty());
    }
    auto excluded = first; excluded["vulkana"] = QJsonObject{{"min","0.3.8"},{"exclude",QJsonArray{"0.3.8"}}};
    CHECK(!parse(workerFeed({excluded})) && error.isEmpty());
    CHECK(!parse(workerFeed({first,first})) && !error.isEmpty());
    CHECK(!parse(QByteArray(512 * 1024 + 1,' ')) && !error.isEmpty());
    auto unsupported = first; unsupported["protocol"] = 2;
    CHECK(!parse(workerFeed({unsupported})) && error.isEmpty());
    auto vulkan = first; vulkan["backend"]="vulkan"; vulkan["id"]="vulkan-1.0.0-linux-x86_64";
    CHECK(!parse(workerFeed({vulkan})) && error.isEmpty());
    auto badSize = first; badSize["size"] = 1.5; CHECK(!parse(workerFeed({badSize})) && !error.isEmpty());
}
void workerCheckIntegrationTests() {
    Network network;
    network.responses.push_back({manifest("0.3.8")});
    Response slow{hostWorkerFeed()}; slow.delayMs = 60; network.workerResponses.push_back(slow);
    u::UpdateService app("0.3.8",nullptr,&network);
    auto* workers = app.workerUpdates();
    app.check(1000);
    CHECK(network.requests.size() == 1 && network.workerRequests.size() == 1 && workers->checking());
    CHECK(waitFor([&]{return !app.busy();}));
    CHECK(workers->checking() && app.state() == State::Current);
    CHECK(waitFor([&]{return !workers->checking();})); CHECK(workers->release());
    for (int n=0;n<3;++n) { u::DiffusionPanel panel; panel.setWorkerUpdates(workers); panel.refreshWorkers(); }
    CHECK(network.workerRequests.size() == 1); // Panel opens use the session cache.
    CHECK(network.workerRequests.front().rawHeader("X-Vulkana-Token").isEmpty());
    network.workerResponses.push_back({{},200,QNetworkReply::NoError,true});
    workers->check(15); CHECK(waitFor([&]{return !workers->checking();}));
    CHECK(!workers->release() && !workers->error().isEmpty());
    network.workerResponses.push_back({hostWorkerFeed()}); workers->check();
    CHECK(waitFor([&]{return !workers->checking();})); CHECK(workers->release());
    network.responses.push_back({"app unavailable",503});
    network.workerResponses.push_back({hostWorkerFeed("1.2.0")});
    app.check(); CHECK(waitFor([&]{return !app.busy() && !workers->checking();}));
    CHECK(app.state()==State::Error && workers->release() && workers->release()->artifact.version=="1.2.0");
    // Credential gating matches the app updater, including cached/download paths.
    TestServiceConfig missing;
    CHECK(missing.write({{"version",1},{"domain","https://updates.example"},{"apiKey",""}}));
    Network offline;
    u::WorkerUpdates disabled("0.3.8",nullptr,&offline);
    disabled.check(); disabled.download("unused.zip");
    CHECK(!disabled.configured() && offline.requests.empty() && offline.workerRequests.empty());
    u::DiffusionPanel panel; panel.setWorkerUpdates(&disabled);
    CHECK(!panel.findChild<QToolButton*>("DiffusionDownloadWorker_cuda")->isEnabled());
    CHECK(!panel.findChild<QToolButton*>("DiffusionDownloadWorker_vulkan"));
    CHECK(!panel.findChild<QPushButton*>("DiffusionAddWorker_vulkan"));
}
void workerDownloadTests() {
    for (int scenario=0;scenario<6;++scenario) {
        Network network; u::WorkerUpdates service("0.3.8",nullptr,&network);
        network.workerResponses.push_back({hostWorkerFeed()}); service.check();
        CHECK(waitFor([&]{return !service.checking();})); CHECK(service.release());
        QTemporaryDir folder; const auto path=folder.filePath("worker.archive"); seedFile(path);
        const auto originalSetting=QSettings().value("diffusion/workers/cuda");
        QByteArray payload=hostWorkerPayload();
        if(scenario==1) payload[20]='z';
        if(scenario==2) payload.chop(1);
        if(scenario==3) payload.append('z');
        network.responses.push_back({payload, scenario==4?503:200, QNetworkReply::NoError, scenario==5});
        service.download(path);
        if(scenario==5)service.cancelDownload();
        CHECK(waitFor([&]{return !service.downloading();}));
        CHECK(readFile(path)==(scenario==0?payload:QByteArray("previous")));
        CHECK(QSettings().value("diffusion/workers/cuda")==originalSetting); // Download never registers anything.
        CHECK(network.requests.size()==1);
        CHECK(network.requests.front().rawHeader("X-Vulkana-Token").isEmpty());
        CHECK(network.requests.front().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()==QNetworkRequest::SameOriginRedirectPolicy);
    }
}
void liveWorkerCheck()
{
    u::UpdateService app(QCoreApplication::applicationVersion());
    auto* workers=app.workerUpdates();
    CHECK(workers->configured());
    app.check(15000);
    CHECK(waitFor([&]{return !app.busy() && !workers->checking();},17000));
    CHECK(workers->release());
    if (workers->release()) std::cout << "Live CUDA recommendation: " << workers->release()->artifact.version.toStdString()
        << ", archive bytes: " << workers->release()->size << '\n';
}
void workerPanelTests() {
    QSettings().remove("diffusion/workers/cuda");
    Network network; u::WorkerUpdates updates("0.3.8",nullptr,&network);
    network.workerResponses.push_back({hostWorkerFeed("1.1.0")}); updates.check();
    CHECK(waitFor([&]{return !updates.checking();}));
    u::DiffusionPanel panel; panel.setWorkerUpdates(&updates); panel.resize(480,760); panel.show();
    auto* download=panel.findChild<QToolButton*>("DiffusionDownloadWorker_cuda");
    auto* add=panel.findChild<QPushButton*>("DiffusionAddWorker_cuda");
    auto* bar=panel.findChild<QProgressBar*>("DiffusionWorkerDownloadProgress");
    CHECK(download && add && bar && download->isEnabled() && !download->isHidden() && bar->isHidden());
    QTemporaryDir package; QDir().mkpath(package.filePath("bin"));
    const QString entry="bin/vulkana-diffusion-worker-cuda" + QString(u::WorkerUpdates::hostPlatform()=="windows"?".exe":"");
    const auto executable=package.filePath(entry); QFile file(executable); CHECK(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();file.setPermissions(QFile::ReadOwner|QFile::ExeOwner|QFile::WriteOwner);
    auto metadata=[&](const QString& version){QFile m(package.filePath("worker.json"));CHECK(m.open(QIODevice::WriteOnly));m.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"protocol",1},{"backend","cuda"},{"version",version},{"executable",entry}}).toJson());};
    // Add/Change uses a directory-only chooser: package and bin both work.
    const auto chooseFolder = [&](const QString& directory, bool accept = true) {
        bool seen = false;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, &panel, [&] {
            auto* chooser = panel.window()->findChild<QFileDialog*>("DiffusionWorkerFolderDialog");
            if (!chooser || !chooser->isVisible()) return;
            seen = true;
            CHECK(chooser->fileMode() == QFileDialog::Directory);
            CHECK(chooser->testOption(QFileDialog::ShowDirsOnly));
            if (accept) {
                chooser->setDirectory(directory);
                QMetaObject::invokeMethod(chooser, "accept", Qt::QueuedConnection);
            } else chooser->reject();
            responder.stop();
        });
        QTimer watchdog; watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &panel, [&] {
            if (auto* chooser = panel.window()->findChild<QFileDialog*>("DiffusionWorkerFolderDialog")) chooser->reject();
        });
        responder.start(5); watchdog.start(3000); add->click(); CHECK(seen);
    };
    chooseFolder(package.path());
    CHECK(QSettings().value("diffusion/workers/cuda").toString() == executable);
    QSettings().remove("diffusion/workers/cuda"); panel.refreshWorkers();
    chooseFolder(package.filePath("bin"));
    CHECK(QSettings().value("diffusion/workers/cuda").toString() == executable);
    QTemporaryDir empty;
    chooseFolder(empty.path());
    CHECK(QSettings().value("diffusion/workers/cuda").toString() == executable);
    chooseFolder(empty.path(), false);
    CHECK(QSettings().value("diffusion/workers/cuda").toString() == executable);
    metadata("1.0.0"); panel.refreshWorkers();
    CHECK(download->isEnabled() && !download->isHidden());
    metadata("1.1.0"); panel.refreshWorkers(); CHECK(download->isHidden());
    metadata("2.0.0"); panel.refreshWorkers(); CHECK(download->isHidden());
    metadata("1.0.0"); panel.refreshWorkers();
    QTest::qWait(5);
    auto* configuration = panel.findChild<QComboBox*>("DiffusionConfigurationModels");
    const auto configurationY = configuration->mapTo(&panel, QPoint()).y();
    network.responses.push_back({hostWorkerPayload(),200,QNetworkReply::NoError,true});
    updates.download(package.filePath("saved.archive"));
    CHECK(!download->isEnabled() && !bar->isHidden()); QTest::qWait(5); CHECK(bar->height()==3);
    CHECK(configuration->mapTo(&panel, QPoint()).y()==configurationY);
    panel.hide(); CHECK(updates.downloading()); panel.show(); CHECK(!download->isEnabled());
    updates.cancelDownload(); CHECK(bar->isHidden() && download->isEnabled());
    // Exercise the real button, owned file chooser and extension handling.
    const auto saveBase = package.filePath("downloaded-worker");
    bool pickerSeen = false;
    bool saveDirectorySelected = false;
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, &panel, [&] {
        auto* chooser = panel.window()->findChild<QFileDialog*>("DiffusionWorkerSaveDialog");
        if (!chooser || !chooser->isVisible()) return;
        pickerSeen = true;
        if (!saveDirectorySelected) {
            chooser->setDirectory(package.path());
            saveDirectorySelected = true;
            return;
        }
        // Enter a filename after changing directories, as a user would. An
        // absolute selectFile() can be overwritten by Qt's directory loading.
        auto* filename = chooser->findChild<QLineEdit*>("fileNameEdit");
        CHECK(filename);
        if (filename) filename->setText(QFileInfo(saveBase).fileName());
        responder.stop();
        QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
    });
    responder.start(5);
    network.responses.push_back({hostWorkerPayload()});
    QTimer::singleShot(3000,&panel,[&]{ if (!pickerSeen) for(auto* widget:QApplication::topLevelWidgets()) if(auto* chooser=qobject_cast<QFileDialog*>(widget)) chooser->reject(); });
    download->click();
    CHECK(pickerSeen && waitFor([&]{return !updates.downloading();}));
    CHECK(readFile(saveBase+(u::WorkerUpdates::hostPlatform()=="windows"?".zip":".tar.xz"))==hostWorkerPayload());
    CHECK(QFile::remove(executable)); panel.refreshWorkers(); CHECK(!download->isHidden() && !add->isHidden());
    if(const auto capture=qEnvironmentVariable("VULKANA_WORKER_PANEL_CAPTURE");!capture.isEmpty()) {QTest::qWait(30);CHECK(panel.grab().save(capture));}
    QSettings().remove("diffusion/workers/cuda");
}
}
