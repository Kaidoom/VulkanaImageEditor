#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVulkanInstance>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

void settle()
{
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 5000)
{
    QElapsedTimer timer; timer.start();
    do { settle(); if (predicate()) return true; QTest::qWait(2); }
    while (timer.elapsed() < timeout);
    return predicate();
}
template<class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF position, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {})
{
    QMouseEvent event(type, position, position, QPointF(receiver.mapToGlobal(position.toPoint())),
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}
void key(QObject* receiver, QEvent::Type type, int code, Qt::KeyboardModifiers modifiers = {},
    const QString& text = {})
{
    QKeyEvent event(type, code, modifiers, text);
    QCoreApplication::sendEvent(receiver, &event);
    settle();
}
bool same(const core::SelectionState& a, const core::SelectionState& b)
{ return a == b || (a && b && a->equivalent(*b)); }
int pixel(const core::SelectionState& selection, int x, int y)
{ return selection ? selection->coverageAtDocumentPixel(x, y) : -1; }

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::OverlayDockWorkspace* workspace {};
    ui::CrossWindowPointerRouter* router {};
    explicit Fixture(QVulkanInstance* instance = nullptr) : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1640, 940); window.show(); settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        // Two disconnected exact matches, a near match, an unrelated color,
        // transparent pixels with distinct hidden RGB, and partial opacity.
        QImage source(64, 48, QImage::Format_RGBA8888);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
            QColor color(20, 30, 210, 255);
            if (x < 8 || (x >= 24 && x < 32)) color = QColor(100, 20, 20, 255);
            else if (x < 16) color = QColor(130, 20, 20, 255);
            else if (x >= 40 && x < 48) color = QColor(y < 24 ? 100 : 250, 20, 20, 0);
            else if (x >= 48 && x < 56) color = QColor(100, 20, 20, 128);
            source.setPixelColor(x, y, color);
        }
        const auto path = assets.filePath(QStringLiteral("select-by-color-source.png"));
        CHECK(source.save(path)); CHECK(window.openImageFromPath(path));
        action("ToolAction_colorselect"); settle();
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return canvas && workspace && router && window.editorSession().document(); }
    core::EditorSession& session() { return const_cast<core::EditorSession&>(window.editorSession()); }
    core::Document& document() { return *session().document(); }
    const core::RasterSurface& surface()
    { return *std::get<core::RasterLayer>(document().layers().front().payload).surface; }
    core::SelectionState selection() { return document().selection(); }
    template<class T> T* find(const char* name)
    {
        auto* result = dynamic_cast<T*>(window.findChild<QObject*>(QString::fromLatin1(name)));
        CHECK(result); return result;
    }
    void action(const char* name)
    { if (auto* target = find<QAction>(name)) target->trigger(); settle(); }
    void shortcut(const char* sequence)
    {
        for (auto* target : window.findChildren<QAction*>())
            if (target->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence)))) {
                target->trigger(); settle(); return;
            }
        CHECK(false);
    }
    void mode(const char* suffix)
    {
        const auto name = QStringLiteral("SelectionMode") + QString::fromLatin1(suffix);
        auto* button = window.findChild<QToolButton*>(name); CHECK(button);
        if (button) button->click();
        settle();
    }
    bool idle()
    {
        auto* timer = find<QTimer>("ColorSelectionTimer");
        return timer && waitFor([&] { return !timer->isActive(); });
    }
    QPointF logical(core::Vec2d point) const
    {
        const auto extent = window.editorSession().document()->canvas().extent;
        const auto& scene = canvas->scene();
        const auto mapped = scene.viewport.documentToViewport(point,
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return {mapped.x, mapped.y};
    }
    void press(core::Vec2d point, Qt::KeyboardModifiers modifiers = {})
    {
        mouse(*canvas, QEvent::MouseMove, logical(point), Qt::NoButton, Qt::NoButton, modifiers);
        mouse(*canvas, QEvent::MouseButtonPress, logical(point), Qt::LeftButton, Qt::LeftButton, modifiers);
    }
    void move(core::Vec2d point, Qt::KeyboardModifiers modifiers = {})
    { mouse(*canvas, QEvent::MouseMove, logical(point), Qt::NoButton, Qt::LeftButton, modifiers); }
    void release(core::Vec2d point, Qt::KeyboardModifiers modifiers = {})
    {
        mouse(*canvas, QEvent::MouseButtonRelease, logical(point), Qt::LeftButton, Qt::NoButton, modifiers);
        settle();
    }
    void click(core::Vec2d point, Qt::KeyboardModifiers modifiers = {})
    { press(point, modifiers); release(point, modifiers); CHECK(idle()); }
    void rectangle(core::Vec2d a, core::Vec2d b)
    {
        action("ToolAction_marquee"); mode("Replace");
        press(a); move(b); release(b); action("ToolAction_colorselect");
    }
    void refine(int fuzziness)
    {
        auto* control = find<ui::CompactValueControl>("ColorSelectionFuzziness");
        if (!control) return;
        beginRefinement(*control);
        control->setValue(fuzziness);
        endRefinement(*control);
        CHECK(idle());
    }
    void beginRefinement(ui::CompactValueControl& control)
    {
        const auto rect = control.valueFieldRect();
        const QPointF point(rect.left() + control.value() / 255.0 * (rect.width() - 1), rect.center().y());
        mouse(control, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        CHECK(control.interactionActive());
    }
    void endRefinement(ui::CompactValueControl& control)
    {
        const auto rect = control.valueFieldRect();
        const QPointF point(rect.left() + control.value() / 255.0 * (rect.width() - 1), rect.center().y());
        mouse(control, QEvent::MouseButtonRelease, point,
            Qt::LeftButton, Qt::NoButton);
        settle();
    }
    void escape() { key(canvas, QEvent::KeyPress, Qt::Key_Escape); }
};

void controlsAndTypedShortcutOwnership()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    auto* action = f.find<QAction>("ToolAction_colorselect");
    auto* controls = f.find<QWidget>("ColorSelectionControls");
    auto* fuzziness = f.find<ui::CompactValueControl>("ColorSelectionFuzziness");
    auto* sample = f.find<QLabel>("ColorSelectionSample");
    auto* options = dynamic_cast<ui::ToolOptionsBar*>(f.find<QToolBar>("ToolOptionsBar"));
    CHECK(action && action->shortcut() == QKeySequence(QStringLiteral("Shift+O")));
    CHECK(f.session().activeTool() == core::ToolId::SelectByColor);
    CHECK(controls && controls->isVisible());
    CHECK(fuzziness && fuzziness->minimum() == 0 && fuzziness->maximum() == 255
        && fuzziness->decimals() == 0 && fuzziness->value() == 16);
    CHECK(sample && !sample->text().isEmpty());
    CHECK(options && options->pageForTool(core::ToolId::SelectByColor));
    if (!fuzziness) return;
    const auto geometry = f.workspace->canvasContainer()->geometry();
    for (const auto* suffix : {"Replace", "Add", "Subtract", "Intersect"}) {
        f.mode(suffix);
        const auto name = QStringLiteral("SelectionMode") + QString::fromLatin1(suffix);
        auto* button = f.window.findChild<QToolButton*>(name);
        CHECK(button && button->isChecked());
    }
    f.mode("Replace");
    auto* merged = f.find<QToolButton>("ColorSelectionMergedVisible");
    auto* active = f.find<QToolButton>("ColorSelectionActiveLayer");
    if (active && merged) {
        active->click(); CHECK(active->isChecked());
        merged->click(); CHECK(merged->isChecked());
    }
    f.action("ToolAction_brush"); CHECK(controls && !controls->isVisible());
    f.action("ToolAction_colorselect"); CHECK(controls && controls->isVisible());
    CHECK(f.workspace->canvasContainer()->geometry() == geometry);
    // Numeric entry owns select-all/undo and letter shortcuts while editing.
    fuzziness->setFocus();
    QTest::keyClick(fuzziness, Qt::Key_1);
    settle(); CHECK(fuzziness->isManualEntryActive());
    auto* editor = fuzziness->findChild<QLineEdit*>(); CHECK(editor);
    if (editor) {
        QSignalSpy activations(action, &QAction::triggered);
        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
        CHECK(!f.selection());
        QTest::keyClick(editor, Qt::Key_O, Qt::ShiftModifier);
        CHECK(f.session().activeTool() == core::ToolId::SelectByColor);
        CHECK(activations.isEmpty());
        QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
        CHECK(f.session().history().undoDepth() == 0);
        QTest::keyClick(editor, Qt::Key_Escape);
    }
}

void clickFindsDisconnectedMatchesWithoutChangingContent()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto colors = f.session().colors();
    f.document().markSaved();
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto content = f.document().contentState();
    const auto depth = f.session().history().undoDepth();
    const auto unsampled = f.find<QLabel>("ColorSelectionSample")->text();
    f.press({3.5, 10.5});
    CHECK(f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    CHECK(f.idle());
    CHECK(!f.selection());
    CHECK(f.session().history().undoDepth() == depth);
    CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
    f.move({18.5, 30.5}); // The pressed reference is retained across a drag.
    f.release({18.5, 30.5}); CHECK(f.idle());
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 27, 40) == 255);
    CHECK(pixel(f.selection(), 12, 10) == 0 && pixel(f.selection(), 18, 10) == 0);
    CHECK(pixel(f.selection(), 44, 10) == 0 && pixel(f.selection(), 51, 10) == 0);
    CHECK(f.find<QLabel>("ColorSelectionSample")->text() != unsampled);
    CHECK(f.session().history().undoDepth() == depth + 1);
    const auto result = f.selection();
    f.shortcut("Ctrl+Z"); CHECK(!f.selection());
    f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), result));
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels);
    CHECK(f.document().contentState() == content && !f.document().isModified());
    CHECK(f.session().colors() == colors);
}

void clickingInsideSelectionSamplesAndModifierModesLatch()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.rectangle({0, 0}, {64, 48});
    f.press({3.5, 10.5}); CHECK(f.idle());
    f.move({18.5, 30.5}); f.release({18.5, 30.5}); CHECK(f.idle());
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 27, 40) == 255);
    CHECK(pixel(f.selection(), 18, 10) == 0); // No selection translation or resampling on move.
    const auto red = f.selection();
    const auto redEdges = red->boundaryEdges();
    f.press({12.5, 10.5}, Qt::ShiftModifier); CHECK(f.idle());
    auto* add = f.find<QToolButton>("SelectionModeAdd"); CHECK(add && add->isChecked());
    CHECK(same(f.selection(), red));
    CHECK(f.canvas->scene().selectionRetainedEdges
        && *f.canvas->scene().selectionRetainedEdges == redEdges);
    f.move({18.5, 30.5}, Qt::AltModifier); f.release({18.5, 30.5}); CHECK(f.idle());
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 12, 10) == 255);
    CHECK(pixel(f.selection(), 18, 10) == 0);
    const auto added = f.selection();
    f.press({3.5, 10.5}, Qt::AltModifier); CHECK(f.idle());
    auto* subtract = f.find<QToolButton>("SelectionModeSubtract"); CHECK(subtract && subtract->isChecked());
    CHECK(same(f.selection(), added)); CHECK(f.canvas->scene().selectionRetainedEdges);
    f.release({3.5, 10.5}, Qt::ShiftModifier); CHECK(f.idle());
    CHECK(pixel(f.selection(), 3, 10) == 0 && pixel(f.selection(), 12, 10) == 255);
    f.press({44.5, 10.5}, Qt::ShiftModifier | Qt::AltModifier); CHECK(f.idle());
    auto* intersect = f.find<QToolButton>("SelectionModeIntersect"); CHECK(intersect && intersect->isChecked());
    CHECK(f.canvas->scene().selectionRetainedEdges);
    f.release({44.5, 10.5}); CHECK(f.idle());
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(!f.canvas->selectionAnimationActive());
}

void refinementsAlwaysRecombineFromOriginalClickSelection()
{
    for (const auto* mode : {"Add", "Subtract", "Intersect"}) {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        f.rectangle({0, 4}, {64, 20});
        const auto original = f.selection();
        f.mode(mode); f.click({3.5, 10.5});
        const auto narrow = f.selection();
        const auto depth = f.session().history().undoDepth();
        f.refine(40);
        const auto wider = f.selection();
        CHECK(!same(wider, narrow));
        CHECK(f.session().history().undoDepth() == depth + 1);
        f.refine(16);
        CHECK(same(f.selection(), narrow));
        CHECK(f.session().history().undoDepth() == depth + 2);
        f.refine(40); CHECK(same(f.selection(), wider));
        f.refine(16); CHECK(same(f.selection(), narrow));
        // Repeated wide/narrow previews in one adjustment must also be reversible.
        auto* control = f.find<ui::CompactValueControl>("ColorSelectionFuzziness");
        if (!control) continue;
        const auto adjustmentDepth = f.session().history().undoDepth();
        f.beginRefinement(*control);
        for (const int value : {40, 16, 255, 40}) {
            control->setValue(value); CHECK(f.idle());
            CHECK(same(f.selection(), narrow));
            CHECK(f.session().history().undoDepth() == adjustmentDepth);
            CHECK(f.canvas->scene().selectionRetainedEdges);
        }
        f.endRefinement(*control); CHECK(f.idle());
        CHECK(same(f.selection(), wider));
        CHECK(f.session().history().undoDepth() == adjustmentDepth + 1);
        f.shortcut("Ctrl+Z"); CHECK(same(f.selection(), narrow));
        f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), wider));
        CHECK(!same(original, wider));
    }
}

void cancellationAndNoopsPreserveSelectionHistoryAndRedo()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.click({3.5, 10.5});
    const auto narrow = f.selection();
    f.refine(40); f.shortcut("Ctrl+Z");
    auto* control = f.find<ui::CompactValueControl>("ColorSelectionFuzziness");
    if (!control) return;
    // Undo invalidates the sample; set the next click's fuzziness now.
    control->setValue(16);
    const auto depth = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    const auto revision = f.document().selectionRevision();
    // Cancel before the cooperative reference/worker has produced any result.
    f.press({18.5, 10.5}); f.escape(); CHECK(f.idle());
    CHECK(same(f.selection(), narrow));
    CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
    f.release({18.5, 10.5});
    // An identical click preserves an existing redo branch.
    control->setValue(16); f.click({3.5, 10.5});
    CHECK(same(f.selection(), narrow));
    CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
    CHECK(f.document().selectionRevision() == revision);
    for (int reason = 0; reason < 3; ++reason) {
        f.action("ToolAction_colorselect"); f.press({18.5, 10.5}); CHECK(f.idle());
        CHECK(same(f.selection(), narrow));
        if (reason == 0) f.escape();
        if (reason == 1) {
            QFocusEvent focus(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(f.canvas, &focus); settle();
        }
        if (reason == 2) f.action("ToolAction_brush");
        CHECK(f.idle());
        CHECK(same(f.selection(), narrow));
        CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
        CHECK(!f.canvas->selectionDragging());
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        f.release({18.5, 10.5}); CHECK(f.idle()); CHECK(same(f.selection(), narrow));
    }
    f.action("ToolAction_colorselect"); f.click({3.5, 10.5});
    f.beginRefinement(*control); control->setValue(40); CHECK(f.idle());
    CHECK(same(f.selection(), narrow)); f.escape(); CHECK(f.idle());
    CHECK(same(f.selection(), narrow) && control->value() == 16);
    f.endRefinement(*control); CHECK(f.idle());
    CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
}

void transparencySourceChangesAndUndoInvalidateReference()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.click({3.5, 10.5}); f.refine(255);
    CHECK(pixel(f.selection(), 18, 10) == 255 && pixel(f.selection(), 51, 10) == 255);
    CHECK(pixel(f.selection(), 44, 10) == 0 && pixel(f.selection(), 44, 35) == 0);
    f.click({44.5, 10.5});
    CHECK(pixel(f.selection(), 44, 10) == 255 && pixel(f.selection(), 44, 35) == 255);
    CHECK(pixel(f.selection(), 3, 10) == 0 && pixel(f.selection(), 51, 10) == 0);
    const auto transparent = f.selection();
    f.refine(0); CHECK(same(f.selection(), transparent));
    f.click({51.5, 10.5});
    CHECK(pixel(f.selection(), 51, 10) == 255 && pixel(f.selection(), 3, 10) == 0);
    CHECK(pixel(f.selection(), 44, 10) == 0);
    const auto partial = f.selection();
    const auto depth = f.session().history().undoDepth();
    f.shortcut("Ctrl+Z"); CHECK(same(f.selection(), transparent));
    f.refine(255); CHECK(same(f.selection(), transparent)); // Undo clears retained sample context.
    CHECK(f.session().history().undoDepth() == depth - 1);
    f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), partial));
    f.refine(40); CHECK(same(f.selection(), partial));
    f.find<QToolButton>("ColorSelectionActiveLayer")->click(); settle();
    f.click({3.5, 10.5}); const auto active = f.selection();
    f.find<QToolButton>("ColorSelectionMergedVisible")->click(); settle();
    f.refine(255); CHECK(same(f.selection(), active)); // Source switches require a fresh click.
    f.find<QToolButton>("ColorSelectionActiveLayer")->click(); settle();
    CHECK(f.document().setLayerTransform(*f.session().activeLayer(), {1, 0, 8, 0, 1, 0}));
    f.canvas->setDocument(f.document().snapshot(), false);
    const auto beforeOutside = f.selection();
    const auto outsideDepth = f.session().history().undoDepth();
    f.click({3.5, 10.5}); // On-canvas but outside this transformed active layer.
    CHECK(same(f.selection(), beforeOutside));
    CHECK(f.session().history().undoDepth() == outsideDepth);
}

void crossPanelReleaseAndSharedSelectionAdjustments()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    QWidget recipient(f.workspace->panelOverlay());
    recipient.setGeometry(15, 80, 160, 100); recipient.show();
    const auto routes = f.router->routedEventCount();
    f.press({3.5, 10.5}); CHECK(f.idle());
    const auto global = f.canvas->mapToGlobal(f.logical({18.5, 30.5}).toPoint());
    const QPointF position(recipient.mapFromGlobal(global));
    mouse(recipient, QEvent::MouseMove, position, Qt::NoButton, Qt::LeftButton);
    mouse(recipient, QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
    CHECK(f.idle()); CHECK(f.router->routedEventCount() >= routes + 2);
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 18, 10) == 0);
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    const auto selected = f.selection();
    auto* adjustments = f.find<QToolButton>("SelectionTransform");
    CHECK(adjustments && adjustments->isEnabled());
    if (adjustments && !adjustments->isChecked()) adjustments->click();
    settle();
    auto* growX = f.find<QDoubleSpinBox>("SelectionGrowHorizontal");
    auto* growY = f.find<QDoubleSpinBox>("SelectionGrowVertical");
    auto* apply = f.find<QToolButton>("SelectionGrowApply");
    if (growX && growY && apply) {
        growX->setValue(2); growY->setValue(0); apply->click(); settle();
        CHECK(same(f.selection(), selected->adjusted(2, 0)));
        f.shortcut("Ctrl+Z"); CHECK(same(f.selection(), selected));
    }
    const auto count = f.document().layers().size();
    f.shortcut("Ctrl+J"); CHECK(f.document().layers().size() == count + 1);
    CHECK(same(f.selection(), selected));
    f.shortcut("Ctrl+Z"); CHECK(f.document().layers().size() == count);
}

void sourceMutationsDiscardRetainedAndPendingReferences()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.click({3.5, 10.5});
    const auto selected = f.selection();
    const auto id = *f.session().activeLayer();
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(id, .5F)));
    const auto depth = f.session().history().undoDepth();
    f.refine(255); CHECK(same(f.selection(), selected));
    CHECK(f.session().history().undoDepth() == depth);
    CHECK(f.find<QLabel>("ColorSelectionSample")->text() == QStringLiteral("Click a color"));
    auto* active = f.find<QToolButton>("ColorSelectionActiveLayer");
    if (!active) return;
    active->click(); settle();
    f.refine(16); f.click({3.5, 10.5});
    f.session().setActiveLayer({}); f.refine(255);
    CHECK(same(f.selection(), selected));
    CHECK(f.find<QLabel>("ColorSelectionSample")->text() == QStringLiteral("Click a color"));
    f.session().setActiveLayer(id);
    // Change source geometry after press but before queued work can publish.
    f.press({18.5, 10.5});
    CHECK(f.document().setLayerTransform(id, {1, 0, 8, 0, 1, 0}));
    f.release({18.5, 10.5}); CHECK(f.idle());
    CHECK(same(f.selection(), selected));
    CHECK(f.session().history().undoDepth() == depth);
}

void typedReferencesUseDocumentResolutionWithoutChangingViewportCaches()
{
    for (const bool textLayer : {false, true}) {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        core::TextLayer text;
        text.utf8 = "H H";
        text.defaultStyle.sizePixels = 21.5;
        text.defaultStyle.color = {210, 35, 90, 255};
        core::ShapeLayer shape;
        shape.kind = core::ShapeKind::Ellipse;
        shape.size = {27.75, 24.25};
        shape.fillColor = {210, 35, 90, 255};
        shape.strokeEnabled = true;
        shape.strokeColor = {15, 110, 225, 255};
        shape.strokeWidth = 1.25;
        auto typed = textLayer ? core::Layer::text("Typed sample text", text)
            : core::Layer::shape("Typed sample ellipse", shape);
        typed.localToDocument = {1.1, -.15, 8.25, .1, .95, 2.75};
        const auto id = typed.id;
        CHECK(f.session().execute(std::make_unique<core::AddLayerCommand>(
            std::move(typed), f.document().layers().size(), f.session().activeLayer())));
        // Exercise the normal document synchronization/cache preparation path.
        f.action("ToolAction_colorselect");
        f.shortcut("Ctrl+Z"); f.shortcut("Ctrl+Shift+Z");
        CHECK(f.session().activeLayer() == id);
        f.document().markSaved();
        const auto documentRevision = f.document().revision();
        const auto content = f.document().contentState();
        const auto colors = f.session().colors();
        const auto* layer = f.document().layer(id);
        CHECK(layer && layer->renderCache && layer->renderCache->surface);
        if (!layer || !layer->renderCache || !layer->renderCache->surface) continue;
        const auto initialViewportCache = layer->renderCache;
        auto referenceCache = ui::prepareDocumentSampleCache(*layer, 1'000'000);
        CHECK(referenceCache && referenceCache->surface);
        CHECK(layer->renderCache == initialViewportCache);
        if (!referenceCache || !referenceCache->surface) continue;
        CHECK(referenceCache->surface != initialViewportCache->surface);
        const std::array prepared {core::SampleCacheOverride {id, referenceCache}};
        core::PinnedDocumentSampler activeReference(f.document(), id, core::ColorSampleSource::ActiveLayer,
            core::SampleFiltering::AlphaAware, prepared);
        core::Vec2d seed {};
        int greatestAlpha = -1;
        bool partialAlpha = false;
        // Font metrics are platform dependent, so find an actual opaque glyph
        // or shape interior from its document-resolution Qt rasterization.
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
            const core::Vec2d point {x + .5, y + .5};
            if (!activeReference.validSample(point)) continue;
            const auto color = activeReference.sample(point);
            partialAlpha = partialAlpha || (color.alpha > 0 && color.alpha < 255);
            if (color.alpha > greatestAlpha) { greatestAlpha = color.alpha; seed = point; }
        }
        CHECK(greatestAlpha == 255 && partialAlpha);
        if (greatestAlpha <= 0) continue;
        for (const auto source : {core::ColorSampleSource::ActiveLayer, core::ColorSampleSource::MergedVisible}) {
            f.find<QToolButton>(source == core::ColorSampleSource::ActiveLayer
                ? "ColorSelectionActiveLayer" : "ColorSelectionMergedVisible")->click();
            settle();
            core::PinnedDocumentSampler expectedSampler(f.document(), id, source,
                core::SampleFiltering::AlphaAware, prepared);
            const auto expectedColor = expectedSampler.sample(seed);
            const auto expectedLabel = QColor(expectedColor.red, expectedColor.green, expectedColor.blue).name().toUpper()
                + QStringLiteral(" · A: %1").arg(expectedColor.alpha);
            core::SelectionState baseline;
            double firstDensity = 0;
            for (const bool actualSize : {false, true, false}) {
                if (actualSize) f.canvas->resetTo100Percent();
                else f.canvas->fitDocumentToView();
                // Viewport caches intentionally settle at their normal density
                // before checking that sampling/refinement leaves them alone.
                QTest::qWait(150); settle();
                layer = f.document().layer(id);
                const auto viewportCache = layer->renderCache;
                CHECK(viewportCache && viewportCache->surface);
                if (!viewportCache || !viewportCache->surface) continue;
                if (!firstDensity) firstDensity = viewportCache->density;
                if (actualSize) CHECK(viewportCache->density != firstDensity);
                const auto surfaceId = viewportCache->surface->id();
                const auto surfaceRevision = viewportCache->surface->revision();
                const auto unchanged = [&] {
                    CHECK(f.document().layer(id)->renderCache == viewportCache);
                    CHECK(viewportCache->surface->id() == surfaceId && viewportCache->surface->revision() == surfaceRevision);
                    CHECK(f.document().revision() == documentRevision && f.document().contentState() == content);
                    CHECK(!f.document().isModified() && f.session().colors() == colors);
                };
                const auto starting = f.selection();
                f.press(seed); CHECK(f.idle());
                CHECK(same(f.selection(), starting));
                CHECK(f.find<QLabel>("ColorSelectionSample")->text() == expectedLabel);
                unchanged();
                f.release(seed); CHECK(f.idle()); unchanged();
                CHECK(pixel(f.selection(), int(seed.x), int(seed.y)) == 255);
                if (!baseline) baseline = f.selection();
                else CHECK(same(f.selection(), baseline));
                f.refine(40); unchanged();
                f.refine(16); unchanged();
                CHECK(same(f.selection(), baseline));
            }
        }
    }
}

void rapidSecondClickDiscardsTheFirstWorkersResult()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    auto* timer = f.find<QTimer>("ColorSelectionTimer");
    if (!timer) return;
    int referenceTicks = 0;
    const auto connection = QObject::connect(timer, &QTimer::timeout, [&] { ++referenceTicks; });
    const auto depth = f.session().history().undoDepth();
    f.press({3.5, 10.5});
    // One cooperative timer tick finishes this small reference and launches
    // its worker. No result can publish until the next scheduled timer tick.
    QElapsedTimer wait; wait.start();
    while (!referenceTicks && wait.elapsed() < 1000) QCoreApplication::processEvents();
    CHECK(referenceTicks == 1 && timer->isActive());
    CHECK(!f.selection());
    CHECK(f.find<QLabel>("ColorSelectionSample")->text().startsWith(QStringLiteral("#641414")));
    QObject::disconnect(connection);
    // Deliver release/next press without servicing the first worker's result.
    mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({3.5, 10.5}), Qt::LeftButton, Qt::NoButton);
    f.press({18.5, 10.5});
    f.release({18.5, 10.5}); CHECK(f.idle());
    CHECK(pixel(f.selection(), 18, 10) == 255 && pixel(f.selection(), 3, 10) == 0);
    CHECK(f.find<QLabel>("ColorSelectionSample")->text().startsWith(QStringLiteral("#141ED2")));
    CHECK(f.session().history().undoDepth() == depth + 1);
    const auto blue = f.selection();
    QTest::qWait(30); settle();
    CHECK(same(f.selection(), blue));
    f.shortcut("Ctrl+Z"); CHECK(!f.selection());
    f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), blue));
}

void largeReferenceSamplingYieldsToTheEventLoop()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    QImage source(2048, 2048, QImage::Format_RGBA8888);
    source.fill(QColor(100, 20, 20, 255));
    const auto path = f.assets.filePath(QStringLiteral("large-color-source.png"));
    CHECK(source.save(path)); CHECK(f.window.openImageFromPath(path)); settle();
    f.action("ToolAction_colorselect");
    int eventLoopTicks = 0;
    QElapsedTimer interval; interval.start();
    qint64 longestInterval = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        longestInterval = std::max(longestInterval, interval.restart());
        ++eventLoopTicks;
    });
    heartbeat.start(1);
    QElapsedTimer pressDuration; pressDuration.start();
    f.press({100.5, 100.5});
    CHECK(pressDuration.elapsed() < 250);
    f.release({100.5, 100.5});
    auto* timer = f.find<QTimer>("ColorSelectionTimer");
    CHECK(timer && waitFor([&] { return !timer->isActive(); }, 20000));
    heartbeat.stop();
    CHECK(eventLoopTicks > 8);
    CHECK(longestInterval < 250);
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({0, 0, 2048, 2048}));
    CHECK(f.session().history().undoDepth() == 1);
    std::cout << "Large Select by Color: " << eventLoopTicks << " event-loop ticks, longest interval "
              << longestInterval << " ms\n";
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 77;
    std::atomic_uint64_t warnings {0}, errors {0};
    QVulkanInstance instance; instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) return EXIT_FAILURE;
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << "Vulkan Select by Color: " << (data ? data->pMessage : "unknown") << '\n';
        return true;
    });
    if (!instance.create()) return EXIT_FAILURE;
    {
        Fixture f(&instance); CHECK(f.valid()); if (!f.valid()) return EXIT_FAILURE;
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads > 0
            && !f.canvas->presentationSuppressedForResize(); }));
        QTest::qWait(150);
        const auto baseline = f.canvas->rendererStats();
        const auto pixels = f.surface().revision(), revision = f.document().revision();
        f.click({3.5, 10.5});
        CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 27, 40) == 255);
        f.refine(40);
        const auto review = qEnvironmentVariable("IMAGEEDITOR_COLOR_SELECTION_REVIEW");
        if (!review.isEmpty()) {
            f.window.activateWindow(); QTest::qWait(200);
            QProcess capture;
            capture.start(QStringLiteral("spectacle"), {QStringLiteral("--background"),QStringLiteral("--nonotify"),
                QStringLiteral("--activewindow"),QStringLiteral("--output"),review});
            CHECK(capture.waitForFinished(5000) && capture.exitCode()==0);
        }
        f.refine(16);
        f.mode("Add"); f.press({12.5, 10.5}); CHECK(f.idle());
        CHECK(f.canvas->scene().selectionRetainedEdges && !f.canvas->scene().selectionRetainedEdges->empty());
        for (int i = 0; i < 4; ++i) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        }
        f.release({12.5, 10.5}); CHECK(f.idle());
        f.shortcut("Ctrl+Z"); f.shortcut("Ctrl+Shift+Z");
        f.mode("Subtract"); f.press({3.5, 10.5}); CHECK(f.idle()); f.escape(); CHECK(f.idle());
        QTest::qWait(150);
        const auto final = f.canvas->rendererStats();
        CHECK(final.fullUploads == baseline.fullUploads && final.regionalUploads == baseline.regionalUploads
            && final.uploadedBytes == baseline.uploadedBytes);
        CHECK(final.resourceGeneration == baseline.resourceGeneration && final.swapchainGeneration == baseline.swapchainGeneration);
        CHECK(f.surface().revision() == pixels && f.document().revision() == revision);
    }
    instance.destroy(); settle(); CHECK(warnings == 0 && errors == 0);
    std::cout << "Native Select by Color Vulkan validation: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ColorSelectionInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings; CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (application.arguments().contains(QStringLiteral("--wayland-validation"))) return nativeValidation();
    controlsAndTypedShortcutOwnership();
    clickFindsDisconnectedMatchesWithoutChangingContent();
    clickingInsideSelectionSamplesAndModifierModesLatch();
    refinementsAlwaysRecombineFromOriginalClickSelection();
    cancellationAndNoopsPreserveSelectionHistoryAndRedo();
    transparencySourceChangesAndUndoInvalidateReference();
    crossPanelReleaseAndSharedSelectionAdjustments();
    sourceMutationsDiscardRetainedAndPendingReferences();
    typedReferencesUseDocumentResolutionWithoutChangingViewportCaches();
    rapidSecondClickDiscardsTheFirstWorkersResult();
    largeReferenceSamplingYieldsToTheEventLoop();
    if (failures) std::cerr << failures << " Select by Color assertion(s) failed\n";
    else std::cout << "Select by Color interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
