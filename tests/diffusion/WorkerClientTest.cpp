#include <imageeditor/diffusion/WorkerClient.h>

#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using imageeditor::diffusion::WorkerClient;

class WorkerClientTest final : public QObject {
    Q_OBJECT
private:
    static void scenario(const QTemporaryDir& directory, const QByteArray& value)
    {
        QFile file(directory.filePath(QStringLiteral("scenario.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(value), value.size());
    }
    static QString executable() { return QString::fromUtf8(FAKE_WORKER_EXECUTABLE); }
private slots:
    void startStopStart()
    {
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        for (int iteration = 1; iteration <= 2; ++iteration) {
            QVERIFY(client.start(executable(), directory.path()));
            QTRY_COMPARE(ready.count(), iteration);
            QCOMPARE(client.state(), WorkerClient::State::Idle);
            QVERIFY(client.processId() > 0);
            QVERIFY(!client.start(executable(), directory.path()));
            client.stop();
            QTRY_COMPARE(client.state(), WorkerClient::State::Stopped);
            QCOMPARE(client.pendingRequestCount(), 0);
            QVERIFY(!client.isRunning());
        }
        QCOMPARE(failed.count(), 0);
    }
    void requestIdsSurviveNewClientObjects()
    {
        QTemporaryDir directory;
        QSet<QString> ids;
        const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
        for (int iteration = 0; iteration < 2; ++iteration) {
            WorkerClient client;
            QSignalSpy ready(&client, &WorkerClient::ready);
            QSignalSpy finished(&client, &WorkerClient::requestFinished);
            QVERIFY(client.start(executable(), directory.path()));
            QTRY_COMPARE(ready.count(), 1);
            const QString id = client.request("ping");
            QVERIFY(uuid.match(id).hasMatch());
            QVERIFY(!ids.contains(id));
            ids.insert(id);
            QTRY_COMPARE(finished.count(), 1);
            client.stop();
            QTRY_COMPARE(client.state(), WorkerClient::State::Stopped);
        }
        QCOMPARE(ids.size(), 2);
    }
    void jobsStatusAndValidation()
    {
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy finished(&client, &WorkerClient::requestFinished);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString load = client.request("load", {{"delay", 200}});
        const QString ping = client.request("ping", {{"key", "value"}});
        const QString overlapping = client.request("generate");
        QVERIFY(!load.isEmpty() && load != ping && ping != overlapping);
        QCOMPARE(client.state(), WorkerClient::State::Busy);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), overlapping);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("busy"));
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(finished.at(0).at(0).toString(), ping);
        QCOMPARE(finished.at(0).at(1).toJsonObject().value("echo").toObject().value("key").toString(), QStringLiteral("value"));
        QCOMPARE(client.state(), WorkerClient::State::Idle);
        const QString invalid = client.request("load", {{"invalid", true}});
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(0).toString(), invalid);
        QCOMPARE(client.state(), WorkerClient::State::Idle);
        client.request("generate");
        QTRY_COMPARE(finished.count(), 3);
        client.request("unload");
        QTRY_COMPARE(finished.count(), 4);
        QCOMPARE(client.state(), WorkerClient::State::Idle);
    }
    void cooperativeCancellationWaitsForJob()
    {
        WorkerClient client;
        client.setTimeouts(1000, 1000, 1000);
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy finished(&client, &WorkerClient::requestFinished);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString job = client.request("generate", {{"delay", 1000}});
        const QString cancel = client.cancel();
        QCOMPARE(client.state(), WorkerClient::State::Cancelling);
        QTRY_VERIFY(finished.count() >= 1);
        QCOMPARE(finished.at(0).at(0).toString(), cancel);
        // Receipt of cancellation alone does not report the job as cancelled.
        if (failed.isEmpty()) QCOMPARE(client.state(), WorkerClient::State::Cancelling);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), job);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("cancelled"));
        QCOMPARE(client.state(), WorkerClient::State::Idle);
        QVERIFY(client.isRunning());
        client.request("generate");
        QTRY_COMPARE(finished.count(), 2);
    }
    void cancellationTimeoutAndRecovery()
    {
        WorkerClient client;
        client.setTimeouts(1000, 120, 120);
        QTemporaryDir directory;
        scenario(directory, "hang-cancel");
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString job = client.request("load");
        client.cancel();
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QTRY_VERIFY(!client.isRunning());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), job);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("worker_terminated"));
        scenario(directory, "");
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(client.state(), WorkerClient::State::Idle);
        QCOMPARE(client.pendingRequestCount(), 0);
    }
    void unexpectedExitFailsPendingOnce()
    {
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString job = client.request("generate", {{"delay", 1000}});
        const QString crash = client.request("crash");
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QTRY_VERIFY(!client.isRunning());
        QCOMPARE(failed.count(), 2);
        QSet<QString> ids;
        for (const auto& args : failed) { ids.insert(args.at(0).toString()); QCOMPARE(args.at(1).toString(), QStringLiteral("worker_exit")); }
        QCOMPARE(ids, QSet<QString>({job, crash}));
        QCOMPARE(client.pendingRequestCount(), 0);
    }
    void stoppingBlockedJob()
    {
        WorkerClient client;
        client.setTimeouts(1000, 120, 120);
        QTemporaryDir directory;
        scenario(directory, "hang-stop");
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString job = client.request("generate");
        client.stop();
        QCOMPARE(client.state(), WorkerClient::State::Stopping);
        QTRY_COMPARE(client.state(), WorkerClient::State::Stopped);
        QVERIFY(!client.isRunning());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), job);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("worker_terminated"));
    }
    void handshakeFailure_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("code");
        QTest::newRow("timeout") << QByteArray("silent") << QStringLiteral("handshake_timeout");
        QTest::newRow("unsupported") << QByteArray("bad-handshake") << QStringLiteral("protocol_error");
        QTest::newRow("fatal") << QByteArray("fatal-handshake") << QStringLiteral("initialization_failed");
    }
    void handshakeFailure()
    {
        QFETCH(QByteArray, mode); QFETCH(QString, code);
        WorkerClient client;
        client.setTimeouts(120, 120, 120);
        QTemporaryDir directory;
        scenario(directory, mode);
        QSignalSpy errors(&client, &WorkerClient::errorOccurred);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QTRY_VERIFY(!client.isRunning());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.at(0).at(0).toString(), code);
    }
    void protocolErrors_data()
    {
        QTest::addColumn<QString>("method");
        QTest::newRow("bad-json") << QStringLiteral("invalid-json");
        QTest::newRow("oversized") << QStringLiteral("large-line");
        QTest::newRow("wrong-id") << QStringLiteral("wrong-id");
    }
    void protocolErrors()
    {
        QFETCH(QString, method);
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString request = client.request(method);
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QTRY_VERIFY(!client.isRunning());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), request);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("protocol_error"));
    }
    void queuedDeletedPreviewDoesNotInvalidateCancellation()
    {
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy events(&client, &WorkerClient::eventReceived);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QSignalSpy errors(&client, &WorkerClient::errorOccurred);
        QSignalSpy finished(&client, &WorkerClient::requestFinished);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString job = client.request("generate", {{"queuedPreviewCancel", true}});
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), job);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("cancelled"));
        QCOMPARE(errors.count(), 0);
        QCOMPARE(events.count(), 0);
        QCOMPARE(client.state(), WorkerClient::State::Idle);
        QVERIFY(client.isRunning());
        client.request("stale-preview");
        QTRY_COMPARE(finished.count(), 1);
        client.request("ping");
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(events.count(), 0);
        QCOMPARE(errors.count(), 0);
    }
    void missingEscapingPreviewsStillFail_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("outside-missing") << QStringLiteral("outside");
        QTest::newRow("relative") << QStringLiteral("relative");
#ifndef Q_OS_WIN
        QTest::newRow("symlink-missing") << QStringLiteral("symlink");
#endif
    }
    void missingEscapingPreviewsStillFail()
    {
        QFETCH(QString, mode);
        WorkerClient client;
        QTemporaryDir directory;
        QTemporaryDir outside;
        QString path = outside.filePath("missing.png");
        if (mode == QLatin1String("relative")) path = QStringLiteral("relative.png");
        if (mode == QLatin1String("symlink")) {
            QVERIFY(QFile::link(outside.path(), directory.filePath("escape")));
            path = directory.filePath("escape/missing-subdirectory/missing.png");
        }
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QSignalSpy events(&client, &WorkerClient::eventReceived);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        const QString id = client.request("preview-path", {{"path", path}});
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QTRY_VERIFY(!client.isRunning());
        QCOMPARE(events.count(), 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), id);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("unsafe_output_path"));
    }
    void fatalEventTerminatesWorkerAndAllowsRecovery()
    {
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QSignalSpy errors(&client, &WorkerClient::errorOccurred);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        client.request("generate", {{"delay", 1000}});
        client.request("fatal");
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QTRY_VERIFY(!client.isRunning());
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.at(0).at(0).toString(), QStringLiteral("backend_failed"));
        QVERIFY(errors.at(0).at(1).toString().contains(QStringLiteral("Restart")));
        QCOMPARE(failed.count(), 2);
        for (const auto& args : failed) QCOMPARE(args.at(1).toString(), QStringLiteral("backend_failed"));
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(client.state(), WorkerClient::State::Idle);
    }
    void outputPathsAndDiagnostics()
    {
        WorkerClient client;
        QTemporaryDir directory;
        QSignalSpy ready(&client, &WorkerClient::ready);
        QSignalSpy events(&client, &WorkerClient::eventReceived);
        QSignalSpy finished(&client, &WorkerClient::requestFinished);
        QSignalSpy failed(&client, &WorkerClient::requestFailed);
        QSignalSpy logs(&client, &WorkerClient::diagnostic);
        QVERIFY(client.start(executable(), directory.path()));
        QTRY_COMPARE(ready.count(), 1);
        client.request("diagnostics");
        client.request("preview");
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(events.count(), 1);
        QTRY_VERIFY(!logs.isEmpty());
        qsizetype bytes = 0;
        for (const auto& args : logs) { bytes += args.at(0).toString().size(); QVERIFY(args.at(0).toString().size() <= 8192); }
        QCOMPARE(bytes, qsizetype(30000));
        client.request("unsafe-output");
        QTRY_COMPARE(client.state(), WorkerClient::State::Error);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("unsafe_output_path"));
    }
};

QTEST_GUILESS_MAIN(WorkerClientTest)
#include "WorkerClientTest.moc"
