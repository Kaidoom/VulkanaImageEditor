#include "imageeditor/platform/SingleInstance.hpp"

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#ifdef Q_OS_WIN
#include <QCryptographicHash>
#include <QScopeGuard>
#include <windows.h>
#include <sddl.h>
#include <vector>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace imageeditor::platform {
namespace {
constexpr qsizetype maxMessage = 65536;
constexpr qsizetype maxFiles = 64;
constexpr std::size_t maxPending = 128;
}
struct SingleInstance::Impl {
    SingleInstance& owner;
    QString directory, endpoint, error;
    QLocalServer server;
#ifdef Q_OS_WIN
    HANDLE mutex {};
    bool ownsMutex {false};
#else
    int lockFd {-1};
#endif
    std::deque<QStringList> pending;
    std::function<void(const QStringList&)> handler;

    Impl(SingleInstance& object, QString path) : owner(object), directory(std::move(path)) {}
    ~Impl()
    {
        // Only the elected owner may unlink the socket. Never unlink the lock:
        // contenders may already have opened that inode.
        server.close();
#ifdef Q_OS_WIN
        if (ownsMutex) ReleaseMutex(mutex);
        if (mutex) CloseHandle(mutex);
#else
        if (lockFd >= 0) ::close(lockFd); // kernel also releases it after a crash
#endif
    }
    void drain()
    {
        while (handler && !pending.empty()) {
            auto files = std::move(pending.front()); pending.pop_front();
            handler(files);
        }
    }
    void acceptConnections()
    {
        while (auto* socket = server.nextPendingConnection()) {
            socket->setParent(&owner);
            socket->setReadBufferSize(maxMessage + 1);
            auto* deadline = new QTimer(socket);
            deadline->setSingleShot(true);
            QObject::connect(deadline, &QTimer::timeout, socket, [socket] { socket->abort(); socket->deleteLater(); });
            deadline->start(3000);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            auto buffer = std::make_shared<QByteArray>();
            const auto read = [this, socket, deadline, buffer] {
                buffer->append(socket->readAll());
                if (buffer->size() > maxMessage) { socket->abort(); return; }
                if (!buffer->contains('\n')) return;
                deadline->stop();
                const auto object = QJsonDocument::fromJson(*buffer).object();
                const auto values = object.value(QStringLiteral("files"));
                bool valid = object.value(QStringLiteral("protocol")).toInt() == 1 && values.isArray()
                    && values.toArray().size() <= maxFiles && pending.size() < maxPending;
                QStringList files;
                for (const auto& value : values.toArray()) {
                    const auto path = value.toString();
                    valid &= value.isString() && !path.contains(QChar::Null) && QFileInfo(path).isAbsolute();
                    files.append(path);
                }
                // One request per connection; no commands or shell evaluation.
                QObject::disconnect(socket, &QLocalSocket::readyRead, socket, nullptr);
                if (valid) pending.push_back(std::move(files));
                socket->write(valid ? "OK\n" : "ERROR\n");
                socket->disconnectFromServer();
                if (valid) QTimer::singleShot(0, &owner, [this] { drain(); });
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, read);
            read();
        }
    }
};

SingleInstance::SingleInstance(QString directory, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, std::move(directory)))
{
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        // Don't acknowledge new opens into a dying UI. Keep the lock through
        // window/device teardown; subsequent launches retry until it releases.
        impl_->server.close();
        impl_->handler = {};
    });
}
SingleInstance::~SingleInstance() = default;
QString SingleInstance::errorString() const { return impl_->error; }
void SingleInstance::setOpenHandler(std::function<void(const QStringList&)> handler)
{
    impl_->handler = std::move(handler);
    QTimer::singleShot(0, this, [this] { impl_->drain(); });
}
SingleInstance::Result SingleInstance::start(const QStringList& files, bool waitForPreviousExit, int timeoutMs)
{
    auto& state = *impl_;
    const auto fail = [&](QString reason) {
        state.error = std::move(reason);
        return Result::Error;
    };
#ifdef Q_OS_WIN
    if (state.mutex) return fail(tr("Instance coordination was already started."));
#else
    if (state.lockFd >= 0) return fail(tr("Instance coordination was already started."));
#endif
    QJsonArray paths;
    for (const auto& file : files) paths.append(QFileInfo(file).absoluteFilePath());
    auto message = QJsonDocument(QJsonObject{{QStringLiteral("protocol"), 1},
        {QStringLiteral("files"), paths}}).toJson(QJsonDocument::Compact) + '\n';
    if (paths.size() > maxFiles || message.size() > maxMessage)
        return fail(tr("Too many files in one launch. Open them in smaller batches."));

#ifdef Q_OS_WIN
    // Named kernel ownership survives executable relocation and is released on
    // process death. Scope IPC to the current user and interactive logon session;
    // no writable shared-directory lock, PID heuristic or portable-folder state.
    HANDLE token {};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return fail(QStringLiteral("Cannot query the Windows user token."));
    const auto tokenGuard = qScopeGuard([&] { CloseHandle(token); });
    DWORD size {};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size) return fail(QStringLiteral("Cannot query the Windows user identity."));
    std::vector<unsigned char> tokenData(size);
    if (!GetTokenInformation(token, TokenUser, tokenData.data(), size, &size))
        return fail(QStringLiteral("Cannot read the Windows user identity."));
    LPWSTR sid {};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenData.data())->User.Sid, &sid))
        return fail(QStringLiteral("Cannot encode the Windows user identity."));
    const auto sidGuard = qScopeGuard([&] { LocalFree(sid); });
    const auto identity = QString::fromWCharArray(sid);
    DWORD session {};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return fail(QStringLiteral("Cannot determine the Windows logon session."));
    const auto scope = state.directory.isEmpty() ? QStringLiteral("default")
        : QDir::cleanPath(QFileInfo(state.directory).absoluteFilePath()).toCaseFolded();
    const auto key = QStringLiteral("vulkana-editor-v1-") + QString::fromLatin1(QCryptographicHash::hash(
        (identity + QLatin1Char('|') + QString::number(session) + QLatin1Char('|') + scope).toUtf8(),
        QCryptographicHash::Sha256).toHex());
    state.endpoint = key;
    const auto mutexName = (QStringLiteral("Local\\") + key).toStdWString();
    const auto descriptor = (QStringLiteral("D:P(A;;GA;;;") + identity + QLatin1Char(')')).toStdWString();
    PSECURITY_DESCRIPTOR security {};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr))
        return fail(QStringLiteral("Cannot create private instance-lock permissions."));
    const auto securityGuard = qScopeGuard([&] { LocalFree(security); });
    SECURITY_ATTRIBUTES attributes {sizeof(SECURITY_ATTRIBUTES), security, FALSE};
    state.mutex = CreateMutexW(&attributes, FALSE, mutexName.c_str());
    if (!state.mutex) return fail(QStringLiteral("Cannot open the private Windows instance lock (%1).").arg(GetLastError()));
#else
    if (state.directory.isEmpty()) {
        const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtime.isEmpty()) return fail(tr("No writable user runtime directory is available."));
        state.directory = QDir(runtime).filePath(QStringLiteral("vulkana-editor"));
    }
    const auto directory = QFile::encodeName(state.directory);
    if (::mkdir(directory.constData(), 0700) != 0 && errno != EEXIST)
        return fail(tr("Failed to create Vulkana's runtime directory. Check permissions."));
    struct stat info {};
    if (::lstat(directory.constData(), &info) != 0 || !S_ISDIR(info.st_mode)
        || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
        return fail(tr("Vulkana's runtime directory must be private and owned by this user."));
    const auto lockPath = QFile::encodeName(QDir(state.directory).filePath(QStringLiteral("instance.lock")));
    state.lockFd = ::open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (state.lockFd < 0 || ::fstat(state.lockFd, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0 || info.st_nlink != 1)
        return fail(tr("Failed to open Vulkana's instance lock. Check runtime-directory permissions."));
    state.endpoint = QDir(state.directory).filePath(QStringLiteral("instance-v1.sock"));
#endif
    QElapsedTimer timer; timer.start();
    const auto remaining = [&] { return std::max(0, timeoutMs - int(timer.elapsed())); };
    do {
#ifdef Q_OS_WIN
        const auto acquisition = WaitForSingleObject(state.mutex, 0);
        const bool acquired = acquisition == WAIT_OBJECT_0 || acquisition == WAIT_ABANDONED;
        state.ownsMutex = acquired;
#else
        const bool acquired = ::flock(state.lockFd, LOCK_EX | LOCK_NB) == 0;
#endif
        if (acquired) {
            // The lock (not socket existence or a PID/process name) arbitrates
            // ownership. Only now is cleanup of a crashed primary safe.
            QLocalServer::removeServer(state.endpoint);
            state.server.setSocketOptions(QLocalServer::UserAccessOption);
            state.server.setMaxPendingConnections(16);
            if (!state.server.listen(state.endpoint))
                return fail(tr("Failed to listen for document requests: %1").arg(state.server.errorString()));
            connect(&state.server, &QLocalServer::newConnection, this, [&state] { state.acceptConnections(); });
            return Result::Primary;
        }
#ifdef Q_OS_WIN
        if (acquisition != WAIT_TIMEOUT)
            return fail(QStringLiteral("Cannot acquire the Windows instance lock (%1).").arg(GetLastError()));
#else
        if (errno != EWOULDBLOCK && errno != EAGAIN)
            return fail(tr("Failed to acquire Vulkana's instance lock: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
#endif
        if (!waitForPreviousExit) {
            QLocalSocket socket;
            socket.connectToServer(state.endpoint);
            if (socket.waitForConnected(std::min(200, remaining()))) {
                socket.write(message);
                QByteArray response;
                while (remaining() > 0 && !response.contains('\n')
                    && socket.state() == QLocalSocket::ConnectedState) {
                    socket.waitForReadyRead(remaining());
                    response.append(socket.readAll());
                    if (response.size() > 32) break;
                }
                if (response == "OK\n") return Result::Forwarded;
                // Don't retry an ambiguously delivered request (duplicate tabs).
                return fail(tr("Vulkana is already running but could not accept the request. Switch to its window and try again."));
            }
        }
        QThread::msleep(static_cast<unsigned long>(std::min(50, remaining())));
    } while (remaining() > 0);
    return fail(waitForPreviousExit
        ? tr("The previous Vulkana instance has not finished closing. Please reopen Vulkana once it exits.")
        : tr("Vulkana is already running but is not responding. Switch to its window and try again."));
}
} // namespace imageeditor::platform
