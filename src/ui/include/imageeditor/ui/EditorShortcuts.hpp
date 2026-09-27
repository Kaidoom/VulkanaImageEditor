#pragma once
#include <QKeySequence>
#include <QMap>
#include <QStringList>
class QKeyEvent;
class QSettings;
class QWidget;
namespace imageeditor::ui {
enum ShortcutContext : unsigned {
    CanvasKeys = 1, TransformKeys = 2, ConstructionKeys = 4, CurveKeys = 8,
    LayerListKeys = 16, ControlKeys = 32, TextKeys = 64, StrokeKeys = 128,
    EditorKeys = CanvasKeys | TransformKeys | ConstructionKeys | CurveKeys | LayerListKeys | ControlKeys | StrokeKeys
};
struct ShortcutDefinition {
    QString id, label, category;
    QList<QKeySequence> defaults;
    unsigned contexts = EditorKeys;
    bool repeat = false;
    bool hold = false;
};
using ShortcutBindings = QMap<QString, QList<QKeySequence>>;
const QList<ShortcutDefinition>& shortcutDefinitions();
const ShortcutDefinition* shortcutDefinition(const QString& id);
ShortcutBindings defaultShortcutBindings();
ShortcutBindings loadShortcutBindings(QSettings&);
bool saveShortcutBindings(QSettings&, const ShortcutBindings&);
QKeySequence shortcutKey(const QKeyEvent&, bool ignoreShift = false);
bool shortcutMatches(const ShortcutBindings&, const QString& id, const QKeyEvent&, bool ignoreShift = false);
QString shortcutLabel(const ShortcutBindings&, const QString& id);
QString validateShortcut(const QString& id, const QKeySequence&);
QStringList shortcutConflicts(const ShortcutBindings&, const QString& id, const QKeySequence&);
void assignShortcut(ShortcutBindings&, const QString& id, const QKeySequence&, bool clearConflicts);
// Explicit command tokens keep user-facing help synchronized with bindings.
void updateShortcutHints(QWidget*, const ShortcutBindings&);
}
