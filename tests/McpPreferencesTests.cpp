#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/McpPanel.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/mcp/Integration.hpp"
#include "imageeditor/mcp/LocalServer.hpp"
#include "imageeditor/platform/McpSettings.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <iostream>
#include <cstring>
#ifdef Q_OS_UNIX
#include <cstdio>
#endif
namespace ui = imageeditor::ui;
namespace mcp = imageeditor::mcp;
namespace platform = imageeditor::platform;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n'; ++failures; } } while(false)
template<class F> bool spin(F f) { return QTest::qWaitFor(f, 5000); }
QString endpoint(const QTemporaryDir& dir, const QString& name)
{
#ifdef Q_OS_WIN
    Q_UNUSED(dir);
    return "vulkana-mcp-test-" + name + QUuid::createUuid().toString(QUuid::Id128);
#else
    return dir.filePath(name + ".sock");
#endif
}
QByteArray request(int id, QString method, QJsonObject parameters = {})
{ return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", parameters}}).toJson(QJsonDocument::Compact) + '\n'; }
QJsonObject initialization()
{ return {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "preferences-test"}, {"version", "1"}}}}; }
QJsonObject exchange(QLocalSocket& socket, const QByteArray& bytes)
{
    socket.write(bytes); CHECK(spin([&] { return socket.canReadLine(); }));
    return QJsonDocument::fromJson(socket.readLine()).object();
}
void handshake(QLocalSocket& socket)
{
    CHECK(exchange(socket, request(1, "initialize", initialization())).contains("result"));
    socket.write("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
}
void stoppingCompletesOnlyTheCurrentCommand()
{
    QTemporaryDir dir; const auto path = endpoint(dir, "inflight");
    mcp::ToolRegistry registry; mcp::LocalServer server(registry, "test");
    int applied = 0;
    registry.add({"finish", "Finish current command", {{"type", "object"}}, {}, [&](const QJsonObject&) {
        server.close(); CHECK(server.stopping() && !server.listening());
        CHECK(!server.listen(path));
        ++applied;
        return mcp::toolSuccess({{"changed", true}});
    }});
    CHECK(server.listen(path));
    QLocalSocket client; client.connectToServer(path); CHECK(spin([&] { return server.connected(); })); handshake(client);
    const auto response = exchange(client, request(2, "tools/call", {{"name", "finish"}}) + request(3, "tools/call", {{"name", "finish"}}));
    CHECK(response["result"].toObject()["structuredContent"].toObject()["changed"].toBool());
    CHECK(spin([&] { return !server.stopping() && client.state() == QLocalSocket::UnconnectedState; }));
    CHECK(applied == 1 && !server.listening());
    CHECK(server.listen(path)); server.close();
#ifdef Q_OS_UNIX
    CHECK(!QFileInfo::exists(path));
    CHECK(server.listen(path));
    QLocalServer foreign; const auto foreignPath = dir.filePath("foreign.sock"); CHECK(foreign.listen(foreignPath));
    CHECK(QFile::remove(path));
    CHECK(::rename(QFile::encodeName(foreignPath).constData(), QFile::encodeName(path).constData()) == 0);
    server.close(); // The public path now belongs to a different live listener.
    CHECK(QFileInfo::exists(path));
    client.connectToServer(path); CHECK(spin([&] { return foreign.hasPendingConnections(); }));
    if (auto* peer = foreign.nextPendingConnection()) delete peer;
    client.abort(); foreign.close(); CHECK(QFile::remove(path));
#endif
}
void settingsAndDiscovery()
{
    QTemporaryDir dir; QSettings file(dir.filePath("settings.ini"), QSettings::IniFormat);
    CHECK(!platform::loadMcpSettings(file).autoStart);
    const platform::McpSettings saved{true, endpoint(dir, "saved")};
    CHECK(platform::saveMcpSettings(file, saved));
    QSettings reopened(file.fileName(), QSettings::IniFormat);
    CHECK(platform::loadMcpSettings(reopened) == saved);
    CHECK(!platform::shouldStartMcp({}, false));
    CHECK(platform::shouldStartMcp({}, true));
    CHECK(platform::shouldStartMcp(saved, false));
    CHECK(!platform::shouldStartMcp(saved, false, true));
    CHECK(platform::shouldStartMcp(saved, true, true));
    auto executable = [&](const QString& path) {
        QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); CHECK(f.open(QIODevice::WriteOnly)); f.write("test"); f.close();
        CHECK(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)); return path;
    };
#ifdef Q_OS_WIN
    const auto app = executable(dir.filePath("portable with spaces/bin/imageeditor.exe"));
    const auto bridge = executable(dir.filePath("portable with spaces/bin/vulkana-mcp.exe"));
#else
    const auto app = executable(dir.filePath("AppDir/usr/bin/imageeditor"));
    const auto bridge = executable(dir.filePath("AppDir/usr/bin/vulkana-mcp"));
#endif
    CHECK(mcp::discoverBridge(app).command == QStringList{bridge});
    const auto config = mcp::openCodeConfiguration(mcp::discoverBridge(app), saved.customEndpoint);
    const auto server = config["mcp"].toObject()["servers"].toObject()["vulkana"].toObject();
    CHECK(server["type"] == "local");
    CHECK(server["command"].toArray() == QJsonArray({bridge, "--socket", saved.customEndpoint}));
    CHECK(!server.contains("protocol")); // OpenCode V2 defaults to the compatible classic handshake.
#ifdef Q_OS_UNIX
    const auto image = executable(dir.filePath("Vulkana with spaces.AppImage"));
    CHECK(mcp::discoverBridge(app, image, dir.filePath("AppDir")).command == QStringList({image, "--mcp-bridge"}));
    CHECK(mcp::discoverBridge(app, image, dir.filePath("unrelated")).command == QStringList{bridge});
    CHECK(!mcp::discoverBridge(app, dir.filePath("missing.AppImage"), dir.filePath("AppDir")));
    CHECK(QFile::remove(bridge));
    CHECK(mcp::discoverBridge(app).command == QStringList({app, "--mcp-bridge"}));
    CHECK(!mcp::validateEndpoint("relative.sock").isEmpty());
    CHECK(!mcp::validateEndpoint(dir.filePath(QString(200, 'x'))).isEmpty());
#else
    CHECK(!mcp::validateEndpoint("\\\\remote\\pipe\\unsafe").isEmpty());
#endif
}
void preferencesAndStartup(const QString& application)
{
    QTemporaryDir dir;
    const auto first = endpoint(dir, "first"), second = endpoint(dir, "second");
    ui::MainWindow window(nullptr, false, false); window.setUnsavedPromptEnabled(false); window.show();
    CHECK(imageeditor::test::waitForGraphicsStartup(window));
    CHECK(window.configureMcpStartup(false));
    auto& controller = window.mcpController();
    CHECK(controller.status() == ui::McpController::Status::Stopped);
    ui::PreferencesDialog dialog({}, &window); dialog.mcpPanel()->setController(&controller);
    auto* tabs = dialog.findChild<QTabWidget*>("PreferencesTabs"); tabs->setCurrentWidget(dialog.mcpPanel()); dialog.show();
    auto* custom = dialog.findChild<QCheckBox*>("McpUseCustomEndpoint");
    auto* endpointField = dialog.findChild<QLineEdit*>("McpCustomEndpoint");
    auto* automatic = dialog.findChild<QCheckBox*>("McpAutoStart");
    auto* toggle = dialog.findChild<QPushButton*>("McpServerToggle");
    CHECK(!automatic->isChecked()); custom->setChecked(true); endpointField->setText(first); toggle->click();
    CHECK(controller.status() == ui::McpController::Status::Listening);
    QLocalSocket client; client.connectToServer(first); CHECK(spin([&] { return controller.status() == ui::McpController::Status::Connected; }));
    handshake(client);
    CHECK(spin([&] { return dialog.findChild<QLabel*>("McpServerStatus")->text() == "Connected · Clients: 1"; }));
    QLocalSocket other; other.connectToServer(first);
    CHECK(spin([&] { return controller.clientCount() == 2; })); handshake(other);
    CHECK(spin([&] { return dialog.findChild<QLabel*>("McpServerStatus")->text() == "Connected · Clients: 2"; }));
    automatic->setChecked(true); endpointField->setText(second);
    dialog.onApply = [](const ui::PreferencesState& value) { QSettings settings; return platform::saveMcpSettings(settings, value.mcp); };
    dialog.findChild<QPushButton*>("PreferencesApply")->click();
    CHECK(controller.endpoint() == first && controller.status() == ui::McpController::Status::Connected);
    dialog.findChild<QPushButton*>("McpCopyConfiguration")->click();
    const auto copied = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
    CHECK(copied["mcp"].toObject()["servers"].toObject()["vulkana"].toObject()["command"].toArray().last() == first);
    other.disconnectFromServer();
    CHECK(spin([&] { return controller.clientCount() == 1
        && dialog.findChild<QLabel*>("McpServerStatus")->text() == "Connected · Clients: 1"; }));
    toggle->click(); CHECK(spin([&] { return !controller.running() && client.state() == QLocalSocket::UnconnectedState; }));
    toggle->click(); CHECK(controller.endpoint() == second && controller.status() == ui::McpController::Status::Listening);
    CHECK(window.findChildren<QPushButton*>("McpStopButton").size() == 1);
    client.connectToServer(second); CHECK(spin([&] { return controller.status() == ui::McpController::Status::Connected; })); handshake(client);
    client.disconnectFromServer(); CHECK(spin([&] { return controller.status() == ui::McpController::Status::Listening; }));
    CHECK(spin([&] { return dialog.findChild<QLabel*>("McpServerStatus")->text() == "Listening"; }));
    automatic->setChecked(false); dialog.reject();
    QSettings stored; CHECK(platform::loadMcpSettings(stored) == (platform::McpSettings{true, second}));
    CHECK(controller.running()); // Cancel does not undo an explicit Start.
    controller.stop();
    CHECK(window.configureMcpStartup(false)); CHECK(controller.endpoint() == second && controller.running()); controller.stop();
    CHECK(window.configureMcpStartup(true, first)); CHECK(controller.endpoint() == first); controller.stop();
    CHECK(platform::loadMcpSettings(stored).customEndpoint == second); // CLI override is not persisted.
#ifdef Q_OS_UNIX
    QFile conflict(first); CHECK(conflict.open(QIODevice::WriteOnly)); conflict.write("preserve"); conflict.close();
    CHECK(!controller.start(first)); CHECK(controller.status() == ui::McpController::Status::Error);
    CHECK(conflict.open(QIODevice::ReadOnly)); CHECK(conflict.readAll() == "preserve"); conflict.close();
    CHECK(controller.start(second)); CHECK(controller.status() == ui::McpController::Status::Listening);
    CHECK(!controller.start(first)); CHECK(controller.endpoint() == second && controller.running());
    controller.stop();
    CHECK(!controller.start(dir.filePath("missing-parent/deeper/socket")));
    CHECK(platform::saveMcpSettings(stored, {true, dir.filePath("missing-parent/deeper/socket")}));
    CHECK(!window.configureMcpStartup(false));
    CHECK(window.isVisible() && controller.status() == ui::McpController::Status::Error);
    CHECK(platform::saveMcpSettings(stored, {true, second}));
    CHECK(controller.start(second));
#else
    CHECK(controller.start(second));
#endif
    // The actual application entry point must speak stdio without GUI/driver
    // startup, even when display variables are absent.
    QProcess bridge; auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto* key : {"DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "QT_QPA_PLATFORM"}) environment.remove(key);
    bridge.setProcessEnvironment(environment);
    bridge.start(application, {"--mcp-bridge", "--socket", second, "--timeout", "5"}); CHECK(bridge.waitForStarted());
    bridge.write(request(1, "initialize", initialization()));
    QByteArray output;
    CHECK(spin([&] { output += bridge.readAllStandardOutput(); return output.contains('\n'); }));
    const auto reply = QJsonDocument::fromJson(output.trimmed()).object();
    CHECK(reply["result"].toObject()["protocolVersion"] == "2025-11-25");
    bridge.closeWriteChannel(); CHECK(spin([&] { return bridge.state() == QProcess::NotRunning; }));
    CHECK(bridge.exitCode() == 0 && bridge.readAllStandardError().isEmpty());
    controller.stop();
    if (bridge.state() != QProcess::NotRunning) { bridge.kill(); bridge.waitForFinished(); }
    CHECK(controller.start(second));
    // Exercise the actual Preferences action/persistence wiring, not just a
    // panel with a test callback. The old direct dialog remains hidden.
    bool applied = false;
    QTimer responder;
    responder.setInterval(5);
    QObject::connect(&responder, &QTimer::timeout, &window, [&] {
        for (auto* candidate : window.findChildren<QDialog*>("PreferencesDialog")) {
            auto* actual = dynamic_cast<ui::PreferencesDialog*>(candidate);
            if (!actual || actual == &dialog || !actual->isVisible()) continue;
            responder.stop();
            actual->findChild<QCheckBox*>("McpAutoStart")->setChecked(false);
            actual->findChild<QPushButton*>("PreferencesApply")->click();
            QSettings check;
            CHECK(!platform::loadMcpSettings(check).autoStart);
            CHECK(controller.running() && controller.endpoint() == second);
            applied = true;
            actual->findChild<QPushButton*>("PreferencesOk")->click();
        }
    });
    responder.start();
    auto* action = window.findChild<QAction*>("PreferencesAction"); CHECK(action);
    if (action) action->trigger();
    responder.stop(); CHECK(applied); controller.stop();
    dialog.show(); dialog.mcpPanel()->refreshStatus(); QApplication::processEvents();
    const auto screenshot = qEnvironmentVariable("VULKANA_MCP_PREFS_SCREENSHOT");
    if (!screenshot.isEmpty()) CHECK(dialog.grab().save(screenshot));
    dialog.reject(); window.close(); QApplication::processEvents();
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); app.setApplicationName("McpPreferencesTests"); app.setOrganizationName("ImageEditorTests");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    ui::applyEditorTheme(app);
    settingsAndDiscovery(); stoppingCompletesOnlyTheCurrentCommand();
    CHECK(app.arguments().size() > 1);
    if (app.arguments().size() > 1) preferencesAndStartup(app.arguments()[1]);
    std::cout << "MCP preferences/lifecycle: " << failures << " failures\n";
    return failures ? 1 : 0;
}
