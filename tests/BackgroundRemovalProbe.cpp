#include "imageeditor/ui/BackgroundRemoval.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
qint64 residentKiB(const QByteArray& key)
{
#ifdef Q_OS_LINUX
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) {
        for (const auto& raw : status.readAll().split('\n')) {
            const auto line = raw.simplified();
            if (line.startsWith(key))
                return line.split(' ').value(1).toLongLong();
        }
    }
#else
    Q_UNUSED(key);
#endif
    return -1;
}
}

// Standalone benchmark with an explicit trusted model directory. Run one model
// per invocation to isolate peak RSS; omitted model-id compares both models
// sequentially. Automated coverage lives in the bundled/interaction tests.
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    if (args.size() < 3 || args.size() > 7) {
        QTextStream(stderr) << "Usage: background_removal_probe MODEL_DIR IMAGE [MODEL_ID|all]"
                               " [OUTPUT_DIR|-] [REPEATS] [CANCEL_AFTER_MS]\n";
        return 2;
    }
    QImageReader reader(args[2]);
    reader.setAutoTransform(true);
    const auto source = reader.read();
    if (source.isNull()) {
        QTextStream(stderr) << "Cannot read source image: " << reader.errorString() << '\n';
        return 2;
    }
    const auto requested = args.value(3, QStringLiteral("all"));
    const auto outputDirectory = args.value(4, QStringLiteral("-"));
    if (outputDirectory != QStringLiteral("-") && !QDir().mkpath(outputDirectory)) {
        QTextStream(stderr) << "Cannot create output directory.\n";
        return 2;
    }
    const int repeats = args.value(5, QStringLiteral("1")).toInt();
    const int cancelAfter = args.value(6, QStringLiteral("-1")).toInt();
    imageeditor::ui::BackgroundRemovalEngine engine(args[1]);
    bool failed = false, evaluated = false;
    for (const auto& model : engine.models()) {
        if (requested != QStringLiteral("all") && requested != model.id)
            continue;
        for (int iteration = 0; iteration < repeats; ++iteration) {
            evaluated = true;
            std::atomic_bool cancel { false };
            std::jthread cancellation;
            // Cancel only the first run, so a repeated invocation also checks
            // that a terminated RunOptions does not poison the cached session.
            if (cancelAfter >= 0 && iteration == 0)
                cancellation = std::jthread([&](std::stop_token stop) {
                    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(cancelAfter);
                    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    if (!stop.stop_requested())
                        cancel.store(true);
                });
            const auto before = residentKiB("VmRSS:");
            const auto result = engine.evaluate(model.id, source, cancel);
            cancellation.request_stop();
            QJsonObject report {
                { QStringLiteral("id"), model.id },
                { QStringLiteral("name"), model.name },
                { QStringLiteral("iteration"), iteration + 1 },
                { QStringLiteral("sourceWidth"), source.width() },
                { QStringLiteral("sourceHeight"), source.height() },
                { QStringLiteral("initializationMs"), result.timings.initializationMs },
                { QStringLiteral("preprocessingMs"), result.timings.preprocessingMs },
                { QStringLiteral("inferenceMs"), result.timings.inferenceMs },
                { QStringLiteral("postprocessingMs"), result.timings.postprocessingMs },
                { QStringLiteral("totalMs"), result.timings.totalMs },
                { QStringLiteral("residentBeforeKiB"), before },
                { QStringLiteral("residentAfterKiB"), residentKiB("VmRSS:") },
                { QStringLiteral("peakResidentKiB"), residentKiB("VmHWM:") },
                { QStringLiteral("details"), result.details },
                { QStringLiteral("error"), result.error },
                { QStringLiteral("cancelled"), result.cancelled },
            };
            if (!result.mask.isNull()) {
                quint64 sum = 0, partial = 0;
                int low = 255, high = 0;
                for (int y = 0; y < result.mask.height(); ++y) {
                    const auto* row = result.mask.constScanLine(y);
                    for (int x = 0; x < result.mask.width(); ++x) {
                        sum += row[x];
                        partial += row[x] > 0 && row[x] < 255;
                        low = std::min(low, int(row[x]));
                        high = std::max(high, int(row[x]));
                    }
                }
                report.insert(QStringLiteral("maskWidth"), result.mask.width());
                report.insert(QStringLiteral("maskHeight"), result.mask.height());
                report.insert(QStringLiteral("maskMinimum"), low);
                report.insert(QStringLiteral("maskMaximum"), high);
                report.insert(QStringLiteral("meanCoverage"),
                    double(sum) / (255.0 * result.mask.width() * result.mask.height()));
                report.insert(QStringLiteral("partialPixels"), qint64(partial));
                if (outputDirectory != QStringLiteral("-")) {
                    const auto path = QDir(outputDirectory).filePath(model.id + QStringLiteral(".png"));
                    if (!result.mask.save(path)) {
                        failed = true;
                        report.insert(QStringLiteral("saveError"), QStringLiteral("Could not save output mask."));
                    }
                }
            } else if (!result.cancelled) {
                failed = true;
            }
            QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << Qt::endl;
        }
    }
    return !evaluated || failed ? 1 : 0;
}
