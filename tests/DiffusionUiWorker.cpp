// Deterministic process fixture for editor transaction tests; never used by the app.
#include <imageeditor/diffusion/ModelProfiles.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <cstdio>
#include <unistd.h>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 || args[1] != "--work-dir")
        return 2;
    const auto root = args[2];
    const auto send = [](QJsonObject value) {
        auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
        bytes += '\n';
        std::fwrite(bytes.data(), 1, std::size_t(bytes.size()), stdout);
        std::fflush(stdout);
    };
    const auto ok = [&](const QString& id, QJsonObject result) {
        send({ { "id", id }, { "ok", true }, { "result", result } });
    };
    const auto fail = [&](const QString& id, const QString& code, const QString& message) {
        send({ { "id", id }, { "ok", false },
            { "error", QJsonObject { { "code", code }, { "message", message } } } });
    };
    send({ { "event", "hello" }, { "protocol", 1 }, { "backend", "test" },
        { "referenceSizeLimit", true },
        { "loraParameters", true },
        { "flux3NativeRegions", !qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_SPATIAL_V1") },
        { "spatialVersions", qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_SPATIAL_V1")
              ? QJsonArray { 1 } : QJsonArray { 1, 2, 3 } },
        { "profiles", imageeditor::diffusion::modelProfiles() },
        { "devices", QJsonArray { QJsonObject { { "id", "CPU" }, { "name", "Test CPU" } } } },
        { "samplers", QJsonArray { "euler" } }, { "schedulers", QJsonArray { "discrete" } } });
    QByteArray input;
    QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
    QTimer job;
    job.setSingleShot(true);
    QString active;
    QJsonObject params;
    int serial = 0;
    QJsonArray loadedModels;
    const auto status = [&] {
        return QJsonObject { { "loaded", !loadedModels.isEmpty() },
            { "contextGeneration", serial }, { "loadedModels", loadedModels },
            { "state", active.isEmpty() ? "idle" : "generating" }, { "jobId", active } };
    };
    const auto stateEvent = [&] {
        auto event = status();
        event["event"] = "state";
        send(event);
    };
    QObject::connect(&job, &QTimer::timeout, &app, [&] {
        if (active.isEmpty())
            return;
        const auto id = active;
        active.clear();
        if (params.value("prompt").toString() == "crash")
            std::_Exit(3);
        if (params.value("prompt").toString() == "fail") {
            loadedModels = {};
            fail(id, "generation_failed", "Test inference failure; reload required.");
            stateEvent();
            return;
        }
        const auto folder = QDir(root).filePath("job-" + id);
        QDir().mkpath(folder);
        QImage image(
            params.value("width").toInt(64), params.value("height").toInt(64), QImage::Format_RGBA8888);
        image.fill(QColor(30, 200, 90, params.value("prompt").toString() == "transparent" ? 0 : 255));
        if (params.value("prompt").toString() == "identity") {
            const QImage reference(params["referenceImages"].toArray().first().toString());
            image = reference.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        const auto path = QDir(folder).filePath("image-0.png");
        image.save(path);
        send({ { "event", "preview" }, { "jobId", id }, { "path", path }, { "width", image.width() },
            { "height", image.height() }, { "step", 1 } });
        ok(id,
            { { "images",
                  QJsonArray { QJsonObject {
                      { "path", path }, { "width", image.width() }, { "height", image.height() } } } },
                { "request", params }, { "contextGeneration", serial },
                { "timings", QJsonObject { { "totalMs", 180 } } } });
        stateEvent();
    });
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
        char bytes[65536];
        const auto size = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (size <= 0) {
            app.quit();
            return;
        }
        input.append(bytes, int(size));
        while (input.contains('\n')) {
            const auto end = input.indexOf('\n');
            const auto line = input.left(end);
            input.remove(0, end + 1);
            const auto request = QJsonDocument::fromJson(line).object();
            const auto id = request.value("id").toString(), method = request.value("method").toString();
            if (method == "load") {
                const auto load = request.value("params").toObject();
                if (load.contains("loras")) {
                    fail(id, "invalid_arguments", "LoRAs belong to generate requests, not model loading.");
                    continue;
                }
                if (load["settings"].toObject()["device"] == "invalid-test-device") {
                    fail(id, "invalid_arguments", "Unknown test device; existing model retained.");
                    stateEvent();
                    continue;
                }
                ++serial;
                loadedModels = QJsonArray { QJsonObject {
                    { "contextGeneration", serial }, { "backend", "test" },
                    { "profile", QJsonObject { { "id", load["profile"] },
                        { "name", "Qwen Image 2.1 Turbo" } } },
                    { "files", load["files"] }, { "effectiveSettings", load["settings"] } } };
                ok(id, { { "contextGeneration", serial } });
                stateEvent();
            } else if (method == "generate") {
                if (!active.isEmpty()) {
                    fail(id, "busy", "Busy");
                    return;
                }
                active = id;
                stateEvent();
                params = request.value("params").toObject();
                send({ { "event", "progress" }, { "jobId", id }, { "step", 1 }, { "steps", 2 },
                    { "elapsedMs", 10 } });
                QTimer::singleShot(50,&app,[&,id] {
                    if(active != id) return;
                    const auto folder=QDir(root).filePath("job-"+id);
                    QDir().mkpath(folder);
                    QImage draft(16,16,QImage::Format_RGBA8888); draft.fill(Qt::green);
                    const auto path=QDir(folder).filePath("preview.png"); draft.save(path);
                    send({{"event","preview"},{"jobId",id},{"path",path},{"width",16},{"height",16},{"step",1}});
                });
                job.start(params.value("prompt").toString() == "slow" ? 5000 : 180);
            } else if (method == "cancel") {
                const auto old = active;
                active.clear();
                job.stop();
                ok(id, { { "requested", !old.isEmpty() } });
                if (!old.isEmpty())
                    fail(old, "cancelled", "Cancelled");
                stateEvent();
            } else if (method == "unload") {
                loadedModels = {};
                ok(id, { { "unloaded", true } });
                stateEvent();
            } else if (method == "shutdown") {
                job.stop();
                ok(id, { });
                app.quit();
            } else
                ok(id, status());
        }
    });
    return app.exec();
}
