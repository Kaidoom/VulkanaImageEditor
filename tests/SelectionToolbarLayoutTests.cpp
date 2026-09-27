#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
void settle()
{
    // Tool/page visibility can post a second layout request after the first pass.
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
QRect boundsIn(QWidget* widget, QWidget* parent)
{
    return { widget->mapTo(parent, QPoint {}), widget->size() };
}
QRect visibleRowBounds(QWidget* page, QWidget* relativeTo)
{
    QRect result;
    for (int i = 0; i < page->layout()->count(); ++i) {
        if (auto* widget = page->layout()->itemAt(i)->widget(); widget && widget->isVisible())
            result = result.united(boundsIn(widget, relativeTo));
    }
    return result;
}
bool centered(const QRect& first, const QRect& second)
{
    return std::abs((first.left() + first.right()) - (second.left() + second.right())) <= 3;
}
void sendKey(QObject* target, QEvent::Type type, int key,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeated = false)
{
    QKeyEvent event(type, key, modifiers, QString(), repeated, 1);
    QCoreApplication::sendEvent(target, &event);
    settle();
}
constexpr std::array selectionModeNames { "SelectionModeReplace", "SelectionModeAdd",
    "SelectionModeSubtract", "SelectionModeIntersect" };
struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window { nullptr, false, false };
    ui::ToolOptionsBar* options {};
    ui::OverlayDockWorkspace* workspace {};
    render::CanvasWindow* canvas {};
    QWidget* root {};
    QScrollArea* viewport {};
    Fixture()
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(2400, 900);
        window.show();
        settle();
        options = dynamic_cast<ui::ToolOptionsBar*>(
            window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        root = window.findChild<QWidget*>(QStringLiteral("ToolOptionsRoot"));
        viewport = window.findChild<QScrollArea*>(QStringLiteral("ToolOptionsViewport"));
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        QImage image(96, 72, QImage::Format_RGBA8888);
        image.fill(QColor(123, 151, 181, 255));
        const auto path = assets.filePath(QStringLiteral("selection-toolbar.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        action("ToolAction_marquee");
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return options && workspace && canvas && root && viewport; }
    template <class T = QWidget> T* find(const char* name) const
    {
        auto* result = window.findChild<T*>(QString::fromLatin1(name));
        CHECK(result);
        return result;
    }
    void action(const char* name)
    {
        if (auto* item = find<QAction>(name)) item->trigger();
        settle();
    }
    void button(const char* name)
    {
        if (auto* item = find<QToolButton>(name)) item->click();
        settle();
    }
    void shortcut(const char* sequence)
    {
        for (auto* item : window.findChildren<QAction*>()) {
            if (!item->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence)))) continue;
            item->trigger();
            settle();
            return;
        }
        CHECK(false && "Missing shortcut action");
    }
    core::SelectionState selection() const { return window.editorSession().document()->selection(); }
    void mouse(QEvent::Type type, core::Vec2d point, Qt::MouseButton button,
        Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const auto extent = window.editorSession().document()->canvas().extent;
        const auto& scene = canvas->scene();
        const auto mapped = scene.viewport.documentToViewport(point,
            { double(extent.width), double(extent.height) }, scene.logicalViewport);
        const QPointF p(mapped.x, mapped.y);
        QMouseEvent event(type, p, p, QPointF(canvas->mapToGlobal(p.toPoint())), button, buttons,
            modifiers);
        QCoreApplication::sendEvent(canvas, &event);
    }
    void rectangle(core::Vec2d first, core::Vec2d last)
    {
        action("ToolAction_marquee");
        mouse(QEvent::MouseMove, first, Qt::NoButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, last, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, last, Qt::LeftButton, Qt::NoButton);
        settle();
    }
    void highlighted(int expected) const
    {
        int checked = 0;
        for (std::size_t i = 0; i < selectionModeNames.size(); ++i) {
            const auto* button = find<QToolButton>(selectionModeNames[i]);
            if (!button) continue;
            checked += button->isChecked();
            CHECK(button->isChecked() == (int(i) == expected));
        }
        CHECK(checked == 1);
    }
};

void modifierHighlightsAreExclusiveAndRestoreEveryPersistentMode()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.rectangle({ 12, 12 }, { 44, 38 });
    f.shortcut("Ctrl+Z"); // Display-only changes must preserve this redo branch too.
    const auto mask = f.selection();
    const auto undo = f.window.editorSession().history().undoDepth();
    const auto redo = f.window.editorSession().history().redoDepth();
    auto* panel = f.find("LayersPanel");
    if (!panel) return;
    for (const auto* tool : { "ToolAction_marquee", "ToolAction_lasso" }) {
        f.action(tool);
        for (int selected = 0; selected < int(selectionModeNames.size()); ++selected) {
            f.button(selectionModeNames[std::size_t(selected)]);
            f.highlighted(selected);
            // The embedded QWindow and QWidget panel are separate key recipients
            // on Wayland; neither should become a separate modifier-state owner.
            for (auto* recipient : { static_cast<QObject*>(f.canvas), static_cast<QObject*>(panel) }) {
                if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
                f.highlighted(1);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Shift);
                f.highlighted(selected);
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
                f.highlighted(2);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Alt);
                f.highlighted(selected);

                // Releasing either key from the combination exposes the other
                // modifier, then restores the actual persistent selection mode.
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Alt, Qt::ShiftModifier | Qt::AltModifier);
                f.highlighted(3);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Shift, Qt::AltModifier);
                f.highlighted(2);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Alt);
                f.highlighted(selected);
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier | Qt::AltModifier);
                f.highlighted(3);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Alt, Qt::ShiftModifier);
                f.highlighted(1);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Shift);
                f.highlighted(selected);

                sendKey(recipient, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Shift, Qt::ShiftModifier, true);
                f.highlighted(1);
                sendKey(recipient, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, true);
                f.highlighted(1);
                sendKey(recipient, QEvent::KeyRelease, Qt::Key_Shift);
                f.highlighted(selected);
                CHECK(f.selection() == mask);
                CHECK(f.window.editorSession().history().undoDepth() == undo);
                CHECK(f.window.editorSession().history().redoDepth() == redo);
            }
        }
    }
}

void heldModifierSurvivesSelectionToolAndPanelHandoffs()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.button("SelectionModeSubtract");
    sendKey(f.canvas, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    f.highlighted(1);
    f.action("ToolAction_lasso");
    f.highlighted(1);
    for (const auto* mode : { "LassoModePolygonal", "LassoModeMagnetic", "LassoModeFreehand" }) {
        f.button(mode);
        f.highlighted(1);
    }
    f.action("ToolAction_marquee");
    f.highlighted(1);
    f.button("SelectionModeIntersect");
    f.highlighted(1); // Clicking while held changes the persistent mode, not the override.
    // A native-child focus handoff is not application deactivation. A release
    // received by the panel still restores the newly chosen (Intersect) mode.
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(f.canvas, &focusOut);
    settle();
    f.highlighted(1);
    sendKey(f.workspace->panelOverlay(), QEvent::KeyRelease, Qt::Key_Shift);
    f.highlighted(3);
}

void pointerSnapshotsRefreshHighlightsWithoutChangingSelection()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.rectangle({ 12, 12 }, { 44, 38 });
    f.button("SelectionModeSubtract");
    const auto mask = f.selection();
    const auto depth = f.window.editorSession().history().undoDepth();
    const std::array modifiers { Qt::KeyboardModifiers(Qt::ShiftModifier),
        Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::AltModifier),
        Qt::KeyboardModifiers(Qt::AltModifier), Qt::KeyboardModifiers(Qt::NoModifier) };
    const std::array expected { 1, 3, 2, 2 };
    auto* panel = f.find("LayersPanel");
    if (!panel) return;
    for (const auto* tool : { "ToolAction_marquee", "ToolAction_lasso" }) {
        f.action(tool);
        for (std::size_t i = 0; i < modifiers.size(); ++i) {
            f.mouse(QEvent::MouseMove, { 8, 8 }, Qt::NoButton, Qt::NoButton, modifiers[i]);
            settle();
            f.highlighted(expected[i]);
        }
        for (std::size_t i = 0; i < modifiers.size(); ++i) {
            const QPointF point(15, 15);
            QMouseEvent event(QEvent::MouseMove, point, point,
                QPointF(panel->mapToGlobal(point.toPoint())), Qt::NoButton, Qt::NoButton, modifiers[i]);
            QCoreApplication::sendEvent(panel, &event);
            settle();
            f.highlighted(expected[i]);
        }
    }
    CHECK(f.selection() == mask);
    CHECK(f.window.editorSession().history().undoDepth() == depth);
}

void numericEditingOwnsModifiersAndTerminalEventsClearHeldHighlight()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.rectangle({ 12, 12 }, { 44, 38 });
    f.button("SelectionTransform");
    auto* horizontal = f.find<QDoubleSpinBox>("SelectionGrowHorizontal");
    auto* editor = horizontal ? horizontal->findChild<QLineEdit*>() : nullptr;
    CHECK(editor);
    if (!editor) return;
    const auto mask = f.selection();
    const auto depth = f.window.editorSession().history().undoDepth();
    editor->setFocus(Qt::OtherFocusReason);
    settle();
    CHECK(QApplication::focusWidget() == editor || QApplication::focusWidget() == horizontal);
    for (const auto key : { Qt::Key_Shift, Qt::Key_Alt }) {
        const auto modifier = key == Qt::Key_Shift ? Qt::ShiftModifier : Qt::AltModifier;
        sendKey(editor, QEvent::KeyPress, key, modifier);
        f.highlighted(0);
        sendKey(editor, QEvent::KeyRelease, key);
        f.highlighted(0);
    }
    editor->clearFocus();
    horizontal->clearFocus();
    settle();
    sendKey(f.canvas, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    f.highlighted(1);
    // Even if typing takes ownership midway through a hold, the terminal key
    // release must not leave a stale Add indicator after editing ends.
    editor->setFocus(Qt::OtherFocusReason);
    settle();
    sendKey(editor, QEvent::KeyRelease, Qt::Key_Shift);
    editor->clearFocus();
    horizontal->clearFocus();
    settle();
    f.highlighted(0);
    for (const auto type : { QEvent::WindowDeactivate, QEvent::ApplicationDeactivate }) {
        sendKey(f.canvas, QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
        f.highlighted(2);
        QEvent deactivate(type);
        QCoreApplication::sendEvent(type == QEvent::WindowDeactivate
                ? static_cast<QObject*>(&f.window) : static_cast<QObject*>(qApp), &deactivate);
        settle();
        f.highlighted(0);
        QEvent activate(type == QEvent::WindowDeactivate ? QEvent::WindowActivate : QEvent::ApplicationActivate);
        QCoreApplication::sendEvent(type == QEvent::WindowDeactivate
                ? static_cast<QObject*>(&f.window) : static_cast<QObject*>(qApp), &activate);
        settle();
    }
    CHECK(f.selection() == mask);
    CHECK(f.window.editorSession().history().undoDepth() == depth);
}

void gestureOperationRemainsLatchedWhenHighlightReturnsToPersistentMode()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.rectangle({ 12, 12 }, { 32, 32 });
    const auto mask = f.selection();
    const auto depth = f.window.editorSession().history().undoDepth();
    sendKey(f.canvas, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    f.highlighted(1);
    f.mouse(QEvent::MouseButtonPress, { 50, 12 }, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    f.mouse(QEvent::MouseMove, { 70, 32 }, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    sendKey(f.canvas, QEvent::KeyRelease, Qt::Key_Shift);
    f.highlighted(0);
    CHECK(f.selection() == mask);
    f.mouse(QEvent::MouseButtonRelease, { 70, 32 }, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(f.selection());
    if (!f.selection()) return;
    CHECK(f.selection()->coverageAtDocumentPixel(20, 20) == 255);
    CHECK(f.selection()->coverageAtDocumentPixel(60, 20) == 255);
    CHECK(f.window.editorSession().history().undoDepth() == depth + 1);
    f.shortcut("Ctrl+Z");
    CHECK(f.selection() == mask);
    f.shortcut("Ctrl+Shift+Z");
    CHECK(f.selection()->coverageAtDocumentPixel(20, 20) == 255);
    CHECK(f.selection()->coverageAtDocumentPixel(60, 20) == 255);
    f.highlighted(0);
}

void leadingModesAreCachedAndIndependentOfCentering()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    auto* page = f.options->pageForTool(core::ToolId::Marquee);
    auto* leading = f.options->leadingWidgetForTool(core::ToolId::Lasso);
    auto* label = f.find<QLabel>("ToolOptionsContext");
    CHECK(page && page == f.options->pageForTool(core::ToolId::Lasso));
    CHECK(leading && !page->isAncestorOf(leading));
    auto* selectLeading = f.options->leadingWidgetForTool(core::ToolId::Marquee);
    CHECK(selectLeading && selectLeading != leading && !page->isAncestorOf(selectLeading));
    if (!page || !leading || !label) return;
    CHECK(!leading->isVisible());
    CHECK(selectLeading && selectLeading->isVisible());
    const auto canvasGeometry = f.workspace->canvasContainer()->geometry();
    const auto controlsRectangle = visibleRowBounds(page, f.root);
    CHECK(centered(controlsRectangle, f.root->rect()));
    CHECK(centered(boundsIn(f.viewport, f.root), f.root->rect()));

    for (int pass = 0; pass < 3; ++pass) {
        f.action("ToolAction_lasso");
        CHECK(f.options->leadingWidgetForTool(core::ToolId::Lasso) == leading);
        CHECK(leading->isVisible());
        CHECK(selectLeading && !selectLeading->isVisible());
        CHECK(centered(visibleRowBounds(page, f.root), f.root->rect()));
        CHECK(centered(visibleRowBounds(page, f.root), controlsRectangle));
        CHECK(centered(boundsIn(f.viewport, f.root), f.root->rect()));
        CHECK(boundsIn(label, f.root).right() < boundsIn(leading, f.root).left());
        CHECK(boundsIn(leading, f.root).right() < boundsIn(f.viewport, f.root).left());
        for (const auto* name : { "LassoModeFreehand", "LassoModePolygonal", "LassoModeMagnetic" }) {
            auto* button = f.find<QToolButton>(name);
            CHECK(button && leading->isAncestorOf(button) && button->isVisible());
            CHECK(button && !button->icon().isNull() && !button->toolTip().isEmpty());
        }
        for (auto* text : leading->findChildren<QLabel*>())
            CHECK(text->text() != QStringLiteral("Select mode"));
        f.action("ToolAction_marquee");
        CHECK(!leading->isVisible());
        CHECK(f.options->leadingWidgetForTool(core::ToolId::Marquee) == selectLeading);
        CHECK(selectLeading && selectLeading->isVisible());
        CHECK(centered(visibleRowBounds(page, f.root), controlsRectangle));
        CHECK(f.workspace->canvasContainer()->geometry() == canvasGeometry);
    }
}

void magneticControlsFollowCombinationModesAndPrecedeCommands()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.action("ToolAction_lasso");
    f.button("LassoModeMagnetic");
    const auto names = { "SelectionModeReplace", "SelectionModeAdd", "SelectionModeSubtract",
        "SelectionModeIntersect", "MagneticRadiusControl", "MagneticSourceMergedVisible",
        "MagneticSourceActiveLayer", "SelectionCommand0", "SelectionCommand1", "SelectionCommand2",
        "SelectionTransform" };
    int previousRight = -1;
    for (const auto* name : names) {
        auto* control = f.find(name);
        if (!control) continue;
        CHECK(control->isVisible());
        const auto rectangle = boundsIn(control, f.root);
        CHECK(rectangle.left() > previousRight);
        previousRight = rectangle.right();
    }
    CHECK(centered(visibleRowBounds(f.options->pageForTool(core::ToolId::Lasso), f.root), f.root->rect()));
    f.button("LassoModePolygonal");
    CHECK(!f.find("MagneticRadiusControl")->isVisible());
    CHECK(!f.find("MagneticSourceMergedVisible")->isVisible());
    CHECK(!f.find("MagneticSourceActiveLayer")->isVisible());
}

void actionAvailabilityDistinguishesInactiveFromActiveEmpty()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    const auto checkAvailability = [&f](bool active) {
        for (const auto* name : { "DeselectAction", "InvertSelectionAction" })
            CHECK(f.find<QAction>(name)->isEnabled() == active);
        for (const auto* name : { "SelectionCommand1", "SelectionCommand2" })
            CHECK(f.find<QToolButton>(name)->isEnabled() == active);
        CHECK(f.find<QToolButton>("SelectionCommand0")->isEnabled());
    };
    CHECK(!f.selection());
    checkAvailability(false);
    f.rectangle({ 12, 12 }, { 12, 12 });
    CHECK(f.selection() && f.selection()->bounds().empty());
    checkAvailability(true);
    f.action("ToolAction_lasso");
    checkAvailability(true);
    f.button("SelectionCommand2");
    CHECK(f.selection() && !f.selection()->bounds().empty());
    checkAvailability(true);
    f.button("SelectionCommand1");
    CHECK(!f.selection());
    checkAvailability(false);
}

void adjustmentDisclosureIsCachedAndSeparateFromCtrlT()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    auto* disclosure = f.find<QToolButton>("SelectionTransform");
    auto* group = f.find("SelectionAdjustmentControls");
    auto* growth = f.find("SelectionGrowthPair");
    if (!disclosure || !group || !growth) return;
    CHECK(!disclosure->isChecked() && !group->isVisible());
    CHECK(!disclosure->isEnabled());
    f.rectangle({ 12, 12 }, { 44, 38 });
    CHECK(disclosure->isEnabled());
    CHECK(!group->isVisible());
    const auto mask = f.selection();
    const auto historySize = f.window.editorSession().history().undoDepth();
    f.button("SelectionTransform");
    CHECK(disclosure->isChecked() && group->isVisible());
    CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
    CHECK(boundsIn(disclosure, f.root).right() < boundsIn(group, f.root).left());
    CHECK(growth->layout()->spacing() == 0);
    CHECK(growth->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly).empty());
    auto* horizontal = f.find<QDoubleSpinBox>("SelectionGrowHorizontal");
    auto* vertical = f.find<QDoubleSpinBox>("SelectionGrowVertical");
    CHECK(horizontal && vertical);
    if (horizontal && vertical)
        CHECK(boundsIn(horizontal, growth).right() + 1 == boundsIn(vertical, growth).left());
    f.action("ToolAction_lasso");
    CHECK(f.find("SelectionAdjustmentControls") == group);
    CHECK(disclosure->isChecked() && group->isVisible());
    f.button("SelectionTransform");
    CHECK(!disclosure->isChecked() && !group->isVisible());
    CHECK(f.selection() == mask);
    CHECK(f.window.editorSession().history().undoDepth() == historySize);

    f.action("LayerTransformAction");
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Transform);
    CHECK(f.canvas->scene().transformOverlay.has_value());
    CHECK(f.find<QToolButton>("TransformModeActive")->isChecked());
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Lasso);
    CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.selection() == mask);
    CHECK(f.window.editorSession().history().undoDepth() == historySize);
    f.button("SelectionTransform");
    CHECK(group->isVisible());
    f.button("SelectionCommand1");
    CHECK(!group->isVisible() && !disclosure->isChecked() && !disclosure->isEnabled());
}

void selectMenuReusesToolsAndSafelyRevealsGrowShrink()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    auto* menu = f.find<QMenu>("SelectMenu");
    auto* color = f.find<QAction>("ToolAction_colorselect");
    auto* grow = f.find<QAction>("GrowShrinkSelectionAction");
    auto* disclosure = f.find<QToolButton>("SelectionTransform");
    auto* controls = f.find("SelectionAdjustmentControls");
    if (!menu || !color || !grow || !disclosure || !controls) return;
    CHECK(menu->actions().contains(color)); // The rail and menu share shortcut updates.
    CHECK(menu->actions().contains(grow));
    CHECK(color->shortcut() == QKeySequence("Shift+O"));
    color->trigger();
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::SelectByColor);
    CHECK(!grow->isEnabled());
    for (const auto* name:{"TransformSelectionAction", "TransformSelectedPixelsAction"})
        CHECK(!f.find<QAction>(name)->isEnabled());
    f.rectangle({12,12}, {12,12});
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(!grow->isEnabled());
    for (const auto* name:{"TransformSelectionAction", "TransformSelectedPixelsAction"})
        CHECK(!f.find<QAction>(name)->isEnabled());
    f.rectangle({12,12}, {44,38});
    for (const auto* name:{"TransformSelectionAction", "TransformSelectedPixelsAction"})
        CHECK(f.find<QAction>(name)->isEnabled());
    const auto mask = f.selection();
    const auto depth = f.window.editorSession().history().undoDepth();
    f.action("ToolAction_move");
    const auto menuPoint = f.window.menuBar()->actionGeometry(menu->menuAction()).center();
    QTest::mouseClick(f.window.menuBar(), Qt::LeftButton, Qt::NoModifier, menuPoint);
    settle();
    CHECK(menu->isVisible());
    CHECK(grow->isEnabled());
    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(grow).center());
    settle();
    CHECK(!menu->isVisible());
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
    CHECK(disclosure->isChecked() && controls->isVisible());
    for (const auto* tool : {"ToolAction_marquee", "ToolAction_lasso", "ToolAction_colorselect", "ToolAction_smartselect"}) {
        f.action(tool);
        const auto previous = f.window.editorSession().activeTool();
        disclosure->setChecked(false);
        CHECK(grow->isEnabled());
        grow->trigger();
        settle();
        CHECK(f.window.editorSession().activeTool() == previous);
        CHECK(disclosure->isChecked() && controls->isVisible());
        grow->trigger(); // An already-open disclosure stays open.
        CHECK(disclosure->isChecked());
    }
    f.action("ToolAction_move");
    grow->trigger();
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
    CHECK(disclosure->isChecked() && controls->isVisible());
    CHECK(f.selection() == mask);
    CHECK(f.window.editorSession().history().undoDepth() == depth);

    for (const auto* action : {"LayerTransformAction", "TransformSelectedPixelsAction", "ToolAction_crop"}) {
        f.action("ToolAction_move");
        f.action(action);
        CHECK(!grow->isEnabled());
        QTest::keyClick(f.canvas, Qt::Key_Escape);
        settle();
        CHECK(grow->isEnabled());
    }
    f.action("ToolAction_marquee");
    f.action("LayerTransformAction"); // Ctrl+T's selection-boundary context, too.
    CHECK(!grow->isEnabled());
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    settle();
    CHECK(grow->isEnabled());

    f.mouse(QEvent::MouseButtonPress, {2,2}, Qt::LeftButton, Qt::LeftButton);
    // Menu opening rechecks conflicts, even if a live gesture hasn't yet
    // published another complete UI synchronization.
    CHECK(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
    CHECK(!grow->isEnabled());
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    f.mouse(QEvent::MouseButtonRelease, {2,2}, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(grow->isEnabled());
    CHECK(f.selection() == mask);
    CHECK(f.window.editorSession().history().undoDepth() == depth);
    f.action("DeselectAction");
    CHECK(!grow->isEnabled());
}

void toolNotificationsAreTransientAndPositionedWithoutInterceptingInput()
{
    Fixture f; CHECK(f.valid()); if(!f.valid())return;
    auto* notice=f.find<QLabel>("StatusNotification");
    auto* timer=f.find<QTimer>("StatusNotificationTimer");
    if(!notice || !timer)return;
    CHECK(notice->testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(notice->focusPolicy()==Qt::NoFocus);
    f.window.statusBar()->showMessage("Transform hint without an explicit timeout"); settle();
    CHECK(notice->isVisible());
    CHECK(std::abs(notice->geometry().center().x()-notice->parentWidget()->rect().center().x())<=1);
    CHECK(timer->isActive() && timer->isSingleShot() && timer->interval()==4000);
    const auto position=[&](int index){
        QTimer::singleShot(0,[&]{
            auto* choice=f.find<QComboBox>("ToolHintPosition");
            auto* ok=f.find<QPushButton>("PreferencesOk");
            if(choice)choice->setCurrentIndex(index);
            if(ok)ok->click();
        });
        f.action("PreferencesAction");
        CHECK(QSettings().value("preferences/ui/toolHintPosition").toInt()==index);
    };
    position(0);
    f.window.statusBar()->showMessage("Left hint"); settle();
    CHECK(notice->isVisible() && notice->x()==8);
    position(1);
    f.window.statusBar()->showMessage("Centered hint"); settle();
    CHECK(notice->isVisible());
    CHECK(std::abs(notice->geometry().center().x()-notice->parentWidget()->rect().center().x())<=1);
    // Mouse enters the displayed area through the underlying widget/native
    // surface, not through the transparent label itself.
    const auto global=notice->mapToGlobal(notice->rect().center());
    const QPointF p=f.canvas->mapFromGlobal(global);
    QMouseEvent move(QEvent::MouseMove,p,p,QPointF(global),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(f.canvas,&move); settle();
    CHECK(!notice->isVisible() && f.window.statusBar()->currentMessage().isEmpty());
    position(2);
    f.window.statusBar()->showMessage("Disabled hint"); settle(); CHECK(!notice->isVisible());
    position(0);
    f.window.statusBar()->showMessage("This previously persistent hint expires"); settle();
    CHECK(notice->isVisible());
    QTest::qWait(4200); settle();
    CHECK(!notice->isVisible() && f.window.statusBar()->currentMessage().isEmpty());
    CHECK(!timer->isActive());
}

void narrowLayoutUsesOverflowWithoutCoveringLeadingModes()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.rectangle({ 12, 12 }, { 44, 38 });
    f.action("ToolAction_lasso");
    f.button("LassoModeMagnetic");
    f.button("SelectionTransform");
    f.window.resize(980, 700);
    settle();
    auto* leading = f.options->leadingWidgetForTool(core::ToolId::Lasso);
    auto* next = f.find<QToolButton>("NextToolOptions");
    auto* previous = f.find<QToolButton>("PreviousToolOptions");
    auto* scroll = f.viewport->horizontalScrollBar();
    CHECK(leading && next && previous);
    if (!leading || !next || !previous) return;
    CHECK(scroll->maximum() > scroll->minimum());
    CHECK(next->isVisible() && previous->isVisible());
    CHECK(f.viewport->width() > 0);
    CHECK(boundsIn(leading, f.root).right() < boundsIn(f.viewport, f.root).left());
    CHECK(centered(boundsIn(f.viewport, f.root), f.root->rect()));
    const auto leadingBefore = boundsIn(leading, f.root);
    const auto canvasBefore = f.workspace->canvasContainer()->geometry();
    scroll->setValue(scroll->minimum());
    f.button("NextToolOptions");
    CHECK(scroll->value() > scroll->minimum());
    for (int i = 0; i < 20 && next->isEnabled(); ++i) f.button("NextToolOptions");
    CHECK(scroll->value() == scroll->maximum());
    CHECK(!next->isEnabled() && previous->isEnabled());
    CHECK(boundsIn(leading, f.root) == leadingBefore);
    CHECK(f.workspace->canvasContainer()->geometry() == canvasBefore);
    f.button("PreviousToolOptions");
    CHECK(scroll->value() < scroll->maximum());
    CHECK(f.find<QToolButton>("LassoModeMagnetic")->isVisible());
}

void captureReviewSheet(const QString& output)
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.window.resize(2000, 850);
    settle();
    QImage sheet(2000, 800, QImage::Format_RGBA8888);
    sheet.fill(QColor(20, 23, 29));
    QPainter painter(&sheet);
    painter.setPen(QColor(202, 211, 228));
    QFont font = painter.font();
    font.setPixelSize(15);
    painter.setFont(font);
    int y = 0;
    const auto capture = [&f, &painter, &y](const QString& caption) {
        settle();
        painter.drawText(12, y + 23, caption);
        painter.drawPixmap(QPoint(0, y + 36), f.options->grab());
        y += 100;
    };
    capture(QStringLiteral("Rectangle · no selection · inactive commands disabled"));
    f.rectangle({12,12}, {44,38});
    capture(QStringLiteral("Rectangle · selection active · adjustments collapsed"));
    f.button("SelectionTransform");
    capture(QStringLiteral("Rectangle · adjustments expanded"));
    f.button("SelectionTransform");
    f.action("ToolAction_lasso");
    capture(QStringLiteral("Freehand · leading modes do not move the centered controls"));
    f.button("LassoModeMagnetic");
    capture(QStringLiteral("Magnetic · radius and reference sit before Select All"));
    f.button("SelectionTransform");
    capture(QStringLiteral("Magnetic · expanded adjustments remain on the right"));
    f.window.resize(980,700);
    capture(QStringLiteral("Narrow 980 px · leading modes remain accessible; center uses overflow"));
    auto* scroll = f.viewport->horizontalScrollBar();
    scroll->setValue(scroll->maximum());
    capture(QStringLiteral("Narrow 980 px · scrolled to the final adjustment"));
    painter.end();
    CHECK(sheet.save(output));
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SelectionToolbarLayout"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(app);
    if (const auto output = qEnvironmentVariable("IMAGEEDITOR_TEST_SELECTION_TOOLBAR_PREVIEW"); !output.isEmpty()) {
        captureReviewSheet(output);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    leadingModesAreCachedAndIndependentOfCentering();
    modifierHighlightsAreExclusiveAndRestoreEveryPersistentMode();
    heldModifierSurvivesSelectionToolAndPanelHandoffs();
    pointerSnapshotsRefreshHighlightsWithoutChangingSelection();
    numericEditingOwnsModifiersAndTerminalEventsClearHeldHighlight();
    gestureOperationRemainsLatchedWhenHighlightReturnsToPersistentMode();
    magneticControlsFollowCombinationModesAndPrecedeCommands();
    actionAvailabilityDistinguishesInactiveFromActiveEmpty();
    adjustmentDisclosureIsCachedAndSeparateFromCtrlT();
    selectMenuReusesToolsAndSafelyRevealsGrowShrink();
    toolNotificationsAreTransientAndPositionedWithoutInterceptingInput();
    narrowLayoutUsesOverflowWithoutCoveringLeadingModes();
    if (failures) std::cerr << failures << " selection toolbar layout assertion(s) failed\n";
    else std::cout << "All selection toolbar layout tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
