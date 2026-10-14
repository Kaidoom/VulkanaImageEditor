#include "imageeditor/ui/EditorShortcuts.hpp"
#include "imageeditor/ui/Localization.hpp"
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
        const auto add = [&](const char* id, const i18n::Message& label, const i18n::Message& category, const char* key = "",
                             unsigned contexts = EditorKeys, bool repeat = false, bool hold = false) {
            out.append({QString::fromLatin1(id), label, category,
                *key ? QList<QKeySequence>{QKeySequence(QString::fromLatin1(key))} : QList<QKeySequence>{}, contexts, repeat, hold});
        };
        add("NewDocumentAction", i18n::text("editor.shortcuts.new.document"), i18n::text("editor.shortcuts.file"), "Ctrl+N");
        add("OpenDocumentAction", i18n::text("editor.shortcuts.open.document"), i18n::text("editor.shortcuts.file"), "Ctrl+O");
        add("TakeScreenshotAction", i18n::text("screenshot.action"), i18n::text("editor.shortcuts.file"));
        add("CloseDocumentAction", i18n::text("editor.shortcuts.close.document"), i18n::text("editor.shortcuts.file"), "Ctrl+W", EditorKeys | TextKeys);
        add("NextDocumentAction", i18n::text("editor.shortcuts.next.document"), i18n::text("editor.shortcuts.view"), "Ctrl+Tab", EditorKeys | TextKeys);
        add("PreviousDocumentAction", i18n::text("editor.shortcuts.previous.document"), i18n::text("editor.shortcuts.view"), "Ctrl+Shift+Tab", EditorKeys | TextKeys);
        add("SaveDocumentAction", i18n::text("editor.shortcuts.save"), i18n::text("editor.shortcuts.file"), "Ctrl+S", EditorKeys | TextKeys);
        add("SaveDocumentAsAction", i18n::text("editor.shortcuts.save.as"), i18n::text("editor.shortcuts.file"), "Ctrl+Shift+S", EditorKeys | TextKeys);
        add("ImportImageAction", i18n::text("editor.shortcuts.import.image.as.layer"), i18n::text("editor.shortcuts.file"));
        add("ExportImageAction", i18n::text("editor.shortcuts.export"), i18n::text("editor.shortcuts.file"), "Ctrl+Shift+E");
        add("ExportAgainAction", i18n::text("editor.shortcuts.export.again"), i18n::text("editor.shortcuts.file"));
        add("CopyCanvasToClipboardAction", i18n::text("clipboard.copy.canvas"), i18n::text("editor.shortcuts.file"));
        add("QuitAction", i18n::text("editor.shortcuts.quit"), i18n::text("editor.shortcuts.file"), "Ctrl+Q");
        add("UndoAction", i18n::text("editor.shortcuts.undo"), i18n::text("editor.shortcuts.edit"), "Ctrl+Z", EditorKeys | TextKeys);
        add("RedoAction", i18n::text("editor.shortcuts.redo"), i18n::text("editor.shortcuts.edit"), "Ctrl+Shift+Z", EditorKeys | TextKeys);
        out.last().defaults.append(QKeySequence("Ctrl+Y"));
        add("PasteImageAsLayerAction", i18n::text("editor.shortcuts.paste.image.as.layer"), i18n::text("editor.shortcuts.edit"), "Ctrl+V");
        add("PreferencesAction", i18n::text("editor.shortcuts.preferences"), i18n::text("editor.shortcuts.edit"));
        add("LayerTransformAction", i18n::text("editor.shortcuts.transform.layers.selection"), i18n::text("editor.shortcuts.edit"), "Ctrl+T", EditorKeys | TextKeys);
        add("TransformSelectionAction", i18n::text("editor.shortcuts.transform.selection"), i18n::text("editor.shortcuts.select"), "", EditorKeys);
        add("TransformSelectedPixelsAction", i18n::text("editor.shortcuts.transform.selected.pixels"), i18n::text("editor.shortcuts.select"), "Shift+T", EditorKeys);
        add("ErasePixelsAction", i18n::text("editor.shortcuts.erase.selection.clear.raster.layer"), i18n::text("editor.shortcuts.edit"), "Delete", CanvasKeys);
        add("FillForegroundAction", i18n::text("editor.shortcuts.fill.with.foreground"), i18n::text("editor.shortcuts.edit"), "Alt+Backspace");
        add("FillBackgroundAction", i18n::text("editor.shortcuts.fill.with.background"), i18n::text("editor.shortcuts.edit"), "Ctrl+Backspace");
        add("FinishOperationAction", i18n::text("editor.shortcuts.apply.finish.canvas.operation"), i18n::text("editor.shortcuts.editing.operations"), "Return", TransformKeys | ConstructionKeys);
        add("RemovePointAction", i18n::text("editor.shortcuts.remove.point.last.anchor"), i18n::text("editor.shortcuts.editing.operations"), "Backspace", ConstructionKeys | CurveKeys);
        add("FinishTextAction", i18n::text("editor.shortcuts.finish.text.editing"), i18n::text("editor.shortcuts.editing.operations"), "Ctrl+Return", TextKeys);
        add("NudgeLeftAction", i18n::text("editor.shortcuts.nudge.layers.selection.left"), i18n::text("editor.shortcuts.editing.operations"), "Left", CanvasKeys | TransformKeys, true);
        add("NudgeRightAction", i18n::text("editor.shortcuts.nudge.layers.selection.right"), i18n::text("editor.shortcuts.editing.operations"), "Right", CanvasKeys | TransformKeys, true);
        add("NudgeUpAction", i18n::text("editor.shortcuts.nudge.layers.selection.up"), i18n::text("editor.shortcuts.editing.operations"), "Up", CanvasKeys | TransformKeys, true);
        add("NudgeDownAction", i18n::text("editor.shortcuts.nudge.layers.selection.down"), i18n::text("editor.shortcuts.editing.operations"), "Down", CanvasKeys | TransformKeys, true);
        add("NewRasterLayerAction", i18n::text("editor.shortcuts.new.raster.layer"), i18n::text("editor.shortcuts.layers"), "Ctrl+Shift+N");
        add("DeleteSelectedLayersAction", i18n::text("editor.shortcuts.delete.selected.layers"), i18n::text("editor.shortcuts.layers"), "Shift+Delete");
        add("DuplicateLayersAction", i18n::text("editor.shortcuts.duplicate.selected.layers"), i18n::text("editor.shortcuts.layers"), "Shift+D");
        add("RenameLayerItemAction", i18n::text("editor.shortcuts.rename.layer.folder.group"), i18n::text("editor.shortcuts.layers"), "F2");
        add("NewLayerFolderAction", i18n::text("editor.shortcuts.new.folder"), i18n::text("editor.shortcuts.layers"));
        add("GroupLayersAction", i18n::text("editor.shortcuts.group.selected"), i18n::text("editor.shortcuts.layers"));
        add("UngroupLayersAction", i18n::text("editor.shortcuts.ungroup"), i18n::text("editor.shortcuts.layers"));
        add("MergeLayersAction", i18n::text("editor.shortcuts.merge.selected"), i18n::text("editor.shortcuts.layers"));
        add("RasterizeLayersAction", i18n::text("editor.shortcuts.rasterize.layers"), i18n::text("editor.shortcuts.layers"));
        add("HideSelectedLayersAction", i18n::text("editor.shortcuts.hide.selected"), i18n::text("editor.shortcuts.layers"), "H");
        add("ShowSelectedLayersAction", i18n::text("editor.shortcuts.reveal.selected"), i18n::text("editor.shortcuts.layers"), "Alt+H");
        add("IsolateSelectedLayersAction", i18n::text("editor.shortcuts.isolate.selected"), i18n::text("editor.shortcuts.layers"), "Shift+H");
        add("ShowAllLayersAction", i18n::text("editor.shortcuts.show.all.layers"), i18n::text("editor.shortcuts.layers"), "Shift+Alt+H");
        add("SelectAllAction", i18n::text("editor.shortcuts.select.all.pixels"), i18n::text("editor.shortcuts.selection"), "Ctrl+A");
        add("DeselectAction", i18n::text("editor.shortcuts.deselect"), i18n::text("editor.shortcuts.selection"), "Ctrl+D");
        add("InvertSelectionAction", i18n::text("editor.shortcuts.invert.selection"), i18n::text("editor.shortcuts.selection"), "Ctrl+Shift+I");
        add("ReselectAction", i18n::text("editor.shortcuts.reselect.last.selection"), i18n::text("editor.shortcuts.selection"), "Ctrl+Shift+D");
        add("GrowShrinkSelectionAction", i18n::text("editor.shortcuts.show.grow.shrink.controls"), i18n::text("editor.shortcuts.selection"));
        add("RefineSelectionAction", i18n::text("editor.shortcuts.refine.selection"), i18n::text("editor.shortcuts.selection"));
        add("RefineMaskAction", i18n::text("editor.shortcuts.refine.mask"), i18n::text("editor.shortcuts.layers"));
        add("CopyLayerMaskAction", i18n::text("layer.mask.copy"), i18n::text("editor.shortcuts.layers"));
        add("PasteLayerMaskAction", i18n::text("layer.mask.paste"), i18n::text("editor.shortcuts.layers"));
        add("LayerViaCopyAction", i18n::text("editor.shortcuts.layer.via.copy"), i18n::text("editor.shortcuts.selection"), "Ctrl+J");
        add("ToolAction_move", i18n::text("editor.shortcuts.move"), i18n::text("editor.shortcuts.tools"), "V");
        add("ToolAction_crop", i18n::text("editor.shortcuts.layer.crop"), i18n::text("editor.shortcuts.tools"), "C");
        add("ToolAction_marquee", i18n::text("editor.shortcuts.select"), i18n::text("editor.shortcuts.tools"), "M");
        add("ToolAction_lasso", i18n::text("editor.shortcuts.lasso"), i18n::text("editor.shortcuts.tools"), "L");
        add("ToolAction_colorselect", i18n::text("editor.shortcuts.select.by.color"), i18n::text("editor.shortcuts.tools"), "Shift+O");
        add("ToolAction_smartselect", i18n::text("editor.shortcuts.smart.select"), i18n::text("editor.shortcuts.tools"), "W");
        add("ToolAction_brush", i18n::text("editor.shortcuts.brush"), i18n::text("editor.shortcuts.tools"), "B");
        add("ToolAction_eraser", i18n::text("editor.shortcuts.toggle.erase.mode"), i18n::text("editor.shortcuts.tools"), "E");
        add("ToolAction_cloning", i18n::text("editor.shortcuts.cloning"), i18n::text("editor.shortcuts.tools"), "S");
        add("ToolAction_local_blur", i18n::text("editor.shortcuts.local.blur"), i18n::text("editor.shortcuts.tools"), "K");
        add("ToolAction_fill", i18n::text("editor.shortcuts.fill"), i18n::text("editor.shortcuts.tools"), "G");
        add("ToolAction_text", i18n::text("editor.shortcuts.text"), i18n::text("editor.shortcuts.tools"), "T");
        add("ToolAction_shape", i18n::text("editor.shortcuts.shape"), i18n::text("editor.shortcuts.tools"), "U");
        add("ToolAction_measure", i18n::text("editor.shortcuts.measure.tool"), i18n::text("editor.shortcuts.tools"), "Shift+R");
        add("TemporaryMeasureAction", i18n::text("editor.shortcuts.temporary.measure.hold"), i18n::text("editor.shortcuts.tools"), "R", CanvasKeys, false, true);
        add("ToolAction_eyedropper", i18n::text("editor.shortcuts.eyedropper"), i18n::text("editor.shortcuts.tools"), "I");
        add("DecreaseBrushSizeAction", i18n::text("editor.shortcuts.decrease.brush.tool.size"), i18n::text("editor.shortcuts.tools"), "[", EditorKeys, true);
        add("IncreaseBrushSizeAction", i18n::text("editor.shortcuts.increase.brush.tool.size"), i18n::text("editor.shortcuts.tools"), "]", EditorKeys, true);
        add("SwapColorsAction", i18n::text("editor.shortcuts.switch.active.color"), i18n::text("editor.shortcuts.tools"), "X");
        add("ChangeCanvasSizeAction", i18n::text("editor.shortcuts.change.canvas.size"), i18n::text("editor.shortcuts.view.canvas"), "Ctrl+Alt+C");
        add("FitCanvasAction", i18n::text("editor.shortcuts.fit.canvas"), i18n::text("editor.shortcuts.view.canvas"), "Ctrl+0");
        add("ActualSizeAction", i18n::text("editor.shortcuts.100.zoom"), i18n::text("editor.shortcuts.view.canvas"), "Ctrl+1");
        add("PanCanvasAction", i18n::text("editor.shortcuts.pan.canvas.hold"), i18n::text("editor.shortcuts.view.canvas"), "Space", CanvasKeys | TransformKeys | ConstructionKeys | StrokeKeys, false, true);
        add("PixelPreviewAction", i18n::text("editor.shortcuts.pixel.preview"), i18n::text("editor.shortcuts.view.canvas"), "Shift+P");
        add("ToggleLayerOutlinesAction", i18n::text("editor.shortcuts.selected.layer.outlines"), i18n::text("editor.shortcuts.view.canvas"), "O");
        add("SnappingAction", i18n::text("editor.shortcuts.toggle.snapping"), i18n::text("editor.shortcuts.view.canvas"));
        add("SnapCanvasAction", i18n::text("editor.shortcuts.snap.to.canvas"), i18n::text("editor.shortcuts.view.canvas"));
        add("SnapLayersAction", i18n::text("editor.shortcuts.snap.to.layers"), i18n::text("editor.shortcuts.view.canvas"));
        add("ViewHorizontalRuler", i18n::text("editor.shortcuts.horizontal.ruler"), i18n::text("editor.shortcuts.view.canvas"));
        add("ViewVerticalRuler", i18n::text("editor.shortcuts.vertical.ruler"), i18n::text("editor.shortcuts.view.canvas"));
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
    if (key.count() != 1) return i18n::text("editor.shortcuts.use.one.key.or.key.combination.not.a.multi");
    const auto k = key[0].key();
    if (k == Qt::Key_Escape) return i18n::text("editor.shortcuts.escape.is.reserved.for.cancellation");
    if (k == Qt::Key_Shift || k == Qt::Key_Control || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_AltGr
        || k == Qt::Key_unknown)
        return i18n::text("editor.shortcuts.modifier.keys.on.their.own.are.reserved");
    const auto* command = shortcutDefinition(id);
    if (command && (command->contexts & TextKeys) && !(key[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        && k < Qt::Key_F1)
        return i18n::text("editor.shortcuts.keep.typing.and.enter.available.for.text.use.a");
    if (command && (command->contexts & TextKeys) && key[0].keyboardModifiers().testFlag(Qt::ControlModifier)
        && !(key[0].keyboardModifiers() & (Qt::AltModifier | Qt::MetaModifier))
        && (k == Qt::Key_A || k == Qt::Key_C || k == Qt::Key_X || k == Qt::Key_V || k == Qt::Key_Backspace || k == Qt::Key_Delete
            || (k >= Qt::Key_Home && k <= Qt::Key_PageDown)))
        return i18n::text("editor.shortcuts.that.key.combination.is.reserved.for.text.editing.and");
    if (id.startsWith("Nudge") && key[0].keyboardModifiers().testFlag(Qt::ShiftModifier))
        return i18n::text("editor.shortcuts.shift.is.reserved.for.the.larger.nudge.step.choose");
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
    const auto render = [token, bindings](QString source) {
        auto match = token.match(source);
        while (match.hasMatch()) {
            source.replace(match.capturedStart(), match.capturedLength(), shortcutLabel(bindings, match.captured(1)));
            match = token.match(source);
        }
        return source;
    };
    i18n::Localization::instance().setShortcutFormatter(render);
    for (auto* widget : widgets) {
        if (auto* label = qobject_cast<QLabel*>(widget)) {
            if (i18n::Localization::instance().isBound(label, "text")) continue;
            if (!label->property("shortcutTextTemplate").isValid() && label->text().contains(token))
                label->setProperty("shortcutTextTemplate", label->text());
            if (label->property("shortcutTextTemplate").isValid())
                label->setText(render(label->property("shortcutTextTemplate").toString()));
        }
        if (i18n::Localization::instance().isBound(widget, "toolTip")) continue;
        if (!widget->property("shortcutTooltipTemplate").isValid() && widget->toolTip().contains(token))
            widget->setProperty("shortcutTooltipTemplate", widget->toolTip());
        if (widget->property("shortcutTooltipTemplate").isValid())
            widget->setToolTip(render(widget->property("shortcutTooltipTemplate").toString()));
    }
}
}
