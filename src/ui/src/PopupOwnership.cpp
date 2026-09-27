#include "imageeditor/ui/PopupOwnership.hpp"
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QEvent>
#include <QPointer>
#include <QVariant>
#include <QWidget>
#include <QWindow>

namespace imageeditor::ui {
namespace {
constexpr auto linkName = "VulkanaPopupOrigin";
class Origin final : public QObject {
public:
    Origin(QWidget* popup, QWidget* original) : QObject(popup), widget(original) { setObjectName(QLatin1String(linkName)); }
    QPointer<QWidget> widget;
};
class Policy final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show) {
            auto* popup = qobject_cast<QWidget*>(watched);
            auto* combo = popup ? qobject_cast<QComboBox*>(popupLogicalParent(popup)) : nullptr;
            if (popup && popup->isWindow() && popup->windowHandle() && combo
                && popup->parentWidget() != combo) {
                // Native reparenting stops enabled-state inheritance from
                // the combo. A popup created before document loading can
                // retain WA_Disabled even after its control becomes enabled;
                // QWidgetWindow then closes it on the opening mouse release.
                // Restore that logical relationship before Qt shows it.
                popup->setEnabled(combo->isEnabled());
                // Qt 6.11 computes a combo's anchor in QWidget::window()
                // coordinates (our child plane), even with a corrected native
                // owner. Its Wayland positioner override must use the real
                // owner's coordinates. Keep Qt's flipping/constraint policy.
                // Source: qtbase qwaylandxdgshell.cpp createPositioner().
                auto* owner = popupTopLevelOwner(combo);
                if (owner && !combo->isEditable() && QGuiApplication::platformName() == "wayland") popup->windowHandle()->setProperty("_q_waylandPopupAnchorRect",
                    QRect(combo->mapTo(owner, QPoint()), combo->size()));
            }
            return false;
        }
        if (event->type() != QEvent::ParentChange && event->type() != QEvent::Polish) return false;
        auto* popup = qobject_cast<QWidget*>(watched);
        if (!popup || !popup->parentWidget()) return false;
        const auto type = popup->windowType();
        // Dialogs may be constructed as windows, then deliberately embedded
        // into WorkspaceDialog. Their call sites choose the native owner;
        // never interfere with that construction/embedding transition.
        if (type != Qt::Popup && type != Qt::ToolTip) return false;
        preparePopupOwnership(popup, popup->parentWidget());
        return false;
    }
};
}
void preparePopupOwnership(QWidget* popup, QWidget* origin)
{
    auto* owner = popupTopLevelOwner(origin);
    if (!popup || !origin || !owner || owner == origin->window() || popup->parentWidget() == owner) return;
    if (!popup->findChild<QObject*>(QLatin1String(linkName), Qt::FindDirectChildrenOnly)) {
        new Origin(popup, origin);
        // Native ownership changes, not control lifetime. A deleted combo or
        // editor must not leave an orphaned menu on the host.
        QObject::connect(origin, &QObject::destroyed, popup, &QObject::deleteLater);
    }
    // Before native creation Qt derives transientParent from QWidget ancestry.
    // Reparent only popup windows; never the plane or ordinary floating panels.
    popup->setParent(owner, popup->windowFlags());
}
void showOwnedPopupMenu(QMenu* menu, QWidget* origin)
{
    preparePopupOwnership(menu, origin);
    menu->setAttribute(Qt::WA_NoMouseReplay);
    menu->popup(origin->mapToGlobal(QPoint(0, origin->height())));
}
QWidget* popupTopLevelOwner(QWidget* origin)
{
    for (auto* widget = origin; widget; widget = widget->parentWidget()) {
        if (!widget->isWindow() || widget->property("vulkanaNativeChildSurface").toBool()) continue;
        if (auto* native = widget->windowHandle(); native && !native->isTopLevel()) continue;
        return widget;
    }
    return nullptr;
}
QObject* popupLogicalParent(const QObject* object)
{
    if (!object) return nullptr;
    if (const auto* link = dynamic_cast<Origin*>(object->findChild<QObject*>(
            QLatin1String(linkName), Qt::FindDirectChildrenOnly)); link && link->widget)
        return link->widget;
    return object->parent();
}
bool popupBelongsTo(const QObject* object, const QObject* owner)
{
    for (; object; object = popupLogicalParent(object)) if (object == owner) return true;
    return false;
}
void installPopupOwnershipPolicy()
{
    if (!qApp || qApp->findChild<QObject*>(QStringLiteral("VulkanaPopupOwnershipPolicy"), Qt::FindDirectChildrenOnly)) return;
    auto* policy = new Policy(qApp);
    policy->setObjectName(QStringLiteral("VulkanaPopupOwnershipPolicy"));
    qApp->installEventFilter(policy);
}
} // namespace imageeditor::ui
