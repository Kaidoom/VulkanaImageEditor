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
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif
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
    auto profiles = imageeditor::diffusion::modelProfiles();
    const bool stacks = !qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_NO_STACK");
    if (!stacks)
        for (qsizetype i = profiles.size(); i-- > 0;)
            if (profiles[i].toObject()["id"] == "qwen-image-layered") profiles.removeAt(i);
    const bool named = qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_NAMED");
    if (!named)
        for (qsizetype i = profiles.size(); i-- > 0;)
            if (profiles[i].toObject()["id"] == "see-through-layerdiff-v3") profiles.removeAt(i);
    auto backend = qEnvironmentVariable("VULKANA_TEST_FAKE_BACKEND");
    if (backend.isEmpty()) backend = qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_PHASES") ? "vulkan" : named ? "cuda" : "test";
    send({ { "namedLayerStacks", named }, { "event", "hello" }, { "protocol", 1 }, { "backend", backend },
        { "referenceSizeLimit", true }, { "layerStackResults", stacks },
        { "loraParameters", true },
        { "flux3NativeRegions", !qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_SPATIAL_V1") },
        { "spatialVersions", qEnvironmentVariableIsSet("VULKANA_TEST_DIFFUSION_SPATIAL_V1")
              ? QJsonArray { 1 } : QJsonArray { 1, 2, 3 } },
        { "profiles", profiles },
        { "devices", QJsonArray { QJsonObject { { "id", "CPU" }, { "name", "Test CPU" } } } },
        { "samplers", QJsonArray { "euler" } }, { "schedulers", QJsonArray { "discrete" } } });
    QByteArray input;
#ifdef Q_OS_WIN
    // Windows anonymous pipes cannot be monitored by QSocketNotifier.
    QTimer notifier;
    notifier.setInterval(5);
    notifier.start();
#else
    QSocketNotifier notifier(STDIN_FILENO, QSocketNotifier::Read);
#endif
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
        if (params.contains("processingResolution")) {
            QJsonArray parts;
            const QString invalidPart = qEnvironmentVariable("VULKANA_TEST_INVALID_PART");
            const int count = invalidPart == "count" ? 30 : 12;
            for (int i = 0; i < count; ++i) {
                QImage part(3, 4, QImage::Format_RGBA8888);
                part.fill(QColor(i, 41, 20, 220));
                const auto path = QDir(folder).filePath(QString("part-%1.png").arg(i));
                part.save(path);
                const int x = invalidPart == "bounds" ? image.width() : 3 + i % 12;
                const auto name = QString("part %1").arg(invalidPart == "name" ? 0 : i);
                parts.append(QJsonObject{{"path",path},{"width",3},{"height",4},{"x",x},{"y",7},{"name",name}});
            }
            ok(id, {{"images",parts},{"resultKind","layerStack"},{"stackWidth",image.width()},
                    {"stackHeight",image.height()},{"request",params},{"contextGeneration",serial}});
            stateEvent();
            return;
        }
        if (params.contains("layerCount")) {
            QJsonArray layers;
            for (int i = 0; i < params["layerCount"].toInt(); ++i) {
                image.fill(i == 0 ? QColor(Qt::blue) : QColor(255, 0, 0, 110));
                const auto path = QDir(folder).filePath(QString("layer-%1.png").arg(i));
                image.save(path);
                layers.append(QJsonObject{{"path",path},{"width",image.width()},{"height",image.height()},{"channels",4}});
            }
            if (params["prompt"] == "incomplete") layers.removeLast();
            ok(id, {{"images",layers},{"resultKind","layerStack"},{"request",params},{"contextGeneration",serial}});
            stateEvent();
            return;
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
#ifdef Q_OS_WIN
    QObject::connect(&notifier, &QTimer::timeout, &app, [&] {
#else
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
#endif
        char bytes[65536];
#ifdef Q_OS_WIN
        const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0, count = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            app.quit();
            return;
        }
        if (!available) return;
        const auto size = ReadFile(pipe, bytes, sizeof(bytes), &count, nullptr) ? int(count) : -1;
#else
        const auto size = ::read(STDIN_FILENO, bytes, sizeof(bytes));
#endif
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
                if (params.value("prompt") == "phases") {
                    send({{"event","progress"},{"jobId",id},{"stage","conditioning"},{"detail","positive_prompt"}});
                    for (const auto& phase : {QPair{250,"sampling"}, QPair{600,"vae_decode"}, QPair{900,"saving"}})
                        QTimer::singleShot(phase.first, &app, [&, id, stage=phase.second] {
                            if (active == id) send({{"event","progress"},{"jobId",id},{"stage",stage},
                                {"unit","steps"},{"step",QString(stage)=="sampling"?1:0},{"steps",QString(stage)=="sampling"?2:0}});
                        });
                    QTimer::singleShot(50,&app,[&, id] {
                        if (active == id) send({{"event","progress"},{"jobId","expired"},{"stage","vae_decode"}});
                    });
                } else send({ { "event", "progress" }, { "jobId", id }, { "step", 1 }, { "steps", 2 },
                    { "elapsedMs", 10 } });
                QTimer::singleShot(50,&app,[&,id] {
                    if(active != id) return;
                    const auto folder=QDir(root).filePath("job-"+id);
                    QDir().mkpath(folder);
                    QImage draft(16,16,QImage::Format_RGBA8888); draft.fill(Qt::green);
                    const auto path=QDir(folder).filePath("preview.png"); draft.save(path);
                    send({{"event","preview"},{"jobId",id},{"path",path},{"width",16},{"height",16},{"step",1}});
                });
                job.start(params.value("prompt").toString() == "slow" ? 5000 : params.value("prompt") == "phases" ? 1200 : 180);
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
