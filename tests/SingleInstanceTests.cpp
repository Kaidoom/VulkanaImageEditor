#include "imageeditor/platform/SingleInstance.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <memory>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using imageeditor::platform::SingleInstance;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #condition << '\n'; } } while (false)
struct Peer {
    QProcess process;
    QByteArray output;
    ~Peer() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); } }
    void start(const QString& directory, const QStringList& paths = {}, const QString& mode = "normal",
        int lifetime = 15000, int timeout = 3000, const QString& executable = {}, const QString& cwd = {})
    {
        if (!cwd.isEmpty()) process.setWorkingDirectory(cwd);
        process.start(executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable,
            QStringList{"--peer", directory, mode, QString::number(lifetime), QString::number(timeout)} + paths);
        CHECK(process.waitForStarted());
    }
    bool contains(const QByteArray& expected, int timeout = 5000)
    {
        QElapsedTimer timer; timer.start();
        do {
            output += process.readAllStandardOutput();
            if (output.contains(expected)) return true;
            if (process.state() == QProcess::NotRunning) break;
            process.waitForReadyRead(std::max(1, timeout - int(timer.elapsed())));
        } while (timer.elapsed() < timeout);
        output += process.readAllStandardOutput();
        if (output.contains(expected)) return true;
        std::cerr << "Missing " << expected.constData() << " in " << output.constData()
                  << process.readAllStandardError().constData() << '\n';
        return false;
    }
    void finish() { CHECK(process.waitForFinished(5000) || process.state() == QProcess::NotRunning); }
};
QByteArray request(const QStringList& files)
{
    return "REQUEST " + QJsonDocument(QJsonArray::fromStringList(files)).toJson(QJsonDocument::Compact) + '\n';
}
void sharedIdentityAndFiles(const QString& root)
{
    const auto directory = root + "/shared";
    // Different executable names/locations model RPM and changing AppImage
    // mount paths. Neither executable basename nor path enters the lock key.
    const auto rpm = root + "/vulkana-editor";
    const auto portable = root + QString::fromUtf8("/AppImage 空間");
    CHECK(QFile::copy(QCoreApplication::applicationFilePath(), rpm));
    CHECK(QFile::copy(QCoreApplication::applicationFilePath(), portable));
    Peer primary; primary.start(directory, {}, "normal", 15000, 3000, rpm);
    CHECK(primary.contains("PRIMARY\n"));
    Peer second;
    second.start(directory, {QString::fromUtf8("folder/file 空間.png"), "another.vulkana"}, "normal", 15000, 3000, portable, root);
    second.finish(); CHECK(second.contains("FORWARDED\n")); CHECK(second.process.exitCode() == 0);
    CHECK(primary.contains(request({root + QString::fromUtf8("/folder/file 空間.png"), root + "/another.vulkana"})));
    Peer activation; activation.start(directory); activation.finish();
    CHECK(activation.contains("FORWARDED\n")); CHECK(primary.contains("REQUEST []\n"));
    // Reject malformed/oversized IPC without damaging the primary or starting
    // another editor. These are local data, never command-line/shell commands.
    for (const auto& bytes : {QByteArray("{\"protocol\":99,\"files\":[]}\n"), QByteArray(65537, 'x'),
             QByteArray("{\"protocol\":1,\"files\":[\"relative.png\"]}\n")}) {
        QLocalSocket socket; socket.connectToServer(directory + "/instance-v1.sock");
        CHECK(socket.waitForConnected()); socket.write(bytes);
        socket.waitForReadyRead(2000);
        CHECK(socket.readAll() != "OK\n");
    }
    Peer after; after.start(directory); after.finish(); CHECK(after.contains("FORWARDED\n"));
    CHECK(QFileInfo(directory).permissions() == (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
        | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser));
    // SIGKILL leaves the socket/lock files behind; kernel ownership, not their
    // existence, permits the next primary. No PID liveness heuristic involved.
    primary.process.kill(); primary.process.waitForFinished();
    Peer recovery; recovery.start(directory); CHECK(recovery.contains("PRIMARY\n"));
    Peer recoveredClient; recoveredClient.start(directory); recoveredClient.finish();
    CHECK(recoveredClient.contains("FORWARDED\n"));
}
void simultaneousLaunch(const QString& root)
{
    std::vector<std::unique_ptr<Peer>> peers;
    for (int i = 0; i < 8; ++i) {
        auto peer = std::make_unique<Peer>(); peer->start(root + "/race"); peers.push_back(std::move(peer));
    }
    int primaries = 0, forwarded = 0;
    for (auto& peer : peers) {
        CHECK(peer->contains("\n"));
        if (peer->output.contains("PRIMARY\n")) ++primaries;
        else { peer->finish(); CHECK(peer->contains("FORWARDED\n")); ++forwarded; }
    }
    CHECK(primaries == 1 && forwarded == 7);
}
void restartWait(const QString& root)
{
    Peer primary; primary.start(root + "/restart", {}, "normal", 1200);
    CHECK(primary.contains("PRIMARY\n"));
    Peer replacement; replacement.start(root + "/restart", {}, "wait");
    CHECK(!replacement.process.waitForReadyRead(150)); // never forwards to the closing instance
    primary.finish(); CHECK(primary.process.exitCode() == 0);
    CHECK(replacement.contains("PRIMARY\n")); CHECK(!replacement.output.contains("FORWARDED"));
    Peer launch; launch.start(root + "/restart"); launch.finish(); CHECK(launch.contains("FORWARDED\n"));
}
void heldLockAndSecurity(const QString& root)
{
    const auto directory = root + "/held";
    CHECK(QDir().mkdir(directory));
    CHECK(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const auto name = QFile::encodeName(directory + "/instance.lock");
    const int lock = ::open(name.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    CHECK(lock >= 0); CHECK(::flock(lock, LOCK_EX | LOCK_NB) == 0);
    Peer locked; locked.start(directory, {}, "normal", 15000, 250); locked.finish();
    CHECK(locked.contains("ERROR ")); CHECK(locked.process.exitCode() == 2);
    // Restart mode also cannot bypass a live owner, even without a socket.
    Peer restart; restart.start(directory, {}, "wait", 15000, 250); restart.finish();
    CHECK(restart.contains("ERROR ")); CHECK(restart.process.exitCode() == 2);
    ::close(lock);
    Peer recovery; recovery.start(directory); CHECK(recovery.contains("PRIMARY\n"));
    const auto alias = root + "/symlink";
    CHECK(QFile::link(directory, alias));
    SingleInstance symlink(alias); CHECK(symlink.start({}) == SingleInstance::Result::Error);
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() >= 6 && args[1] == "--peer") {
        SingleInstance instance(args[2]);
        const auto result = instance.start(args.mid(6), args[3] == "wait", args[5].toInt());
        if (result == SingleInstance::Result::Error) {
            std::cout << "ERROR " << instance.errorString().toStdString() << std::endl; return 2;
        }
        if (result == SingleInstance::Result::Forwarded) { std::cout << "FORWARDED" << std::endl; return 0; }
        std::cout << "PRIMARY" << std::endl;
        instance.setOpenHandler([](const QStringList& files) { std::cout << request(files).constData() << std::flush; });
        QTimer::singleShot(args[4].toInt(), &app, &QCoreApplication::quit);
        return app.exec();
    }
    QTemporaryDir temporary; CHECK(temporary.isValid());
    sharedIdentityAndFiles(temporary.path());
    simultaneousLaunch(temporary.path());
    restartWait(temporary.path());
    heldLockAndSecurity(temporary.path());
    return failures ? 1 : 0;
}
