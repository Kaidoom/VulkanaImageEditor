#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

namespace {
void send(const QJsonObject& object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}
void reply(const QString& id, const QJsonObject& result = {})
{
    send({{"id", id}, {"ok", true}, {"result", result}});
}
void error(const QString& id, const QString& code)
{
    send({{"id", id}, {"ok", false}, {"error", QJsonObject{{"code", code}, {"message", code}}}});
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString workDirectory = app.arguments().value(2);
    QFile scenarioFile(QDir(workDirectory).filePath(QStringLiteral("scenario.txt")));
    const QString scenario = scenarioFile.open(QIODevice::ReadOnly)
        ? QString::fromUtf8(scenarioFile.readAll()).trimmed() : QString();
    QString job;
    QTimer jobTimer;
    jobTimer.setSingleShot(true);
    bool cancelled = false;
    QString jobMethod;
    QObject::connect(&jobTimer, &QTimer::timeout, &app, [&] {
        const QString id = std::exchange(job, {});
        if (id.isEmpty()) return;
        if (cancelled) error(id, QStringLiteral("cancelled"));
        else reply(id, {{"contextGeneration", 1}, {"method", jobMethod}});
        send({{"event", "state"}, {"state", "idle"}});
    });
    auto receive = [&](const QJsonObject& object) {
        const QString id = object.value("id").toString();
        const QString method = object.value("method").toString();
        const QJsonObject params = object.value("params").toObject();
        if (method == QLatin1String("shutdown")) {
            if (scenario == QLatin1String("hang-stop")) return;
            reply(id);
            app.quit();
        } else if (method == QLatin1String("cancel")) {
            cancelled = !job.isEmpty();
            reply(id, {{"requested", cancelled}, {"jobId", job}});
            if (scenario != QLatin1String("hang-cancel") && !job.isEmpty()) jobTimer.start(60);
        } else if (method == QLatin1String("ping") || method == QLatin1String("status")) {
            reply(id, {{"echo", params}});
        } else if (method == QLatin1String("crash")) {
            std::_Exit(23);
        } else if (method == QLatin1String("invalid-json")) {
            std::fputs("not-json\n", stdout); std::fflush(stdout);
        } else if (method == QLatin1String("large-line")) {
            const QByteArray oversized(1024 * 1024 + 1, 'a');
            std::fwrite(oversized.constData(), 1, static_cast<size_t>(oversized.size()), stdout);
            std::fflush(stdout);
        } else if (method == QLatin1String("wrong-id")) {
            reply(QStringLiteral("unknown"));
        } else if (method == QLatin1String("unsafe-output")) {
            reply(id, {{"images", QJsonArray{QJsonObject{{"path", QDir(workDirectory).filePath("../outside.png")}}}}});
        } else if (method == QLatin1String("fatal")) {
            send({{"event", "fatal"}, {"error", QJsonObject{{"code", "backend_failed"}, {"message", "Backend failed"}}}});
        } else if (method == QLatin1String("preview-path")) {
            send({{"event", "preview"}, {"jobId", id}, {"path", params.value("path")}});
            reply(id);
        } else if (method == QLatin1String("stale-preview")) {
            reply(id);
            send({{"event", "preview"}, {"jobId", id}, {"path", QDir(workDirectory).filePath("expired.png")}});
        } else if (method == QLatin1String("preview")) {
            const QString path = QDir(workDirectory).filePath("preview.png");
            QFile image(path);
            if (!image.open(QIODevice::WriteOnly) || image.write("png") != 3) { error(id, "write_failed"); return; }
            image.close();
            send({{"event", "preview"}, {"jobId", id}, {"path", path}, {"width", 1}, {"height", 1}, {"step", 1}});
            reply(id, {{"images", QJsonArray{QJsonObject{{"path", path}}}}});
        } else if (method == QLatin1String("diagnostics")) {
            const QByteArray output(30000, 'D');
            std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stderr);
            std::fflush(stderr);
            reply(id);
        } else if (method == QLatin1String("load") || method == QLatin1String("generate") || method == QLatin1String("unload")) {
            if (!job.isEmpty()) { error(id, QStringLiteral("busy")); return; }
            if (params.value("invalid").toBool()) { error(id, QStringLiteral("invalid_argument")); return; }
            if (params.value("queuedPreviewCancel").toBool()) {
                const QString directory = QDir(workDirectory).filePath("job-" + id);
                if (!QDir().mkpath(directory)) { error(id, "write_failed"); return; }
                const QString path = QDir(directory).filePath("preview.png");
                QFile image(path);
                if (!image.open(QIODevice::WriteOnly) || image.write("png") != 3) { error(id, "write_failed"); return; }
                image.close();
                const QJsonObject preview{{"event", "preview"}, {"jobId", id}, {"path", path}};
                const QJsonObject terminal{{"id", id}, {"ok", false},
                    {"error", QJsonObject{{"code", "cancelled"}, {"message", "Cancelled"}}}};
                const QByteArray batch = QJsonDocument(preview).toJson(QJsonDocument::Compact) + '\n'
                    + QJsonDocument(terminal).toJson(QJsonDocument::Compact) + '\n';
                // Both events are queued before cancellation removes transient images.
                if (!QDir(directory).removeRecursively()) { error(id, "remove_failed"); return; }
                std::fwrite(batch.constData(), 1, static_cast<size_t>(batch.size()), stdout);
                std::fflush(stdout);
                return;
            }
            job = id;
            jobMethod = method;
            cancelled = false;
            send({{"event", "state"}, {"state", "generating"}, {"jobId", id}});
            if (scenario != QLatin1String("hang-cancel") && scenario != QLatin1String("hang-stop"))
                jobTimer.start(params.value("delay").toInt(100));
        } else error(id, QStringLiteral("unknown_method"));
    };
    if (scenario != QLatin1String("silent")) {
        if (scenario == QLatin1String("bad-handshake")) send({{"event", "hello"}, {"protocol", 2}});
        else if (scenario == QLatin1String("fatal-handshake")) send({{"event", "fatal"},
            {"error", QJsonObject{{"code", "initialization_failed"}, {"message", "Cannot initialize backend"}}}});
        else send({{"event", "hello"}, {"protocol", 1}, {"engineCommit", "test"},
            {"backend", "test"}, {"devices", QJsonArray{}}, {"profiles", QJsonArray{}}});
    }
    std::thread([&app, receive] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
            QMetaObject::invokeMethod(&app, [receive, request] { receive(request); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    }).detach();
    const int status = app.exec();
    // The test process intentionally has a blocking stdin reader; the OS reclaims it.
    std::_Exit(status);
}
