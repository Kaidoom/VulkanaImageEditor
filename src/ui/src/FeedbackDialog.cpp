#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/FeedbackDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSysInfo>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>
#ifdef Q_OS_LINUX
#include <sys/sysinfo.h>
#endif

namespace imageeditor::ui {
namespace {
qsizetype characterCount(const QString& text)
{
    // Count Unicode code points, so supplementary characters (e.g. emoji)
    // consume one character and are never cut between surrogate pairs.
    return text.size() - std::count_if(text.begin(), text.end(), [](QChar c) { return c.isLowSurrogate(); });
}

class FeedbackEditor final : public QPlainTextEdit {
public:
    explicit FeedbackEditor(QWidget* parent) : QPlainTextEdit(parent)
    {
        connect(document(), &QTextDocument::contentsChange, this, [this](int position, int, int added) {
            if (limiting_) return;
            const auto text = toPlainText();
            auto excess = characterCount(text) - FeedbackDialog::maximumCharacters;
            if (excess <= 0) return;
            const QScopedValueRollback guard(limiting_, true);
            // Trim the end of the incoming edit, not the existing suffix. This
            // also covers paste, input-method commits, drops and undo/redo.
            int end = std::min(position + added, int(text.size()));
            int start = end;
            while (excess-- > 0 && start > position) {
                --start;
                if (text[start].isLowSurrogate() && start > position && text[start - 1].isHighSurrogate()) --start;
            }
            QTextCursor trim(document());
            trim.joinPreviousEditBlock();
            trim.setPosition(start);
            trim.setPosition(end, QTextCursor::KeepAnchor);
            trim.removeSelectedText();
            trim.endEditBlock();
        });
    }
private:
    bool limiting_ {false};
};

QString oneLine(QString value)
{
    // Only a model/name, never arbitrary diagnostics or multiline data.
    return value.simplified().left(256);
}
}

QString FeedbackDialog::basicSystemInfo(const QString& rendererDeviceName)
{
    QString cpu = QSysInfo::currentCpuArchitecture();
    QString ram = i18n::text("feedback.dialog.unavailable");
#ifdef Q_OS_LINUX
    // Read only the first model-name field; never expose the raw cpuinfo file
    // (some architectures include serial numbers there).
    QFile cpuInfo(QStringLiteral("/proc/cpuinfo"));
    if (cpuInfo.open(QIODevice::ReadOnly)) {
        int budget = 64 * 1024;
        while (budget > 0) {
            const auto line = cpuInfo.readLine(1024);
            if (line.isEmpty()) break;
            budget -= int(line.size());
            const auto colon = line.indexOf(':');
            const auto key = line.first(std::max(qsizetype(0), colon)).trimmed();
            if (colon >= 0 && (key == "model name" || key == "Hardware")) {
                cpu = QString::fromUtf8(line.mid(colon + 1)).trimmed();
                break;
            }
        }
    }
    struct sysinfo memory {};
    if (::sysinfo(&memory) == 0) {
        const double gib = double(memory.totalram) * double(memory.mem_unit) / (1024.0 * 1024.0 * 1024.0);
        ram = i18n::text("feedback.dialog.1.gib").arg(QLocale().toString(gib, 'f', 1));
    }
#endif
    const auto os = QSysInfo::prettyProductName();
    return i18n::text("feedback.dialog.os.1.gpu.2.cpu.3.ram.4")
        .arg(oneLine(os.isEmpty() ? QSysInfo::productType() : os),
            rendererDeviceName.isEmpty() ? i18n::text("feedback.dialog.unavailable") : oneLine(rendererDeviceName),
            cpu.isEmpty() ? i18n::text("feedback.dialog.unavailable") : oneLine(cpu), ram);
}

FeedbackDialog::FeedbackDialog(QWidget* parent, const QString& systemPreview, FeedbackService* service)
    : QDialog(parent, Qt::SubWindow), service_(service ? service : new FeedbackService(this))
{
    setObjectName(QStringLiteral("FeedbackDialog"));
    i18n::setWindowTitle(this, i18n::text("feedback.dialog.submit.feedback.bug.report"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedWidth(580);
    auto* shell = new QVBoxLayout(this);
    shell->setContentsMargins(0,0,0,0);
    form_ = new QWidget(this);
    shell->addWidget(form_);
    auto* layout = new QVBoxLayout(form_);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(10);
    auto* header = new QHBoxLayout;
    header->setSpacing(16);
    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("FeedbackLogo"));
    logo->setPixmap(QApplication::windowIcon().pixmap(QSize(64, 64), devicePixelRatioF()));
    header->addWidget(logo, 0, Qt::AlignTop);
    auto* greeting = new QVBoxLayout;
    auto* title = i18n::make<QLabel>(i18n::text("feedback.dialog.we.appreciate.your.feedback"), this);
    auto font = title->font(); font.setPointSizeF(font.pointSizeF() + 2); font.setBold(true); title->setFont(font);
    greeting->addWidget(title);
    auto* intro = i18n::make<QLabel>(i18n::text("feedback.dialog.share.an.idea.or.tell.us.what.went.wrong"), this);
    intro->setWordWrap(true);
    greeting->addWidget(intro);
    header->addLayout(greeting, 1);
    layout->addLayout(header);

    auto* categoryRow = new QHBoxLayout;
    auto* categoryLabel = i18n::make<QLabel>(i18n::text("feedback.dialog.category"), this);
    categoryRow->addWidget(categoryLabel);
    category_ = new QComboBox(this);
    category_->setObjectName(QStringLiteral("FeedbackCategory"));
    category_->setFocusPolicy(Qt::ClickFocus); // Initial card focus belongs to the message editor.
    i18n::addItem(category_, i18n::text("feedback.dialog.bug.report"), QStringLiteral("bug"));
    i18n::addItem(category_, i18n::text("feedback.dialog.feature.request"), QStringLiteral("feature"));
    i18n::addItem(category_, i18n::text("feedback.dialog.improvement"), QStringLiteral("improvement"));
    i18n::addItem(category_, i18n::text("feedback.dialog.other"), QStringLiteral("other"));
    categoryLabel->setBuddy(category_);
    categoryRow->addWidget(category_,1);
    layout->addLayout(categoryRow);

    auto* feedbackRow = new QHBoxLayout;
    auto* feedbackLabel = i18n::make<QLabel>(i18n::text("feedback.dialog.feedback.or.bug.report"), this);
    feedbackRow->addWidget(feedbackLabel);
    feedbackRow->addStretch();
    auto* count = new QLabel(this);
    count->setObjectName(QStringLiteral("FeedbackCharacterCount"));
    feedbackRow->addWidget(count);
    layout->addLayout(feedbackRow);
    feedback_ = new FeedbackEditor(this);
    feedback_->setObjectName(QStringLiteral("FeedbackText"));
    i18n::setAccessibleName(feedback_, i18n::text("feedback.dialog.feedback.or.bug.report"));
    i18n::setPlaceholderText(feedback_, i18n::text("feedback.dialog.describe.your.feedback.here"));
    feedback_->setFixedHeight(150);
    feedbackLabel->setBuddy(feedback_);
    layout->addWidget(feedback_);
    const auto updateCount = [this, count] {
        i18n::setText(count, i18n::text("feedback.dialog.1.2").arg(characterCount(feedback())).arg(maximumCharacters));
    };
    connect(feedback_, &QPlainTextEdit::textChanged, this, updateCount);
    updateCount();

    auto* emailLabel = i18n::make<QLabel>(i18n::text("feedback.dialog.email.optional.for.follow.up"), this);
    layout->addWidget(emailLabel);
    email_ = new QLineEdit(this);
    email_->setObjectName(QStringLiteral("FeedbackEmail"));
    email_->setMaxLength(254);
    i18n::setPlaceholderText(email_, i18n::text("feedback.dialog.you.example.com"));
    email_->setAccessibleName(emailLabel->text());
    emailLabel->setBuddy(email_);
    layout->addWidget(email_);

    includeSystem_ = i18n::make<QCheckBox>(i18n::text("feedback.dialog.submit.system.info"), this);
    includeSystem_->setObjectName(QStringLiteral("FeedbackIncludeSystem"));
    includeSystem_->setChecked(false);
    layout->addWidget(includeSystem_);
    system_ = new QPlainTextEdit(this);
    system_->setObjectName(QStringLiteral("FeedbackSystemPreview"));
    i18n::setAccessibleName(system_, i18n::text("feedback.dialog.system.info.preview"));
    system_->setReadOnly(true);
    system_->setPlainText(systemPreview);
    system_->setFixedHeight(system_->fontMetrics().lineSpacing() * 4 + 18);
    system_->setEnabled(false);
    layout->addWidget(system_);
    connect(includeSystem_, &QCheckBox::toggled, this, [this](bool included) {
        if (!included) {
            auto cursor = system_->textCursor();
            cursor.clearSelection();
            system_->setTextCursor(cursor);
        }
        system_->setEnabled(included);
    });
    auto* privacy = i18n::make<QLabel>(i18n::text("feedback.dialog.system.details.are.included.only.if.checked.your.message"), this);
    privacy->setWordWrap(true);
    layout->addWidget(privacy);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("FeedbackServiceStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setFixedHeight(status_->fontMetrics().lineSpacing()*2);
    layout->addWidget(status_);
    auto* footer = new QHBoxLayout;
    footer->addStretch();
    auto* cancel = i18n::make<QPushButton>(i18n::text("feedback.dialog.cancel"), this);
    cancel->setObjectName(QStringLiteral("FeedbackCancel"));
    cancel->setAutoDefault(false);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer->addWidget(cancel);
    submit_ = i18n::make<QPushButton>(i18n::text("feedback.dialog.submit"), this);
    submit_->setObjectName(QStringLiteral("FeedbackSubmit"));
    submit_->setAutoDefault(false);
    connect(submit_, &QPushButton::clicked, this, [this] {
        service_->submit({category_->currentData().toString(),feedback(),email(),includedSystemInfo()});
    });
    footer->addWidget(submit_);
    layout->addLayout(footer);
    for (auto* label : {intro, count, privacy, status_}) {
        auto ink = label->palette();
        ink.setColor(QPalette::WindowText, themeColor(ThemeColor::SecondaryText));
        label->setPalette(ink);
    }
    setFixedHeight(layout->totalHeightForWidth(width()));
    service_->reset();
    service_->onChanged=[this]{refreshSubmission();};
    connect(feedback_,&QPlainTextEdit::textChanged,this,[this]{refreshSubmission();});
    refreshSubmission();
}

FeedbackDialog::~FeedbackDialog() { service_->onChanged={}; service_->reset(); }
void FeedbackDialog::clearFields()
{
    feedback_->clear();email_->clear();includeSystem_->setChecked(false);category_->setCurrentIndex(0);
}
void FeedbackDialog::done(int result)
{
    // Prevent callbacks from recreating a confirmation during teardown. The
    // app-owned service retains its cooldown even if this card is reopened.
    service_->onChanged={};service_->reset();
    clearFields();
    if(thanks_){delete thanks_;thanks_=nullptr;}
    form_->setEnabled(true);
    service_->onChanged=[this]{refreshSubmission();};
    refreshSubmission();
    QDialog::done(result);
}
void FeedbackDialog::refreshSubmission()
{
    const bool busy=service_->busy();
    feedback_->setReadOnly(busy);email_->setReadOnly(busy);
    category_->setEnabled(!busy);includeSystem_->setEnabled(!busy);
    submit_->setEnabled(!thanks_ && service_->canSubmit() && !feedback().trimmed().isEmpty());
    i18n::setText(submit_, busy?i18n::text("feedback.dialog.sending"):i18n::text("feedback.dialog.submit"));
    i18n::setText(status_, service_->configured() ? service_->message() : i18n::text("feedback.dialog.feedback.is.not.configured.for.this.build"));
    i18n::setToolTip(submit_, service_->configured() ? QString{} : i18n::text("feedback.dialog.feedback.is.not.configured.for.this.build"));
    if(service_->state()==FeedbackService::State::Succeeded && !thanks_)showThanks();
}
void FeedbackDialog::showThanks()
{
    // No QMessageBox, QDialog::exec, native handle, or independent window.
    auto* overlay=new QFrame(this);
    thanks_=overlay; // Set before clearing fields, which emits textChanged.
    overlay->setObjectName(QStringLiteral("FeedbackThanksOverlay"));
    overlay->setGeometry(rect());
    clearFields();
    form_->setEnabled(false);
    const auto accent=themeColor(ThemeColor::Accent);
    auto veil=themeColor(ThemeColor::Background);veil.setAlpha(210);
    overlay->setStyleSheet(QStringLiteral(
        "QFrame#FeedbackThanksOverlay { background: %1; }"
        "QFrame#FeedbackThanksCard { background: %2; border: 1px solid %3; border-radius: 8px; }")
        .arg(veil.name(QColor::HexArgb),themeColor(ThemeColor::Surface).name(),accent.name()));
    auto* outer=new QVBoxLayout(overlay);
    outer->setContentsMargins(24,24,24,24);outer->addStretch();
    auto* card=new QFrame(overlay);card->setObjectName(QStringLiteral("FeedbackThanksCard"));
    auto* content=new QVBoxLayout(card);content->setContentsMargins(24,24,24,24);content->setSpacing(14);
    auto* icon=new QLabel(card);icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(toolGlyph(ToolGlyph::CheckCircle,accent).pixmap(QSize(36,36),devicePixelRatioF()));
    content->addWidget(icon);
    auto* title=i18n::make<QLabel>(i18n::text("feedback.dialog.thank.you.for.your.feedback"), card);title->setAlignment(Qt::AlignCenter);
    auto font=title->font();font.setBold(true);title->setFont(font);
    auto ink=title->palette();ink.setColor(QPalette::WindowText,accent);title->setPalette(ink);
    content->addWidget(title);
    auto* detail=i18n::make<QLabel>(i18n::text("feedback.dialog.your.report.has.been.submitted.thank.you.for.helping"), card);
    detail->setWordWrap(true);detail->setAlignment(Qt::AlignCenter);content->addWidget(detail);
    auto* ok=i18n::make<QPushButton>(i18n::text("feedback.dialog.ok"), card);ok->setObjectName(QStringLiteral("FeedbackThanksOk"));
    ok->setAutoDefault(false);content->addWidget(ok,0,Qt::AlignHCenter);
    connect(ok,&QPushButton::clicked,this,&QDialog::accept);
    outer->addWidget(card);outer->addStretch();
    overlay->show();overlay->raise();ok->setFocus(Qt::OtherFocusReason);
}
void FeedbackDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if(thanks_)thanks_->setGeometry(rect());
}

QString FeedbackDialog::feedback() const { return feedback_->toPlainText(); }
QString FeedbackDialog::email() const { return email_->text().trimmed(); }
QString FeedbackDialog::includedSystemInfo() const
{
    return includeSystem_->isChecked() ? system_->toPlainText() : QString();
}
} // namespace imageeditor::ui
