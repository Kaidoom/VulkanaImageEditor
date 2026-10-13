#include "imageeditor/ui/ProjectIntegrationDialog.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/platform/ProjectIntegration.hpp"
#include <QCoreApplication>
#include <QSettings>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace imageeditor::ui {
ProjectIntegrationDialog::ProjectIntegrationDialog(bool firstRun, QWidget* parent)
    : QDialog(parent, Qt::SubWindow)
{
    setObjectName(QStringLiteral("ProjectIntegrationDialog"));
    i18n::setWindowTitle(this, i18n::text("project.integration.title"));
    setFixedWidth(450);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18); layout->setSpacing(14);
    auto* title = i18n::make<QLabel>(i18n::text("project.integration.title"), this);
    auto font = title->font(); font.setBold(true); title->setFont(font); layout->addWidget(title);
    auto* description = i18n::make<QLabel>(i18n::text("project.integration.description"), this);
    description->setWordWrap(true); layout->addWidget(description);
    auto* location = i18n::make<QLabel>(i18n::text("project.integration.location"), this);
    location->setWordWrap(true); location->setObjectName(QStringLiteral("MutedLabel")); layout->addWidget(location);
    auto* status = new QLabel(this); status->setObjectName(QStringLiteral("ProjectIntegrationError"));
    status->setWordWrap(true); status->hide(); layout->addWidget(status);
    auto* buttons = new QHBoxLayout;
    const bool registered = QSettings().value(platform::projectIntegrationChoiceKey).toString() == "registered";
    if (registered) {
        auto* remove = i18n::make<QPushButton>(i18n::text("project.integration.remove"), this);
        remove->setObjectName(QStringLiteral("ProjectIntegrationRemove")); buttons->addWidget(remove);
        connect(remove, &QPushButton::clicked, this, [this, status] {
            QSettings settings;
            const auto error = platform::unregisterProjectFiles(settings);
            if (error.isEmpty()) accept(); else { status->setText(error); status->show(); adjustSize(); }
        });
    }
    buttons->addStretch();
    auto* decline = i18n::make<QPushButton>(firstRun ? i18n::text("project.integration.decline") : i18n::text("project.integration.close"), this);
    decline->setObjectName(QStringLiteral("ProjectIntegrationDecline")); buttons->addWidget(decline);
    connect(decline, &QPushButton::clicked, this, &QDialog::reject);
    auto* install = i18n::make<QPushButton>(i18n::text("project.integration.register"), this);
    install->setObjectName(QStringLiteral("ProjectIntegrationRegister")); buttons->addWidget(install);
    connect(install, &QPushButton::clicked, this, [this, status] {
        QSettings settings;
        const auto error = platform::registerProjectFiles(settings, QCoreApplication::applicationFilePath());
        if (error.isEmpty()) accept(); else { status->setText(error); status->show(); adjustSize(); }
    });
    if (firstRun) connect(this, &QDialog::rejected, this, [] {
        QSettings().setValue(platform::projectIntegrationChoiceKey, "declined");
    });
    layout->addLayout(buttons);
}
}
