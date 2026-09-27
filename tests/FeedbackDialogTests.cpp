#include "imageeditor/ui/FeedbackDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/platform/BuildInfo.hpp"
#include "TestServiceConfig.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>
#include <iostream>
#include <cstring>
#include <deque>

namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool ok, const char* expression, int line)
{
    if (!ok) { ++failures; std::cerr << "FAIL " << line << ": " << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)

const QString preview = QStringLiteral("OS: Test Linux 1.0\nGPU: Test GPU\nCPU: Test CPU\nRAM: 16.0 GiB");
template<class F> bool waitFor(F condition,int timeout=3000)
{
    QElapsedTimer timer;timer.start();
    while(!condition() && timer.elapsed()<timeout)QTest::qWait(5);
    return condition();
}
struct Response {
    int status {201};
    QByteArray body {"{\"ok\":true}"};
    bool stall {false};
    QNetworkReply::NetworkError error {QNetworkReply::NoError};
    QByteArray retryAfter {};
};
class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request,Response response,QObject* parent,int& aborted)
        : QNetworkReply(parent), response_(std::move(response)), aborted_(aborted)
    {
        setRequest(request);setUrl(request.url());setOperation(QNetworkAccessManager::PostOperation);
        open(QIODevice::ReadOnly|QIODevice::Unbuffered);
        QTimer::singleShot(0,this,[this]{
            if(isFinished() || response_.stall)return;
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute,response_.status);
            setRawHeader("Retry-After",response_.retryAfter);
            ready_=true;emit readyRead();if(isFinished())return;
            if(response_.error!=NoError)setError(response_.error,"Untrusted server details");
            setFinished(true);emit finished();
        });
    }
    void abort() override {
        if(isFinished())return;
        ++aborted_;setError(OperationCanceledError,"Cancelled");setFinished(true);emit finished();
    }
    qint64 bytesAvailable() const override {return (ready_?response_.body.size()-position_:0)+QNetworkReply::bytesAvailable();}
protected:
    qint64 readData(char* data,qint64 maximum) override {
        const auto size=ready_?std::min(maximum,qint64(response_.body.size())-position_):0;
        if(size==0)return isFinished()?-1:0;
        std::memcpy(data,response_.body.constData()+position_,std::size_t(size));position_+=size;return size;
    }
private:
    Response response_;
    int& aborted_;
    qint64 position_ {0};
    bool ready_ {false};
};
class Network final : public QNetworkAccessManager {
public:
    std::deque<Response> responses;
    QList<QNetworkRequest> requests;
    QList<QByteArray> payloads;
    int aborted {0};
protected:
    QNetworkReply* createRequest(Operation operation,const QNetworkRequest& request,QIODevice* body) override {
        CHECK(operation==PostOperation);CHECK(body);
        requests.append(request);payloads.append(body?body->readAll():QByteArray{});
        CHECK(!responses.empty());
        Response response;
        if(!responses.empty()){response=std::move(responses.front());responses.pop_front();}
        else response.status=500;
        return new Reply(request,std::move(response),this,aborted);
    }
};

void formTests(const QString& snapshot)
{
    QWidget owner;
    owner.resize(700, 720); owner.show();
    u::FeedbackDialog dialog(&owner, preview);
    dialog.show();
    auto* text = dialog.findChild<QPlainTextEdit*>("FeedbackText");
    auto* email = dialog.findChild<QLineEdit*>("FeedbackEmail");
    auto* include = dialog.findChild<QCheckBox*>("FeedbackIncludeSystem");
    auto* system = dialog.findChild<QPlainTextEdit*>("FeedbackSystemPreview");
    auto* submit = dialog.findChild<QPushButton*>("FeedbackSubmit");
    auto* count = dialog.findChild<QLabel*>("FeedbackCharacterCount");
    auto* category = dialog.findChild<QComboBox*>("FeedbackCategory");
    CHECK(category && category->count()==4);
    if(category) {
        CHECK(category->itemData(0)=="bug");CHECK(category->itemData(1)=="feature");
        CHECK(category->itemData(2)=="improvement");CHECK(category->itemData(3)=="other");
    }
    CHECK(text && email && include && system && submit && count);
    if (!text || !email || !include || !system || !submit || !count) return;
    CHECK(!dialog.isWindow() && !dialog.windowHandle());
    CHECK(dialog.feedback().isEmpty() && dialog.email().isEmpty());
    CHECK(!include->isChecked() && !system->isEnabled() && system->isReadOnly());
    CHECK(system->toPlainText() == preview && dialog.includedSystemInfo().isEmpty());
    CHECK(!dialog.findChild<QLabel*>("FeedbackLogo")->pixmap().isNull());
    CHECK(!submit->isEnabled());
    CHECK(count->text() == "0 / 2000");

    include->click();
    CHECK(system->isEnabled() && system->isReadOnly() && dialog.includedSystemInfo() == preview);
    system->selectAll(); QTest::keyClicks(system, "No modifications");
    CHECK(system->toPlainText() == preview);
    include->click();
    CHECK(dialog.includedSystemInfo().isEmpty() && system->toPlainText() == preview);
    CHECK(!system->textCursor().hasSelection());
    email->setText(" tester@example.com ");
    CHECK(dialog.email() == "tester@example.com");

    text->setFocus();
    QTest::keyClicks(text, "First line");
    QTest::keyClick(text, Qt::Key_Return);
    QTest::keyClicks(text, "Second line");
    CHECK(dialog.feedback() == "First line\nSecond line");
    CHECK(dialog.isVisible()); // Enter inserts a newline, never submits/closes.
    if (!snapshot.isEmpty()) {
        QTest::qWait(20);
        CHECK(dialog.grab().save(snapshot));
    }

    text->setPlainText(QString(2100, 'a'));
    CHECK(dialog.feedback() == QString(2000, 'a'));
    CHECK(count->text() == "2000 / 2000");
    text->moveCursor(QTextCursor::End);
    QTest::keyClicks(text, "extra");
    CHECK(dialog.feedback() == QString(2000, 'a'));
    QTest::keyClick(text, Qt::Key_Backspace);
    CHECK(dialog.feedback().size() == 1999);

    text->setPlainText(QString(1990, 'a') + "tail");
    auto cursor = text->textCursor(); cursor.setPosition(1990); text->setTextCursor(cursor);
    QApplication::clipboard()->setText("01234567890123456789");
    text->paste();
    CHECK(dialog.feedback() == QString(1990, 'a') + "012345tail");
    text->undo();
    CHECK(dialog.feedback() == QString(1990, 'a') + "tail");
    text->redo();
    CHECK(dialog.feedback() == QString(1990, 'a') + "012345tail");

    text->selectAll();
    text->insertPlainText("Replacement");
    CHECK(dialog.feedback() == "Replacement");
    const auto emoji = QString::fromUcs4(U"\U0001f642");
    text->setPlainText(emoji.repeated(2001));
    CHECK(dialog.feedback() == emoji.repeated(2000));
    CHECK(count->text() == "2000 / 2000");
    text->setPlainText(QString(1999, 'a'));
    text->moveCursor(QTextCursor::End);
    QInputMethodEvent commit;
    commit.setCommitString(emoji + "extra");
    QApplication::sendEvent(text, &commit);
    CHECK(dialog.feedback() == QString(1999, 'a') + emoji);
    CHECK(submit->isEnabled());
    text->setPlainText(" \n\t");CHECK(!submit->isEnabled());
    dialog.findChild<QPushButton*>("FeedbackCancel")->click();
    CHECK(!dialog.isVisible() && dialog.result() == QDialog::Rejected);
    u::FeedbackDialog reopened(&owner, preview);
    CHECK(reopened.feedback().isEmpty() && reopened.email().isEmpty() && reopened.includedSystemInfo().isEmpty());

    const auto actual = u::FeedbackDialog::basicSystemInfo("Known GPU");
    const auto lines = actual.split('\n');
    CHECK(lines.size() == 4);
    CHECK(lines[0].startsWith("OS: ") && lines[1] == "GPU: Known GPU");
    CHECK(lines[2].startsWith("CPU: ") && lines[3].startsWith("RAM: "));
    CHECK(!actual.contains("Serial", Qt::CaseInsensitive) && !actual.contains("/proc"));
    CHECK(u::FeedbackDialog::basicSystemInfo({}).contains("GPU: Unavailable"));
}

void unavailableConfigurationTests()
{
    TestServiceConfig file;
    QWidget owner;
    for (const auto& bytes : {QByteArray("not JSON"),
             QByteArray("{\"version\":1,\"domain\":\"https://updates.example\",\"apiKey\":\"\"}")}) {
        CHECK(file.writeBytes(bytes));
        Network network;
        u::FeedbackService service(nullptr, &network);
        u::FeedbackDialog dialog(&owner, preview, &service);
        dialog.findChild<QPlainTextEdit*>("FeedbackText")->setPlainText("Valid feedback");
        CHECK(!service.configured() && !service.canSubmit());
        CHECK(!dialog.findChild<QPushButton*>("FeedbackSubmit")->isEnabled());
        CHECK(!service.submit({"bug", "Valid feedback", {}, {}}));
        CHECK(network.requests.empty());
    }
    CHECK(QFile::remove(file.path()));
    Network network;
    u::FeedbackService missing(nullptr, &network);
    CHECK(!missing.canSubmit() && !missing.submit({"bug", "Valid feedback", {}, {}}));
    CHECK(network.requests.empty());
}

void submissionTests()
{
    QWidget owner;owner.resize(700,800);owner.show();
    Network network;
    u::FeedbackService service(nullptr,&network);
    u::FeedbackDialog dialog(&owner,preview,&service);dialog.show();
    auto* text=dialog.findChild<QPlainTextEdit*>("FeedbackText");
    auto* email=dialog.findChild<QLineEdit*>("FeedbackEmail");
    auto* category=dialog.findChild<QComboBox*>("FeedbackCategory");
    auto* submit=dialog.findChild<QPushButton*>("FeedbackSubmit");
    text->setPlainText(QString::fromUtf8("Feature test — café 🙂"));email->setText(" tester@example.com ");category->setCurrentIndex(1);
    network.responses.push_back({});
    submit->click();
    CHECK(!submit->isEnabled() && text->isReadOnly() && !category->isEnabled());
    for(int i=0;i<5;++i)submit->click();
    CHECK(!service.submit({"other","Attempted duplicate",{}, {}}));
    CHECK(network.requests.size()==1);
    CHECK(waitFor([&]{return service.state()==u::FeedbackService::State::Succeeded;}));
    const auto payload=QJsonDocument::fromJson(network.payloads[0]).object();
    CHECK(payload.size()==6 && payload["source"]=="app");
    CHECK(payload["app_version"]==QCoreApplication::applicationVersion());
    CHECK(payload["category"]=="feature" && payload["message"].toString()==QString::fromUtf8("Feature test — café 🙂"));
    CHECK(payload["contact"]=="tester@example.com" && payload["system_info"].toString().isEmpty());
    const auto& request=network.requests[0];
    CHECK(request.url()==QUrl("https://updates.example/api/feedback-add"));
    CHECK(request.rawHeader("X-Vulkana-Token")=="test-client-token");
    CHECK(request.header(QNetworkRequest::ContentTypeHeader)=="application/json");
    CHECK(request.attribute(QNetworkRequest::RedirectPolicyAttribute)==QNetworkRequest::ManualRedirectPolicy);
    CHECK(request.attribute(QNetworkRequest::CookieLoadControlAttribute)==QNetworkRequest::Manual);
    CHECK(request.attribute(QNetworkRequest::CookieSaveControlAttribute)==QNetworkRequest::Manual);
    auto* overlay=dialog.findChild<QWidget*>("FeedbackThanksOverlay");
    CHECK(overlay && overlay->isVisible() && !overlay->isWindow() && !overlay->windowHandle());
    if(overlay)CHECK(overlay->parentWidget()==&dialog && overlay->geometry()==dialog.rect());
    CHECK(dialog.feedback().isEmpty() && dialog.email().isEmpty() && dialog.includedSystemInfo().isEmpty());
    CHECK(!submit->isEnabled());
    const auto thanksSnapshot=qEnvironmentVariable("IMAGEEDITOR_FEEDBACK_THANKS_REVIEW");
    if(!thanksSnapshot.isEmpty()) {QTest::qWait(20);CHECK(dialog.grab().save(thanksSnapshot));}
    auto* ok=dialog.findChild<QPushButton*>("FeedbackThanksOk");CHECK(ok);if(ok)ok->click();
    CHECK(!dialog.isVisible() && dialog.result()==QDialog::Accepted);
    dialog.show();CHECK(!dialog.findChild<QWidget*>("FeedbackThanksOverlay"));
    CHECK(dialog.feedback().isEmpty() && dialog.email().isEmpty());
    text->setPlainText("Second explicit test");
    dialog.findChild<QCheckBox*>("FeedbackIncludeSystem")->click();
    CHECK(!submit->isEnabled()); // Closing/reusing the dialog did not reset cooldown.
    CHECK(waitFor([&]{return submit->isEnabled();}));
    network.responses.push_back({204,{}});submit->click();
    CHECK(waitFor([&]{return service.state()==u::FeedbackService::State::Succeeded;}));
    CHECK(QJsonDocument::fromJson(network.payloads[1]).object()["system_info"]==preview);
    CHECK(!service.canSubmit());

    // An in-flight request remains blocked even after the minimum cooldown.
    Network stalled;
    u::FeedbackService waiting(nullptr,&stalled);
    stalled.responses.push_back({200,{},true});
    CHECK(waiting.submit({"bug","Slow report",{}, {}}));
    QTest::qWait(2050);
    CHECK(waiting.busy() && !waiting.canSubmit());
    CHECK(!waiting.submit({"bug","Duplicate",{}, {}}));
    waiting.reset();CHECK(stalled.aborted==1);CHECK(waiting.canSubmit());
    CHECK(stalled.requests.size()==1);

    // Success must not require a service reset/restart to permit another report.
    // The completed form still stays disabled beneath its thank-you overlay.
    CHECK(service.state()==u::FeedbackService::State::Succeeded);
    CHECK(service.canSubmit());
    CHECK(!submit->isEnabled());
    CHECK(network.requests.size()==2);
    dialog.reject();
}

void errorTests()
{
    QWidget owner;
    for(const auto& response: {Response{400},Response{401},Response{413},Response{429,"<b>Private server detail</b>",false,QNetworkReply::NoError,"3"},
            Response{500},Response{302},Response{200,"{\"ok\":false}"},Response{200,"<html>Unexpected page</html>"},
            Response{200,{},false,QNetworkReply::SslHandshakeFailedError},
            Response{201,QByteArray(65537,'x')},Response{200,{},true}}) {
        Network network;network.responses.push_back(response);
        u::FeedbackService service(nullptr,&network,40);
        u::FeedbackDialog dialog(&owner,preview,&service);dialog.show();
        auto* text=dialog.findChild<QPlainTextEdit*>("FeedbackText");text->setPlainText("Keep my report");
        auto* submit=dialog.findChild<QPushButton*>("FeedbackSubmit");submit->click();
        CHECK(waitFor([&]{return service.state()==u::FeedbackService::State::Error;}));
        CHECK(dialog.feedback()=="Keep my report");
        CHECK(!submit->isEnabled() && !text->isReadOnly());
        CHECK(!dialog.findChild<QWidget*>("FeedbackThanksOverlay"));
        CHECK(!service.message().isEmpty() && !service.message().contains("Private server detail"));
        CHECK(network.requests.size()==1); // No implicit redirect or retry.
        dialog.reject();
        u::FeedbackDialog reopened(&owner,preview,&service);
        reopened.findChild<QPlainTextEdit*>("FeedbackText")->setPlainText("Reopened too soon");
        CHECK(!reopened.findChild<QPushButton*>("FeedbackSubmit")->isEnabled());
    }
    Network network;
    u::FeedbackService service(nullptr,&network);
    CHECK(!service.submit({"wrong","Test",{}, {}}));
    CHECK(!service.submit({"bug","  ",{}, {}}));
    CHECK(!service.submit({"bug",QString(2001,'a'),{}, {}}));
    CHECK(network.requests.isEmpty());
    // Closing/deleting while pending disconnects callbacks before abort.
    network.responses.push_back({200,{},true});
    {
        u::FeedbackDialog dialog(&owner,preview,&service);dialog.show();
        dialog.findChild<QPlainTextEdit*>("FeedbackText")->setPlainText("Pending cancellation");
        dialog.findChild<QPushButton*>("FeedbackSubmit")->click();CHECK(service.busy());
    }
    QTest::qWait(10);CHECK(!service.busy() && network.aborted==1);
}

void liveSmokeTest(bool includeSyntheticSystem)
{
    // Explicitly requested only; never part of CTest/default test execution.
    QWidget owner;owner.resize(700,800);owner.show();
    u::FeedbackService service;
    u::FeedbackDialog dialog(&owner,includeSyntheticSystem?preview:QString{},&service);dialog.show();
    dialog.findChild<QComboBox*>("FeedbackCategory")->setCurrentIndex(3);
    dialog.findChild<QPlainTextEdit*>("FeedbackText")->setPlainText(
        "Vulkana integration smoke test (owner-authorized). Testing the in-app feedback submission and thank-you overlay. No reply needed; this test report can be deleted.");
    if(includeSyntheticSystem)dialog.findChild<QCheckBox*>("FeedbackIncludeSystem")->click();
    dialog.findChild<QPushButton*>("FeedbackSubmit")->click();
    CHECK(waitFor([&]{return !service.busy();},10000));
    CHECK(service.state()==u::FeedbackService::State::Succeeded);
    auto* ok=dialog.findChild<QPushButton*>("FeedbackThanksOk");CHECK(ok);if(ok)ok->click();
    CHECK(!dialog.isVisible() && dialog.result()==QDialog::Accepted);
    std::cout<<"Live API smoke: "<<(service.state()==u::FeedbackService::State::Error?service.message().toStdString():"completed")
             <<"; no contact or real system information sent; system_info="<<(includeSyntheticSystem?"synthetic":"empty")
             <<"; failures="<<failures<<'\n';
}

void ownershipTests()
{
    u::MainWindow window(nullptr, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1280, 850); window.show();
    const auto state = window.editorSession().document()->contentState();
    const auto depth = window.editorSession().history().undoDepth();
    const auto tool = window.editorSession().activeTool();
    auto* action = window.findChild<QAction*>("FeedbackAction");
    CHECK(action); if (!action) return;
    bool visited = false;
    QTimer::singleShot(0, &window, [&] {
        auto* card = window.findChild<QDialog*>("FeedbackDialog");
        CHECK(card && card->isVisible());
        if (!card) return;
        visited = true;
        CHECK(!card->isWindow() && !card->windowHandle());
        CHECK(card->parentWidget()->objectName() == "WorkspaceDialogShield");
        CHECK(!QApplication::topLevelWidgets().contains(card));
        CHECK((card->geometry().center() - card->parentWidget()->rect().center()).manhattanLength() <= 2);
        CHECK(card->parentWidget()->rect().contains(card->geometry()));
        auto* text = card->findChild<QPlainTextEdit*>("FeedbackText");
        CHECK(QApplication::focusWidget() == text);
        QTest::keyClicks(text, "b");
        CHECK(text->toPlainText() == "b");
        QTest::keyClick(text, Qt::Key_Escape);
    });
    // Safety net: a regression must not leave a nested overlay event loop stuck.
    QTimer::singleShot(2000, &window, [&] {
        if (auto* card = window.findChild<QDialog*>("FeedbackDialog")) { CHECK(false); card->reject(); }
    });
    action->trigger();
    CHECK(visited && !window.findChild<QDialog*>("FeedbackDialog"));
    CHECK(window.editorSession().document()->contentState() == state);
    CHECK(window.editorSession().history().undoDepth() == depth);
    CHECK(window.editorSession().activeTool() == tool);
    window.close();
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName("ImageEditorTests"); app.setApplicationName("Feedback");
    app.setApplicationVersion(QString::fromLatin1(imageeditor::platform::buildVersion));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    u::applyEditorTheme(app);
    app.setWindowIcon(QIcon(QStringLiteral(":/Vulkana512.png")));
    if(app.arguments().contains("--live-smoke")) {liveSmokeTest(app.arguments().contains("--with-synthetic-system"));return failures?1:0;}
    TestServiceConfig services;
    unavailableConfigurationTests();
    formTests(app.arguments().value(1));
    submissionTests();
    errorTests();
    ownershipTests();
    std::cout << "Feedback checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
