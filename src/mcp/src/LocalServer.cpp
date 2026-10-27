#include "imageeditor/mcp/LocalServer.hpp"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTimer>
#include <QLoggingCategory>
#include <QThread>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#endif
#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace imageeditor::mcp {
Q_LOGGING_CATEGORY(lcMcpServer, "imageeditor.mcp.server")
struct LocalServer::Client {
    Client(std::uint64_t number, QLocalSocket* connection, ToolRegistry& registry, const QString& version)
        : id(number), socket(connection), protocol(registry, version) {}
    std::uint64_t id;
    QPointer<QLocalSocket> socket;
    ProtocolSession protocol;
    QByteArray pending;
    bool queued {};
};
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
}
QString defaultSocketPath()
{
#ifdef Q_OS_WIN
    // UserAccessOption applies a per-user ACL to the named pipe itself.
    return QStringLiteral("vulkana-mcp-editor-") + QString::fromLatin1(
        QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
#else
    auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QDir::tempPath() + "/vulkana-runtime-" + QString::number(geteuid());
    return runtime + "/vulkana-mcp/editor.sock";
#endif
}
QString normalizeEndpoint(const QString& path)
{
#ifdef Q_OS_WIN
    const auto prefix = QStringLiteral("\\\\.\\pipe\\");
    return (path.startsWith(prefix, Qt::CaseInsensitive) ? path.mid(prefix.size()) : path).toLower();
#else
    return QDir::cleanPath(path);
#endif
}
QString validateEndpoint(const QString& input)
{
    if (input.isEmpty() || input.contains(QChar::Null)) return "Invalid MCP endpoint";
    const auto path = normalizeEndpoint(input);
#ifdef Q_OS_WIN
    static const QRegularExpression localName(QStringLiteral("^[A-Za-z0-9_.-]{1,180}$"));
    if (!localName.match(path).hasMatch()) return "Use a local pipe name containing letters, numbers, dots, underscores or hyphens. Remote pipe paths are not allowed.";
#else
    if (!QFileInfo(path).isAbsolute()) return "MCP socket path must be absolute";
    const auto directory = QFileInfo(path).absolutePath();
    if (QFile::encodeName(path).size() >= qsizetype(sizeof(sockaddr_un::sun_path))
        || QFile::encodeName(directory + "/.listener-XXXXXX/socket").size() >= qsizetype(sizeof(sockaddr_un::sun_path)))
        return "MCP socket path is too long; choose a shorter private directory";
    struct stat info {};
    if (::lstat(QFile::encodeName(directory).constData(), &info) == 0) {
        if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0777) != 0700)
            return "MCP socket directory must be a real directory owned by this user with mode 0700";
    } else if (errno != ENOENT || !QFileInfo(QFileInfo(directory).absolutePath()).isDir())
        return "MCP socket parent directory cannot be created; its parent must already exist";
#endif
    return {};
}
bool prepareSocketDirectory(const QString& path, QString* error)
{
    const auto invalid = validateEndpoint(path);
    if (!invalid.isEmpty()) return fail(error, invalid);
#ifdef Q_OS_UNIX
    if (!QFileInfo(path).isAbsolute()) return fail(error, "MCP socket path must be absolute");
    const auto directory = QFileInfo(path).absolutePath();
    const auto encoded = QFile::encodeName(directory);
    struct stat info {};
    if (::lstat(encoded.constData(), &info) != 0) {
        if (errno != ENOENT) return fail(error, "Cannot inspect MCP socket directory: " + QString::fromLocal8Bit(std::strerror(errno)));
        if (::mkdir(encoded.constData(), 0700) != 0)
            return fail(error, "Cannot create private MCP socket directory: " + QString::fromLocal8Bit(std::strerror(errno)));
        if (::lstat(encoded.constData(), &info) != 0) return fail(error, "Cannot inspect newly created MCP socket directory");
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0077) != 0)
        return fail(error, "MCP socket directory must be a real directory owned by this user with mode0700");
    if ((info.st_mode & 0700) != 0700) return fail(error, "MCP socket directory needs owner read/write/execute permissions");
#endif
    return true;
}
LocalServer::LocalServer(ToolRegistry& registry, QString serverVersion, QObject* parent)
    : QObject(parent), registry_(registry), version_(std::move(serverVersion)), server_(this)
{
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] { acceptConnection(); });
}
LocalServer::~LocalServer() { close(); }
bool LocalServer::listen(const QString& input, QString* error)
{
    if (server_.isListening() || stopRequested_ || processing_) return fail(error, "MCP server is already listening or finishing a command");
    const auto path = normalizeEndpoint(input);
    if (!prepareSocketDirectory(path, error)) return false;
    QString lockPath = path + ".lock";
#ifdef Q_OS_WIN
    const auto lockDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/vulkana-mcp";
    if (!QDir().mkpath(lockDirectory)) return fail(error, "Cannot create MCP endpoint lock directory");
    lockPath = lockDirectory + '/' + QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".lock";
#endif
    auto lock = std::make_unique<QLockFile>(lockPath);
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) return fail(error, "MCP endpoint is locked by another editor");
#ifdef Q_OS_UNIX
    // With access options Qt binds a temporary socket and renames it, which
    // would otherwise replace an existing endpoint. Never steal that pathname.
    struct stat existing {};
    if (::lstat(QFile::encodeName(path).constData(), &existing) == 0 || errno != ENOENT)
        return fail(error, "MCP endpoint already exists or cannot be inspected. Check for a running editor or remove a confirmed stale socket manually.");
#endif
#ifdef Q_OS_UNIX
    // Qt owns only a private binding. Publish a hard link atomically, without
    // replacing anything. Closing Qt cannot unlink somebody else's replacement
    // at the public endpoint; we remove that entry only if its inode is ours.
    auto listener = std::make_unique<QTemporaryDir>(QFileInfo(path).absolutePath() + "/.listener-XXXXXX");
    if (!listener->isValid()) return fail(error, "Cannot create private MCP listener directory");
    if (!server_.listen(listener->filePath("socket"))) return fail(error, "Cannot listen on MCP socket: " + server_.errorString());
    struct stat bound {};
    const auto binding = QFile::encodeName(server_.fullServerName());
    if (::lstat(binding.constData(), &bound) != 0 || !S_ISSOCK(bound.st_mode)
        || ::link(binding.constData(), QFile::encodeName(path).constData()) != 0) {
        server_.close();
        return fail(error, "Cannot publish MCP endpoint without replacing an existing path");
    }
    endpointDevice_ = std::uint64_t(bound.st_dev); endpointInode_ = std::uint64_t(bound.st_ino);
    privateListener_ = std::move(listener);
#else
    const auto pipe = QStringLiteral("\\\\.\\pipe\\") + path;
    if (::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipe.utf16()), NMPWAIT_NOWAIT)
        || ::GetLastError() != ERROR_FILE_NOT_FOUND)
        return fail(error, "MCP pipe already exists, is busy, or cannot be inspected");
    if (!server_.listen(path)) {
        return fail(error, "Cannot listen on MCP socket: " + server_.errorString()
            + ". Existing sockets are never removed automatically; check for another running editor or a stale endpoint.");
    }
#endif
    endpoint_ = path;
    endpointLock_ = std::move(lock);
    qCInfo(lcMcpServer) << "Listening" << endpoint_ << "clients" << clientCount();
    if (error) error->clear();
    if (onStateChanged) onStateChanged();
    return true;
}
void LocalServer::closeListener()
{
#ifdef Q_OS_UNIX
    if (privateListener_ && !endpoint_.isEmpty()) {
        struct stat current {};
        const auto path = QFile::encodeName(endpoint_);
        if (::lstat(path.constData(), &current) == 0 && S_ISSOCK(current.st_mode)
            && std::uint64_t(current.st_dev) == endpointDevice_ && std::uint64_t(current.st_ino) == endpointInode_)
            (void)::unlink(path.constData());
    }
#endif
    server_.close();
    privateListener_.reset();
    endpointLock_.reset();
}
void LocalServer::close()
{
    closeListener();
    ready_.clear();
    for (const auto& client : clients_) { client->pending.clear(); client->queued = false; }
    qCInfo(lcMcpServer) << "Stop requested; clients" << clientCount() << "executing" << processing_;
    if (processing_) {
        stopRequested_ = true;
        if (onStateChanged) onStateChanged();
        return; // No nested waits or rollback claims; let the current call finish.
    }
    const auto clients = clients_.values();
    for (const auto& client : clients) disconnectClient(client, "server stopped");
    stopRequested_ = false;
    if (onStateChanged) onStateChanged();
}
void LocalServer::finishStop()
{
    const auto clients = clients_.values();
    for (const auto& client : clients) {
        if (!client->socket) continue;
        client->socket->disconnectFromServer(); // Flush the completed command's reply.
        if (client->socket && client->socket->state() == QLocalSocket::UnconnectedState)
            disconnectClient(client, "server stopped after command");
        else if (client->socket) {
            // An unresponsive reader must not hold Stop open indefinitely.
            // The command is already complete; this only bounds reply draining.
            QTimer::singleShot(1000, this, [this, client] {
                if (client->socket) disconnectClient(client, "server stop reply drain timeout; command completed");
            });
        }
    }
    if (clients_.isEmpty()) stopRequested_ = false;
    if (onStateChanged) onStateChanged();
}
QString LocalServer::socketName() const { return endpoint_; }
bool LocalServer::connected() const { return !clients_.isEmpty(); }
void LocalServer::disconnectClient(ClientPtr client, const char* reason)
{
    if (client->socket) {
        auto* socket = client->socket.data();
        disconnect(socket, nullptr, this, nullptr);
        clients_.remove(socket);
        client->socket.clear();
        socket->abort();
        socket->deleteLater();
        qCInfo(lcMcpServer) << "Disconnected session" << client->id << reason << "clients" << clientCount();
        if (onConnectionChanged) onConnectionChanged(connected());
    }
    // processReady holds a shared reference: disconnect during nested events
    // must not destroy a ProtocolSession while its handler is executing.
    client->pending.clear();
    if (!processing_ && clients_.isEmpty()) stopRequested_ = false;
    if (onStateChanged) onStateChanged();
}
void LocalServer::acceptConnection()
{
    while (server_.hasPendingConnections()) {
        auto* socket = server_.nextPendingConnection();
        if (stopRequested_ || !server_.isListening()) {
            qCWarning(lcMcpServer) << "Rejected connection: server stopping; clients" << clientCount();
            socket->abort(); socket->deleteLater();
            continue;
        }
        const auto client = std::make_shared<Client>(nextClientId_++, socket, registry_, version_);
        clients_.insert(socket, client);
        socket->setReadBufferSize(maximumMessageBytes + 1);
        qint64 peerPid = -1;
#ifdef Q_OS_LINUX
        ucred credentials {}; socklen_t length = sizeof(credentials);
        if (::getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0)
            peerPid = credentials.pid;
#endif
        qCInfo(lcMcpServer) << "Accepted session" << client->id << "peer pid" << peerPid << "clients" << clientCount();
        connect(socket, &QLocalSocket::readyRead, this, [this, client] { receive(client); });
        connect(socket, &QLocalSocket::disconnected, this, [this, client] { disconnectClient(client, "peer disconnected"); });
        if (onConnectionChanged) onConnectionChanged(true);
        if (onStateChanged) onStateChanged();
        receive(client);
    }
}
void LocalServer::receive(ClientPtr client)
{
    if (!client->socket || stopRequested_) return;
    client->pending += client->socket->readAll();
    if (client->pending.size() > maximumMessageBytes) {
        qCWarning(lcMcpServer) << "Rejected session" << client->id << "request buffer limit; clients" << clientCount();
        disconnectClient(client, "request buffer limit"); return;
    }
    if (!client->queued && client->pending.contains('\n')) { client->queued = true; ready_.push_back(client); }
    processReady();
}
void LocalServer::processReady()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (processing_ || stopRequested_) return;
    processing_ = true;
    // Round robin across connections, with one global execution gate even when
    // a synchronous tool pumps UI events. Never run another tool reentrantly.
    int handled = 0;
    while (!ready_.empty() && !stopRequested_ && handled++ < 32) {
        const auto client = ready_.front(); ready_.pop_front(); client->queued = false;
        if (!client->socket) continue;
        const auto newline = client->pending.indexOf('\n');
        if (newline < 0) continue;
        const auto line = client->pending.left(newline);
        client->pending.remove(0, newline + 1);
        const bool initialized = client->protocol.initialized();
        const auto response = client->protocol.handleLine(line);
        if (!initialized && client->protocol.initialized())
            qCInfo(lcMcpServer) << "Initialized session" << client->id << "clients" << clientCount();
        if (response && response->contains("error"))
            qCDebug(lcMcpServer) << "Protocol rejection session" << client->id << "code"
                << response->value("error").toObject().value("code").toInt();
        if (response && client->socket) {
            const auto encoded = QJsonDocument(*response).toJson(QJsonDocument::Compact) + '\n';
            if (encoded.size() > 32 * 1024 * 1024 || client->socket->bytesToWrite() > 32 * 1024 * 1024) {
                qCWarning(lcMcpServer) << "Rejected session" << client->id << "response buffer limit; clients" << clientCount();
                disconnectClient(client, "response buffer limit"); continue;
            }
            client->socket->write(encoded);
        }
        if (client->socket && !stopRequested_ && !client->queued && client->pending.contains('\n')) {
            client->queued = true; ready_.push_back(client);
        }
    }
    processing_ = false;
    if (stopRequested_) { finishStop(); return; }
    if (!ready_.empty()) QTimer::singleShot(0, this, [this] { processReady(); });
}

} // namespace imageeditor::mcp
