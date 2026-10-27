#include "imageeditor/mcp/LocalServer.hpp"
#include "imageeditor/mcp/Integration.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
Q_LOGGING_CATEGORY(lcMcpBridge, "imageeditor.mcp.bridge", QtWarningMsg)
void trace(const char* event)
{
    // Explicit stderr: stdout belongs exclusively to the MCP wire protocol.
    if (lcMcpBridge().isDebugEnabled()) {
        std::fprintf(stderr, "vulkana-mcp bridge pid=%lld: %s\n",
            static_cast<long long>(QCoreApplication::applicationPid()), event);
        std::fflush(stderr);
    }
}
struct TraceLifetime { ~TraceLifetime() { trace("exiting; local connection released"); } };
int fail(const QString& message)
{
    const auto bytes = ("vulkana-mcp: " + message + '\n').toUtf8();
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stderr);
    return 1;
}
}
int imageeditor::mcp::runBridge(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("vulkana-mcp"));
    QCoreApplication::setApplicationVersion(QStringLiteral(VULKANA_MCP_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("MCP stdio bridge for an explicitly enabled Vulkana editor session. Does not launch or close the editor."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"socket", "Local editor socket or Windows pipe name.", "path", imageeditor::mcp::defaultSocketPath()});
    parser.addOption({"timeout", "Maximum seconds to wait for each editor response (1–600).", "seconds", "120"});
    parser.process(app);
    trace("starting");
    const TraceLifetime lifetime;
    bool valid = false;
    const int timeout = parser.value("timeout").toInt(&valid);
    if (!valid || timeout < 1 || timeout > 600) return fail("--timeout must be between 1 and 600 seconds");
    if (!parser.positionalArguments().isEmpty()) return fail("Unexpected positional argument");
#ifdef Q_OS_WIN
    const auto invalidEndpoint = imageeditor::mcp::validateEndpoint(parser.value("socket"));
    if (!invalidEndpoint.isEmpty()) return fail(invalidEndpoint);
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QLocalSocket socket;
    socket.setReadBufferSize(32 * 1024 * 1024 + 1);
    socket.connectToServer(parser.value("socket"));
    if (!socket.waitForConnected(5000))
        return fail("Cannot connect to the editor: " + socket.errorString() + ". Start Vulkana with its MCP option first.");
    trace("connected to editor");
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly))
        return fail("Cannot open standard input/output");
    QByteArray replies;
    // Requests within this bridge are serialized. The editor serializes work
    // across independent connections as well.
    // Notifications have no reply. EOF detaches; it never closes the editor.
    while (true) {
        auto line = input.readLine(imageeditor::mcp::maximumMessageBytes + 2);
        if (line.isEmpty()) {
            if (input.error() != QFileDevice::NoError) return fail("Unable to read standard input");
            break;
        }
        if (line.size() > imageeditor::mcp::maximumMessageBytes || !line.endsWith('\n'))
            return fail("Every MCP input must be one newline-terminated JSON message of at most 8 MiB");
        QJsonParseError parse;
        const auto request = QJsonDocument::fromJson(line, &parse);
        bool knownEnvelope = true;
        const auto object = request.object();
        const auto method = object.value("method").toString();
        if (method == "initialize") trace("forwarding initialize");
        else if (method == "tools/list") trace("forwarding tools/list");
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key() != "jsonrpc" && it.key() != "id" && it.key() != "method" && it.key() != "params")
                knownEnvelope = false;
        const bool notification = parse.error == QJsonParseError::NoError && request.isObject()
            && knownEnvelope && !object.contains("id") && object.value("method").isString()
            && object.value("jsonrpc") == QJsonValue("2.0");
        if (socket.state() != QLocalSocket::ConnectedState) return fail("Editor disconnected");
        if (socket.write(line) != line.size())
            return fail("Unable to send request to editor: " + socket.errorString());
        QElapsedTimer sending;
        sending.start();
        while (socket.bytesToWrite() > 0) {
            const auto remaining = 5000 - sending.elapsed();
            if (remaining <= 0 || !socket.waitForBytesWritten(int(remaining)))
                return fail("Unable to send request to editor: " + socket.errorString());
        }
        if (notification) continue;
        QElapsedTimer elapsed;
        elapsed.start();
        while (!replies.contains('\n')) {
            if (socket.bytesAvailable() > 0) replies += socket.readAll();
            if (replies.size() > 32 * 1024 * 1024) return fail("Editor response exceeds 32 MiB");
            if (replies.contains('\n')) break;
            const auto remaining = timeout * 1000 - elapsed.elapsed();
            if (remaining <= 0 || !socket.waitForReadyRead(int(remaining)))
                return fail("Editor response unavailable: " + socket.errorString()
                    + ". The operation may already have completed; reconnect and query state before retrying an edit.");
        }
        const auto newline = replies.indexOf('\n');
        const auto response = replies.left(newline + 1);
        replies.remove(0, newline + 1);
        if (lcMcpBridge().isDebugEnabled() && (method == "initialize" || method == "tools/list")) {
            const auto reply = QJsonDocument::fromJson(response).object();
            if (reply.contains("error")) trace("received JSON-RPC error");
            else trace(method == "initialize" ? "initialize succeeded" : "tools/list succeeded");
        }
        if (output.write(response) != response.size() || !output.flush()) return fail("Unable to write standard output");
    }
    socket.disconnectFromServer();
    trace("stdin EOF; disconnected");
    return 0;
}
