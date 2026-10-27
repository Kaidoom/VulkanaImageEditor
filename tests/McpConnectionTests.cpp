#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/mcp/LocalServer.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUuid>
#include <iostream>

namespace mcp = imageeditor::mcp;
namespace ui = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAIL " << __LINE__ << ": " << #value << '\n'; ++failures; } } while(false)
template<class F> bool spin(F predicate) { return QTest::qWaitFor(predicate, 5000); }
QString endpoint(const QTemporaryDir& dir)
{
#ifdef Q_OS_WIN
    Q_UNUSED(dir);
    return "vulkana-mcp-connections-" + QUuid::createUuid().toString(QUuid::Id128);
#else
    return dir.filePath("editor.sock");
#endif
}
QByteArray request(int id, const QString& method, QJsonObject params = {})
{ return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n'; }
const QByteArray initialized = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
QJsonObject initialization(const QString& version = "2025-11-25")
{ return {{"protocolVersion", version}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "connection-test"}, {"version", "1"}}}}; }
QJsonObject read(QLocalSocket& socket)
{ CHECK(spin([&] { return socket.canReadLine(); })); return QJsonDocument::fromJson(socket.readLine()).object(); }
void handshake(QLocalSocket& socket)
{
    socket.write(request(1, "initialize", initialization())); CHECK(read(socket).contains("result"));
    socket.write(initialized);
}
struct Bridge {
    QProcess process;
    QByteArray output;
    int nextId = 1;
    Bridge(const QString& executable, const QStringList& prefix, const QString& path)
    {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_LOGGING_RULES", "imageeditor.mcp.bridge.debug=true");
        for (const auto* key : {"DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET"}) env.remove(key);
        process.setProcessEnvironment(env);
        process.start(executable, prefix + QStringList{"--socket", path, "--timeout", "5"});
        CHECK(process.waitForStarted());
    }
    ~Bridge() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); } }
    void send(const QString& method, QJsonObject params = {}) { process.write(request(nextId++, method, params)); }
    void tool(const QString& name, QJsonObject args = {}) { send("tools/call", {{"name", name}, {"arguments", args}}); }
    QJsonObject reply()
    {
        CHECK(spin([&] { output += process.readAllStandardOutput(); return output.contains('\n'); }));
        const auto end = output.indexOf('\n'); if (end < 0) return {};
        const auto value = QJsonDocument::fromJson(output.left(end)).object(); output.remove(0, end + 1);
        CHECK(value["jsonrpc"] == "2.0"); // Diagnostic logging must never contaminate stdout.
        return value;
    }
    void eof() { process.closeWriteChannel(); CHECK(spin([&] { return process.state() == QProcess::NotRunning; })); CHECK(process.exitCode() == 0); }
};
QJsonObject data(const QJsonObject& reply) { return reply["result"].toObject()["structuredContent"].toObject(); }

void bridgesAndEditor(const QString& application, const QString& standalone)
{
    QTemporaryDir dir; const auto path = endpoint(dir);
    ui::MainWindow window(nullptr, false, false); window.setUnsavedPromptEnabled(false); window.show();
    CHECK(imageeditor::test::waitForGraphicsStartup(window));
    auto& controller = window.mcpController(); CHECK(controller.start(path));
    Bridge a(application, {"--mcp-bridge"}, path), b(standalone, {}, path);
    // Overlap both handshakes and discovery, including identical request IDs.
    a.send("initialize", initialization("2025-06-18")); b.send("initialize", initialization());
    CHECK(a.reply()["result"].toObject()["protocolVersion"] == "2025-06-18");
    CHECK(b.reply()["result"].toObject()["protocolVersion"] == "2025-11-25");
    CHECK(controller.clientCount() == 2);
    a.process.write(initialized); b.process.write(initialized);
    a.send("tools/list"); b.send("tools/list");
    const auto tools = a.reply()["result"].toObject()["tools"].toArray();
    CHECK(tools.size() >= 51); CHECK(b.reply()["result"].toObject()["tools"].toArray() == tools);
    a.tool("vulkana_get_state"); b.tool("vulkana_get_state");
    CHECK(data(a.reply())["connectedClients"].toInt() == 2);
    CHECK(data(b.reply())["connectedClients"].toInt() == 2);
    a.tool("vulkana_document_create", {{"width", 96}, {"height", 64}});
    const auto doc = data(a.reply())["document"].toObject(); const auto docId = doc["documentId"];
    QJsonObject resize{{"documentId", docId}, {"expectedRevision", doc["revision"]}, {"width", 80}, {"height", 50}};
    const auto depth = window.editorSession().history().undoDepth();
    a.tool("vulkana_canvas_resize", resize); resize["width"] = 70; b.tool("vulkana_canvas_resize", resize);
    const auto ar = a.reply(), br = b.reply();
    const bool aFailed = ar["result"].toObject()["isError"].toBool();
    CHECK(aFailed != br["result"].toObject()["isError"].toBool());
    CHECK(data(aFailed ? ar : br)["error"].toObject()["code"] == "stale_revision");
    CHECK(window.editorSession().history().undoDepth() == depth + 1);
    CHECK(window.editorSession().document()->canvas().extent.width == (aFailed ? 70u : 80u));
    b.tool("vulkana_history", {{"documentId", docId}, {"operation", "undo"}});
    CHECK(!b.reply()["result"].toObject()["isError"].toBool());
    CHECK(window.editorSession().document()->canvas().extent.width == 96);
    a.tool("vulkana_canvas_resize", {{"documentId", "999999999"}, {"width", 50}, {"height", 50}});
    CHECK(data(a.reply())["error"].toObject()["code"] == "wrong_document");
    a.eof(); CHECK(spin([&] { return controller.clientCount() == 1; }));
    b.tool("vulkana_get_state"); CHECK(data(b.reply())["document"].toObject()["documentId"] == docId);
    CHECK(b.process.readAllStandardError().contains("tools/list succeeded"));
    Bridge c(application, {"--mcp-bridge"}, path);
    c.send("initialize", initialization()); CHECK(c.reply().contains("result"));
    CHECK(controller.clientCount() == 2);
    controller.stop(); CHECK(controller.clientCount() == 0);
    // A bridge blocked on client stdin observes Stop on its next request.
    b.send("ping"); c.send("ping");
    CHECK(spin([&] { return b.process.state() == QProcess::NotRunning && c.process.state() == QProcess::NotRunning; }));
    CHECK(b.process.exitCode() != 0 && c.process.exitCode() != 0);
    CHECK(controller.start(path));
    Bridge d(standalone, {}, path); d.send("initialize", initialization()); CHECK(d.reply().contains("result"));
    d.process.write(initialized); d.send("tools/list"); CHECK(d.reply()["result"].toObject()["tools"].toArray() == tools);
    d.tool("vulkana_get_state"); CHECK(data(d.reply())["document"].toObject()["documentId"] == docId);
    d.eof(); CHECK(spin([&] { return controller.clientCount() == 0; })); controller.stop(); window.close();
}

void nestedEventsAndStop()
{
    QTemporaryDir dir; const auto path = endpoint(dir);
    mcp::ToolRegistry registry; mcp::LocalServer server(registry, "test");
    QLocalSocket a, b, newcomer;
    int depth = 0, completed = 0; bool stopInside = false;
    registry.add({"edit", "Serialized test edit", {{"type", "object"}}, {}, [&](const QJsonObject&) {
        CHECK(QThread::currentThread() == qApp->thread()); CHECK(++depth == 1);
        if (completed == 0) {
            b.write(request(2, "tools/call", {{"name", "edit"}})); b.flush();
            if (stopInside) server.close();
            else {
                // Disconnect the executing caller and accept another client
                // inside a nested UI event loop without replacing its session.
                a.abort(); newcomer.connectToServer(path);
            }
            QTest::qWait(30); CHECK(depth == 1 && completed == 0);
            if (stopInside) CHECK(server.stopping());
        }
        ++completed; --depth; return mcp::toolSuccess({{"completed", completed}});
    }});
    CHECK(server.listen(path));
    a.connectToServer(path); b.connectToServer(path); CHECK(spin([&] { return server.clientCount() == 2; }));
    handshake(a); handshake(b);
    a.write(request(2, "tools/call", {{"name", "edit"}}));
    CHECK(read(b).contains("result")); CHECK(completed == 2 && depth == 0);
    CHECK(server.clientCount() == 2); handshake(newcomer);
    b.abort(); newcomer.abort(); CHECK(spin([&] { return server.clientCount() == 0; }));
    stopInside = true; completed = 0;
    a.connectToServer(path); b.connectToServer(path); CHECK(spin([&] { return server.clientCount() == 2; }));
    handshake(a); handshake(b);
    a.write(request(2, "tools/call", {{"name", "edit"}}));
    CHECK(data(read(a))["completed"].toInt() == 1);
    CHECK(spin([&] { return !server.stopping() && server.clientCount() == 0
        && a.state() == QLocalSocket::UnconnectedState && b.state() == QLocalSocket::UnconnectedState; }));
    CHECK(completed == 1 && b.bytesAvailable() == 0);
    CHECK(server.listen(path)); server.close();
}
void stoppingWithUnreadResponse()
{
    QTemporaryDir dir; const auto path = endpoint(dir);
    mcp::ToolRegistry registry; mcp::LocalServer server(registry, "test");
    int completed = 0;
    registry.add({"large", "Completed edit with a large reply", {{"type", "object"}}, {}, [&](const QJsonObject&) {
        server.close(); ++completed;
        return mcp::toolSuccess({{"payload", QString(4 * 1024 * 1024, 'x')}});
    }});
    CHECK(server.listen(path)); QLocalSocket client; client.connectToServer(path);
    CHECK(spin([&] { return server.clientCount() == 1; })); handshake(client);
    client.setReadBufferSize(1); // Simulate a client that stops consuming output.
    client.write(request(2, "tools/call", {{"name", "large"}}));
    CHECK(spin([&] { return completed == 1 && !server.stopping() && server.clientCount() == 0; }));
    CHECK(server.listen(path)); client.abort(); server.close();
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); app.setApplicationName("McpConnectionTests"); app.setOrganizationName("ImageEditorTests");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    ui::applyEditorTheme(app);
    nestedEventsAndStop(); stoppingWithUnreadResponse(); CHECK(app.arguments().size() == 3);
    if (app.arguments().size() == 3) bridgesAndEditor(app.arguments()[1], app.arguments()[2]);
    std::cout << "MCP multi-client connections: " << failures << " failures\n"; return failures ? 1 : 0;
}
