#include <imageeditor/diffusion/WorkerClient.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace imageeditor::diffusion {
namespace {
constexpr qsizetype MaxLineBytes = 1024 * 1024;
bool isJob(const QString& method)
{
    return method == QLatin1String("load") || method == QLatin1String("generate")
        || method == QLatin1String("unload");
}
}

WorkerClient::WorkerClient(QObject* parent) : QObject(parent)
{
    m_handshakeTimer.setSingleShot(true);
    m_cancelTimer.setSingleShot(true);
    m_stopTimer.setSingleShot(true);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &WorkerClient::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        // Drain diagnostics separately; never accumulate an unbounded log in memory.
        m_process.setReadChannel(QProcess::StandardError);
        while (m_process.bytesAvailable() > 0)
            emit diagnostic(QString::fromUtf8(m_process.read(8192)));
        m_process.setReadChannel(QProcess::StandardOutput);
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, &WorkerClient::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("worker_start_failed"), m_process.errorString());
        else if (error == QProcess::ReadError || error == QProcess::WriteError)
            fail(QStringLiteral("worker_io_error"), m_process.errorString());
    });
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("handshake_timeout"), QStringLiteral("The diffusion worker did not complete its protocol handshake."));
    });
    connect(&m_cancelTimer, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("worker_terminated"), QStringLiteral("The worker did not finish cancellation in time and was terminated. Restart it and reload the model before generating again."));
    });
    connect(&m_stopTimer, &QTimer::timeout, this, [this] {
        // A blocked native kernel or model constructor cannot always be interrupted.
        failPending(QStringLiteral("worker_terminated"), QStringLiteral("The worker was terminated while stopping; pending work did not complete."));
        m_process.kill();
    });
}

WorkerClient::~WorkerClient()
{
    disconnect(&m_process, nullptr, this, nullptr);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void WorkerClient::setTimeouts(int handshakeMs, int cancellationGraceMs, int stopGraceMs)
{
    m_handshakeMs = std::max(1, handshakeMs);
    m_cancelMs = std::max(1, cancellationGraceMs);
    m_stopMs = std::max(1, stopGraceMs);
}

bool WorkerClient::start(const QString& executable, const QString& workDirectory, QString* error)
{
    auto reject = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (m_process.state() != QProcess::NotRunning || m_state == State::Starting || m_processingExit)
        return reject(QStringLiteral("The diffusion worker is already running or still stopping."));
    const QFileInfo executableInfo(executable);
    if (!executableInfo.isAbsolute() || !executableInfo.isFile() || !executableInfo.isExecutable())
        return reject(QStringLiteral("Choose an existing executable using its absolute path."));
    if (!QFileInfo(workDirectory).isAbsolute())
        return reject(QStringLiteral("The diffusion work directory must be an absolute path."));
    const bool exists = QFileInfo::exists(workDirectory);
    if (!QDir().mkpath(workDirectory))
        return reject(QStringLiteral("Cannot create the diffusion work directory."));
    if (!exists && !QFile::setPermissions(workDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return reject(QStringLiteral("Cannot restrict access to the diffusion work directory."));
    m_workDirectory = QFileInfo(workDirectory).canonicalFilePath();
    if (m_workDirectory.isEmpty() || m_workDirectory == QDir::rootPath())
        return reject(QStringLiteral("Choose a dedicated diffusion work directory."));
    m_output.clear();
    m_hello = {};
    m_pending.clear();
    m_failed = false;
    m_stopping = false;
    m_handshakeTimer.stop();
    m_cancelTimer.stop();
    m_stopTimer.stop();
    setState(State::Starting);
    m_process.setProgram(executableInfo.absoluteFilePath());
    m_process.setArguments({QStringLiteral("--work-dir"), m_workDirectory});
    m_process.start();
    m_handshakeTimer.start(m_handshakeMs);
    return true;
}

void WorkerClient::setState(State state)
{
    if (state == m_state) return;
    m_state = state;
    emit stateChanged(state);
}

QString WorkerClient::send(const QString& method, const QJsonObject& params, bool internal)
{
    // Outputs outlive the process and client object; IDs must not reuse job directories.
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("protocol"), 1},
        {QStringLiteral("id"), id}, {QStringLiteral("method"), method},
        {QStringLiteral("params"), params}}).toJson(QJsonDocument::Compact) + '\n';
    if (data.size() > MaxLineBytes) {
        emit errorOccurred(QStringLiteral("request_too_large"), QStringLiteral("The diffusion request exceeds the protocol size limit."));
        return {};
    }
    m_pending.insert(id, {method, internal});
    if (m_process.write(data) != data.size()) {
        fail(QStringLiteral("worker_io_error"), QStringLiteral("Cannot send a request to the diffusion worker."));
        return {};
    }
    updateActivity();
    return id;
}

QString WorkerClient::request(const QString& method, const QJsonObject& params)
{
    if (method == QLatin1String("cancel")) return cancel();
    if (method == QLatin1String("shutdown")) {
        emit errorOccurred(QStringLiteral("invalid_method"), QStringLiteral("Use stop() to shut down the worker."));
        return {};
    }
    if (m_state != State::Idle && m_state != State::Busy && m_state != State::Cancelling) {
        emit errorOccurred(QStringLiteral("worker_not_ready"), QStringLiteral("Start the diffusion worker and wait for its ready signal before sending requests."));
        return {};
    }
    if (m_state == State::Cancelling && method != QLatin1String("status") && method != QLatin1String("ping")) {
        emit errorOccurred(QStringLiteral("cancelling"), QStringLiteral("Wait for cancellation to finish before starting another job."));
        return {};
    }
    return send(method, params, false);
}

QString WorkerClient::cancel()
{
    if (m_state != State::Idle && m_state != State::Busy && m_state != State::Cancelling) {
        emit errorOccurred(QStringLiteral("worker_not_ready"), QStringLiteral("The diffusion worker is not accepting cancellation requests."));
        return {};
    }
    const bool pendingJob = hasJob();
    const QString id = send(QStringLiteral("cancel"), {}, false);
    if (!id.isEmpty() && pendingJob) {
        setState(State::Cancelling);
        if (!m_cancelTimer.isActive()) m_cancelTimer.start(m_cancelMs);
    }
    return id;
}

void WorkerClient::stop()
{
    if (m_stopping) return;
    m_handshakeTimer.stop();
    m_cancelTimer.stop();
    if (m_process.state() == QProcess::NotRunning) {
        failPending(QStringLiteral("worker_stopped"), QStringLiteral("The diffusion worker has stopped."));
        setState(State::Stopped);
        return;
    }
    m_stopping = true;
    setState(State::Stopping);
    if (!m_hello.isEmpty() && !m_failed) send(QStringLiteral("shutdown"), {}, true);
    else m_process.terminate();
    m_stopTimer.start(m_stopMs);
}

void WorkerClient::readOutput()
{
    if (m_failed) {
        m_process.readAllStandardOutput();
        return;
    }
    m_process.setReadChannel(QProcess::StandardOutput);
    while (m_process.bytesAvailable() > 0) {
        m_output += m_process.read(std::min<qint64>(8192, m_process.bytesAvailable()));
        qsizetype newline;
        while ((newline = m_output.indexOf('\n')) >= 0) {
            if (newline > MaxLineBytes) {
                fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker exceeded the protocol line limit."));
                return;
            }
            const QByteArray line = m_output.left(newline);
            m_output.remove(0, newline + 1);
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker returned invalid JSON on its protocol channel."));
                return;
            }
            receive(document.object());
            if (m_failed) return;
        }
        if (m_output.size() > MaxLineBytes) {
            fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker exceeded the protocol line limit."));
            return;
        }
    }
}

bool WorkerClient::validOutputPath(const QString& path, bool allowMissing) const
{
    auto contained = [this](const QString& candidate) {
        if (candidate.isEmpty()) return false;
        const QString relative = QDir(m_workDirectory).relativeFilePath(candidate);
        return !QDir::isAbsolutePath(relative) && relative != QLatin1String("..")
            && !relative.startsWith(QLatin1String("../"));
    };
    const QFileInfo file(path);
    if (!file.isAbsolute() || !contained(QDir::cleanPath(path))) return false;
    if (file.exists()) {
        if (!file.isFile()) return false;
        const QString canonical = file.canonicalFilePath();
        if (!canonical.isEmpty()) return contained(canonical);
        if (QFileInfo::exists(path)) return false;
    }
    if (!allowMissing || QFileInfo(path).isSymLink()) return false;
    // Cancellation may already have removed the job directory. Check the nearest
    // surviving ancestor so a missing file below an escaping symlink is still rejected.
    QFileInfo ancestor(file.absolutePath());
    for (;;) {
        const QString canonical = ancestor.canonicalFilePath();
        if (!canonical.isEmpty()) return ancestor.isDir() && contained(canonical);
        if (ancestor.isSymLink() || QFileInfo::exists(ancestor.absoluteFilePath())) return false;
        const QString parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath()) return false;
        ancestor.setFile(parent);
    }
}

bool WorkerClient::validOutputFiles(const QJsonObject& object) const
{
    if (!object.contains(QStringLiteral("images"))) return true;
    if (!object.value(QStringLiteral("images")).isArray()) return false;
    for (const auto& item : object.value(QStringLiteral("images")).toArray()) {
        if (!item.isObject() || !item.toObject().value(QStringLiteral("path")).isString()
            || !validOutputPath(item.toObject().value(QStringLiteral("path")).toString())) return false;
    }
    return true;
}

void WorkerClient::receive(const QJsonObject& object)
{
    if (object.contains(QStringLiteral("event"))) {
        const QString event = object.value(QStringLiteral("event")).toString();
        if (event == QLatin1String("fatal")) {
            const QJsonObject error = object.value(QStringLiteral("error")).toObject();
            const QString code = error.value(QStringLiteral("code")).toString();
            const QString message = error.value(QStringLiteral("message")).toString();
            if (code.isEmpty() || message.isEmpty())
                fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker returned a malformed fatal error."));
            else
                fail(code, message + QStringLiteral(" Restart the worker and reload the model before continuing."));
            return;
        }
        if (event == QLatin1String("hello")) {
            if (!m_hello.isEmpty() || object.value(QStringLiteral("protocol")).toInt(-1) != 1) {
                fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker uses an unsupported or repeated protocol handshake."));
                return;
            }
            m_hello = object;
            m_handshakeTimer.stop();
            if (!m_stopping) {
                updateActivity();
                emit ready(object);
            }
            return;
        }
        if (m_hello.isEmpty() || event.isEmpty()) {
            fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker sent an event before its handshake."));
            return;
        }
        if (event == QLatin1String("preview")) {
            const QString jobId = object.value(QStringLiteral("jobId")).toString();
            if (!m_pending.contains(jobId)) return;
            const QString path = object.value(QStringLiteral("path")).toString();
            if (!validOutputPath(path, true)) {
                fail(QStringLiteral("unsafe_output_path"), QStringLiteral("The diffusion worker returned a preview outside its work directory or an invalid preview path."));
                return;
            }
            // Previews are transient. A queued event can arrive after cancellation
            // removed its image, before the cancelled terminal response is consumed.
            if (!QFileInfo(path).isFile()) return;
        }
        emit eventReceived(object);
        return;
    }
    const QString id = object.value(QStringLiteral("id")).toString();
    if (m_hello.isEmpty() || !m_pending.contains(id) || !object.value(QStringLiteral("ok")).isBool()) {
        fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker returned an unknown or malformed response."));
        return;
    }
    const bool ok = object.value(QStringLiteral("ok")).toBool();
    if ((ok && !object.value(QStringLiteral("result")).isObject())
        || (!ok && (!object.value(QStringLiteral("error")).isObject()
            || !object.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).isString()
            || !object.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).isString()))) {
        fail(QStringLiteral("protocol_error"), QStringLiteral("The diffusion worker returned an invalid result or error."));
        return;
    }
    const QJsonObject result = object.value(QStringLiteral("result")).toObject();
    if (ok && !validOutputFiles(result)) {
        fail(QStringLiteral("unsafe_output_path"), QStringLiteral("The diffusion worker returned an image outside its work directory or a missing image file."));
        return;
    }
    const Pending pending = m_pending.take(id);
    updateActivity();
    if (pending.internal) return;
    if (ok) emit requestFinished(id, result);
    else {
        const QJsonObject error = object.value(QStringLiteral("error")).toObject();
        emit requestFailed(id, error.value(QStringLiteral("code")).toString(), error.value(QStringLiteral("message")).toString());
    }
}

bool WorkerClient::hasJob() const
{
    for (const Pending& pending : m_pending)
        if (isJob(pending.method)) return true;
    return false;
}

void WorkerClient::updateActivity()
{
    if (m_stopping || m_failed || m_hello.isEmpty()) return;
    if (m_state == State::Cancelling && hasJob()) return;
    if (!hasJob()) m_cancelTimer.stop();
    setState(hasJob() ? State::Busy : State::Idle);
}

void WorkerClient::failPending(const QString& code, const QString& message)
{
    const auto pending = std::exchange(m_pending, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        if (!it.value().internal) emit requestFailed(it.key(), code, message);
}

void WorkerClient::fail(const QString& code, const QString& message)
{
    if (m_failed) return;
    m_failed = true;
    m_handshakeTimer.stop();
    m_cancelTimer.stop();
    m_stopTimer.stop();
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
    setState(State::Error);
    failPending(code, message);
    emit errorOccurred(code, message);
}

void WorkerClient::processFinished(int code, QProcess::ExitStatus status)
{
    m_processingExit = true;
    readOutput();
    m_handshakeTimer.stop();
    m_cancelTimer.stop();
    m_stopTimer.stop();
    if (m_stopping) {
        failPending(QStringLiteral("worker_stopped"), QStringLiteral("The diffusion worker stopped before this request completed."));
        m_stopping = false;
        setState(State::Stopped);
    } else if (!m_failed) {
        fail(QStringLiteral("worker_exit"), QStringLiteral("The diffusion worker exited unexpectedly (code %1, %2). Restart it and reload the model.")
            .arg(code).arg(status == QProcess::CrashExit ? QStringLiteral("crashed") : QStringLiteral("exited")));
    }
    m_output.clear();
    m_hello = {};
    m_processingExit = false;
}

} // namespace imageeditor::diffusion
