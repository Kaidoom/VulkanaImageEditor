#include <imageeditor/diffusion/WorkerClient.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <stdexcept>

using imageeditor::diffusion::WorkerClient;

namespace {
void require(bool condition, const QString& message)
{
    if (!condition) throw std::runtime_error(message.toStdString());
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read configuration: %1").arg(path));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
        QStringLiteral("Configuration must contain a JSON object: %1").arg(path));
    return document.object();
}

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Cannot read output: %1").arg(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), QStringLiteral("Cannot hash output: %1").arg(path));
    return hash.result().toHex();
}

struct Response {
    bool ok = false;
    QJsonObject result;
    QString code;
    QString message;
};
struct Activity {
    int progress = 0;
    int previews = 0;
};

class Smoke {
public:
    WorkerClient client;
    QHash<QString, Response> responses;
    QHash<QString, Activity> activity;
    QString fatal;
    QString cancelJob;
    QString cancelRequest;
    QString cancelPing;
    QJsonArray records;
    qint64 ticks = 0;
    qint64 largestGap = 0;
    QElapsedTimer clock;
    qint64 lastTick = 0;
    QTimer heartbeat;

    Smoke()
    {
        clock.start();
        heartbeat.setInterval(10);
        QObject::connect(&heartbeat, &QTimer::timeout, &client, [this] {
            const qint64 now = clock.elapsed();
            largestGap = qMax(largestGap, now - lastTick);
            lastTick = now;
            ++ticks;
        });
        heartbeat.start();
        client.setTimeouts(30000, 15000, 15000);
        QObject::connect(&client, &WorkerClient::requestFinished, &client, [this](const QString& id, const QJsonObject& result) {
            responses.insert(id, {true, result, {}, {}});
        });
        QObject::connect(&client, &WorkerClient::requestFailed, &client,
            [this](const QString& id, const QString& code, const QString& message) {
                responses.insert(id, {false, {}, code, message});
            });
        QObject::connect(&client, &WorkerClient::errorOccurred, &client,
            [this](const QString& code, const QString& message) { fatal = code + ": " + message; });
        QObject::connect(&client, &WorkerClient::diagnostic, &client, [](const QString& text) {
            const QByteArray bytes = text.toUtf8();
            std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
            std::fflush(stderr);
        });
        QObject::connect(&client, &WorkerClient::eventReceived, &client, [this](const QJsonObject& event) {
            const QString kind = event.value("event").toString();
            const QString id = event.value("jobId").toString();
            if (kind == QLatin1String("progress")) {
                ++activity[id].progress;
                if (id == cancelJob && cancelRequest.isEmpty()) {
                    cancelPing = client.request(QStringLiteral("status"));
                    cancelRequest = client.cancel();
                }
            } else if (kind == QLatin1String("preview")) {
                ++activity[id].previews;
                if (!QFileInfo(event.value("path").toString()).isFile())
                    fatal = QStringLiteral("A reported preview file is missing.");
            }
        });
    }

    void waitFor(const std::function<bool()>& done, int timeoutMs = 300000)
    {
        if (done()) return;
        QEventLoop loop;
        QTimer poll;
        QTimer deadline;
        deadline.setSingleShot(true);
        poll.setInterval(5);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (done() || !fatal.isEmpty()) loop.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        poll.start();
        deadline.start(timeoutMs);
        loop.exec();
        require(fatal.isEmpty(), fatal);
        require(done(), QStringLiteral("Timed out waiting for the worker after %1 ms.").arg(timeoutMs));
    }

    Response take(const QString& id)
    {
        require(!id.isEmpty(), QStringLiteral("The supervisor did not accept the request: %1").arg(fatal));
        waitFor([&] { return responses.contains(id); });
        return responses.take(id);
    }

    QJsonObject success(const QString& id)
    {
        const Response response = take(id);
        require(response.ok, response.code + ": " + response.message);
        return response.result;
    }

    void start(const QString& executable, const QString& directory)
    {
        QString error;
        require(client.start(executable, directory, &error), error);
        waitFor([&] { return client.state() == WorkerClient::State::Idle; }, 30000);
        require(client.hello().value("protocol").toInt() == 1, QStringLiteral("Invalid worker protocol."));
    }

    QJsonObject generate(const QString& label, const QJsonObject& parameters, qint64 generation)
    {
        const qint64 previousTicks = ticks;
        QElapsedTimer elapsed;
        elapsed.start();
        const QString id = client.request(QStringLiteral("generate"), parameters);
        const QJsonObject result = success(id);
        require(result.value("contextGeneration").toInteger() == generation, QStringLiteral("A warm generation changed the model context."));
        require(ticks > previousTicks, QStringLiteral("The host event loop did not run during generation."));
        require(activity[id].progress > 0, QStringLiteral("Generation returned no progress events."));
        require(activity[id].previews > 0, QStringLiteral("Generation returned no preview events."));
        const QJsonArray images = result.value("images").toArray();
        require(!images.isEmpty(), QStringLiteral("Generation returned no images."));
        QJsonArray checked;
        for (const auto& value : images) {
            const QJsonObject image = value.toObject();
            const QString path = image.value("path").toString();
            require(QFileInfo(path).isFile() && QFileInfo(path).size() > 8, QStringLiteral("Generated image is missing or empty: %1").arg(path));
            QFile file(path);
            require(file.open(QIODevice::ReadOnly) && file.read(8) == QByteArray::fromHex("89504e470d0a1a0a"),
                QStringLiteral("Generated output is not a PNG: %1").arg(path));
            checked.append(QJsonObject{{"path", path}, {"sha256", QString::fromLatin1(fileHash(path))}});
        }
        records.append(QJsonObject{{"stage", label}, {"requestId", id}, {"wallMs", elapsed.elapsed()},
            {"heartbeatTicks", ticks - previousTicks}, {"progressEvents", activity[id].progress},
            {"previewEvents", activity[id].previews}, {"contextGeneration", generation},
            {"timings", result.value("timings")}, {"images", checked}});
        return result;
    }

    void stop()
    {
        client.stop();
        waitFor([&] { return client.state() == WorkerClient::State::Stopped; }, 30000);
        require(!client.isRunning(), QStringLiteral("Worker is still running after Stop."));
        require(client.pendingRequestCount() == 0, QStringLiteral("Requests remain pending after Stop."));
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 5) {
        std::fputs("Usage: diffusion-client-smoke WORKER_EXECUTABLE PRIVATE_WORK_DIRECTORY LOAD_JSON GENERATE_JSON\n", stderr);
        return 2;
    }
    Smoke smoke;
    bool passed = false;
    QString failure;
    try {
        const QString executable = app.arguments().at(1);
        const QString directory = app.arguments().at(2);
        const QJsonObject load = readObject(app.arguments().at(3));
        QJsonObject generate = readObject(app.arguments().at(4));
        // Progress previews are part of this optional integration check.
        generate.insert(QStringLiteral("preview"), true);
        generate.insert(QStringLiteral("previewInterval"), 1);
        smoke.start(executable, directory);
        const qint64 pid = smoke.client.processId();
        QElapsedTimer loadTime;
        loadTime.start();
        const QJsonObject loaded = smoke.success(smoke.client.request(QStringLiteral("load"), load));
        const qint64 generation = loaded.value("contextGeneration").toInteger();
        require(generation > 0, QStringLiteral("Load returned no context generation."));
        smoke.records.append(QJsonObject{{"stage", "load"}, {"wallMs", loadTime.elapsed()},
            {"contextGeneration", generation}, {"result", loaded}});
        const QJsonObject first = smoke.generate(QStringLiteral("first"), generate, generation);
        const QString firstPath = first.value("images").toArray().first().toObject().value("path").toString();
        const QByteArray firstHash = fileHash(firstPath);
        smoke.generate(QStringLiteral("warm"), generate, generation);
        require(fileHash(firstPath) == firstHash, QStringLiteral("A later generation modified the previous output."));
        require(smoke.client.processId() == pid, QStringLiteral("Warm generation restarted the worker."));

        QElapsedTimer cancelTime;
        cancelTime.start();
        smoke.cancelJob = smoke.client.request(QStringLiteral("generate"), generate);
        const Response cancelled = smoke.take(smoke.cancelJob);
        require(!cancelled.ok && cancelled.code == QLatin1String("cancelled"),
            QStringLiteral("Generation did not finish with a cancelled terminal response: %1 %2").arg(cancelled.code, cancelled.message));
        require(!smoke.cancelRequest.isEmpty(), QStringLiteral("Cancellation was not sent from the progress callback."));
        const QJsonObject cancellationAck = smoke.success(smoke.cancelRequest);
        require(cancellationAck.value("requested").toBool(), QStringLiteral("The worker did not acknowledge an active cancellation."));
        const QJsonObject ping = smoke.success(smoke.cancelPing);
        require(ping.value("contextGeneration").toInteger() == generation, QStringLiteral("Status during cancellation returned the wrong context."));
        const QString concurrentState = ping.value("state").toString();
        require(concurrentState == QLatin1String("generating") || concurrentState == QLatin1String("cancelling"),
            QStringLiteral("Status was not answered while generation was still in progress."));
        smoke.records.append(QJsonObject{{"stage", "cancel"}, {"wallMs", cancelTime.elapsed()},
            {"requestId", smoke.cancelJob}, {"progressEvents", smoke.activity[smoke.cancelJob].progress},
            {"terminalCode", cancelled.code}, {"ack", cancellationAck}, {"concurrentStatus", ping}});
        smoke.cancelJob.clear();
        smoke.generate(QStringLiteral("recovery"), generate, generation);
        require(smoke.client.processId() == pid, QStringLiteral("Cooperative cancellation restarted the worker."));
        require(fileHash(firstPath) == firstHash, QStringLiteral("Cancellation or recovery modified a prior output."));
        require(smoke.success(smoke.client.request(QStringLiteral("unload"))).value("unloaded").toBool(),
            QStringLiteral("Unload did not complete."));
        smoke.stop();
        smoke.start(executable, directory);
        const QJsonObject restarted = smoke.success(smoke.client.request(QStringLiteral("ping")));
        require(!restarted.value("loaded").toBool(), QStringLiteral("A restarted worker unexpectedly retained a context."));
        smoke.records.append(QJsonObject{{"stage", "restart"}, {"result", restarted}});
        smoke.stop();
        passed = true;
    } catch (const std::exception& error) {
        failure = QString::fromUtf8(error.what());
    }
    const QJsonObject report{{"passed", passed}, {"error", failure}, {"stages", smoke.records},
        {"heartbeatTicks", smoke.ticks}, {"largestHeartbeatGapMs", smoke.largestGap},
        {"totalMs", smoke.clock.elapsed()}};
    const QByteArray output = QJsonDocument(report).toJson(QJsonDocument::Indented);
    std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
    std::fflush(stdout);
    return passed ? 0 : 1;
}
