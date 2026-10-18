#pragma once

#include "imageeditor/ui/Localization.hpp"

#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace imageeditor::ui {

// Shared compact section header. The theme supplies its subtle expanded stripe;
// callers own the contents and can change the header's initial checked state.
inline QWidget* collapsibleSection(const i18n::Message& title,
    const QString& objectName, QVBoxLayout*& sectionLayout)
{
    auto* container = new QWidget;
    container->setObjectName(objectName + QStringLiteral("Section"));
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(9);

    auto* header = new QToolButton(container);
    header->setObjectName(objectName + QStringLiteral("SectionHeader"));
    header->setProperty("propertiesSectionHeader", true);
    i18n::setText(header, title);
    header->setCheckable(true);
    header->setChecked(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(Qt::DownArrow);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(header);

    auto* body = new QWidget(container);
    body->setObjectName(objectName + QStringLiteral("SectionBody"));
    sectionLayout = new QVBoxLayout(body);
    sectionLayout->setContentsMargins(8, 0, 0, 0);
    sectionLayout->setSpacing(9);
    layout->addWidget(body);
    QObject::connect(header, &QToolButton::toggled, body, &QWidget::setVisible);
    QObject::connect(header, &QToolButton::toggled, header,
        [header](bool expanded) {
            header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        });
    return container;
}

} // namespace imageeditor::ui
