#include "imageeditor/ui/FeedbackService.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <algorithm>

namespace imageeditor::ui {
namespace {
constexpr qint64 maximumResponseBytes = 64 * 1024;
qsizetype characterCount(const QString& value)
{
    return value.size()-std::count_if(value.begin(),value.end(),[](QChar c){return c.isLowSurrogate();});
}
}

FeedbackService::FeedbackService(QObject* parent, QNetworkAccessManager* transport, int timeoutMs)
    : QObject(parent), network_(transport ? transport : new QNetworkAccessManager(this)),
      timeoutMs_(std::clamp(timeoutMs, 1, 30000))
{
    deadline_.setSingleShot(true);
    cooldown_.setSingleShot(true);
    cooldown_.setTimerType(Qt::PreciseTimer); // Never release the two-second gate early.
    connect(&cooldown_, &QTimer::timeout, this, [this] { notify(); });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        fail(tr("Submission could not be confirmed. Check your connection before trying again."));
    });
}
FeedbackService::~FeedbackService() { onChanged={}; stopRequest(); }
QUrl FeedbackService::endpoint() const { return config_.url(QStringLiteral("/api/feedback-add")); }
void FeedbackService::notify() { if(onChanged)onChanged(); }

bool FeedbackService::submit(const FeedbackSubmission& value)
{
    if(!canSubmit())return false;
    const auto message=value.message.trimmed();
    if (message.isEmpty() || characterCount(value.message)>maximumCharacters
        || value.contact.size()>254 || value.systemInfo.size()>2048
        || (value.category!="bug" && value.category!="feature" && value.category!="improvement" && value.category!="other")) {
        fail(tr("Choose a category and enter feedback (up to 2,000 characters)."));
        return false;
    }
    const auto body=QJsonDocument(QJsonObject {
        {"source", "app"}, {"app_version", QCoreApplication::applicationVersion()},
        {"category",value.category}, {"message",message}, {"contact",value.contact.trimmed()},
        {"system_info",value.systemInfo}
    }).toJson(QJsonDocument::Compact);
    QNetworkRequest request(endpoint());
    request.setHeader(QNetworkRequest::ContentTypeHeader,QStringLiteral("application/json"));
    request.setRawHeader("X-Vulkana-Token",config_.apiKey);
    request.setRawHeader("Accept","application/json");
    // A redirect must never forward the client token or a private report.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
    request.setTransferTimeout(timeoutMs_);
    state_=State::Submitting;
    message_=tr("Sending feedback…");
    response_.clear();
    cooldown_.start(cooldownMs);
    deadline_.start(timeoutMs_);
    reply_=network_->post(request,body);
    reply_->setReadBufferSize(maximumResponseBytes+1);
    connect(reply_,&QNetworkReply::readyRead,this,[this]{readResponse();});
    connect(reply_,&QNetworkReply::finished,this,[this]{finish();});
    notify();
    return true;
}
void FeedbackService::readResponse()
{
    if(!reply_)return;
    // Bound even chunked responses. Only the API's success flag is used; never
    // display arbitrary HTML/server diagnostics or log response/report contents.
    response_+=reply_->read(maximumResponseBytes-response_.size()+1);
    if(response_.size()>maximumResponseBytes)
        fail(tr("The feedback service returned an unexpected response. Please try again later."));
}
void FeedbackService::finish()
{
    readResponse();
    if(!reply_)return;
    const int status=reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(status==429) {
        bool ok=false;
        const auto seconds=reply_->rawHeader("Retry-After").toLongLong(&ok);
        if(ok && seconds>0)cooldown_.start(std::max(cooldown_.remainingTime(),int(std::min(seconds,qint64(300)))*1000));
        fail(tr("Too many reports. Please wait before trying again."));
    } else if(reply_->error()!=QNetworkReply::NoError || status<200 || status>=300) {
        fail(status==400 || status==413 || status==422
            ? tr("The report was not accepted. Check its contents and try again.")
            : tr("Could not send feedback. Please try again later."));
    } else if (status!=204 && !QJsonDocument::fromJson(response_).object().value("ok").toBool()) {
        fail(tr("Submission could not be confirmed. Please try again later."));
    } else {
        stopRequest();
        state_=State::Succeeded;
        message_.clear();
        notify();
    }
}
void FeedbackService::stopRequest()
{
    deadline_.stop();
    response_.clear();
    if(!reply_)return;
    auto* reply=reply_.data();reply_=nullptr;
    disconnect(reply,nullptr,this,nullptr);
    if(!reply->isFinished())reply->abort();
    reply->deleteLater();
}
void FeedbackService::fail(const QString& message)
{
    stopRequest();state_=State::Error;message_=message;notify();
}
void FeedbackService::reset()
{
    stopRequest();state_=State::Idle;message_.clear();notify();
}
} // namespace imageeditor::ui
