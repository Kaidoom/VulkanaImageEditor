#include "imageeditor/ui/McpPanel.hpp"
#include "imageeditor/ui/McpController.hpp"
#include "imageeditor/ui/CollapsibleSection.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/mcp/Integration.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QToolButton>
#include <QVBoxLayout>
namespace imageeditor::ui {
McpPanel::McpPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("PreferencesMcpPage");
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); outer->addWidget(scroll);
    auto* body = new QWidget(scroll); auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 12, 16, 12); layout->setSpacing(10); scroll->setWidget(body);
    auto* row = new QHBoxLayout;
    status_ = new QLabel(body); status_->setObjectName("McpServerStatus"); row->addWidget(status_, 1);
    toggle_ = new QPushButton(body); toggle_->setObjectName("McpServerToggle"); toggle_->setAutoDefault(false);
    row->addWidget(toggle_); layout->addLayout(row);
    autoStart_ = i18n::make<QCheckBox>(i18n::text("mcp.preferences.autostart"), body);
    autoStart_->setObjectName("McpAutoStart"); layout->addWidget(autoStart_);
    error_ = new QLabel(body); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); error_->setObjectName("McpServerError"); layout->addWidget(error_);
    auto label = [&](const char* key, const char* name) {
        auto* value = i18n::make<QLabel>(i18n::text(key), body);
        value->setWordWrap(true); value->setTextFormat(Qt::PlainText); value->setObjectName(name); layout->addWidget(value); return value;
    };
    label("mcp.preferences.security", "McpSecurityExplanation");
    label("mcp.preferences.connection", "SectionLabel");
    label("mcp.preferences.local", "MutedLabel");
    auto* form = new QFormLayout; form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto field = [&](const char* key, const char* name) {
        auto* value = new QLineEdit(body); value->setReadOnly(true); value->setObjectName(name);
        value->setLayoutDirection(Qt::LeftToRight); value->setMinimumWidth(0);
        auto* title = i18n::make<QLabel>(i18n::text(key), body); title->setBuddy(value); form->addRow(title, value); return value;
    };
    command_ = field("mcp.preferences.command", "McpBridgeCommand");
    connection_ = field("mcp.preferences.endpoint", "McpConnectionEndpoint");
    layout->addLayout(form);
    auto* copyRow = new QHBoxLayout;
    copy_ = i18n::make<QPushButton>(toolGlyph(ToolGlyph::Copy), i18n::text("mcp.preferences.copy"), body);
    copy_->setObjectName("McpCopyConfiguration"); copy_->setAutoDefault(false); copyRow->addWidget(copy_);
    copied_ = i18n::make<QLabel>(i18n::text("mcp.preferences.copied"), body); copied_->setObjectName("McpConfigurationCopied");
    copied_->hide(); copyRow->addWidget(copied_); copyRow->addStretch(); layout->addLayout(copyRow);
    QVBoxLayout* advancedLayout;
    auto* advanced = collapsibleSection(i18n::text("mcp.preferences.advanced"), "McpAdvanced", advancedLayout);
    custom_ = i18n::make<QCheckBox>(i18n::text("mcp.preferences.custom"), advanced);
    custom_->setObjectName("McpUseCustomEndpoint"); advancedLayout->addWidget(custom_);
    endpoint_ = new QLineEdit(advanced); endpoint_->setObjectName("McpCustomEndpoint"); endpoint_->setLayoutDirection(Qt::LeftToRight);
    endpoint_->setPlaceholderText(mcp::defaultSocketPath()); advancedLayout->addWidget(endpoint_);
    pending_ = i18n::make<QLabel>(i18n::text("mcp.preferences.next.start"), advanced); pending_->setWordWrap(true); advancedLayout->addWidget(pending_);
    advanced->findChild<QToolButton*>("McpAdvancedSectionHeader")->setChecked(false); layout->addWidget(advanced);
    label("mcp.preferences.apply.note", "MutedLabel");
    layout->addStretch();
    connect(autoStart_, &QCheckBox::toggled, this, [this] { changed(); });
    connect(custom_, &QCheckBox::toggled, this, [this] { changed(); });
    connect(endpoint_, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(toggle_, &QPushButton::clicked, this, [this] {
        if (!controller_) return;
        if (controller_->running()) controller_->stop();
        else {
            QString error;
            if (!validate(&error)) { error_->setText(error); error_->show(); return; }
            controller_->start(settings().customEndpoint);
        }
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        if ((!controller_ || !controller_->running()) && !validate()) return;
        const auto config = mcp::openCodeConfiguration(mcp::installedBridge(), connectionEndpoint());
        if (config.isEmpty()) return;
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented)));
        copied_->show();
    });
    setSettings({});
}
void McpPanel::setController(McpController* controller)
{
    if (controller_ != controller) {
        if (controller_) disconnect(controller_, nullptr, this, nullptr);
        controller_ = controller;
        if (controller_) {
            connect(controller_, &McpController::statusChanged, this, &McpPanel::refreshStatus);
            connect(controller_, &QObject::destroyed, this, [this] { controller_ = nullptr; refreshStatus(); });
        }
    }
    refreshStatus();
}
void McpPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshStatus();
}
void McpPanel::setSettings(const platform::McpSettings& value)
{
    const QSignalBlocker a(autoStart_), b(custom_), c(endpoint_);
    autoStart_->setChecked(value.autoStart); custom_->setChecked(!value.customEndpoint.isEmpty()); endpoint_->setText(value.customEndpoint);
    refreshStatus();
}
platform::McpSettings McpPanel::settings() const
{ return {autoStart_->isChecked(), custom_->isChecked() ? endpoint_->text().trimmed() : QString()}; }
bool McpPanel::validate(QString* error) const
{
    QString message;
    if (custom_->isChecked()) message = mcp::validateEndpoint(settings().customEndpoint);
    if (error) *error = message;
    return message.isEmpty();
}
QString McpPanel::connectionEndpoint() const
{
    if (controller_ && controller_->running()) return controller_->endpoint();
    const auto custom = settings().customEndpoint;
    return custom.isEmpty() ? mcp::defaultSocketPath() : mcp::normalizeEndpoint(custom);
}
void McpPanel::changed()
{ copied_->hide(); refreshStatus(); if (onChanged) onChanged(); }
void McpPanel::refreshStatus()
{
    const auto state = controller_ ? controller_->status() : McpController::Status::Stopped;
    const auto key = state == McpController::Status::Stopped ? "mcp.preferences.stopped"
        : state == McpController::Status::Listening ? "mcp.preferences.listening"
        : state == McpController::Status::Connected ? "mcp.preferences.connected"
        : state == McpController::Status::Stopping ? "mcp.preferences.stopping" : "mcp.preferences.error";
    i18n::setText(status_, state == McpController::Status::Connected
        ? i18n::text(key).arg(controller_->clientCount()) : i18n::text(key));
    const bool running = controller_ && controller_->running();
    i18n::setText(toggle_, i18n::text(running ? "mcp.preferences.stop" : "mcp.preferences.start"));
    toggle_->setIcon(toolGlyph(running ? ToolGlyph::Close : ToolGlyph::Play));
    toggle_->setEnabled(controller_ && state != McpController::Status::Stopping);
    endpoint_->setEnabled(custom_->isChecked());
    const auto bridge = mcp::installedBridge();
    const auto command = QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(bridge.command)).toJson(QJsonDocument::Compact));
    if (command_->text() != command) { command_->setText(command); command_->setCursorPosition(0); }
    command_->setToolTip(command);
    const auto endpoint = connectionEndpoint();
    if (connection_->text() != endpoint) { connection_->setText(endpoint); connection_->setCursorPosition(0); }
    connection_->setToolTip(endpoint);
    QString error;
    if (state == McpController::Status::Stopping) error = i18n::text("mcp.preferences.finishing");
    else if (controller_ && !controller_->error().isEmpty()) error = controller_->error();
    else if (!bridge) error = bridge.error;
    else if (!running) (void)validate(&error);
    error_->setText(error); error_->setVisible(!error.isEmpty());
    copy_->setEnabled(bool(bridge) && (running || validate()));
}
}
