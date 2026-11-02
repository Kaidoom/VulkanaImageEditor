#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace imageeditor::diffusion {

// Owns one child process. All methods and signals belong to this object's thread.
class WorkerClient final : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Starting, Idle, Busy, Cancelling, Stopping, Error };
    Q_ENUM(State)

    explicit WorkerClient(QObject* parent = nullptr);
    ~WorkerClient() override;

    bool start(const QString& executable, const QString& workDirectory, QString* error = nullptr);
    QString request(const QString& method, const QJsonObject& params = {});
    QString cancel();
    void stop();
    void setTimeouts(int handshakeMs, int cancellationGraceMs, int stopGraceMs);

    State state() const { return m_state; }
    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    qint64 processId() const { return m_process.processId(); }
    QString workDirectory() const { return m_workDirectory; }
    QJsonObject hello() const { return m_hello; }
    qsizetype pendingRequestCount() const { return m_pending.size(); }

signals:
    void ready(const QJsonObject& hello);
    void stateChanged(imageeditor::diffusion::WorkerClient::State state);
    void eventReceived(const QJsonObject& event);
    void requestFinished(const QString& id, const QJsonObject& result);
    void requestFailed(const QString& id, const QString& code, const QString& message);
    void errorOccurred(const QString& code, const QString& message);
    void diagnostic(const QString& text);

private:
    struct Pending { QString method; bool internal = false; };
    void setState(State state);
    QString send(const QString& method, const QJsonObject& params, bool internal);
    void readOutput();
    void receive(const QJsonObject& object);
    void processFinished(int code, QProcess::ExitStatus status);
    void fail(const QString& code, const QString& message);
    void failPending(const QString& code, const QString& message);
    void updateActivity();
    bool hasJob() const;
    bool validOutputPath(const QString& path, bool allowMissing = false) const;
    bool validOutputFiles(const QJsonObject& object) const;

    QProcess m_process;
    QTimer m_handshakeTimer;
    QTimer m_cancelTimer;
    QTimer m_stopTimer;
    QHash<QString, Pending> m_pending;
    QByteArray m_output;
    QJsonObject m_hello;
    QString m_workDirectory;
    State m_state = State::Stopped;
    int m_handshakeMs = 30000;
    int m_cancelMs = 5000;
    int m_stopMs = 5000;
    bool m_failed = false;
    bool m_stopping = false;
    bool m_processingExit = false;
};

} // namespace imageeditor::diffusion
