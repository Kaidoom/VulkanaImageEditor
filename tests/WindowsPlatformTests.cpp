#include "imageeditor/platform/SingleInstance.hpp"
#include "imageeditor/platform/AtomicFile.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/ui/UpdateService.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QProcess>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QTimer>
#include <iostream>
#include <memory>
#include <vector>

namespace p = imageeditor::platform;
namespace u = imageeditor::ui;
static int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #value "\n"; } } while (false)
struct Peer {
    QProcess process;
    QByteArray output;
    ~Peer() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); } }
    void start(const QString& scope, const QString& mode = "normal", int lifetime = 10000, const QString& file = {}) {
        process.start(QCoreApplication::applicationFilePath(), {"--peer", scope, mode, QString::number(lifetime), file});
        CHECK(process.waitForStarted());
    }
    bool contains(const QByteArray& text) {
        QElapsedTimer timer; timer.start();
        do {
            output += process.readAllStandardOutput();
            if (output.contains(text)) return true;
            if (process.state() == QProcess::NotRunning) break;
            process.waitForReadyRead(std::max(1, 5000 - int(timer.elapsed())));
        } while (timer.elapsed() < 5000);
        output += process.readAllStandardOutput();
        if (output.contains(text)) return true;
        std::cerr << output.constData() << process.readAllStandardError().constData();
        return false;
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() >= 6 && args[1] == "--peer") {
        p::SingleInstance instance(args[2]);
        const QStringList files = args[5].isEmpty() ? QStringList{} : QStringList{args[5]};
        const auto result = instance.start(files, args[3] == "wait", 3000);
        if (result == p::SingleInstance::Result::Forwarded) { std::cout << "FORWARDED\n" << std::flush; return 0; }
        if (result != p::SingleInstance::Result::Primary) { std::cerr << instance.errorString().toStdString(); return 2; }
        std::cout << "PRIMARY\n" << std::flush;
        instance.setOpenHandler([](const QStringList& paths) {
            for (const auto& path : paths) std::cout << path.toUtf8().constData() << '\n';
            std::cout << "REQUEST\n" << std::flush;
        });
        QTimer::singleShot(args[4].toInt(), &app, &QCoreApplication::quit);
        return app.exec();
    }
    QTemporaryDir dir; CHECK(dir.isValid());
    const auto scope = dir.filePath("scope");
    Peer primary; primary.start(scope); CHECK(primary.contains("PRIMARY"));
    const auto unicode = dir.filePath(QString::fromUtf8("画像 café ملف.vulkana"));
    Peer client; client.start(scope, "normal", 10000, unicode); CHECK(client.contains("FORWARDED"));
    CHECK(primary.contains(unicode.toUtf8()));
    Peer independent; independent.start(dir.filePath("other")); CHECK(independent.contains("PRIMARY"));
    primary.process.kill(); CHECK(primary.process.waitForFinished());
    Peer recovery; recovery.start(scope); CHECK(recovery.contains("PRIMARY"));
    std::vector<std::unique_ptr<Peer>> race;
    for (int i = 0; i < 6; ++i) {
        auto peer = std::make_unique<Peer>(); peer->start(dir.filePath("race")); race.push_back(std::move(peer));
    }
    int owners = 0, forwarded = 0;
    for (auto& peer : race) {
        CHECK(peer->contains("\n"));
        owners += peer->output.contains("PRIMARY");
        forwarded += peer->output.contains("FORWARDED");
    }
    CHECK(owners == 1 && forwarded == 5);
    Peer closing; closing.start(dir.filePath("restart"), "normal", 900); CHECK(closing.contains("PRIMARY"));
    Peer restart; restart.start(dir.filePath("restart"), "wait"); CHECK(restart.contains("PRIMARY"));
    CHECK(!restart.output.contains("FORWARDED"));

    const auto target = dir.filePath(QString::fromUtf8("保存 café.vulkana"));
    for (const auto bytes : {QByteArray("old"), QByteArray("new bytes")}) {
        auto temp = std::make_unique<QTemporaryFile>(dir.filePath("stage-XXXXXX"));
        CHECK(temp->open()); CHECK(temp->write(bytes) == bytes.size()); CHECK(p::syncFile(*temp));
        const auto staged = temp->fileName(); temp->setAutoRemove(false); temp.reset(); QString error;
        CHECK(p::replaceFileAtomically(staged, target, error));
        if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
        QFile::remove(staged);
        QFile file(target); CHECK(file.open(QIODevice::ReadOnly)); CHECK(file.readAll() == bytes);
    }
    CHECK(p::availableMemoryBytes().value_or(0) > 0);
    CHECK(p::processResidentMemoryBytes().value_or(0) > 0);
    CHECK(!p::userDataDirectory().startsWith(QCoreApplication::applicationDirPath()));
    CHECK(!p::userCacheDirectory().startsWith(QCoreApplication::applicationDirPath()));
    CHECK(u::UpdateService::buildPackage() == u::UpdatePackage::Unavailable);
    u::UpdateService updates("0.3.2"); CHECK(!updates.configured());
    updates.check(); CHECK(updates.state() == u::UpdateService::State::Error);
    QString error;
    CHECK(!u::UpdateService::parseManifest("{}", error, u::UpdatePackage::Unavailable));
    std::cout << "Windows platform checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
