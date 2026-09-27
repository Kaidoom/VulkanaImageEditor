#pragma once
class QObject;
class QWidget;
class QMenu;
namespace imageeditor::ui {
// Panel planes have their own backing store, but their QWindows are children.
// Native transient windows must be owned by a real top-level, not that plane.
void installPopupOwnershipPolicy();
void preparePopupOwnership(QWidget* popup, QWidget* origin);
// Button-owned menus cannot use Qt's implicit setMenu path: that path resets
// transientParent to the button's logical child plane during QMenu::Show.
void showOwnedPopupMenu(QMenu* menu, QWidget* origin);
[[nodiscard]] QWidget* popupTopLevelOwner(QWidget* origin);
[[nodiscard]] QObject* popupLogicalParent(const QObject* object);
[[nodiscard]] bool popupBelongsTo(const QObject* object, const QObject* owner);
} // namespace imageeditor::ui
