#include "imageeditor/ui/EditorShortcuts.hpp"
#include "imageeditor/ui/ShortcutsPanel.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QVulkanInstance>
#include <iostream>

namespace u = imageeditor::ui;
namespace c = imageeditor::core;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)
void settle() { for (int i = 0; i < 3; ++i) QCoreApplication::processEvents(); }
void key(QObject* target, int k, Qt::KeyboardModifiers mods = {}, QEvent::Type type = QEvent::KeyPress)
{
    QKeyEvent override(QEvent::ShortcutOverride, k, mods);
    if (type == QEvent::KeyPress) QCoreApplication::sendEvent(target, &override);
    QKeyEvent e(type, k, mods); QCoreApplication::sendEvent(target, &e); settle();
}
void select(u::ShortcutsPanel& panel, const QString& id)
{
    auto* list = panel.findChild<QTreeWidget*>("ShortcutCommands");
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        if (list->topLevelItem(i)->data(0, Qt::UserRole).toString() == id) {
            list->setCurrentItem(list->topLevelItem(i)); return;
        }
    CHECK(false);
}
void bind(u::ShortcutsPanel& panel, const QString& id, int k, Qt::KeyboardModifiers mods = {})
{
    select(panel, id); panel.findChild<QPushButton*>("ShortcutChange")->click(); key(&panel, k, mods);
}
void registry()
{
    auto bindings = u::defaultShortcutBindings();
    for (const auto& d : u::shortcutDefinitions()) for (const auto& k : d.defaults) {
        CHECK(u::validateShortcut(d.id, k).isEmpty());
        CHECK(u::shortcutConflicts(bindings, d.id, k).isEmpty());
    }
    CHECK(!u::validateShortcut("ToolAction_brush", QKeySequence("Esc")).isEmpty());
    CHECK(u::validateShortcut("PixelPreviewAction", QKeySequence("Tab")).isEmpty());
    CHECK(u::validateShortcut("PixelPreviewAction", QKeySequence("Shift+Tab")).isEmpty());
    QKeyEvent backtab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
    CHECK(u::shortcutKey(backtab) == QKeySequence("Shift+Tab"));
    CHECK(!u::validateShortcut("NudgeLeftAction", QKeySequence("Shift+Left")).isEmpty());
    CHECK(!u::validateShortcut("FinishTextAction", QKeySequence("Return")).isEmpty());
    CHECK(u::shortcutConflicts(bindings, "FinishOperationAction", QKeySequence("Ctrl+Return")).isEmpty());
    CHECK(u::shortcutConflicts(bindings, "ToolAction_brush", QKeySequence("Shift+Left")).contains("NudgeLeftAction"));
    u::assignShortcut(bindings, "ToolAction_brush", QKeySequence("V"), true);
    CHECK(bindings.value("ToolAction_move").isEmpty());
    u::assignShortcut(bindings, "ErasePixelsAction", {}, false);
    QSettings settings; CHECK(u::saveShortcutBindings(settings, bindings));
    CHECK(u::loadShortcutBindings(settings) == bindings);
    settings.clear();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier);
    CHECK(u::shortcutMatches(u::defaultShortcutBindings(), "FinishOperationAction", enter));
    CHECK(QKeySequence::mnemonic("&Help").isEmpty());
}
void panel()
{
    u::PreferencesDialog dialog({}); dialog.show();
    dialog.findChild<QTabWidget*>("PreferencesTabs")->setCurrentWidget(dialog.findChild<QWidget*>("ShortcutsPanel")); settle();
    auto* panel = dynamic_cast<u::ShortcutsPanel*>(dialog.findChild<QWidget*>("ShortcutsPanel")); CHECK(panel);
    bind(*panel, "ToolAction_brush", Qt::Key_V);
    CHECK(panel->pending());
    CHECK(dialog.draft().shortcuts == u::defaultShortcutBindings());
    dialog.reject(); // Same path as the owning WorkspaceDialog Escape barrier.
    CHECK(!panel->pending() && dialog.isVisible());
    bind(*panel, "ToolAction_brush", Qt::Key_V);
    panel->findChild<QPushButton*>("ShortcutConfirmConflict")->click();
    CHECK(dialog.draft().shortcuts.value("ToolAction_move").isEmpty());
    CHECK(dialog.draft().shortcuts.value("ToolAction_brush") == QList<QKeySequence>{QKeySequence("V")});
    select(*panel, "ToolAction_brush"); panel->findChild<QPushButton*>("ShortcutClear")->click();
    CHECK(dialog.draft().shortcuts.value("ToolAction_brush").isEmpty());
    bind(*panel, "FinishOperationAction", Qt::Key_Return, Qt::ControlModifier);
    CHECK(!panel->pending()); // Disjoint contexts may share a chord.
    CHECK(dialog.draft().shortcuts.value("FinishTextAction") == QList<QKeySequence>{QKeySequence("Ctrl+Return")});
    panel->findChild<QPushButton*>("ShortcutChange")->click(); CHECK(panel->pending());
    key(panel, Qt::Key_Escape); CHECK(!panel->pending() && dialog.isVisible());
    if (qApp->arguments().contains("--screenshot")) dialog.grab().save("/tmp/vulkana-shortcuts-preferences.png");
    dialog.close();
}
void application(QVulkanInstance* instance)
{
    auto bindings = u::defaultShortcutBindings();
    const std::pair<const char*, const char*> overrides[] {
        {"PixelPreviewAction", "Shift+F3"}, {"TemporaryMeasureAction", "F8"},
        {"PanCanvasAction", "F9"}, {"RemovePointAction", "F4"}, {"FinishOperationAction", "F6"},
        {"FinishTextAction", "Ctrl+F6"}, {"LayerTransformAction", "Ctrl+F8"},
        {"NudgeRightAction", "F7"}, {"UndoAction", "Ctrl+F10"}, {"RedoAction", "Ctrl+Shift+F10"}
    };
    for (const auto& [id, chord] : overrides) u::assignShortcut(bindings, id, QKeySequence(chord), true);
    QSettings settings; CHECK(u::saveShortcutBindings(settings, bindings));
    QTemporaryDir data;
    u::MainWindow window(instance, true); window.setUnsavedPromptEnabled(false);
    window.resize(1400, 900); window.show(); window.activateWindow(); QTest::qWait(120);
    imageeditor::render::CanvasWindow* canvas = nullptr;
    for (auto* w : QGuiApplication::allWindows())
        if (auto* candidate = dynamic_cast<imageeditor::render::CanvasWindow*>(w)) canvas = candidate;
    CHECK(canvas); if (!canvas) return;
    auto action = [&](const char* name) { auto* a = window.findChild<QAction*>(name); CHECK(a); return a; };
    QImage image(64, 48, QImage::Format_RGBA8888); image.fill(QColor(60, 110, 190));
    const auto path = data.filePath("source.png"); CHECK(image.save(path)); CHECK(window.openImageFromPath(path));
    action("ToolAction_move")->trigger(); settle();
    for (const auto& d : u::shortcutDefinitions()) {
        auto* a = action(qPrintable(d.id)); if (a) CHECK(a->shortcuts() == bindings.value(d.id));
    }
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    const auto first = *session.activeLayer();
    const auto start = session.document()->layer(first)->localToDocument;
    if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    key(canvas, Qt::Key_Right); CHECK(session.document()->layer(first)->localToDocument == start); // Old binding is gone.
    key(canvas, Qt::Key_F7); CHECK(session.document()->layer(first)->localToDocument != start);
    key(canvas, Qt::Key_F10, Qt::ControlModifier); CHECK(session.document()->layer(first)->localToDocument == start);
    key(canvas, Qt::Key_F7, Qt::ShiftModifier);
    CHECK(session.document()->layer(first)->localToDocument.m02 == start.m02 + 10);
    key(canvas, Qt::Key_F10, Qt::ControlModifier);
    key(canvas, Qt::Key_T, Qt::ControlModifier); CHECK(session.activeTool() == c::ToolId::Move);
    key(canvas, Qt::Key_F8, Qt::ControlModifier); CHECK(session.activeTool() == c::ToolId::Transform);
    key(canvas, Qt::Key_F6); CHECK(session.activeTool() == c::ToolId::Move);
    key(canvas, Qt::Key_F8); CHECK(canvas->temporaryMeasureActive());
    key(canvas, Qt::Key_Shift, Qt::ShiftModifier); CHECK(canvas->temporaryMeasureActive());
    key(canvas, Qt::Key_F8, Qt::ShiftModifier, QEvent::KeyRelease); CHECK(!canvas->temporaryMeasureActive());
    key(canvas, Qt::Key_R); CHECK(!canvas->temporaryMeasureActive());
    key(canvas, Qt::Key_R, Qt::ShiftModifier); CHECK(session.activeTool() == c::ToolId::Measure);
    action("ToolAction_move")->trigger();
    key(canvas, Qt::Key_Space); CHECK(!canvas->spacePanHeld());
    key(canvas, Qt::Key_F9); CHECK(canvas->spacePanHeld());
    key(canvas, Qt::Key_F9, Qt::ShiftModifier, QEvent::KeyRelease); CHECK(!canvas->spacePanHeld());
    key(canvas, Qt::Key_P, Qt::ShiftModifier); CHECK(!action("PixelPreviewAction")->isChecked());
    key(canvas, Qt::Key_F3, Qt::ShiftModifier); CHECK(action("PixelPreviewAction")->isChecked());
    key(canvas, Qt::Key_F3, Qt::ShiftModifier); CHECK(!action("PixelPreviewAction")->isChecked());

    action("DuplicateLayersAction")->trigger(); settle();
    u::LayerListView* list = nullptr;
    for (auto* w : window.findChildren<QWidget*>()) if (auto* p = dynamic_cast<u::LayerListView*>(w)) list = p;
    CHECK(list);
    if (list) {
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualRect(list->model()->index(0, 0)).center()); settle();
        const auto selected = session.activeLayer();
        const auto history = session.history().undoDepth();
        key(window.windowHandle(), Qt::Key_Down);
        CHECK(session.activeLayer() != selected); CHECK(session.history().undoDepth() == history);
        // A rebound nudge is canvas-context only, even when native keys enter via the host.
        key(window.windowHandle(), Qt::Key_F7); CHECK(session.history().undoDepth() == history);
        const auto p = canvas->scene().viewport.documentToViewport({32, 24}, {64, 48}, canvas->scene().logicalViewport);
        QTest::mouseClick(canvas, Qt::LeftButton, {}, QPoint(int(p.x), int(p.y))); settle();
        const auto beforeNudge = session.history().undoDepth();
        key(window.windowHandle(), Qt::Key_F7);
        CHECK(session.history().undoDepth() == beforeNudge + 1);
    }
    u::CompactValueControl* slider = nullptr;
    for (auto* w : window.findChildren<QWidget*>()) if (auto* p = dynamic_cast<u::CompactValueControl*>(w); p && p->isVisible() && p->isEnabled()) { slider = p; break; }
    CHECK(slider);
    if (slider) {
        // A real click establishes cross-window keyboard ownership; setFocus
        // alone on an inactive offscreen panel does not model that handoff.
        QTest::mouseClick(slider, Qt::LeftButton, {}, slider->rect().center());
        slider->setValue((slider->minimum() + slider->maximum()) * .5); settle();
        const auto v = slider->value(); key(window.windowHandle(), Qt::Key_Up); CHECK(slider->value() > v);
        // F2 after using a slider still reaches inline Rename.
        key(window.windowHandle(), Qt::Key_F2);
        QLineEdit* rename = nullptr;
        for (auto* field : list->findChildren<QLineEdit*>()) if (field->isVisible()) rename = field;
        CHECK(rename); if (rename) { QTest::keyClick(rename, Qt::Key_Escape); settle(); }
    }
    // Actual adjustment mouse-release/commit used to clear focus and send the
    // next key to the canvas. Exercise native entry, not a setFocus workaround.
    auto* exposure = dynamic_cast<u::CompactValueControl*>(window.findChild<QDoubleSpinBox*>("AdjustmentExposure"));
    auto* navigation = window.findChild<QComboBox*>("AdjustmentNavigation0");
    CHECK(exposure && navigation);
    if (exposure && navigation) {
        navigation->setCurrentIndex(0); settle();
        QTest::mouseClick(exposure, Qt::LeftButton, {}, exposure->valueFieldRect().center()); settle();
        const auto value = exposure->value();
        const auto depth = session.history().undoDepth();
        for (int i = 0; i < 2; ++i) {
            key(window.windowHandle(), Qt::Key_Up);
            key(window.windowHandle(), Qt::Key_Up, {}, QEvent::KeyRelease);
        }
        CHECK(exposure->value() > value);
        CHECK(session.history().undoDepth() == depth + 2);
        // Local arrows do not swallow global editor commands.
        key(window.windowHandle(), Qt::Key_F3, Qt::ShiftModifier);
        CHECK(action("PixelPreviewAction")->isChecked());
        key(window.windowHandle(), Qt::Key_F3, Qt::ShiftModifier);
        CHECK(!action("PixelPreviewAction")->isChecked());
        u::MainWindow::FileInteractions files;
        const auto project = data.filePath("focused-save.vulkana");
        files.chooseSavePath = [&] { return project; };
        files.reportError = [&](const QString& error) { std::cerr << error.toStdString() << '\n'; CHECK(false); };
        window.setFileInteractions(std::move(files));
        CHECK(session.document()->isModified());
        key(window.windowHandle(), Qt::Key_S, Qt::ControlModifier);
        CHECK(window.projectPath() == project && !session.document()->isModified());
        // Save also settles typed numeric input, without requiring a click out.
        QTest::mouseDClick(exposure, Qt::LeftButton, {}, exposure->valueFieldRect().center());
        QTest::mouseRelease(exposure, Qt::LeftButton, {}, exposure->valueFieldRect().center());
        CHECK(exposure->isManualEntryActive());
        QTest::keyClicks(exposure, "2"); CHECK(exposure->isManualEntryActive());
        key(window.windowHandle(), Qt::Key_S, Qt::ControlModifier);
        CHECK(!exposure->isManualEntryActive() && exposure->value() == 2);
        CHECK(!session.document()->isModified());
        auto* focused = QApplication::focusWidget();
        CHECK(focused == exposure || exposure->isAncestorOf(focused));
        key(window.windowHandle(), Qt::Key_Tab);
        key(window.windowHandle(), Qt::Key_Backtab, Qt::ShiftModifier);
        CHECK(QApplication::focusWidget() == focused);
        navigation->setCurrentIndex(3); settle();
        auto* curve = dynamic_cast<u::AdjustmentCurveEditor*>(window.findChild<QWidget*>("AdjustmentCurveEditor"));
        CHECK(curve);
        if (curve) {
            for (auto* parent = curve->parentWidget(); parent; parent = parent->parentWidget())
                if (auto* scroll = qobject_cast<QScrollArea*>(parent)) scroll->ensureWidgetVisible(curve);
            settle();
            QTest::mouseClick(curve, Qt::LeftButton, {}, curve->rect().center()); settle();
            CHECK(curve->hasFocus()); CHECK(curve->points().size() == 3);
            CHECK(curve->toolTip().isEmpty());
            CHECK(session.document()->isModified());
            key(window.windowHandle(), Qt::Key_S, Qt::ControlModifier);
            CHECK(!session.document()->isModified());
            CHECK(curve->hasFocus());
            const auto target = *session.activeLayer();
            const auto surface = std::get<c::RasterLayer>(session.document()->layer(target)->payload).surface;
            const auto revision = surface->revision();
            const auto history = session.history().undoDepth();
            key(window.windowHandle(), Qt::Key_Delete);
            CHECK(session.history().undoDepth() == history);
            CHECK(surface->revision() == revision);
            CHECK(std::get<c::RasterLayer>(session.document()->layer(target)->payload).surface == surface);
            key(window.windowHandle(), Qt::Key_F4); // Configured Remove point.
            CHECK(curve->points().size() == 2);
            CHECK(session.history().undoDepth() == history + 1);
            CHECK(curve->hasFocus());
            key(window.windowHandle(), Qt::Key_Delete);
            CHECK(session.history().undoDepth() == history + 1 && surface->revision() == revision);
            // A canvas click deliberately gives keyboard ownership back.
            const auto p = canvas->scene().viewport.documentToViewport({32, 24}, {64, 48}, canvas->scene().logicalViewport);
            QTest::mouseClick(canvas, Qt::LeftButton, {}, QPoint(int(p.x), int(p.y))); settle();
            const auto before = session.history().undoDepth();
            key(window.windowHandle(), Qt::Key_F7);
            CHECK(session.history().undoDepth() == before + 1);
        }
    }
    // Preferences captures keys on the same embedded card, and Escape cancels
    // a capture rather than dismissing the entire window.
    QTimer::singleShot(0, &window, [&] {
        u::PreferencesDialog* dialog = nullptr;
        for (auto* w : window.findChildren<QDialog*>()) if (auto* p = dynamic_cast<u::PreferencesDialog*>(w)) dialog = p;
        CHECK(dialog); if (!dialog) return;
        dialog->findChild<QTabWidget*>("PreferencesTabs")->setCurrentWidget(dialog->findChild<QWidget*>("ShortcutsPanel"));
        auto* p = dynamic_cast<u::ShortcutsPanel*>(dialog->findChild<QWidget*>("ShortcutsPanel"));
        select(*p, "PixelPreviewAction"); p->findChild<QPushButton*>("ShortcutChange")->click();
        key(window.windowHandle(), Qt::Key_Escape); CHECK(dialog->isVisible() && !p->pending());
        bind(*p, "PixelPreviewAction", Qt::Key_F5, Qt::ShiftModifier);
        CHECK(action("PixelPreviewAction")->shortcut() == QKeySequence("Shift+F5"));
        select(*p, "PixelPreviewAction"); p->findChild<QPushButton*>("ShortcutChange")->click();
        key(window.windowHandle(), Qt::Key_Tab);
        CHECK(!p->pending()); CHECK(action("PixelPreviewAction")->shortcut() == QKeySequence("Tab"));
        p->findChild<QPushButton*>("ShortcutChange")->click();
        key(window.windowHandle(), Qt::Key_Backtab, Qt::ShiftModifier);
        CHECK(!p->pending()); CHECK(action("PixelPreviewAction")->shortcut() == QKeySequence("Shift+Tab"));
        dialog->reject();
        CHECK(action("PixelPreviewAction")->shortcut() == QKeySequence("Shift+F3"));
    });
    action("PreferencesAction")->trigger();
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("PreferencesDialog");
        CHECK(dialog); if (!dialog) return;
        dialog->findChild<QTabWidget*>("PreferencesTabs")->setCurrentWidget(dialog->findChild<QWidget*>("ShortcutsPanel"));
        auto* p = dynamic_cast<u::ShortcutsPanel*>(dialog->findChild<QWidget*>("ShortcutsPanel"));
        bind(*p, "PixelPreviewAction", Qt::Key_Tab);
        dialog->accept();
    });
    action("PreferencesAction")->trigger();
    const auto preview = action("PixelPreviewAction")->isChecked();
    key(canvas, Qt::Key_Tab); CHECK(action("PixelPreviewAction")->isChecked() != preview);
    key(canvas, Qt::Key_Tab); CHECK(action("PixelPreviewAction")->isChecked() == preview);
    window.close(); settle();
    settings.clear();
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName("ImageEditorTests"); app.setApplicationName("Shortcuts");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir config; QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    u::applyEditorTheme(app);
    const bool native = app.arguments().contains("--native");
    QVulkanInstance instance; int validation = 0;
    if (native) {
        instance.setApiVersion(QVersionNumber(1, 2)); instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
            QVulkanInstance::DebugMessageTypeFlags types, const void* message) {
            if (types.testFlag(QVulkanInstance::ValidationMessage) && (flags & (QVulkanInstance::ErrorSeverity | QVulkanInstance::WarningSeverity))) {
                ++validation; std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage << '\n';
            } return false;
        });
        if (!instance.create()) return 1;
    }
    registry(); panel(); application(native ? &instance : nullptr);
    if (native) { instance.destroy(); CHECK(validation == 0); std::cout << "Vulkan validation messages: " << validation << '\n'; }
    std::cout << "Shortcut test failures: " << failures << '\n';
    return failures ? 1 : 0;
}
