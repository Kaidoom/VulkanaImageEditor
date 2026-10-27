#include "imageeditor/mcp/LocalServer.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

namespace mcp = imageeditor::mcp;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAIL " << __LINE__ << ": " #value "\n"; ++failures; } } while (false)

QByteArray rpc(int id, const QString& method, QJsonObject params = {})
{
    return QJsonDocument(QJsonObject {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact);
}
QByteArray notification(const QString& method, QJsonObject params = {})
{
    return QJsonDocument(QJsonObject {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact);
}
QJsonObject initializeParams()
{
    return {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject {}},
        {"clientInfo", QJsonObject {{"name", "protocol-tests"}, {"version", "1"}}}};
}
bool spin(const std::function<bool()>& done)
{
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents();
        if (done()) return true;
        QTest::qWait(2);
    } while (elapsed.elapsed() < 3000);
    return done();
}
void registryAndProtocol()
{
    int calls = 0;
    mcp::ToolRegistry registry;
    const QJsonObject schema {{"type", "object"}, {"additionalProperties", false},
        {"properties", QJsonObject {
            {"count", QJsonObject {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
            {"label", QJsonObject {{"type", "string"}, {"maxLength", 10}, {"pattern", "^[A-Z]+$"}}},
            {"mode", QJsonObject {{"type", "string"}, {"enum", QJsonArray {"a", "b"}}}},
            {"items", QJsonObject {{"type", "array"}, {"minItems", 1}, {"maxItems", 3}, {"uniqueItems", true},
                {"items", QJsonObject {{"type", "object"}, {"properties", QJsonObject {
                    {"x", QJsonObject {{"type", "number"}, {"exclusiveMinimum", -1}, {"exclusiveMaximum", 1}}}}},
                    {"required", QJsonArray {"x"}}, {"additionalProperties", false}}}}}}},
        {"required", QJsonArray {"count"}}};
    registry.add({"test.echo", "Return checked arguments", schema, {{"readOnlyHint", true}},
        [&](const QJsonObject& args) { ++calls; return mcp::toolSuccess(args); }});
    CHECK(registry.list().size() == 1);
    CHECK(!registry.call("test.echo", {{"count", 1}}).value("isError").toBool());
    CHECK(calls == 1);
    for (const auto args : {QJsonObject {}, QJsonObject {{"count", "1"}}, QJsonObject {{"count", 1.5}},
             QJsonObject {{"count", 4}}, QJsonObject {{"count", -1}}, QJsonObject {{"count", 1}, {"extra", 1}},
             QJsonObject {{"count", 1}, {"label", "lower"}}, QJsonObject {{"count", 1}, {"label", "ABCDEFGHIJK"}},
             QJsonObject {{"count", 1}, {"mode", "unknown"}}, QJsonObject {{"count", 1}, {"items", QJsonArray {}}},
             QJsonObject {{"count", 1}, {"items", QJsonArray {QJsonObject {{"x", 1}}}}},
             QJsonObject {{"count", 1}, {"items", QJsonArray {QJsonObject {{"x", 0}, {"y", 0}}}}},
             QJsonObject {{"count", 1}, {"items", QJsonArray {QJsonObject {{"x", 0}}, QJsonObject {{"x", 0}}}}}})
        CHECK(registry.call("test.echo", args).value("isError").toBool());
    CHECK(calls == 1);
    CHECK(registry.call("missing", {}).value("isError").toBool());
    CHECK(!registry.call("test.echo", {{"count", 2}, {"label", "OK"}, {"mode", "b"},
        {"items", QJsonArray {QJsonObject {{"x", .5}}}}}).value("isError").toBool());
    CHECK(calls == 2);
    const QJsonObject unionSchema {{"type", "object"}, {"oneOf", QJsonArray {
        QJsonObject {{"type", "object"}, {"properties", QJsonObject {{"text", QJsonObject {{"type", "string"}}}}}, {"required", QJsonArray {"text"}}},
        QJsonObject {{"type", "object"}, {"properties", QJsonObject {{"number", QJsonObject {{"type", "number"}}}}}, {"required", QJsonArray {"number"}}}}}};
    CHECK(mcp::validateArguments(unionSchema, {{"text", "valid"}}).isEmpty());
    CHECK(!mcp::validateArguments(unionSchema, {{"text", "invalid"}, {"number", 1}}).isEmpty());
    bool rejected = false;
    try { registry.add({"test.echo", "Duplicate", schema, {}, [](const QJsonObject&) { return QJsonObject {}; }}); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { registry.add({"unsupported", "Invalid schema", {{"type", "object"}, {"badKeyword", true}}, {},
        [](const QJsonObject&) { return QJsonObject {}; }}); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    registry.add({"throws", "Exceptions become tool errors", {{"type", "object"}}, {},
        [](const QJsonObject&) -> QJsonObject { throw std::runtime_error("Expected failure"); }});
    CHECK(registry.call("throws", {}).value("isError").toBool());

    mcp::ProtocolSession session(registry, "test-version");
    auto response = session.handleLine("{");
    CHECK(response && response->value("error").toObject().value("code") == -32700);
    response = session.handleLine("[]");
    CHECK(response && response->value("error").toObject().value("code") == -32600);
    response = session.handleLine(rpc(1, "ping"));
    CHECK(response && response->contains("result"));
    response = session.handleLine(rpc(2, "tools/list"));
    CHECK(response && response->value("error").toObject().value("code") == -32002);
    response = session.handleLine(rpc(3, "initialize", initializeParams()));
    CHECK(response && response->value("result").toObject().value("protocolVersion") == "2025-11-25");
    CHECK(!session.initialized());
    CHECK(!session.handleLine(notification("notifications/initialized")));
    CHECK(session.initialized());
    response = session.handleLine(rpc(4, "tools/list"));
    CHECK(response && response->value("result").toObject().value("tools").toArray().size() == 2);
    response = session.handleLine(rpc(5, "tools/call", {{"name", "test.echo"}, {"arguments", QJsonObject {{"count", 2}}}}));
    CHECK(response && !response->value("result").toObject().value("isError").toBool());
    CHECK(calls == 3);
    response = session.handleLine(rpc(5, "tools/call", {{"name", "test.echo"}, {"arguments", QJsonObject {{"count", 2}}}}));
    CHECK(response && response->value("error").toObject().value("code") == -32600);
    CHECK(calls == 3);
    CHECK(!session.handleLine(notification("tools/call", {{"name", "test.echo"}, {"arguments", QJsonObject {{"count", 2}}}})));
    CHECK(calls == 3);
    CHECK(!session.handleLine(notification("notifications/cancelled", {{"requestId", 5}})));
    response = session.handleLine(rpc(6, "tools/call", {{"name", "test.echo"}, {"arguments", QJsonObject {{"count", 99}}}}));
    CHECK(response && response->value("result").toObject().value("isError").toBool());
    CHECK(calls == 3);
    response = session.handleLine(rpc(7, "tools/call", {{"name", "missing"}}));
    CHECK(response && response->value("error").toObject().value("code") == -32602);
    response = session.handleLine(rpc(8, "shell/execute"));
    CHECK(response && response->value("error").toObject().value("code") == -32601);
    response = session.handleLine(rpc(9, "initialize", initializeParams()));
    CHECK(response && response->contains("error"));
    response = session.handleLine(rpc(10, "tools/list", {{"cursor", "next"}}));
    CHECK(response && response->contains("error"));
    response = session.handleLine(QByteArray(mcp::maximumMessageBytes + 1, ' '));
    CHECK(response && response->contains("error"));
}

void transport()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path = directory.path() + "/private/editor.sock";
    mcp::ToolRegistry registry;
    mcp::LocalServer server(registry, "test");
    QString error;
    CHECK(server.listen(path, &error));
    CHECK(error.isEmpty());
    mcp::LocalServer duplicate(registry, "test");
    CHECK(!duplicate.listen(path, &error));
    QLocalSocket client;
    client.connectToServer(path);
    CHECK(spin([&] { return client.state() == QLocalSocket::ConnectedState && server.connected(); }));
    auto exchange = [&](const QByteArray& request) {
        client.write(request + '\n');
        CHECK(spin([&] { return client.canReadLine(); }));
        return QJsonDocument::fromJson(client.readLine()).object();
    };
    CHECK(exchange(rpc(1, "initialize", initializeParams())).contains("result"));
    client.write(notification("notifications/initialized") + '\n');
    CHECK(exchange(rpc(2, "tools/list")).value("result").toObject().value("tools").toArray().isEmpty());
    QLocalSocket second;
    second.connectToServer(path);
    CHECK(spin([&] { return server.clientCount() == 2; }));
    second.write(rpc(1, "tools/list") + '\n');
    CHECK(spin([&] { return second.canReadLine(); }));
    CHECK(QJsonDocument::fromJson(second.readLine()).object()["error"].toObject()["code"] == -32002);
    second.write(rpc(2, "initialize", initializeParams()) + '\n');
    CHECK(spin([&] { return second.canReadLine(); }));
    CHECK(QJsonDocument::fromJson(second.readLine()).object().contains("result"));
    second.write(notification("notifications/initialized") + '\n' + rpc(3, "tools/list") + '\n');
    CHECK(spin([&] { return second.canReadLine(); }));
    CHECK(QJsonDocument::fromJson(second.readLine()).object().contains("result"));
    CHECK(server.connected());
    CHECK(exchange(rpc(3, "ping")).contains("result"));
    client.disconnectFromServer();
    CHECK(spin([&] { return server.clientCount() == 1; }));
    second.write(rpc(4, "ping") + '\n');
    CHECK(spin([&] { return second.canReadLine(); }));
    CHECK(QJsonDocument::fromJson(second.readLine()).object().contains("result"));
    client.connectToServer(path);
    CHECK(spin([&] { return server.clientCount() == 2; }));
    // A reconnect starts a fresh handshake and request-ID namespace.
    CHECK(exchange(rpc(1, "tools/list")).contains("error"));
    client.write(QByteArray(mcp::maximumMessageBytes + 1, ' '));
    CHECK(spin([&] { return server.clientCount() == 1; }));
    second.write(rpc(5, "ping") + '\n');
    CHECK(spin([&] { return second.canReadLine(); }));
    CHECK(QJsonDocument::fromJson(second.readLine()).object().contains("result"));
    server.close();
    CHECK(spin([&] { return second.state() == QLocalSocket::UnconnectedState; }));
    CHECK(server.clientCount() == 0);
    CHECK(server.listen(path, &error));
    server.close();
#ifdef Q_OS_UNIX
    QFile stale(path);
    CHECK(stale.open(QIODevice::WriteOnly));
    CHECK(stale.write("Do not replace") == 14);
    stale.close();
    CHECK(!server.listen(path, &error));
    CHECK(stale.open(QIODevice::ReadOnly));
    CHECK(stale.readAll() == "Do not replace");
    stale.close();
    const auto unsafe = directory.path() + "/unsafe";
    CHECK(QDir().mkdir(unsafe));
    CHECK(QFile::setPermissions(unsafe, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther));
    CHECK(!server.listen(unsafe + "/editor.sock", &error));
#endif
}
void bridge(const QString& executable)
{
    if (executable.isEmpty()) return;
    QTemporaryDir directory;
    const auto path = directory.path() + "/private/bridge.sock";
    mcp::ToolRegistry registry;
    registry.add({"echo", "Echo", {{"type", "object"}}, {}, [](const QJsonObject&) {
        return mcp::toolSuccess({{"alive", true}});
    }});
    mcp::LocalServer server(registry, "test");
    CHECK(server.listen(path));
    QProcess process;
    process.start(executable, {"--socket", path, "--timeout", "5"});
    CHECK(process.waitForStarted());
    QByteArray output;
    auto exchange = [&](const QByteArray& line) {
        process.write(line + '\n');
        CHECK(spin([&] { output += process.readAllStandardOutput(); return output.contains('\n'); }));
        const auto pos = output.indexOf('\n');
        const auto reply = QJsonDocument::fromJson(output.left(pos)).object();
        output.remove(0, pos + 1);
        return reply;
    };
    CHECK(exchange(rpc(1, "initialize", initializeParams())).contains("result"));
    process.write(notification("notifications/initialized") + '\n');
    CHECK(exchange(rpc(2, "tools/call", {{"name", "echo"}})).value("result").toObject()
        .value("structuredContent").toObject().value("alive").toBool());
    process.closeWriteChannel();
    CHECK(spin([&] { return process.state() == QProcess::NotRunning; }));
    CHECK(process.exitCode() == 0 && process.readAllStandardError().isEmpty());
    CHECK(!server.connected());
    CHECK(server.socketName().endsWith(path)); // Detach does not close the editor server.
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); }
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    registryAndProtocol();
    transport();
    bridge(app.arguments().size() > 1 ? app.arguments()[1] : QString());
    std::cout << "MCP protocol assertions: " << failures << " failures\n";
    return failures ? 1 : 0;
}
