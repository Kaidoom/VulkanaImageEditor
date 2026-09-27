#pragma once

#include "imageeditor/platform/ServiceConfig.hpp"

#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace imageeditor::ui {

struct FeedbackSubmission {
    QString category, message, contact, systemInfo;
};

// One workspace-owned service retains the cooldown across panel reopenings.
// Nothing is persisted, queued offline, logged, or automatically retried.
class FeedbackService final : public QObject {
public:
    enum class State { Idle, Submitting, Succeeded, Error };
    static constexpr int maximumCharacters = 2000;
    static constexpr int cooldownMs = 2000;
    explicit FeedbackService(QObject* parent = nullptr, QNetworkAccessManager* transport = nullptr,
        int timeoutMs = 8000);
    ~FeedbackService() override;
    bool submit(const FeedbackSubmission&);
    void reset(); // Cancels pending I/O, but never resets the rate limiter.
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool busy() const { return state_ == State::Submitting; }
    // Success is not a session-wide lock: only pending I/O and the cooldown
    // prevent another explicit report. The panel owns its thank-you state.
    [[nodiscard]] bool configured() const { return config_.enabled(); }
    [[nodiscard]] bool canSubmit() const { return configured() && !busy() && !cooldown_.isActive(); }
    [[nodiscard]] const QString& message() const { return message_; }
    QUrl endpoint() const;
    std::function<void()> onChanged;
private:
    void notify();
    void stopRequest();
    void fail(const QString&);
    void readResponse();
    void finish();
    const platform::ServiceConfig config_ {platform::ServiceConfig::load()};
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_, cooldown_;
    int timeoutMs_;
    QByteArray response_;
    State state_ {State::Idle};
    QString message_;
};
} // namespace imageeditor::ui
