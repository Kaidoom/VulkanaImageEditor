#include "imageeditor/ui/EditorShortcuts.hpp"
#include <QKeyEvent>
#include <QSettings>
#include <QLabel>
#include <QRegularExpression>
#include <QWidget>
#include <algorithm>
namespace imageeditor::ui {
const QList<ShortcutDefinition>& shortcutDefinitions()
{
    static const auto definitions = [] {
        QList<ShortcutDefinition> out;
        const auto add = [&](const char* id, const char* label, const char* category, const char* key = "",
                             unsigned contexts = EditorKeys, bool repeat = false, bool hold = false) {
            out.append({QString::fromLatin1(id), QString::fromUtf8(label), QString::fromUtf8(category),
                *key ? QList<QKeySequence>{QKeySequence(QString::fromLatin1(key))} : QList<QKeySequence>{}, contexts, repeat, hold});
        };
        add("NewDocumentAction", "New document", "File", "Ctrl+N");
        add("OpenDocumentAction", "Open document", "File", "Ctrl+O");
        add("CloseDocumentAction", "Close document", "File", "Ctrl+W", EditorKeys | TextKeys);
        add("NextDocumentAction", "Next document", "View", "Ctrl+Tab", EditorKeys | TextKeys);
        add("PreviousDocumentAction", "Previous document", "View", "Ctrl+Shift+Tab", EditorKeys | TextKeys);
        add("SaveDocumentAction", "Save", "File", "Ctrl+S", EditorKeys | TextKeys);
        add("SaveDocumentAsAction", "Save as", "File", "Ctrl+Shift+S", EditorKeys | TextKeys);
        add("ImportImageAction", "Import image as layer", "File");
        add("ExportImageAction", "Export", "File", "Ctrl+Shift+E");
        add("ExportAgainAction", "Export again", "File");
        add("QuitAction", "Quit", "File", "Ctrl+Q");
        add("UndoAction", "Undo", "Edit", "Ctrl+Z", EditorKeys | TextKeys);
        add("RedoAction", "Redo", "Edit", "Ctrl+Shift+Z", EditorKeys | TextKeys);
        out.last().defaults.append(QKeySequence("Ctrl+Y"));
        add("PasteImageAsLayerAction", "Paste image as layer", "Edit", "Ctrl+V");
        add("PreferencesAction", "Preferences", "Edit");
        add("LayerTransformAction", "Transform layers / selection", "Edit", "Ctrl+T", EditorKeys | TextKeys);
        add("TransformSelectionAction", "Transform Selection", "Select", "", EditorKeys);
        add("TransformSelectedPixelsAction", "Transform Selected Pixels", "Select", "Shift+T", EditorKeys);
        add("ErasePixelsAction", "Erase selection / clear raster layer", "Edit", "Delete", CanvasKeys);
        add("FillForegroundAction", "Fill with foreground", "Edit", "Alt+Backspace");
        add("FillBackgroundAction", "Fill with background", "Edit", "Ctrl+Backspace");
        add("FinishOperationAction", "Apply / finish canvas operation", "Editing operations", "Return", TransformKeys | ConstructionKeys);
        add("RemovePointAction", "Remove point / last anchor", "Editing operations", "Backspace", ConstructionKeys | CurveKeys);
        add("FinishTextAction", "Finish text editing", "Editing operations", "Ctrl+Return", TextKeys);
        add("NudgeLeftAction", "Nudge layers / selection left", "Editing operations", "Left", CanvasKeys | TransformKeys, true);
        add("NudgeRightAction", "Nudge layers / selection right", "Editing operations", "Right", CanvasKeys | TransformKeys, true);
        add("NudgeUpAction", "Nudge layers / selection up", "Editing operations", "Up", CanvasKeys | TransformKeys, true);
        add("NudgeDownAction", "Nudge layers / selection down", "Editing operations", "Down", CanvasKeys | TransformKeys, true);
        add("NewRasterLayerAction", "New raster layer", "Layers", "Ctrl+Shift+N");
        add("DeleteSelectedLayersAction", "Delete selected layers", "Layers", "Shift+Delete");
        add("DuplicateLayersAction", "Duplicate selected layers", "Layers", "Shift+D");
        add("RenameLayerItemAction", "Rename layer / folder / group", "Layers", "F2");
        add("NewLayerFolderAction", "New folder", "Layers");
        add("GroupLayersAction", "Group selected", "Layers");
        add("UngroupLayersAction", "Ungroup", "Layers");
        add("MergeLayersAction", "Merge selected", "Layers");
        add("RasterizeLayersAction", "Rasterize layers", "Layers");
        add("HideSelectedLayersAction", "Hide selected", "Layers", "H");
        add("ShowSelectedLayersAction", "Reveal selected", "Layers", "Alt+H");
        add("IsolateSelectedLayersAction", "Isolate selected", "Layers", "Shift+H");
        add("ShowAllLayersAction", "Show all layers", "Layers", "Shift+Alt+H");
        add("SelectAllAction", "Select all pixels", "Selection", "Ctrl+A");
        add("DeselectAction", "Deselect", "Selection", "Ctrl+D");
        add("InvertSelectionAction", "Invert selection", "Selection", "Ctrl+Shift+I");
        add("ReselectAction", "Reselect last selection", "Selection", "Ctrl+Shift+D");
        add("GrowShrinkSelectionAction", "Show Grow / Shrink controls", "Selection");
        add("LayerViaCopyAction", "Layer via copy", "Selection", "Ctrl+J");
        add("ToolAction_move", "Move", "Tools", "V");
        add("ToolAction_crop", "Layer crop", "Tools", "C");
        add("ToolAction_marquee", "Select", "Tools", "M");
        add("ToolAction_lasso", "Lasso", "Tools", "L");
        add("ToolAction_colorselect", "Select by color", "Tools", "Shift+O");
        add("ToolAction_smartselect", "Smart select", "Tools", "W");
        add("ToolAction_brush", "Brush", "Tools", "B");
        add("ToolAction_eraser", "Toggle erase mode", "Tools", "E");
        add("ToolAction_cloning", "Cloning", "Tools", "S");
        add("ToolAction_local_blur", "Local blur", "Tools", "K");
        add("ToolAction_fill", "Fill", "Tools", "G");
        add("ToolAction_text", "Text", "Tools", "T");
        add("ToolAction_shape", "Shape", "Tools", "U");
        add("ToolAction_measure", "Measure tool", "Tools", "Shift+R");
        add("TemporaryMeasureAction", "Temporary Measure (hold)", "Tools", "R", CanvasKeys, false, true);
        add("ToolAction_eyedropper", "Eyedropper", "Tools", "I");
        add("DecreaseBrushSizeAction", "Decrease brush / tool size", "Tools", "[", EditorKeys, true);
        add("IncreaseBrushSizeAction", "Increase brush / tool size", "Tools", "]", EditorKeys, true);
        add("SwapColorsAction", "Switch active color", "Tools", "X");
        add("ChangeCanvasSizeAction", "Change canvas size", "View & canvas", "Ctrl+Alt+C");
        add("FitCanvasAction", "Fit canvas", "View & canvas", "Ctrl+0");
        add("ActualSizeAction", "100% zoom", "View & canvas", "Ctrl+1");
        add("PanCanvasAction", "Pan canvas (hold)", "View & canvas", "Space", CanvasKeys | TransformKeys | ConstructionKeys | StrokeKeys, false, true);
        add("PixelPreviewAction", "Pixel preview", "View & canvas", "Shift+P");
        add("ToggleLayerOutlinesAction", "Selected-layer outlines", "View & canvas", "O");
        add("SnappingAction", "Toggle snapping", "View & canvas");
        add("SnapCanvasAction", "Snap to canvas", "View & canvas");
        add("SnapLayersAction", "Snap to layers", "View & canvas");
        add("ViewHorizontalRuler", "Horizontal ruler", "View & canvas");
        add("ViewVerticalRuler", "Vertical ruler", "View & canvas");
        return out;
    }();
    return definitions;
}
const ShortcutDefinition* shortcutDefinition(const QString& id)
{
    for (const auto& d : shortcutDefinitions()) if (d.id == id) return &d;
    return nullptr;
}
ShortcutBindings defaultShortcutBindings()
{
    ShortcutBindings out;
    for (const auto& d : shortcutDefinitions()) out[d.id] = d.defaults;
    return out;
}
QKeySequence shortcutKey(const QKeyEvent& event, bool ignoreShift)
{
    auto mods = event.modifiers() & ~Qt::KeypadModifier;
    if (event.key() == Qt::Key_Backtab) mods |= Qt::ShiftModifier;
    if (ignoreShift) mods &= ~Qt::ShiftModifier;
    const auto key = event.key() == Qt::Key_Enter ? Qt::Key_Return
        : event.key() == Qt::Key_Backtab ? Qt::Key_Tab : event.key();
    return QKeySequence(QKeyCombination(mods, Qt::Key(key)));
}
bool shortcutMatches(const ShortcutBindings& bindings, const QString& id, const QKeyEvent& event, bool ignoreShift)
{
    const auto keys = bindings.value(id);
    return keys.contains(shortcutKey(event)) || (ignoreShift && keys.contains(shortcutKey(event, true)));
}
QString shortcutLabel(const ShortcutBindings& bindings, const QString& id)
{
    QStringList labels;
    for (const auto& key : bindings.value(id)) labels.append(key.toString(QKeySequence::NativeText));
    return labels.isEmpty() ? QStringLiteral("Unassigned") : labels.join(QStringLiteral(" / "));
}
QString validateShortcut(const QString& id, const QKeySequence& key)
{
    if (key.isEmpty()) return {};
    if (key.count() != 1) return QStringLiteral("Use one key or key combination, not a multi-step sequence.");
    const auto k = key[0].key();
    if (k == Qt::Key_Escape) return QStringLiteral("Escape is reserved for cancellation.");
    if (k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_AltGr
        || k == Qt::Key_unknown)
        return QStringLiteral("Modifier keys on their own are reserved.");
    const auto* command = shortcutDefinition(id);
    if (command && (command->contexts & TextKeys) && !(key[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        && k < Qt::Key_F1)
        return QStringLiteral("Keep typing and Enter available for text. Use a modified key or a function key.");
    if (command && (command->contexts & TextKeys) && key[0].keyboardModifiers().testFlag(Qt::ControlModifier)
        && !(key[0].keyboardModifiers() & (Qt::AltModifier | Qt::MetaModifier))
        && (k == Qt::Key_A || k == Qt::Key_C || k == Qt::Key_X || k == Qt::Key_V || k == Qt::Key_Backspace || k == Qt::Key_Delete
            || (k >= Qt::Key_Home && k <= Qt::Key_PageDown)))
        return QStringLiteral("That key combination is reserved for text editing and navigation.");
    if (id.startsWith("Nudge") && key[0].keyboardModifiers().testFlag(Qt::ShiftModifier))
        return QStringLiteral("Shift is reserved for the larger nudge step. Choose the base binding without Shift.");
    return {};
}
QStringList shortcutConflicts(const ShortcutBindings& bindings, const QString& id, const QKeySequence& key)
{
    QStringList out;
    const auto* selected = shortcutDefinition(id);
    if (!selected || key.isEmpty()) return out;
    for (const auto& d : shortcutDefinitions()) {
        if (d.id == id || !(d.contexts & selected->contexts)) continue;
        for (const auto& other : bindings.value(d.id)) {
            const auto shiftAlias = [](const QString& command) {
                return command.startsWith("Nudge") || command == "FinishOperationAction" || command == "RemovePointAction";
            };
            const bool nudgeAlias = (shiftAlias(id) || shiftAlias(d.id))
                && key[0].key() == other[0].key()
                && (key[0].keyboardModifiers() & ~Qt::ShiftModifier) == (other[0].keyboardModifiers() & ~Qt::ShiftModifier);
            if (key == other || nudgeAlias) { out.append(d.id); break; }
        }
    }
    return out;
}
void assignShortcut(ShortcutBindings& bindings, const QString& id, const QKeySequence& key, bool clearConflicts)
{
    if (clearConflicts) for (const auto& conflict : shortcutConflicts(bindings, id, key)) bindings[conflict].clear();
    bindings[id] = key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key};
}
ShortcutBindings loadShortcutBindings(QSettings& settings)
{
    auto out = defaultShortcutBindings();
    settings.beginGroup("shortcuts/v1");
    for (const auto& d : shortcutDefinitions()) {
        if (!settings.contains(d.id)) continue;
        QList<QKeySequence> keys;
        bool valid = true;
        for (const auto& text : settings.value(d.id).toStringList()) {
            const QKeySequence key(text, QKeySequence::PortableText);
            if (key.isEmpty() || !validateShortcut(d.id, key).isEmpty()) { valid = false; break; }
            if (!keys.contains(key)) keys.append(key);
        }
        if (valid) {
            // Explicit stored choices take precedence over newly introduced
            // defaults. Corrupt/manual conflicts resolve deterministically.
            for (const auto& key : keys)
                for (const auto& conflict : shortcutConflicts(out, d.id, key)) out[conflict].clear();
            out[d.id] = keys;
        }
    }
    settings.endGroup();
    return out;
}
bool saveShortcutBindings(QSettings& settings, const ShortcutBindings& bindings)
{
    settings.beginGroup("shortcuts/v1");
    for (const auto& d : shortcutDefinitions()) {
        QStringList keys;
        for (const auto& key : bindings.value(d.id)) keys.append(key.toString(QKeySequence::PortableText));
        settings.setValue(d.id, keys); // Empty is explicitly unbound, not default.
    }
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}
void updateShortcutHints(QWidget* root, const ShortcutBindings& bindings)
{
    if (!root) return;
    auto widgets = root->findChildren<QWidget*>(); widgets.prepend(root);
    const QRegularExpression token(QStringLiteral("\\{\\{([A-Za-z0-9_]+)\\}\\}"));
    const auto render = [&](QString source) {
        auto match = token.match(source);
        while (match.hasMatch()) {
            source.replace(match.capturedStart(), match.capturedLength(), shortcutLabel(bindings, match.captured(1)));
            match = token.match(source);
        }
        return source;
    };
    for (auto* widget : widgets) {
        if (auto* label = qobject_cast<QLabel*>(widget)) {
            if (!label->property("shortcutTextTemplate").isValid() && label->text().contains(token))
                label->setProperty("shortcutTextTemplate", label->text());
            if (label->property("shortcutTextTemplate").isValid())
                label->setText(render(label->property("shortcutTextTemplate").toString()));
        }
        if (!widget->property("shortcutTooltipTemplate").isValid() && widget->toolTip().contains(token))
            widget->setProperty("shortcutTooltipTemplate", widget->toolTip());
        if (widget->property("shortcutTooltipTemplate").isValid())
            widget->setToolTip(render(widget->property("shortcutTooltipTemplate").toString()));
    }
}
}
