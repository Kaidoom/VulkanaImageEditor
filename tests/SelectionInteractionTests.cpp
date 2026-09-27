#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFocusEvent>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPointingDevice>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabletEvent>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVulkanInstance>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

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
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

QAction* shortcutAction(ui::MainWindow& window, const QString& shortcut)
{
    for (auto* action : window.findChildren<QAction*>()) {
        if (action->shortcuts().contains(QKeySequence(shortcut))) return action;
    }
    return nullptr;
}

void sendMouse(render::CanvasWindow& canvas, QEvent::Type type, QPointF local,
    Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, local, local, QPointF(canvas.mapToGlobal(local.toPoint())),
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&canvas, &event);
}

void sendMouse(QWidget& widget, QEvent::Type type, QPointF local,
    Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, local, local, QPointF(widget.mapToGlobal(local.toPoint())),
        button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window {nullptr, false, false};
    render::CanvasWindow* canvas {nullptr};
    ui::OverlayDockWorkspace* workspace {nullptr};
    ui::ToolOptionsBar* options {nullptr};
    ui::CrossWindowPointerRouter* router {nullptr};

    Fixture()
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1420, 860);
        window.show();
        settle();
        settle();
        for (auto* candidate : QGuiApplication::allWindows()) {
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        }
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        options = dynamic_cast<ui::ToolOptionsBar*>(
            window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        QImage image(64, 48, QImage::Format_RGBA8888);
        image.fill(QColor(50, 140, 210, 255));
        const auto path = assets.filePath(QStringLiteral("selection-source.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        settle();
    }

    ~Fixture()
    {
        window.close();
        settle();
    }

    [[nodiscard]] bool valid() const
    { return canvas && workspace && options && router && window.editorSession().document(); }

    [[nodiscard]] const core::Document& document() const
    { return *window.editorSession().document(); }
    [[nodiscard]] const core::History& history() const
    { return window.editorSession().history(); }
    [[nodiscard]] core::SelectionState selection() const { return document().selection(); }

    [[nodiscard]] QPointF logical(core::Vec2d point) const
    {
        const auto extent = document().canvas().extent;
        const auto& scene = canvas->scene();
        const auto p = scene.viewport.documentToViewport(point,
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return {p.x, p.y};
    }

    void action(const char* name)
    {
        auto* target = window.findChild<QAction*>(QString::fromLatin1(name));
        CHECK(target);
        if (target) target->trigger();
        settle();
    }
    void shortcut(const char* key)
    {
        auto* target = shortcutAction(window, QString::fromLatin1(key));
        CHECK(target);
        if (target) target->trigger();
        settle();
    }
    void marquee() { shortcut("M"); }
    void undo() { shortcut("Ctrl+Z"); }
    void redo() { shortcut("Ctrl+Shift+Z"); }
    void adjustments()
    {
        auto* toggle = window.findChild<QToolButton*>(QStringLiteral("SelectionTransform"));
        CHECK(toggle && toggle->isEnabled());
        if (toggle && !toggle->isChecked()) toggle->click();
        settle();
    }

    void mode(const char* suffix)
    {
        auto* button = window.findChild<QToolButton*>(
            QStringLiteral("SelectionMode") + QString::fromLatin1(suffix));
        CHECK(button);
        if (button) button->click();
        settle();
    }
    void press(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        sendMouse(*canvas, QEvent::MouseMove, logical(p), Qt::NoButton, Qt::NoButton, mods);
        sendMouse(*canvas, QEvent::MouseButtonPress, logical(p), Qt::LeftButton, Qt::LeftButton, mods);
    }
    void move(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    { sendMouse(*canvas, QEvent::MouseMove, logical(p), Qt::NoButton, Qt::LeftButton, mods); }
    void release(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        sendMouse(*canvas, QEvent::MouseButtonRelease, logical(p), Qt::LeftButton, Qt::NoButton, mods);
        settle();
    }
    void rectangle(core::Vec2d a, core::Vec2d b,
        Qt::KeyboardModifiers mods = Qt::NoModifier)
    { press(a, mods); move(b, mods); release(b, mods); }
};

void checkPixel(const core::SelectionState& selection, int x, int y, int expected)
{
    CHECK(selection);
    if (selection) CHECK(selection->coverageAtDocumentPixel(x, y) == expected);
}

void marqueeControlsAreCachedStyledAndNeverResizeCanvas()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    const auto base = f.workspace->canvasContainer()->geometry();
    const auto toolbar = f.options->geometry();
    auto* page = f.window.findChild<QWidget*>(QStringLiteral("RectangleSelectionOptionsPage"));
    CHECK(page);
    CHECK(f.options->pageForTool(core::ToolId::Marquee) == page);
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_M);
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
    CHECK(page && page->isVisible());
    for (const auto* name : {"Replace", "Add", "Subtract", "Intersect"}) {
        auto* button = f.window.findChild<QToolButton*>(
            QStringLiteral("SelectionMode") + QString::fromLatin1(name));
        CHECK(button);
        if (!button) continue;
        CHECK(button->property("toolOptionsButton").toBool());
        button->click();
        CHECK(button->isChecked());
    }
    for (int i = 0; i < 3; ++i) {
        f.shortcut("B");
        CHECK(page && !page->isVisible());
        f.marquee();
        CHECK(f.options->pageForTool(core::ToolId::Marquee) == page);
        CHECK(f.workspace->canvasContainer()->geometry() == base);
        CHECK(f.options->geometry() == toolbar);
    }
    CHECK(f.history().undoDepth() == 0);
    auto* rail = f.window.findChild<QToolBar*>(QStringLiteral("ToolRail"));
    auto* eye = shortcutAction(f.window, QStringLiteral("I"));
    CHECK(rail && eye);
    if (rail && eye) {
        const auto actions = rail->actions();
        const auto index = actions.indexOf(eye);
        CHECK(index >= 0 && index + 1 < actions.size());
        if (index >= 0 && index + 1 < actions.size())
            CHECK(rail->widgetForAction(actions[index + 1]) != nullptr);
        CHECK(actions.indexOf(shortcutAction(f.window, QStringLiteral("T"))) < index);
    }
}

void livePreviewIsImmutablePixelAlignedAndOneUndoAction()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    const auto revision = f.document().selectionRevision();
    const auto documentRevision = f.document().revision();
    const auto depth = f.history().undoDepth();
    f.press({3.2, 5.8});
    CHECK(f.canvas->selectionDragging());
    for (int i = 1; i <= 25; ++i) {
        f.move({3.2 + double(i) * 0.6, 5.8 + double(i) * 0.6});
        CHECK(!f.selection());
        CHECK(f.document().selectionRevision() == revision);
        CHECK(f.document().revision() == documentRevision);
        CHECK(f.history().undoDepth() == depth);
    }
    f.move({17.8, 21.2});
    CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
    f.release({17.8, 21.2});
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({3, 6, 15, 15}));
    CHECK(f.document().selectionRevision() > revision);
    CHECK(f.history().undoDepth() == depth + 1);
    checkPixel(f.selection(), 3, 6, 255);
    checkPixel(f.selection(), 17, 20, 255);
    checkPixel(f.selection(), 18, 20, 0);
    const auto committed = f.selection();
    f.undo();
    CHECK(!f.selection());
    f.redo();
    CHECK(f.selection() && f.selection()->equivalent(*committed));
    CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
}

void modesLatchModifiersAndRepresentHolesAndDisconnectedRegions()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({2, 2}, {22, 22});
    f.press({30, 4}, Qt::ShiftModifier);
    f.move({40, 16}); // Releasing Shift cannot change latched Add into Replace.
    f.release({40, 16});
    checkPixel(f.selection(), 5, 5, 255);
    checkPixel(f.selection(), 35, 8, 255);
    checkPixel(f.selection(), 26, 8, 0);
    f.press({7, 7}, Qt::AltModifier);
    f.move({16, 16}, Qt::ShiftModifier); // Modifier changes during drag are ignored.
    f.release({16, 16});
    checkPixel(f.selection(), 10, 10, 0);
    checkPixel(f.selection(), 5, 5, 255);
    checkPixel(f.selection(), 35, 8, 255);
    const auto withHole = f.selection();
    CHECK(f.canvas->scene().selectionEdges && f.canvas->scene().selectionEdges->size() >= 12);
    bool holeEdge = false;
    if (f.canvas->scene().selectionEdges) {
        for (const auto& edge : *f.canvas->scene().selectionEdges) {
            if (edge.from.x == 7 && edge.to.x == 7
                && std::min(edge.from.y, edge.to.y) == 7
                && std::max(edge.from.y, edge.to.y) == 16) holeEdge = true;
        }
    }
    CHECK(holeEdge);
    f.press({0, 0}, Qt::ShiftModifier | Qt::AltModifier);
    f.move({19, 19});
    f.release({19, 19});
    checkPixel(f.selection(), 5, 5, 255);
    checkPixel(f.selection(), 10, 10, 0);
    checkPixel(f.selection(), 35, 8, 0);
    f.undo();
    CHECK(f.selection() && f.selection()->equivalent(*withHole));
    f.mode("Intersect");
    f.rectangle({28, 0}, {44, 20});
    checkPixel(f.selection(), 5, 5, 0);
    checkPixel(f.selection(), 35, 8, 255);
    f.mode("Subtract");
    f.rectangle({30, 4}, {40, 16});
    CHECK(f.selection() && f.selection()->bounds().empty());
}

void inactiveEmptyNoopAndCancelledSelectionsPreserveRedo()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.action("DeselectAction");
    CHECK(f.history().undoDepth() == 0);
    f.rectangle({6, 6}, {6, 6});
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(f.history().undoDepth() == 1);
    CHECK(!f.canvas->selectionAnimationActive());
    f.action("DeselectAction");
    CHECK(!f.selection());
    CHECK(f.history().undoDepth() == 2);
    f.undo();
    CHECK(f.selection() && f.selection()->bounds().empty());
    const auto revision = f.document().selectionRevision();
    const auto redoDepth = f.history().redoDepth();
    const auto memory = f.history().memoryUsed();
    f.rectangle({6, 6}, {6, 6});
    CHECK(f.history().undoDepth() == 1);
    CHECK(f.history().redoDepth() == redoDepth);
    CHECK(f.history().memoryUsed() == memory);
    CHECK(f.document().selectionRevision() == revision);
    f.press({3, 4});
    f.move({18, 19});
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    settle();
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(f.history().redoDepth() == redoDepth);
    CHECK(f.document().selectionRevision() == revision);
    f.redo();
    CHECK(!f.selection());
    f.action("SelectAllAction");
    const auto all = f.selection();
    CHECK(all && all->bounds() == core::RectI({0, 0, 64, 48}));
    const auto fullDepth = f.history().undoDepth();
    f.action("SelectAllAction");
    CHECK(f.history().undoDepth() == fullDepth);
    f.action("InvertSelectionAction");
    CHECK(f.selection() && f.selection()->bounds().empty());
    f.action("InvertSelectionAction");
    CHECK(f.selection() && f.selection()->equivalent(*all));
}

void cancellationRestoresExistingMaskAndClearsCapture()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({3, 4}, {21, 25});
    const auto before = f.selection();
    const auto revision = f.document().selectionRevision();
    const auto depth = f.history().undoDepth();
    for (int reason = 0; reason < 3; ++reason) {
        f.marquee();
        f.press({30, 30});
        f.move({55, 42});
        CHECK(f.canvas->selectionDragging());
        if (reason == 0) QTest::keyClick(f.canvas, Qt::Key_Escape);
        if (reason == 1) {
            QFocusEvent focus(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(f.canvas, &focus);
        }
        if (reason == 2) f.shortcut("V");
        settle();
        CHECK(!f.canvas->selectionDragging());
        CHECK(f.selection() && f.selection()->equivalent(*before));
        CHECK(f.document().selectionRevision() == revision);
        CHECK(f.history().undoDepth() == depth);
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
}

void releaseOverPanelFinishesTheOriginalCanvasGesture()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    auto* panel = f.window.findChild<QWidget*>(QStringLiteral("LayersPanel"));
    CHECK(panel);
    if (!panel) return;
    const auto routes = f.router->routedEventCount();
    f.press({4, 4});
    CHECK(f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    const QPointF point(panel->rect().center());
    sendMouse(*panel, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
    sendMouse(*panel, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.selection() && !f.selection()->bounds().empty());
    CHECK(f.history().undoDepth() == 1);
    CHECK(f.router->routedEventCount() >= routes + 2);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(QWidget::mouseGrabber() == nullptr);
}

void tabletSelectionKeepsOwnershipWhenSpaceIsPressedDuringItsGesture()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    const QPointingDevice pen(QStringLiteral("Selection test pen"), 19342,
        QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure,
        1, 2);
    const auto tablet = [&](QEvent::Type type, core::Vec2d point) {
        const bool released = type == QEvent::TabletRelease;
        const auto local = f.logical(point);
        QTabletEvent event(type, &pen, local, QPointF(f.canvas->mapToGlobal(local.toPoint())),
            released ? 0.0 : 0.5, 0, 0, 0, 0, 0, Qt::NoModifier,
            type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
            released ? Qt::NoButton : Qt::LeftButton);
        QCoreApplication::sendEvent(f.canvas, &event);
    };
    tablet(QEvent::TabletPress, {5, 7});
    tablet(QEvent::TabletMove, {16, 19});
    CHECK(f.canvas->selectionDragging());
    QTest::keyPress(f.canvas, Qt::Key_Space);
    tablet(QEvent::TabletMove, {24, 28});
    tablet(QEvent::TabletRelease, {24, 28});
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({5, 7, 19, 21}));
    CHECK(f.history().undoDepth() == 1);
    QTest::keyRelease(f.canvas, Qt::Key_Space);
    settle();
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    // The release cannot leave a dangling gesture that absorbs the next press.
    f.rectangle({31, 10}, {45, 25});
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({31, 10, 14, 15}));
    CHECK(f.history().undoDepth() == 2);
}

void middlePanCannotStartASelectionOrLoseOwnershipToAnExtraButton()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({5, 7}, {24, 28});
    const auto before = f.selection();
    const auto revision = f.document().selectionRevision();
    const auto depth = f.history().undoDepth();
    const auto point = f.logical({32, 24});
    sendMouse(*f.canvas, QEvent::MouseButtonPress, point,
        Qt::MiddleButton, Qt::MiddleButton);
    sendMouse(*f.canvas, QEvent::MouseMove, point + QPointF(10, 6),
        Qt::NoButton, Qt::MiddleButton);
    const auto panBeforeExtraPress = f.canvas->scene().viewport.pan();
    sendMouse(*f.canvas, QEvent::MouseButtonPress, point + QPointF(10, 6),
        Qt::LeftButton, Qt::MiddleButton | Qt::LeftButton);
    CHECK(!f.canvas->selectionDragging());
    sendMouse(*f.canvas, QEvent::MouseMove, point + QPointF(24, 15),
        Qt::NoButton, Qt::MiddleButton | Qt::LeftButton);
    CHECK(f.canvas->scene().viewport.pan() != panBeforeExtraPress);
    sendMouse(*f.canvas, QEvent::MouseButtonRelease, point + QPointF(24, 15),
        Qt::LeftButton, Qt::MiddleButton);
    CHECK(!f.canvas->selectionDragging());
    const auto panBeforeContinuedMove = f.canvas->scene().viewport.pan();
    sendMouse(*f.canvas, QEvent::MouseMove, point + QPointF(40, 21),
        Qt::NoButton, Qt::MiddleButton);
    CHECK(f.canvas->scene().viewport.pan() != panBeforeContinuedMove);
    sendMouse(*f.canvas, QEvent::MouseButtonRelease, point + QPointF(40, 21),
        Qt::MiddleButton, Qt::NoButton);
    const auto finishedPan = f.canvas->scene().viewport.pan();
    sendMouse(*f.canvas, QEvent::MouseMove, point + QPointF(50, 25),
        Qt::NoButton, Qt::NoButton);
    CHECK(f.canvas->scene().viewport.pan() == finishedPan);
    CHECK(f.selection() && f.selection()->equivalent(*before));
    CHECK(f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void losingTheNativeSurfaceCancelsTheUncommittedSelection()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({3, 5}, {20, 24});
    const auto before = f.selection();
    const auto revision = f.document().selectionRevision();
    const auto depth = f.history().undoDepth();
    f.press({25, 28});
    f.move({45, 40});
    CHECK(f.canvas->selectionDragging());
    QPlatformSurfaceEvent destroyed(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
    QCoreApplication::sendEvent(f.canvas, &destroyed);
    CHECK(!f.canvas->selectionDragging());
    CHECK(!f.canvas->selectionAnimationActive());
    CHECK(f.selection() && f.selection()->equivalent(*before));
    CHECK(f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    // Re-creation cannot publish the stale gesture or keep its input capture.
    QPlatformSurfaceEvent created(QPlatformSurfaceEvent::SurfaceCreated);
    QCoreApplication::sendEvent(f.canvas, &created);
    f.release({45, 40});
    CHECK(f.selection() && f.selection()->equivalent(*before));
    CHECK(f.history().undoDepth() == depth);
}

void textFieldsOwnSelectionAndCopyShortcuts()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({3, 4}, {21, 25});
    const auto before = f.selection();
    const auto layers = f.document().layers().size();
    const auto depth = f.history().undoDepth();
    f.shortcut("B");
    QLineEdit editor(&f.window);
    editor.setText(QStringLiteral("selection shortcut ownership"));
    editor.setGeometry(90, 90, 350, 36);
    editor.show();
    editor.setFocus(Qt::OtherFocusReason);
    settle();
    QTest::keyClick(&editor, Qt::Key_A, Qt::ControlModifier);
    CHECK(editor.selectedText() == editor.text());
    QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyClick(&editor, Qt::Key_J, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_M);
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Brush);
    CHECK(f.selection() && f.selection()->equivalent(*before));
    CHECK(f.document().layers().size() == layers);
    CHECK(f.history().undoDepth() == depth);
    editor.hide();
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_D, Qt::ControlModifier);
    settle();
    CHECK(!f.selection());
    QTest::keyClick(f.canvas, Qt::Key_A, Qt::ControlModifier);
    settle();
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({0, 0, 64, 48}));
    QTest::keyClick(f.canvas, Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.selection() && f.selection()->bounds().empty());
}

void layerViaCopyPreservesMaskAndUndoesActiveLayerAlongsideCreation()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    const auto originalId = f.window.editorSession().activeLayer();
    const auto original = f.document().layer(*originalId);
    const auto surface = std::get<core::RasterLayer>(original->payload).surface;
    const auto rasterRevision = surface->revision();
    f.rectangle({8, 6}, {28, 20});
    const auto selected = f.selection();
    const auto depth = f.history().undoDepth();
    f.action("LayerViaCopyAction");
    CHECK(f.document().layers().size() == 2);
    const auto copiedId = f.window.editorSession().activeLayer();
    CHECK(copiedId && copiedId != originalId);
    CHECK(f.document().layers().back().id == copiedId);
    CHECK(f.selection() && f.selection()->equivalent(*selected));
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(surface->revision() == rasterRevision);
    f.undo();
    CHECK(f.document().layers().size() == 1);
    CHECK(f.window.editorSession().activeLayer() == originalId);
    CHECK(f.selection() && f.selection()->equivalent(*selected));
    f.redo();
    CHECK(f.document().layers().size() == 2);
    CHECK(f.window.editorSession().activeLayer() == copiedId);
    f.undo();
    f.action("InvertSelectionAction");
    f.action("DeselectAction");
    f.action("LayerViaCopyAction"); // No selection means whole-layer duplicate.
    CHECK(f.document().layers().size() == 2);
    const auto& duplicate = std::get<core::RasterLayer>(f.document().layers().back().payload);
    CHECK(duplicate.surface->extent() == surface->extent());
    f.marquee();
    f.rectangle({4, 4}, {4, 4});
    const auto emptyDepth = f.history().undoDepth();
    f.action("LayerViaCopyAction");
    CHECK(f.document().layers().size() == 2);
    CHECK(f.history().undoDepth() == emptyDepth);
}

void canvasResizeUndoesMaskClippingAndRetainsLayerStorage()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({18, 12}, {60, 45});
    const auto before = f.selection();
    const auto& layer = f.document().layers().front();
    const auto surface = std::get<core::RasterLayer>(layer.payload).surface;
    const auto depth = f.history().undoDepth();
    bool resizedDialog = false;
    QTimer::singleShot(0, &f.window, [&] {
        auto* dialog = f.window.findChild<QDialog*>(QStringLiteral("NewDocumentDialog"));
        CHECK(dialog);
        if (!dialog) return;
        auto* width = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
        auto* height = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
        CHECK(width && height);
        if (!width || !height) { dialog->reject(); return; }
        width->setValue(32);
        height->setValue(24);
        resizedDialog = true;
        dialog->accept();
    });
    f.shortcut("Ctrl+Alt+C");
    CHECK(resizedDialog);
    CHECK(f.document().canvas().extent == core::Extent2u({32, 24}));
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({18, 12, 14, 12}));
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(std::get<core::RasterLayer>(f.document().layers().front().payload).surface == surface);
    f.undo();
    CHECK(f.document().canvas().extent == core::Extent2u({64, 48}));
    CHECK(f.selection() && f.selection()->equivalent(*before));
    f.redo();
    CHECK(f.selection() && f.selection()->extent() == core::Extent2u({32, 24}));
}

void redoUsesActualKeyDispatchAcrossCanvasAndPanelsWithoutStealingTyping()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({4, 6}, {30, 24});
    const auto expected = f.selection();
    auto* redo = shortcutAction(f.window, QStringLiteral("Ctrl+Shift+Z"));
    CHECK(redo);
    if (!redo) return;
    const auto bindings = redo->shortcuts();
    for (qsizetype i = 0; i < bindings.size(); ++i) {
        CHECK(bindings.count(bindings[i]) == 1);
    }
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_Z, Qt::ControlModifier);
    settle();
    CHECK(!f.selection());
    CHECK(f.history().redoDepth() == 1);
    QTest::keyClick(f.canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    CHECK(f.history().redoDepth() == 0);

    QWidget panelFocus(f.workspace->panelOverlay());
    panelFocus.setGeometry(80, 80, 160, 32);
    panelFocus.setFocusPolicy(Qt::StrongFocus);
    panelFocus.show();
    f.workspace->panelOverlay()->activateWindow();
    panelFocus.setFocus(Qt::OtherFocusReason);
    settle();
    QTest::keyClick(&panelFocus, Qt::Key_Z, Qt::ControlModifier);
    settle();
    CHECK(!f.selection());
    QTest::keyClick(&panelFocus, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));

    f.undo();
    QLineEdit text(&f.window);
    text.setGeometry(80, 80, 240, 36);
    text.setText(QStringLiteral("before"));
    text.show();
    text.window()->activateWindow();
    text.setFocus(Qt::OtherFocusReason);
    settle();
    CHECK(text.hasFocus());
    QTest::keyClicks(&text, QStringLiteral(" after"));
    QTest::keyClick(&text, Qt::Key_Z, Qt::ControlModifier);
    CHECK(text.isRedoAvailable());
    QTest::keyClick(&text, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(!f.selection());
    CHECK(f.history().redoDepth() == 1);
    CHECK(f.history().undoDepth() == 0);
    CHECK(text.hasFocus());
}

void explicitNewDocumentsKeepDimensionsAndBackgroundWhenImagesAreDropped()
{
    const std::array<std::pair<const char*, core::Rgba8>, 3> backgrounds {{
        {"Transparent", {0, 0, 0, 0}}, {"White", {255, 255, 255, 255}},
        {"Black", {0, 0, 0, 255}},
    }};
    for (const auto& [backgroundName, expectedColor] : backgrounds) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid()) return;
        bool accepted = false;
        QTimer::singleShot(0, &f.window, [&] {
            auto* dialog = dynamic_cast<ui::NewDocumentDialog*>(
                f.window.findChild<QDialog*>(QStringLiteral("NewDocumentDialog")));
            CHECK(dialog);
            if (!dialog) return;
            auto* width = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
            auto* height = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
            auto* background = dialog->findChild<QComboBox*>(QStringLiteral("CanvasBackgroundCombo"));
            CHECK(width && height && background);
            if (!width || !height || !background) { dialog->reject(); return; }
            width->setValue(80);
            height->setValue(60);
            const auto choice = background->findText(QString::fromLatin1(backgroundName));
            CHECK(choice >= 0);
            if (choice < 0) { dialog->reject(); return; }
            background->setCurrentIndex(choice);
            accepted = true;
            dialog->accept();
        });
        f.shortcut("Ctrl+N");
        CHECK(accepted);
        CHECK(f.document().canvas().extent == core::Extent2u({80, 60}));
        CHECK(f.document().layers().size() == 1);
        CHECK(f.history().undoDepth() == 0);
        const auto originalLayerId = f.document().layers().front().id;
        const auto originalSurface = std::get<core::RasterLayer>(f.document().layers().front().payload).surface;
        const auto revision = originalSurface->revision();
        std::array<std::byte, 4> rgba;
        for (const auto corner : {core::Vec2d {0, 0}, core::Vec2d {79, 59}}) {
            originalSurface->copyRgba8({int(corner.x), int(corner.y), 1, 1}, rgba, 4);
            CHECK(std::to_integer<int>(rgba[0]) == expectedColor.red);
            CHECK(std::to_integer<int>(rgba[1]) == expectedColor.green);
            CHECK(std::to_integer<int>(rgba[2]) == expectedColor.blue);
            CHECK(std::to_integer<int>(rgba[3]) == expectedColor.alpha);
        }
        QImage first(11, 7, QImage::Format_RGBA8888);
        first.fill(QColor(220, 30, 80));
        QImage second(23, 17, QImage::Format_RGBA8888);
        second.fill(QColor(30, 150, 220));
        const auto firstPath = f.assets.filePath(QStringLiteral("first-drop.png"));
        const auto secondPath = f.assets.filePath(QStringLiteral("second-drop.png"));
        CHECK(first.save(firstPath));
        CHECK(second.save(secondPath));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(firstPath), QUrl::fromLocalFile(secondPath)});
        // The document strip now owns the top 34 pixels; exercise the canvas.
        QDragEnterEvent enter({240, 140}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(f.canvas, &enter);
        CHECK(enter.isAccepted());
        QDropEvent drop({240, 140}, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(f.canvas, &drop);
        settle();
        CHECK(drop.isAccepted());
        CHECK(f.document().canvas().extent == core::Extent2u({80, 60}));
        CHECK(f.document().layers().size() == 3);
        CHECK(f.history().undoDepth() == 2);
        CHECK(f.document().layers().front().id == originalLayerId);
        CHECK(originalSurface->revision() == revision);
        if (f.document().layers().size() == 3) {
            CHECK(f.document().layers()[1].name == "first-drop");
            CHECK(f.document().layers()[2].name == "second-drop");
            CHECK(f.window.editorSession().activeLayer() == f.document().layers()[2].id);
        }
        f.undo();
        f.undo();
        CHECK(f.document().layers().size() == 1);
        CHECK(f.document().canvas().extent == core::Extent2u({80, 60}));
        CHECK(std::get<core::RasterLayer>(f.document().layers().front().payload).surface == originalSurface);
        f.redo();
        f.redo();
        CHECK(f.document().layers().size() == 3);
        CHECK(f.document().canvas().extent == core::Extent2u({80, 60}));
    }
}

void startupOpenImageUsesImageDimensionsAndFileCancellationKeepsCanvasChoices()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    QImage image(71, 39, QImage::Format_RGBA8888);
    image.fill(QColor(21, 82, 193, 127));
    const auto imagePath = f.assets.filePath(QStringLiteral("startup-image.png"));
    CHECK(image.save(imagePath));
    // Exercise the real file-picker workflow without relying on a desktop
    // portal outside this offscreen test process. Production retains Qt's
    // platform file dialog choice.
    const bool previousNativePolicy = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    int stage = 0;
    int ticks = 0;
    QTimer drive;
    drive.setInterval(10);
    QObject::connect(&drive, &QTimer::timeout, &f.window, [&] {
        ++ticks;
        auto* modal = QApplication::activeModalWidget();
        // Document cards are embedded under the workspace shield, while the
        // nested file picker is still a real modal. Prefer that nested picker.
        if (!modal) modal = f.window.findChild<QDialog*>(QStringLiteral("NewDocumentDialog"));
        if (ticks > 150) {
            if (ticks == 151) CHECK(false);
            // Keep rejecting until both nested file picker and startup dialog
            // have unwound; a failing regression must never hang this suite.
            if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
            return;
        }
        if (auto* dialog = dynamic_cast<ui::NewDocumentDialog*>(modal)) {
            if (stage != 0 && stage != 2) return;
            auto* width = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasWidthSpinBox"));
            auto* height = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CanvasHeightSpinBox"));
            auto* background = dialog->findChild<QComboBox*>(QStringLiteral("CanvasBackgroundCombo"));
            // Native popup ownership is the editor top-level, not the embedded
            // card. The menu action still operates on this startup dialog.
            auto* openImage = f.window.findChild<QAction*>(QStringLiteral("OpenImageFromNewAction"));
            CHECK(width && height && background && openImage);
            if (!width || !height || !background || !openImage) {
                drive.stop();
                dialog->reject();
                return;
            }
            if (stage == 0) {
                width->setValue(333);
                height->setValue(222);
                background->setCurrentIndex(background->findText(QStringLiteral("Black")));
            } else {
                CHECK(width->value() == 333 && height->value() == 222);
                CHECK(dialog->backgroundColor() == core::Rgba8({0, 0, 0, 255}));
            }
            ++stage;
            openImage->trigger();
        } else if (auto* file = qobject_cast<QFileDialog*>(modal)) {
            if (stage == 1) {
                ++stage;
                file->reject();
            } else if (stage == 3) {
                ++stage;
                file->setDirectory(f.assets.path());
                // QFileSystemModel populates asynchronously. Wait for the
                // directory change before setting/accepting the file name.
                QTimer::singleShot(100, file, [file, imagePath] {
                    auto* name = file->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                    CHECK(name);
                    if (name) name->setText(imagePath);
                    CHECK(QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection));
                });
            }
        }
    });
    drive.start();
    f.window.showStartupDocument();
    drive.stop();
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previousNativePolicy);
    settle();
    CHECK(stage == 4);
    CHECK(ticks <= 150);
    CHECK(!QApplication::activeModalWidget());
    CHECK(f.document().canvas().extent == core::Extent2u({71, 39}));
    CHECK(f.document().layers().size() == 1);
    CHECK(f.document().layers().front().name == "startup-image");
    CHECK(f.history().undoDepth() == 0 && f.history().redoDepth() == 0);
    std::array<std::byte, 4> rgba;
    const auto surface = std::get<core::RasterLayer>(f.document().layers().front().payload).surface;
    surface->copyRgba8({0, 0, 1, 1}, rgba, 4);
    CHECK(std::to_integer<int>(rgba[3]) == 127);
}

void zoomChangesNeitherAlignedMaskNorRectangleDirection()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({5.1, 8.2}, {42.8, 31.7});
    const auto expected = f.selection();
    const auto fittedZoom = f.canvas->zoom();
    f.action("DeselectAction");
    f.canvas->resetTo100Percent();
    settle();
    CHECK(std::abs(f.canvas->zoom() - 1.0) < 1e-8);
    CHECK(std::abs(fittedZoom - f.canvas->zoom()) > 0.1);
    f.rectangle({42.8, 31.7}, {5.1, 8.2});
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.action("DeselectAction");
    f.canvas->fitDocumentToView();
    settle();
    f.rectangle({5.1, 31.7}, {42.8, 8.2});
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.action("DeselectAction");
    CHECK(!f.canvas->selectionAnimationActive());
    CHECK(!f.canvas->scene().selectionEdges || f.canvas->scene().selectionEdges->empty());
}

void replacingInsideCoverageMovesAllRegionsWithoutTouchingLayerContent()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({4, 4}, {22, 24});
    f.rectangle({8, 8}, {14, 15}, Qt::AltModifier);
    f.rectangle({32, 9}, {39, 17}, Qt::ShiftModifier);
    const auto before = f.selection();
    const auto depth = f.history().undoDepth();
    const auto selectionRevision = f.document().selectionRevision();
    const auto contentRevision = f.document().revision();
    const auto& layer = f.document().layers().front();
    const auto transform = layer.localToDocument;
    const auto surface = std::get<core::RasterLayer>(layer.payload).surface;
    const auto rasterRevision = surface->revision();
    sendMouse(*f.canvas, QEvent::MouseMove, f.logical({6, 6}), Qt::NoButton, Qt::NoButton);
    CHECK(f.canvas->cursor().shape() == Qt::OpenHandCursor);
    sendMouse(*f.canvas, QEvent::MouseMove, f.logical({10, 10}), Qt::NoButton, Qt::NoButton);
    CHECK(f.canvas->cursor().shape() == Qt::CrossCursor);
    sendMouse(*f.canvas, QEvent::MouseMove, f.logical({27, 12}), Qt::NoButton, Qt::NoButton);
    CHECK(f.canvas->cursor().shape() == Qt::CrossCursor); // bounds gap is not selected
    f.press({6.2, 6.2});
    CHECK(f.canvas->selectionDragging());
    CHECK(f.canvas->cursor().shape() == Qt::ClosedHandCursor);
    f.move({9.8, 3.6}, Qt::ShiftModifier | Qt::AltModifier);
    CHECK(f.selection() == before);
    CHECK(f.document().selectionRevision() == selectionRevision);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.canvas->scene().selectionEdges
        && *f.canvas->scene().selectionEdges == core::translatedSelectionPreviewEdges(before, 4, -3));
    f.release({9.8, 3.6}, Qt::AltModifier); // operation remains a Move, latched at press
    const auto moved = before->translated(4, -3);
    CHECK(f.selection()->equivalent(*moved));
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.document().revision() == contentRevision);
    CHECK(surface->revision() == rasterRevision);
    CHECK(f.document().layers().front().localToDocument == transform);
    CHECK(std::get<core::RasterLayer>(f.document().layers().front().payload).surface == surface);
    f.undo();
    CHECK(f.selection()->equivalent(*before));
    f.redo();
    CHECK(f.selection()->equivalent(*moved));
    f.undo();
    // Starting inside a hole creates a new rectangle instead of moving bounds.
    f.rectangle({10, 10}, {12, 13});
    CHECK(f.selection()->bounds() == core::RectI({10, 10, 2, 3}));
    CHECK(surface->revision() == rasterRevision);
}

void explicitOperationsAndModifiersWinOverInteriorMoveAndRemainLatched()
{
    struct Case { const char* mode; Qt::KeyboardModifiers modifiers; core::SelectionOperation operation; };
    const std::array cases {
        Case {"Add", Qt::NoModifier, core::SelectionOperation::Add},
        Case {"Subtract", Qt::NoModifier, core::SelectionOperation::Subtract},
        Case {"Intersect", Qt::NoModifier, core::SelectionOperation::Intersect},
        Case {"Replace", Qt::ShiftModifier, core::SelectionOperation::Add},
        Case {"Replace", Qt::AltModifier, core::SelectionOperation::Subtract},
        Case {"Replace", Qt::ShiftModifier | Qt::AltModifier, core::SelectionOperation::Intersect},
        Case {"Add", Qt::AltModifier, core::SelectionOperation::Subtract}};
    for (const auto& test : cases) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid()) continue;
        f.marquee();
        f.rectangle({4, 4}, {20, 20});
        const auto before = f.selection();
        f.mode(test.mode);
        sendMouse(*f.canvas, QEvent::MouseMove, f.logical({7, 7}),
            Qt::NoButton, Qt::NoButton, test.modifiers);
        CHECK(f.canvas->cursor().shape() == Qt::CrossCursor);
        f.press({7, 7}, test.modifiers);
        f.move({25, 12});
        f.release({25, 12});
        const auto rectangle = core::SelectionMask::rectangle({64, 48}, {7, 7, 18, 5});
        CHECK(f.selection()->equivalent(*before->combined(*rectangle, test.operation)));
    }
}

void movePreviewOutsideAndBackRetainsTheOriginalAndCancellationKeepsRedo()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({8, 8}, {24, 24});
    const auto before = f.selection();
    f.action("InvertSelectionAction");
    f.undo();
    const auto depth = f.history().undoDepth();
    const auto redoDepth = f.history().redoDepth();
    const auto revision = f.document().selectionRevision();
    f.press({12, 12});
    f.move({90, 12});
    CHECK(f.selection() == before);
    CHECK(f.canvas->scene().selectionEdges && f.canvas->scene().selectionEdges->empty());
    f.move({14, 9});
    CHECK(f.canvas->scene().selectionEdges
        && *f.canvas->scene().selectionEdges == core::translatedSelectionPreviewEdges(before, 2, -3));
    f.move({12, 12});
    f.release({12, 12});
    CHECK(f.selection()->equivalent(*before));
    CHECK(f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.history().redoDepth() == redoDepth);
    for (const bool focusLoss : {false, true}) {
        f.press({12, 12});
        f.move({40, 28});
        if (focusLoss) {
            QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(f.canvas, &event);
        } else QTest::keyClick(f.canvas, Qt::Key_Escape);
        settle();
        CHECK(!f.canvas->selectionDragging());
        CHECK(f.selection()->equivalent(*before));
        CHECK(f.history().undoDepth() == depth);
        CHECK(f.history().redoDepth() == redoDepth);
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    }
}

void moveCaptureCrossesPanelsAndUsesIntegerDocumentOffsetsAtEveryZoom()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    // This checks raw pixel-aligned movement. At 100%, its endpoint is within
    // the new canvas-edge snap tolerance; snapping has dedicated coverage.
    f.window.findChild<QAction*>(QStringLiteral("SnappingAction"))->setChecked(false);
    f.marquee();
    f.rectangle({8, 8}, {24, 24});
    const auto before = f.selection();
    for (const bool actualPixels : {false, true}) {
        if (actualPixels) f.canvas->resetTo100Percent();
        else f.canvas->fitDocumentToView();
        settle();
        const auto depth = f.history().undoDepth();
        f.press({12.2, 12.2});
        f.move({15.8, 9.6});
        f.release({15.8, 9.6});
        CHECK(f.selection()->equivalent(*before->translated(4, -3)));
        CHECK(f.history().undoDepth() == depth + 1);
        f.undo();
        CHECK(f.selection()->equivalent(*before));
    }
    auto* panel = f.window.findChild<QWidget*>(QStringLiteral("LayersPanel"));
    CHECK(panel);
    if (!panel) return;
    const auto depth = f.history().undoDepth();
    const auto routed = f.router->routedEventCount();
    const QPointF panelPoint(panel->rect().center());
    const auto global = panel->mapToGlobal(panelPoint.toPoint());
    const auto logical = f.canvas->mapFromGlobal(global);
    const auto target = f.canvas->documentPositionForLogical(logical);
    const int dx = int(std::round(std::clamp(target.x - 12.0, -64.0, 64.0)));
    const int dy = int(std::round(std::clamp(target.y - 12.0, -48.0, 48.0)));
    f.press({12, 12});
    sendMouse(*panel, QEvent::MouseMove, panelPoint, Qt::NoButton, Qt::LeftButton);
    CHECK(f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    sendMouse(*panel, QEvent::MouseButtonRelease, panelPoint, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(f.selection()->equivalent(*before->translated(dx, dy)));
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.router->routedEventCount() >= routed + 2);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(QWidget::mouseGrabber() == nullptr);
}

void growthFieldsOnlyApplyOnRequestAndSupportSignedPerSideAmounts()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    auto* horizontal = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("SelectionGrowHorizontal"));
    auto* vertical = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("SelectionGrowVertical"));
    auto* apply = f.window.findChild<QToolButton*>(QStringLiteral("SelectionGrowApply"));
    CHECK(horizontal && vertical && apply);
    if (!horizontal || !vertical || !apply) return;
    CHECK(horizontal->value() == 1 && vertical->value() == 1);
    CHECK(horizontal->singleStep() == 1 && vertical->singleStep() == 1);
    CHECK(horizontal->decimals() == 0 && vertical->decimals() == 0);
    CHECK(!apply->isEnabled());
    f.rectangle({16, 14}, {40, 30});
    f.adjustments();
    const auto initial = f.selection();
    const auto depth = f.history().undoDepth();
    const auto revision = f.document().selectionRevision();
    const auto surface = std::get<core::RasterLayer>(f.document().layers().front().payload).surface;
    const auto surfaceRevision = surface->revision();
    horizontal->setValue(3);
    vertical->setValue(2);
    CHECK(f.selection() == initial);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.document().selectionRevision() == revision);
    apply->click();
    settle();
    CHECK(f.selection()->bounds() == core::RectI({13, 12, 30, 20}));
    CHECK(f.history().undoDepth() == depth + 1);
    f.undo();
    CHECK(f.selection()->equivalent(*initial));
    for (const auto amounts : {QPoint(-2, -3), QPoint(4, 0), QPoint(0, 4), QPoint(2, -1)}) {
        horizontal->setValue(amounts.x());
        vertical->setValue(amounts.y());
        apply->click();
        settle();
        CHECK(f.selection()->equivalent(*initial->adjusted(amounts.x(), amounts.y())));
        CHECK(f.history().undoDepth() == depth + 1);
        f.undo();
    }
    const auto redoDepth = f.history().redoDepth();
    horizontal->setValue(0);
    vertical->setValue(0);
    apply->click();
    settle();
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.history().redoDepth() == redoDepth);
    CHECK(surface->revision() == surfaceRevision);
}

void rotationHeldStepperKeepsOriginalPivotAndCommitsOnceAcrossCanvasRelease()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({17, 14}, {40, 29});
    f.rectangle({32, 18}, {37, 23}, Qt::AltModifier);
    f.adjustments();
    auto* angle = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("SelectionAngleControl"));
    CHECK(angle);
    if (!angle) return;
    auto* editor = angle->findChild<QLineEdit*>();
    CHECK(editor);
    if (!editor) return;
    CHECK(angle->singleStep() == 1);
    const auto before = f.selection();
    const auto bounds = before->bounds();
    const core::Vec2d pivot {bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5};
    const auto depth = f.history().undoDepth();
    const auto revision = f.document().selectionRevision();
    const auto surface = std::get<core::RasterLayer>(f.document().layers().front().payload).surface;
    const auto surfaceRevision = surface->revision();
    angle->setFocus();
    angle->selectAll();
    settle();
    const QPoint up(angle->width() - 8, 7);
    QTest::mousePress(angle->window()->windowHandle(), Qt::LeftButton, Qt::NoModifier,
        angle->mapTo(angle->window(), up));
    QTest::qWait(650);
    CHECK(angle->value() >= 2);
    CHECK(f.selection() == before);
    CHECK(f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.router->captureOwner() == angle);
    CHECK(QApplication::focusWidget() != angle);
    CHECK(!QApplication::focusWidget() || !angle->isAncestorOf(QApplication::focusWidget()));
    CHECK(!editor->hasSelectedText());
    const auto delta = angle->value();
    const auto expected = before->rotated(delta, pivot);
    CHECK(f.canvas->scene().selectionEdges
        && *f.canvas->scene().selectionEdges == expected->boundaryEdges());
    const auto routed = f.router->routedEventCount();
    const QPointF canvasPoint(f.canvas->width() * 0.5, f.canvas->height() * 0.5);
    sendMouse(*f.canvas, QEvent::MouseMove, canvasPoint, Qt::NoButton, Qt::LeftButton);
    CHECK(f.router->captureOwner() == angle);
    sendMouse(*f.canvas, QEvent::MouseButtonRelease, canvasPoint, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(angle->value() == 0);
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.selection()->equivalent(*expected));
    CHECK(f.router->routedEventCount() >= routed + 2);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(!editor->hasSelectedText());
    // A real Ctrl+Z key must immediately reach document history after a step.
    auto* receiver = QApplication::focusWidget();
    QTest::keyClick(receiver ? receiver : &f.window, Qt::Key_Z, Qt::ControlModifier);
    settle();
    CHECK(f.selection()->equivalent(*before));
    CHECK(f.history().undoDepth() == depth);
    f.redo();
    CHECK(f.selection()->equivalent(*expected));
    CHECK(surface->revision() == surfaceRevision);
}

void typedRotationCommitsOnceAndChangingToolsFlushesPendingInput()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({17, 14}, {40, 29});
    f.adjustments();
    auto* angle = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("SelectionAngleControl"));
    CHECK(angle);
    if (!angle) return;
    const auto before = f.selection();
    const auto b = before->bounds();
    const core::Vec2d pivot {b.x + b.width * 0.5, b.y + b.height * 0.5};
    const auto depth = f.history().undoDepth();
    angle->setFocus();
    angle->selectAll();
    QTest::keyClicks(angle, QStringLiteral("17.5"));
    CHECK(f.selection() == before);
    CHECK(f.history().undoDepth() == depth);
    QTest::keyClick(angle, Qt::Key_Return);
    settle();
    CHECK(f.selection()->equivalent(*before->rotated(17.5, pivot)));
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(angle->value() == 0);
    CHECK(!angle->hasFocus());
    f.undo();
    CHECK(f.selection()->equivalent(*before));
    angle->setFocus();
    angle->selectAll();
    QTest::keyClicks(angle, QStringLiteral("-23"));
    // Toolbar/action selection is explicit; shortcut letters remain owned by
    // the editor while typing. Leaving the tool must interpret this one edit.
    f.shortcut("B");
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Brush);
    CHECK(f.selection()->equivalent(*before->rotated(-23, pivot)));
    CHECK(f.history().undoDepth() == depth + 1);
    f.undo();
    CHECK(f.selection()->equivalent(*before));
    f.marquee();
    CHECK(angle->value() == 0);
    const auto redoDepth = f.history().redoDepth();
    angle->setValue(360);
    settle();
    CHECK(f.selection()->equivalent(*before));
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.history().redoDepth() == redoDepth);
}

void heldRotationEscapeCancelsItsPreviewAndKeepsTheRedoBranch()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.marquee();
    f.rectangle({17, 14}, {40, 29});
    const auto before = f.selection();
    f.action("InvertSelectionAction");
    f.undo();
    const auto depth = f.history().undoDepth();
    const auto redoDepth = f.history().redoDepth();
    const auto revision = f.document().selectionRevision();
    f.adjustments();
    auto* angle = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("SelectionAngleControl"));
    CHECK(angle);
    if (!angle) return;
    const QPoint up(angle->width() - 8, 7);
    auto* input = angle->window()->windowHandle();
    const auto point = angle->mapTo(angle->window(), up);
    QTest::mousePress(input, Qt::LeftButton, Qt::NoModifier, point);
    settle();
    CHECK(angle->value() == 1);
    CHECK(f.router->captureOwner() == angle);
    CHECK(f.selection() == before);
    QTest::keyClick(&f.window, Qt::Key_Escape);
    settle();
    QTest::mouseRelease(input, Qt::LeftButton, Qt::NoModifier, point);
    settle();
    CHECK(angle->value() == 0);
    CHECK(f.selection() == before);
    CHECK(f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == depth);
    CHECK(f.history().redoDepth() == redoDepth);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(QWidget::mouseGrabber() == nullptr);
}

void heldRotationTerminalEventsCancelBeforeSyntheticReleaseAndStopRepeatTimers()
{
    for (const auto reason : {QEvent::WindowDeactivate, QEvent::FocusOut, QEvent::TouchCancel}) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid()) continue;
        f.marquee();
        f.rectangle({17, 14}, {40, 29});
        const auto before = f.selection();
        f.action("InvertSelectionAction");
        f.undo();
        const auto depth = f.history().undoDepth();
        const auto redoDepth = f.history().redoDepth();
        const auto revision = f.document().selectionRevision();
        const auto surface = std::get<core::RasterLayer>(f.document().layers().front().payload).surface;
        const auto surfaceRevision = surface->revision();
        f.adjustments();
        auto* angle = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("SelectionAngleControl"));
        CHECK(angle);
        if (!angle) continue;
        const QPoint up(angle->width() - 8, 7);
        auto* input = angle->window()->windowHandle();
        const auto point = angle->mapTo(angle->window(), up);
        QTest::mousePress(input, Qt::LeftButton, Qt::NoModifier, point);
        settle();
        CHECK(angle->value() == 1);
        CHECK(f.router->captureOwner() == angle);
        CHECK(f.selection() == before);
        if (reason == QEvent::FocusOut) {
            QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(angle, &event);
        } else {
            QEvent event(reason);
            QCoreApplication::sendEvent(reason == QEvent::WindowDeactivate
                    ? static_cast<QObject*>(input) : static_cast<QObject*>(angle), &event);
        }
        settle();
        const auto assertCancelled = [&] {
            CHECK(angle->value() == 0);
            CHECK(f.selection() == before);
            CHECK(f.document().selectionRevision() == revision);
            CHECK(f.history().undoDepth() == depth);
            CHECK(f.history().redoDepth() == redoDepth);
            CHECK(surface->revision() == surfaceRevision);
            CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
            CHECK(QWidget::mouseGrabber() == nullptr);
        };
        assertCancelled();
        // Cancellation must stop Qt's native hold timer as well as clearing
        // our action state; otherwise a repeat can start a new rotation later.
        QTest::qWait(650);
        assertCancelled();
        QTest::mouseRelease(input, Qt::LeftButton, Qt::NoModifier, point);
        QTest::qWait(40);
        settle();
        assertCancelled();
    }
}

struct UploadCounters {
    std::uint64_t full, batches, regions, uploads, bytes, staging;
    friend bool operator==(const UploadCounters&, const UploadCounters&) = default;
};
UploadCounters uploads(const render::RendererStats& stats)
{
    return {stats.fullUploads, stats.regionalUploadBatches, stats.regionalDirtyRegions,
        stats.regionalUploads, stats.uploadedBytes, stats.stagingBufferAllocations};
}

int captureNativeSelectionPreview(const QString& output)
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        std::cerr << "Selection native preview requires QT_QPA_PLATFORM=wayland\n";
        return 77;
    }
    std::atomic_uint64_t warnings {0}, errors {0};
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) {
        std::cerr << "Selection native preview requires Vulkan validation\n";
        return EXIT_FAILURE;
    }
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << "Vulkan selection validation: " << (data ? data->pMessage : "unknown") << '\n';
        return true;
    });
    if (!instance.create()) return EXIT_FAILURE;
    QTemporaryDir assets;
    QImage reference(512, 320, QImage::Format_RGBA8888);
    reference.fill(QColor(25, 30, 41));
    {
        QPainter painter(&reference);
        painter.fillRect(0, 0, 256, 160, QColor(230, 96, 79));
        painter.fillRect(256, 0, 256, 160, QColor(225, 184, 83));
        painter.fillRect(0, 160, 256, 160, QColor(65, 165, 124));
        painter.fillRect(256, 160, 256, 160, QColor(113, 110, 215));
        painter.setPen(QColor(15, 18, 26));
        painter.drawText(QRect(20, 5, 200, 30), Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("SELECTION / HOLES / ISLANDS"));
    }
    const auto sourcePath = assets.filePath(QStringLiteral("selection-review.png"));
    if (!reference.save(sourcePath)) return EXIT_FAILURE;
    auto window = std::make_unique<ui::MainWindow>(&instance, false, false);
    window->setUnsavedPromptEnabled(false);
    window->resize(1440, 900);
    if (!window->openImageFromPath(sourcePath)) return EXIT_FAILURE;
    window->show();
    window->activateWindow();
    QEventLoop loop;
    QTimer poll;
    QProcess capture;
    render::CanvasWindow* canvas = nullptr;
    std::optional<UploadCounters> originalUploads;
    std::uint64_t frames = 0, geometryUploads = 0, geometryBytes = 0;
    core::Revision rasterRevision = 0, maskRevision = 0;
    core::SelectionState gizmoBefore, gizmoAfter;
    std::vector<std::byte> strokeBefore;
    std::uint64_t strokeFrame = 0, strokeBatches = 0;
    int stage = 0, stable = 0;
    bool passed = false;
    QString failure = QStringLiteral("Native selection preview timed out");
    const auto fail = [&](QString reason) { failure = std::move(reason); loop.quit(); };
    const auto finish = [&] { passed = true; loop.quit(); };
    const auto screenshotFinished = [&] {
        if (canvas->scene().transformOverlay)
            window->findChild<QToolButton*>(QStringLiteral("TransformCancel"))->click();
        // Deselect must stop animation; undo restores the exact mask and starts
        // economical animation again, without changing raster storage.
        auto* deselect = window->findChild<QAction*>(QStringLiteral("DeselectAction"));
        auto* undo = shortcutAction(*window, QStringLiteral("Ctrl+Z"));
        if (!deselect || !undo) { fail(QStringLiteral("Missing selection actions")); return; }
        const auto selected = window->editorSession().document()->selection();
        deselect->trigger();
        if (canvas->selectionAnimationActive()) {
            fail(QStringLiteral("Deselect left the selection timer running")); return;
        }
        undo->trigger();
        const auto restored = window->editorSession().document()->selection();
        if (!restored || !selected || !restored->equivalent(*selected)) {
            fail(QStringLiteral("Selection undo lost mask geometry")); return;
        }
        auto* brush = shortcutAction(*window, QStringLiteral("B"));
        if (!brush) { fail(QStringLiteral("Missing Brush action")); return; }
        const auto surface = std::get<core::RasterLayer>(window->editorSession().document()->layers().front().payload).surface;
        strokeBefore.resize(512U*320U*4U);
        surface->copyRgba8({0,0,512,320}, strokeBefore, 512U*4U);
        brush->trigger();
        const auto logical = [&](core::Vec2d point) {
            const auto& scene = canvas->scene();
            const auto p = scene.viewport.documentToViewport(point, {512,320}, scene.logicalViewport);
            return QPointF(p.x,p.y);
        };
        // A real UI brush crosses the hole and both selected/unselected pixels.
        sendMouse(*canvas, QEvent::MouseButtonPress, logical({55,140}), Qt::LeftButton, Qt::LeftButton);
        for (int x = 65; x <= 300; x += 10)
            sendMouse(*canvas, QEvent::MouseMove, logical({double(x),140}), Qt::NoButton, Qt::LeftButton);
        sendMouse(*canvas, QEvent::MouseButtonRelease, logical({300,140}), Qt::LeftButton, Qt::NoButton);
        std::vector<std::byte> after(strokeBefore.size());
        surface->copyRgba8({0,0,512,320}, after, 512U*4U);
        bool changed = false;
        for (int y = 0; y < 320; ++y) for (int x = 0; x < 512; ++x) {
            const auto offset = std::size_t(y*512+x)*4U;
            if (std::equal(after.begin()+std::ptrdiff_t(offset), after.begin()+std::ptrdiff_t(offset+4),
                    strokeBefore.begin()+std::ptrdiff_t(offset))) continue;
            changed = true;
            if (!restored->coverageAtDocumentPixel(x,y)) {
                fail(QStringLiteral("Native Brush edited outside selection")); return;
            }
        }
        if (!changed) { fail(QStringLiteral("Native selected Brush made no edits")); return; }
        strokeFrame = canvas->rendererStats().framesSubmitted;
        stage = 4;
        poll.start();
    };
    QObject::connect(&capture, &QProcess::errorOccurred, &loop,
        [&](QProcess::ProcessError) { fail(capture.errorString()); });
    QObject::connect(&capture, &QProcess::finished, &loop,
        [&](int code, QProcess::ExitStatus status) {
            if (status != QProcess::NormalExit || code != 0 || QImage(output).isNull()) {
                fail(QStringLiteral("Selection review screenshot failed (exit %1): %2")
                    .arg(code).arg(QString::fromUtf8(capture.readAllStandardError()))); return;
            }
            screenshotFinished();
        });
    poll.setInterval(40);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!canvas) {
            for (auto* candidate : QGuiApplication::allWindows())
                if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                    canvas = dynamic_cast<render::CanvasWindow*>(candidate);
            if (!canvas) return;
        }
        const auto stats = canvas->rendererStats();
        if (stage == 6) {
            if (stats.framesSubmitted < frames + 2) return;
            const auto& document = *window->editorSession().document();
            if (document.selection() != gizmoBefore || !canvas->scene().transformOverlay
                || uploads(stats) != *originalUploads) {
                fail(QStringLiteral("Selection gizmo preview modified mask/raster storage")); return;
            }
            window->findChild<QToolButton*>(QStringLiteral("TransformApply"))->click();
            if (canvas->scene().transformOverlay || !document.selection()->equivalent(*gizmoAfter)) {
                fail(QStringLiteral("Native gizmo Apply did not resolve the original mask")); return;
            }
            canvas->requestActivate();
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
            if (!document.selection()->equivalent(*gizmoBefore)) {
                fail(QStringLiteral("Native gizmo Undo key failed")); return;
            }
            QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            if (!document.selection()->equivalent(*gizmoAfter)) {
                fail(QStringLiteral("Native gizmo Redo key failed")); return;
            }
            maskRevision = document.selectionRevision();
            frames = stats.framesSubmitted;
            geometryUploads = stats.selectionGeometryUploads;
            stable = 0;
            stage = 1;
            return;
        }
        if (stage >= 4) {
            if (stats.framesSubmitted <= strokeFrame) return;
            if (!originalUploads || stats.fullUploads != originalUploads->full) {
                fail(QStringLiteral("Selected Brush/undo performed a full upload")); return;
            }
            if (stage == 4) {
                if (stats.regionalUploadBatches <= originalUploads->batches) {
                    fail(QStringLiteral("Selected Brush did not upload dirty regions")); return;
                }
                strokeBatches = stats.regionalUploadBatches;
                shortcutAction(*window, QStringLiteral("Ctrl+Z"))->trigger();
                const auto surface = std::get<core::RasterLayer>(window->editorSession().document()->layers().front().payload).surface;
                std::vector<std::byte> restored(strokeBefore.size());
                surface->copyRgba8({0,0,512,320}, restored, 512U*4U);
                if (restored != strokeBefore) { fail(QStringLiteral("Native masked Brush undo lost pixels")); return; }
                strokeFrame = stats.framesSubmitted;
                stage = 5;
            } else {
                if (stats.regionalUploadBatches <= strokeBatches) {
                    fail(QStringLiteral("Masked undo failed to upload dirty regions")); return;
                }
                finish();
            }
            return;
        }
        if (stage == 0) {
            if (!stats.framesSubmitted || !stats.fullUploads) return;
            if (originalUploads && uploads(stats) == *originalUploads) ++stable;
            else { originalUploads = uploads(stats); stable = 0; }
            if (stable < 3) return;
            auto* marquee = shortcutAction(*window, QStringLiteral("M"));
            if (!marquee) { fail(QStringLiteral("Missing marquee tool")); return; }
            const auto& originalLayer = window->editorSession().document()->layers().front();
            rasterRevision = std::get<core::RasterLayer>(originalLayer.payload).surface->revision();
            marquee->trigger();
            const auto logical = [&](core::Vec2d point) {
                const auto& scene = canvas->scene();
                const auto p = scene.viewport.documentToViewport(point, {512, 320}, scene.logicalViewport);
                return QPointF(p.x, p.y);
            };
            const auto rectangle = [&](core::Vec2d a, core::Vec2d b, Qt::KeyboardModifiers mods) {
                sendMouse(*canvas, QEvent::MouseMove, logical(a), Qt::NoButton, Qt::NoButton, mods);
                sendMouse(*canvas, QEvent::MouseButtonPress, logical(a), Qt::LeftButton, Qt::LeftButton, mods);
                sendMouse(*canvas, QEvent::MouseMove, logical(b), Qt::NoButton, Qt::LeftButton, mods);
                sendMouse(*canvas, QEvent::MouseButtonRelease, logical(b), Qt::LeftButton, Qt::NoButton, mods);
            };
            rectangle({25, 30}, {320, 235}, Qt::NoModifier);
            rectangle({375, 65}, {480, 280}, Qt::ShiftModifier);
            rectangle({100, 90}, {225, 175}, Qt::AltModifier);
            rectangle({10, 50}, {465, 295}, Qt::ShiftModifier | Qt::AltModifier);
            const auto& document = *window->editorSession().document();
            const auto mask = document.selection();
            if (!mask || mask->coverageAtDocumentPixel(55, 75) != 255
                || mask->coverageAtDocumentPixel(150, 140) != 0
                || mask->coverageAtDocumentPixel(410, 140) != 255
                || mask->coverageAtDocumentPixel(340, 140) != 0
                || window->editorSession().history().undoDepth() != 4) {
                fail(QStringLiteral("Native rectangle operations lost hole/island coverage")); return;
            }
            // Move the actual mask, grow/shrink it and rotate it through the
            // cached toolbar. None of these may invalidate raster textures.
            rectangle({55, 75}, {85, 85}, Qt::NoModifier);
            auto* adjustments = window->findChild<QToolButton*>(QStringLiteral("SelectionTransform"));
            if (!adjustments || !adjustments->isEnabled()) {
                fail(QStringLiteral("Missing selection adjustments disclosure")); return;
            }
            if (!adjustments->isChecked()) adjustments->click();
            auto* growX = window->findChild<QDoubleSpinBox*>(QStringLiteral("SelectionGrowHorizontal"));
            auto* growY = window->findChild<QDoubleSpinBox*>(QStringLiteral("SelectionGrowVertical"));
            auto* apply = window->findChild<QToolButton*>(QStringLiteral("SelectionGrowApply"));
            auto* angle = window->findChild<QDoubleSpinBox*>(QStringLiteral("SelectionAngleControl"));
            auto* undo = shortcutAction(*window, QStringLiteral("Ctrl+Z"));
            auto* redo = shortcutAction(*window, QStringLiteral("Ctrl+Shift+Z"));
            if (!growX || !growY || !apply || !angle || !undo || !redo) {
                fail(QStringLiteral("Missing mask manipulation controls")); return;
            }
            growX->setValue(3);
            growY->setValue(-2);
            apply->click();
            angle->setValue(17);
            const auto grown = mask->translated(30, 10)->adjusted(3, -2);
            const auto bounds = grown->bounds();
            const auto expected = grown->rotated(17,
                {bounds.x + bounds.width * 0.5, bounds.y + bounds.height * 0.5});
            if (!document.selection()->equivalent(*expected)
                || window->editorSession().history().undoDepth() != 7 || angle->value() != 0) {
                fail(QStringLiteral("Native selection manipulation lost mask geometry/grouping")); return;
            }
            for (int i = 0; i < 3; ++i) undo->trigger();
            if (!document.selection()->equivalent(*mask)) {
                fail(QStringLiteral("Native manipulation undo lost original mask")); return;
            }
            for (int i = 0; i < 3; ++i) redo->trigger();
            if (!document.selection()->equivalent(*expected)) {
                fail(QStringLiteral("Native manipulation redo lost final mask")); return;
            }
            gizmoBefore = document.selection();
            shortcutAction(*window, QStringLiteral("Ctrl+T"))->trigger();
            if (!canvas->scene().transformOverlay) {
                fail(QStringLiteral("Native selection gizmo did not open")); return;
            }
            auto* scaleX = window->findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleXControl"));
            auto* scaleY = window->findChild<QDoubleSpinBox*>(QStringLiteral("TransformScaleYControl"));
            auto* rotation = window->findChild<QDoubleSpinBox*>(QStringLiteral("TransformAngleControl"));
            scaleX->setValue(80);
            scaleY->setValue(75);
            rotation->setValue(-12);
            auto mapping = canvas->scene().transformOverlay->localToDocument;
            const auto b = gizmoBefore->bounds();
            mapping.m02 -= mapping.m00 * b.x + mapping.m01 * b.y;
            mapping.m12 -= mapping.m10 * b.x + mapping.m11 * b.y;
            gizmoAfter = gizmoBefore->transformed(mapping);
            frames = stats.framesSubmitted;
            geometryUploads = stats.selectionGeometryUploads;
            stage = 6;
            stable = 0;
            return;
        }
        if (!originalUploads || uploads(stats) != *originalUploads) {
            fail(QStringLiteral("Selection animation uploaded raster pixels")); return;
        }
        const auto& document = *window->editorSession().document();
        if (document.selectionRevision() != maskRevision
            || std::get<core::RasterLayer>(document.layers().front().payload).surface->revision() != rasterRevision) {
            fail(QStringLiteral("Selection animation mutated document pixels or selection")); return;
        }
        if (stage == 1) {
            // All reusable frame slots must hold this edge revision before
            // asserting that animation updates phase only, never geometry.
            if (geometryUploads == stats.selectionGeometryUploads) ++stable;
            else { geometryUploads = stats.selectionGeometryUploads; stable = 0; }
            if (stats.framesSubmitted < frames + 6 || stable < 4) return;
            if (!geometryUploads || !canvas->selectionAnimationActive()) {
                fail(QStringLiteral("Visible selection never animated/rendered")); return;
            }
            geometryBytes = stats.selectionUploadedBytes;
            frames = stats.framesSubmitted;
            stage = 2;
            return;
        }
        if (stage == 2 && stats.framesSubmitted >= frames + 3) {
            if (stats.selectionGeometryUploads != geometryUploads || stats.selectionUploadedBytes != geometryBytes) {
                fail(QStringLiteral("Ants animation reuploaded unchanged selection edges")); return;
            }
            poll.stop();
            const auto spectacle = QStandardPaths::findExecutable(QStringLiteral("spectacle"));
            if (output.isEmpty() || spectacle.isEmpty()) { screenshotFinished(); return; }
            // Show the cached gizmo as well as the final transformed contours
            // in the optional review capture; cancel it before masked painting.
            shortcutAction(*window, QStringLiteral("Ctrl+T"))->trigger();
            window->activateWindow();
            QTimer::singleShot(140, &capture, [&, spectacle] {
                capture.start(spectacle, {QStringLiteral("--background"), QStringLiteral("--nonotify"),
                    QStringLiteral("--activewindow"), QStringLiteral("--no-decoration"),
                    QStringLiteral("--no-shadow"), QStringLiteral("--output"), output});
            });
            stage = 3;
        }
    });
    poll.start();
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
    poll.stop();
    if (capture.state() != QProcess::NotRunning) { capture.kill(); capture.waitForFinished(1000); }
    window->logRendererDiagnostics();
    window->close();
    window.reset();
    settle();
    instance.destroy();
    std::cout << "Native selection validation: warnings=" << warnings.load()
              << " errors=" << errors.load() << '\n';
    if (!passed || warnings.load() || errors.load()) {
        std::cerr << failure.toStdString() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Native selection preview passed: " << output.toStdString() << '\n';
    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SelectionInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    const auto nativePreview = qEnvironmentVariable("IMAGEEDITOR_TEST_SELECTION_PREVIEW_NATIVE");
    if (!nativePreview.isEmpty() || qEnvironmentVariableIntValue("IMAGEEDITOR_TEST_SELECTION_NATIVE"))
        return captureNativeSelectionPreview(nativePreview);
    marqueeControlsAreCachedStyledAndNeverResizeCanvas();
    livePreviewIsImmutablePixelAlignedAndOneUndoAction();
    modesLatchModifiersAndRepresentHolesAndDisconnectedRegions();
    inactiveEmptyNoopAndCancelledSelectionsPreserveRedo();
    cancellationRestoresExistingMaskAndClearsCapture();
    releaseOverPanelFinishesTheOriginalCanvasGesture();
    tabletSelectionKeepsOwnershipWhenSpaceIsPressedDuringItsGesture();
    middlePanCannotStartASelectionOrLoseOwnershipToAnExtraButton();
    losingTheNativeSurfaceCancelsTheUncommittedSelection();
    textFieldsOwnSelectionAndCopyShortcuts();
    layerViaCopyPreservesMaskAndUndoesActiveLayerAlongsideCreation();
    canvasResizeUndoesMaskClippingAndRetainsLayerStorage();
    redoUsesActualKeyDispatchAcrossCanvasAndPanelsWithoutStealingTyping();
    explicitNewDocumentsKeepDimensionsAndBackgroundWhenImagesAreDropped();
    startupOpenImageUsesImageDimensionsAndFileCancellationKeepsCanvasChoices();
    zoomChangesNeitherAlignedMaskNorRectangleDirection();
    replacingInsideCoverageMovesAllRegionsWithoutTouchingLayerContent();
    explicitOperationsAndModifiersWinOverInteriorMoveAndRemainLatched();
    movePreviewOutsideAndBackRetainsTheOriginalAndCancellationKeepsRedo();
    moveCaptureCrossesPanelsAndUsesIntegerDocumentOffsetsAtEveryZoom();
    growthFieldsOnlyApplyOnRequestAndSupportSignedPerSideAmounts();
    rotationHeldStepperKeepsOriginalPivotAndCommitsOnceAcrossCanvasRelease();
    typedRotationCommitsOnceAndChangingToolsFlushesPendingInput();
    heldRotationEscapeCancelsItsPreviewAndKeepsTheRedoBranch();
    heldRotationTerminalEventsCancelBeforeSyntheticReleaseAndStopRepeatTimers();
    if (failures) {
        std::cerr << failures << " selection interaction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All selection interaction tests passed\n";
    return EXIT_SUCCESS;
}
