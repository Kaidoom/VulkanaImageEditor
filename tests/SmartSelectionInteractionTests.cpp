#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/SmartSelectionEvidence.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
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
void key(QObject* receiver, int code, Qt::KeyboardModifiers modifiers = {})
{
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
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
        // Connected exact/near reds, a separate exact red, blue barriers,
        // transparent hidden RGB, and partially opaque red.
        QImage source(64, 48, QImage::Format_RGBA8888);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
            QColor color(20, 30, 210, 255);
            if (x < 8 || (x >= 24 && x < 32)) color = QColor(100, 20, 20, 255);
            else if (x < 16) color = QColor(130, 20, 20, 255);
            else if (x >= 40 && x < 48) color = QColor(y < 24 ? 100 : 250, 20, 20, 0);
            else if (x >= 48 && x < 56) color = QColor(100, 20, 20, 128);
            source.setPixelColor(x, y, color);
        }
        const auto path = assets.filePath(QStringLiteral("smart-selection-source.png"));
        CHECK(source.save(path)); CHECK(window.openImageFromPath(path));
        action("ToolAction_smartselect");
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
    void button(const char* name)
    { if (auto* target = find<QToolButton>(name)) target->click(); settle(); }
    void mode(const char* suffix)
    {
        const auto name = QStringLiteral("SelectionMode") + QString::fromLatin1(suffix);
        auto* target = window.findChild<QToolButton*>(name); CHECK(target);
        if (target) target->click();
        settle();
    }
    void quick()
    {
        button("SmartModeQuick");
        if (auto* size = find<ui::CompactValueControl>("SmartSize")) size->setValue(6);
    }
    void wand() { button("SmartModeWand"); }
    bool idle()
    {
        auto* timer = find<QTimer>("SmartSelectionTimer");
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
        press(a); move(b); release(b); action("ToolAction_smartselect");
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
        mouse(control, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        settle();
    }
    void refine(int tolerance)
    {
        auto* control = find<ui::CompactValueControl>("SmartTolerance");
        if (!control) return;
        beginRefinement(*control); control->setValue(tolerance); endRefinement(*control);
        CHECK(idle());
    }
    void escape() { key(canvas, Qt::Key_Escape); }
};

void controlsAndTypedShortcutOwnership()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    auto* action = f.find<QAction>("ToolAction_smartselect");
    auto* controls = f.find<QWidget>("SmartSelectionControls");
    auto* size = f.find<ui::CompactValueControl>("SmartSize");
    auto* edges = f.find<ui::CompactValueControl>("SmartEdgeSensitivity");
    auto* tolerance = f.find<ui::CompactValueControl>("SmartTolerance");
    auto* options = dynamic_cast<ui::ToolOptionsBar*>(f.find<QToolBar>("ToolOptionsBar"));
    CHECK(action && action->shortcut() == QKeySequence(QStringLiteral("W")));
    CHECK(f.session().activeTool() == core::ToolId::SmartSelect);
    CHECK(controls && controls->isVisible());
    CHECK(options && options->pageForTool(core::ToolId::SmartSelect));
    CHECK(size && size->minimum() == 2 && size->maximum() == 512 && size->value() == 24 && size->isVisible());
    CHECK(edges && edges->minimum() == 0 && edges->maximum() == 100 && edges->value() == 40
        && edges->suffix() == QStringLiteral("%") && edges->isVisible() && edges->isEnabled());
    CHECK(!f.window.findChild<QObject*>(QStringLiteral("SmartReach")));
    CHECK(tolerance && tolerance->minimum() == 0 && tolerance->maximum() == 255 && tolerance->value() == 16
        && !tolerance->isVisible());
    CHECK(f.find<QToolButton>("SmartModeQuick")->isChecked());
    CHECK(f.find<QToolButton>("SmartSourceMerged")->isChecked());
    CHECK(f.find<QToolButton>("SelectionModeAdd")->isChecked());
    CHECK(f.canvas->scene().quickSelectionCursor);
    const auto geometry = f.workspace->canvasContainer()->geometry();
    f.wand();
    CHECK(tolerance && tolerance->isVisible() && size && !size->isVisible());
    CHECK(edges && !edges->isVisible());
    CHECK(!f.canvas->scene().quickSelectionCursor);
    CHECK(f.find<QLabel>("SmartSample")->text() == QStringLiteral("Click a color"));
    f.button("SmartSourceActive"); CHECK(f.find<QToolButton>("SmartSourceActive")->isChecked());
    f.button("SmartSourceMerged"); CHECK(f.find<QToolButton>("SmartSourceMerged")->isChecked());
    f.mode("Subtract"); f.action("ToolAction_brush"); CHECK(controls && !controls->isVisible());
    f.action("ToolAction_smartselect"); CHECK(f.find<QToolButton>("SelectionModeSubtract")->isChecked());
    f.mode("Add");
    CHECK(f.workspace->canvasContainer()->geometry() == geometry);
    if (!tolerance || !action) return;
    tolerance->setFocus(); QTest::keyClick(tolerance, Qt::Key_1); settle();
    CHECK(tolerance->isManualEntryActive());
    auto* editor = tolerance->findChild<QLineEdit*>(); CHECK(editor);
    if (editor) {
        QSignalSpy activations(action, &QAction::triggered);
        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier); CHECK(!f.selection());
        QTest::keyClick(editor, Qt::Key_W);
        CHECK(activations.isEmpty() && f.session().activeTool() == core::ToolId::SmartSelect);
        QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
        CHECK(f.session().history().undoDepth() == 0);
        QTest::keyClick(editor, Qt::Key_Escape);
    }
    f.action("ToolAction_colorselect");
    CHECK(f.session().activeTool() == core::ToolId::SelectByColor);
    CHECK(f.find<QAction>("ToolAction_colorselect")->shortcut() == QKeySequence(QStringLiteral("Shift+O")));
    f.canvas->requestActivate(); settle(); QTest::keyClick(f.canvas, Qt::Key_W); settle();
    CHECK(f.session().activeTool() == core::ToolId::SmartSelect);
    f.quick();
    CHECK(edges == f.find<ui::CompactValueControl>("SmartEdgeSensitivity") && edges->isVisible());
    const auto depth = f.session().history().undoDepth();
    const auto selectionRevision = f.document().selectionRevision();
    const auto wandTolerance = tolerance->value();
    edges->setValue(75); settle();
    CHECK(f.session().history().undoDepth() == depth && f.document().selectionRevision() == selectionRevision);
    f.wand();
    CHECK(!edges->isVisible() && edges->value() == 75 && tolerance->value() == wandTolerance);
    f.quick(); CHECK(edges->isVisible() && edges->value() == 75);
}

void normalizedQuickDragCommitsOnlyOnReleaseAndKeepsArtworkClean()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.quick(); f.document().markSaved();
    const auto colors = f.session().colors();
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto content = f.document().contentState();
    const auto depth = f.session().history().undoDepth();
    f.press({3.5, 10.5}); CHECK(f.canvas->selectionDragging());
    CHECK(!f.find<ui::CompactValueControl>("SmartEdgeSensitivity")->isEnabled());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    CHECK(f.idle()); CHECK(!f.selection());
    CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
    f.move({8.5, 20.5}); f.move({12.5, 30.5}); CHECK(f.idle());
    CHECK(!f.selection() && f.session().history().undoDepth() == depth);
    f.release({12.5, 30.5}); CHECK(f.idle());
    CHECK(f.find<ui::CompactValueControl>("SmartEdgeSensitivity")->isEnabled());
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 8, 20) == 255
        && pixel(f.selection(), 12, 30) == 255);
    CHECK(pixel(f.selection(), 60, 10) == 0);
    CHECK(f.session().history().undoDepth() == depth + 1);
    const auto selected = f.selection();
    f.shortcut("Ctrl+Z"); CHECK(!f.selection());
    f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), selected));
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels);
    CHECK(f.document().contentState() == content && !f.document().isModified());
    CHECK(f.session().colors() == colors);
}

void wandIsContiguousAndModifiersLatchWhileOriginalAntsRemain()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.wand(); f.mode("Replace"); f.click({3.5, 10.5});
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 27, 10) == 0);
    CHECK(pixel(f.selection(), 12, 10) == 0);
    const auto red = f.selection();
    f.press({12.5, 10.5}, Qt::ShiftModifier); CHECK(f.idle());
    CHECK(f.find<QToolButton>("SelectionModeAdd")->isChecked());
    CHECK(same(f.selection(), red));
    CHECK(!f.canvas->scene().selectionRetainedEdges);
    core::SmartSelectionReference reference(f.document(),f.session().activeLayer(),core::ColorSampleSource::MergedVisible);
    while(!reference.step()){}
    const std::atomic_bool cancelled{false};
    const auto proposed=core::buildMagicWand(*reference.image(),{12.5,10.5},16,red,core::SelectionOperation::Add,cancelled);
    CHECK(f.canvas->scene().selectionEdges&&*f.canvas->scene().selectionEdges==proposed.combined->boundaryEdges());
    f.move({18.5, 30.5}, Qt::AltModifier); f.release({18.5, 30.5}); CHECK(f.idle());
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 12, 10) == 255);
    CHECK(pixel(f.selection(), 18, 30) == 0); // Wand retains its pressed seed.
    const auto added = f.selection();
    f.press({3.5, 10.5}, Qt::AltModifier); CHECK(f.idle());
    CHECK(f.find<QToolButton>("SelectionModeSubtract")->isChecked());
    CHECK(same(f.selection(), added));
    f.release({3.5, 10.5}, Qt::ShiftModifier); CHECK(f.idle());
    CHECK(pixel(f.selection(), 3, 10) == 0 && pixel(f.selection(), 12, 10) == 255);
    f.press({12.5, 10.5}, Qt::ShiftModifier | Qt::AltModifier); CHECK(f.idle());
    CHECK(f.find<QToolButton>("SelectionModeIntersect")->isChecked());
    f.release({12.5, 10.5}); CHECK(f.idle());
    f.mode("Replace");
    const auto original = f.selection();
    f.press({27.5, 10.5}); CHECK(f.idle());
    CHECK(same(f.selection(), original));
    CHECK(!f.canvas->scene().selectionRetainedEdges);
    const auto replacement=core::buildMagicWand(*reference.image(),{27.5,10.5},16,original,core::SelectionOperation::Replace,cancelled);
    CHECK(f.canvas->scene().selectionEdges&&*f.canvas->scene().selectionEdges==replacement.combined->boundaryEdges());
    f.release({27.5, 10.5}); CHECK(f.idle());
    CHECK(pixel(f.selection(), 27, 10) == 255 && pixel(f.selection(), 12, 10) == 0);
}

void toleranceRefinementsUseOriginalClickBaseline()
{
    for (const auto* operation : {"Add", "Subtract", "Intersect"}) {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        f.rectangle({0, 4}, {64, 20}); f.wand(); f.mode(operation);
        const auto baseline = f.selection();
        f.click({3.5, 10.5}); const auto narrow = f.selection();
        const auto depth = f.session().history().undoDepth();
        f.refine(40); const auto wider = f.selection(); CHECK(!same(wider, narrow));
        CHECK(f.session().history().undoDepth() == depth + 1);
        f.refine(16); CHECK(same(f.selection(), narrow));
        f.refine(40); CHECK(same(f.selection(), wider));
        f.refine(16); CHECK(same(f.selection(), narrow));
        CHECK(!same(baseline, narrow));
        auto* control = f.find<ui::CompactValueControl>("SmartTolerance"); if (!control) continue;
        const auto adjustingDepth = f.session().history().undoDepth();
        f.beginRefinement(*control);
        for (const int value : {40, 16, 255, 40}) {
            control->setValue(value); CHECK(f.idle());
            CHECK(same(f.selection(), narrow));
            CHECK(f.session().history().undoDepth() == adjustingDepth);
            CHECK(!f.canvas->scene().selectionRetainedEdges);
            core::SmartSelectionReference reference(f.document(),f.session().activeLayer(),core::ColorSampleSource::MergedVisible);
            while(!reference.step()){}
            const std::atomic_bool cancelled{false};
            const auto op=QString::fromLatin1(operation)=="Add"?core::SelectionOperation::Add:
                QString::fromLatin1(operation)=="Subtract"?core::SelectionOperation::Subtract:core::SelectionOperation::Intersect;
            const auto expected=core::buildMagicWand(*reference.image(),{3.5,10.5},value,baseline,op,cancelled);
            CHECK(f.canvas->scene().selectionEdges&&*f.canvas->scene().selectionEdges==expected.combined->boundaryEdges());
        }
        f.endRefinement(*control); CHECK(f.idle()); CHECK(same(f.selection(), wider));
        CHECK(f.session().history().undoDepth() == adjustingDepth + 1);
        f.shortcut("Ctrl+Z"); CHECK(same(f.selection(), narrow));
        f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), wider));
    }
}

void cancellationAndNoopsPreserveHistoryAndRedo()
{
    for (const bool wand : {false, true}) {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        if (wand) f.wand(); else f.quick();
        f.mode("Add"); f.click({3.5, 10.5});
        const auto first = f.selection();
        f.click({27.5, 10.5}); f.shortcut("Ctrl+Z"); CHECK(same(f.selection(), first));
        const auto depth = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
        const auto revision = f.document().selectionRevision();
        f.press({60.5, 10.5}); f.escape(); CHECK(f.idle()); f.release({60.5, 10.5});
        CHECK(same(f.selection(), first));
        f.click({3.5, 10.5}); CHECK(same(f.selection(), first));
        CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
        CHECK(f.document().selectionRevision() == revision);
        for (int reason = 0; reason < 3; ++reason) {
            f.action("ToolAction_smartselect"); f.press({60.5, 10.5}); CHECK(f.idle());
            CHECK(same(f.selection(), first));
            if (reason == 0) f.escape();
            if (reason == 1) {
                QFocusEvent focus(QEvent::FocusOut, Qt::OtherFocusReason);
                QCoreApplication::sendEvent(f.canvas, &focus); settle();
            }
            if (reason == 2) f.action("ToolAction_brush");
            CHECK(f.idle()); CHECK(same(f.selection(), first));
            CHECK(!f.canvas->selectionDragging());
            CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
            f.release({60.5, 10.5}); CHECK(f.idle());
            CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
        }
        if (!wand) continue;
        f.action("ToolAction_smartselect"); f.click({3.5, 10.5});
        auto* control = f.find<ui::CompactValueControl>("SmartTolerance"); if (!control) continue;
        f.beginRefinement(*control); control->setValue(40); CHECK(f.idle());
        CHECK(same(f.selection(), first)); f.escape(); CHECK(f.idle());
        CHECK(same(f.selection(), first) && control->value() == 16);
        f.endRefinement(*control); CHECK(f.idle());
        CHECK(f.session().history().undoDepth() == depth && f.session().history().redoDepth() == redo);
    }
}

void undoAndRedoRestoreQuickHintsBeforeFurtherRefinement()
{
    // Compare history restoration with the same non-undone stroke prefix,
    // including hints, not a fresh solver stripped of all previous corrections.
    for (const bool redo : {false, true}) {
        core::SelectionState restored, refined;
        core::SelectionEvidenceState restoredEvidence, refinedEvidence;
        {
            Fixture f; CHECK(f.valid()); if (!f.valid()) return;
            f.quick(); f.mode("Add"); f.click({3.5, 10.5});
            const auto addedEvidence = f.document().selectionEvidence();
            CHECK(addedEvidence);
            f.mode("Subtract"); f.click({12.5, 12.5});
            const auto subtractedEvidence = f.document().selectionEvidence();
            CHECK(subtractedEvidence && subtractedEvidence != addedEvidence);
            f.shortcut("Ctrl+Z");
            CHECK(f.document().selectionEvidence() == addedEvidence);
            if (redo) f.shortcut("Ctrl+Shift+Z");
            CHECK(f.document().selectionEvidence() == (redo ? subtractedEvidence : addedEvidence));
            restored = f.selection();
            restoredEvidence = f.document().selectionEvidence();
            CHECK(restored && !restored->bounds().empty());
            f.mode("Add"); f.press({11.5, 20.5}); f.move({12.5, 26.5}); f.release({12.5, 26.5});
            CHECK(f.idle()); refined = f.selection(); refinedEvidence = f.document().selectionEvidence();
        }
        Fixture fresh; CHECK(fresh.valid()); if (!fresh.valid()) return;
        fresh.quick(); fresh.mode("Add");
        fresh.click({3.5, 10.5});
        if (redo) { fresh.mode("Subtract"); fresh.click({12.5, 12.5}); }
        CHECK(same(fresh.selection(), restored));
        const auto compareHints = [](const core::SelectionEvidenceState& a, const core::SelectionEvidenceState& b) {
            const auto first = std::dynamic_pointer_cast<const core::SmartSelectionEvidence>(a);
            const auto second = std::dynamic_pointer_cast<const core::SmartSelectionEvidence>(b);
            CHECK(first && second);
            if (!first || !second) return;
            CHECK(same(first->hints().foreground, second->hints().foreground));
            CHECK(same(first->hints().background, second->hints().background));
            CHECK(same(first->hints().rejected, second->hints().rejected));
        };
        compareHints(fresh.document().selectionEvidence(), restoredEvidence);
        fresh.mode("Add");
        fresh.press({11.5, 20.5}); fresh.move({12.5, 26.5}); fresh.release({12.5, 26.5}); CHECK(fresh.idle());
        CHECK(same(fresh.selection(), refined));
        compareHints(fresh.document().selectionEvidence(), refinedEvidence);
        const auto evidence = fresh.document().selectionEvidence();
        fresh.rectangle({40, 4}, {45, 9});
        CHECK(!fresh.document().selectionEvidence());
        fresh.shortcut("Ctrl+Z");
        CHECK(fresh.document().selectionEvidence() == evidence);
        const auto depth = fresh.session().history().undoDepth();
        fresh.press({20.5, 20.5}); fresh.escape(); fresh.release({20.5, 20.5}); CHECK(fresh.idle());
        CHECK(fresh.document().selectionEvidence() == evidence);
        CHECK(fresh.session().history().undoDepth() == depth);
    }
}

void referenceChangesInvalidateRetainedSamplesAndPendingResults()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.wand(); f.mode("Replace"); f.click({3.5, 10.5});
    const auto selected = f.selection();
    f.button("SmartSourceActive"); f.refine(255); CHECK(same(f.selection(), selected));
    f.refine(16); f.click({3.5, 10.5});
    const auto id = *f.session().activeLayer();
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(id, .5F)));
    const auto depth = f.session().history().undoDepth();
    f.refine(255); CHECK(same(f.selection(), selected));
    CHECK(f.find<QLabel>("SmartSample")->text() == QStringLiteral("Click a color"));
    CHECK(f.session().history().undoDepth() == depth);
    f.refine(16); f.click({3.5, 10.5});
    const auto active = f.selection(); const auto activeDepth = f.session().history().undoDepth();
    f.session().setActiveLayer({}); f.refine(255); CHECK(same(f.selection(), active));
    CHECK(f.session().history().undoDepth() == activeDepth);
    CHECK(f.find<QLabel>("SmartSample")->text() == QStringLiteral("Click a color"));
    f.session().setActiveLayer(id); f.refine(16);
    f.press({18.5, 10.5});
    CHECK(f.document().setLayerTransform(id, {1, 0, 8, 0, 1, 0}));
    f.release({18.5, 10.5}); CHECK(f.idle());
    CHECK(same(f.selection(), active) && f.session().history().undoDepth() == activeDepth);
    f.canvas->setDocument(f.document().snapshot(), false);
    f.click({3.5, 10.5}); // Outside the transformed active source's valid extent.
    CHECK(same(f.selection(), active) && f.session().history().undoDepth() == activeDepth);
}

void rapidCompletedGesturesCommitInOrderFromAColdReference()
{
    for (const bool wand : {false, true}) {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        if (wand) f.wand(); else f.quick();
        f.mode("Add");
        const auto depth = f.session().history().undoDepth();
        // Deliberately do not process events between either press/release pair:
        // both complete before the cold reference or first worker can finish.
        f.press({3.5, 10.5});
        mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({3.5, 10.5}), Qt::LeftButton, Qt::NoButton);
        f.press({27.5, 30.5});
        mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({27.5, 30.5}), Qt::LeftButton, Qt::NoButton);
        CHECK(!f.selection()); CHECK(f.idle());
        CHECK(pixel(f.selection(), 27, 30) == 255 && pixel(f.selection(), 3, 10) == 255);
        CHECK(f.session().history().undoDepth() == depth + 2);
        const auto second = f.selection();
        QTest::qWait(30); settle(); CHECK(same(f.selection(), second));
        f.shortcut("Ctrl+Z");
        CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 27, 30) == 0);
        f.shortcut("Ctrl+Z"); CHECK(!f.selection());
        f.shortcut("Ctrl+Shift+Z"); f.shortcut("Ctrl+Shift+Z"); CHECK(same(f.selection(), second));
    }
}

void queuedHeldStrokeStartsLiveRefinementAfterPrecedingCommit()
{
    for (const bool cancel : {false, true}) {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        f.quick(); f.mode("Add");
        const auto depth = f.session().history().undoDepth();
        f.press({3.5, 10.5});
        mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({3.5, 10.5}), Qt::LeftButton, Qt::NoButton);
        f.press({27.5, 30.5}); // Still held when the first stroke finishes.
        CHECK(f.idle());
        CHECK(f.session().history().undoDepth() == depth + 1);
        CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 27, 30) == 0);
        CHECK(f.canvas->scene().selectionPathPreview);
        CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
        CHECK(!f.canvas->scene().selectionRetainedEdges);
        f.move({27.5, 35.5}); CHECK(f.idle());
        CHECK(f.session().history().undoDepth() == depth + 1);
        if (cancel) {
            f.escape(); f.release({27.5, 35.5}); CHECK(f.idle());
            CHECK(f.session().history().undoDepth() == depth + 1);
            CHECK(pixel(f.selection(), 27, 30) == 0);
        } else {
            f.release({27.5, 35.5}); CHECK(f.idle());
            CHECK(f.session().history().undoDepth() == depth + 2);
            CHECK(pixel(f.selection(), 27, 30) == 255 && pixel(f.selection(), 27, 35) == 255);
        }
        CHECK(!f.canvas->selectionDragging());
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    }
}

void crossPanelReleaseAndSharedSelectionTransform()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.quick();
    QWidget recipient(f.workspace->panelOverlay());
    recipient.setGeometry(15, 80, 160, 100); recipient.show();
    const auto routes = f.router->routedEventCount();
    f.press({3.5, 10.5}); CHECK(f.idle());
    const auto global = f.canvas->mapToGlobal(f.logical({12.5, 30.5}).toPoint());
    const QPointF position(recipient.mapFromGlobal(global));
    mouse(recipient, QEvent::MouseMove, position, Qt::NoButton, Qt::LeftButton);
    mouse(recipient, QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
    CHECK(f.idle()); CHECK(f.router->routedEventCount() >= routes + 2);
    CHECK(!f.canvas->selectionDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 12, 30) == 255);
    const auto selected = f.selection();
    const auto matrix = f.document().layers().front().localToDocument;
    const auto pixels = f.surface().revision();
    f.shortcut("Ctrl+T"); CHECK(f.session().activeTool() == core::ToolId::Transform);
    CHECK(f.canvas->scene().transformOverlay.has_value());
    CHECK(f.find<QAction>("ToolAction_smartselect")->isChecked());
    f.button("TransformCancel"); CHECK(f.session().activeTool() == core::ToolId::SmartSelect);
    CHECK(same(f.selection(), selected) && !f.canvas->scene().transformOverlay);
    CHECK(f.document().layers().front().localToDocument == matrix && f.surface().revision() == pixels);
    f.button("SelectionTransform");
    auto* growX = f.find<QDoubleSpinBox>("SelectionGrowHorizontal");
    auto* growY = f.find<QDoubleSpinBox>("SelectionGrowVertical");
    if (growX && growY && selected) {
        growX->setValue(2); growY->setValue(0); f.button("SelectionGrowApply");
        CHECK(same(f.selection(), selected->adjusted(2, 0)));
        f.shortcut("Ctrl+Z"); CHECK(same(f.selection(), selected));
    }
    f.mode("Subtract"); f.action("ToolAction_marquee");
    CHECK(f.find<QToolButton>("SelectionModeSubtract")->isChecked());
    f.mode("Intersect"); f.action("ToolAction_smartselect");
    CHECK(f.find<QToolButton>("SelectionModeIntersect")->isChecked());
    const auto count = f.document().layers().size();
    f.shortcut("Ctrl+J"); CHECK(f.document().layers().size() == count + 1);
    CHECK(same(f.selection(), selected)); f.shortcut("Ctrl+Z"); CHECK(f.document().layers().size() == count);
}

void highResolutionResponsiveness()
{
    for(const auto extent:{QSize(3840,2160),QSize(5120,2880)}) {
        Fixture f;CHECK(f.valid());if(!f.valid())return;
        QImage image(extent,QImage::Format_RGBA8888);image.fill(QColor(12,25,90));
        const int cx=extent.width()/2,cy=extent.height()/2;
        for(int y=cy-100;y<cy+100;++y)for(int x=cx-130;x<cx+130;++x)
            image.setPixelColor(x,y,QColor(160+(x-cx+130)*90/260,60+(x-cx+130)*80/260,20));
        const auto path=f.assets.filePath(QStringLiteral("large-smart.png"));CHECK(image.save(path));
        CHECK(f.window.openImageFromPath(path));f.action("ToolAction_smartselect");f.quick();
        f.find<ui::CompactValueControl>("SmartSize")->setValue(24);
        int heartbeats=0;qint64 maxGap=0;QElapsedTimer gap;gap.start();QTimer heartbeat;
        QObject::connect(&heartbeat,&QTimer::timeout,[&]{++heartbeats;maxGap=std::max(maxGap,gap.restart());});
        heartbeat.start(1);
        QElapsedTimer total;total.start();QElapsedTimer press;press.start();
        f.press({double(cx-105)+.5,double(cy)+.5});const auto pressMs=double(press.nsecsElapsed())/1e6;
        f.move({double(cx+105)+.5,double(cy)+.5});f.release({double(cx+105)+.5,double(cy)+.5});
        auto* timer=f.find<QTimer>("SmartSelectionTimer");CHECK(waitFor([&]{return !timer->isActive();},30000));
        const auto cold=total.elapsed();CHECK(f.session().history().undoDepth()==1);
        CHECK(pixel(f.selection(),cx,cy)==255&&pixel(f.selection(),cx+200,cy)==0);
        CHECK(!f.document().isModified());
        QElapsedTimer warm;warm.start();f.press({double(cx)+.5,double(cy+70)+.5},Qt::AltModifier);
        f.release({double(cx)+.5,double(cy+70)+.5});CHECK(waitFor([&]{return !timer->isActive();},30000));
        const auto warmMs=warm.elapsed();heartbeat.stop();
        CHECK(heartbeats>8&&maxGap<250&&pressMs<100);
        std::cout<<extent.width()<<'x'<<extent.height()<<" UI press_ms="<<pressMs<<" cold_release_to_idle_ms="<<cold
            <<" warm_correction_ms="<<warmMs<<" heartbeat_ticks="<<heartbeats<<" largest_loop_gap_ms="<<maxGap<<'\n';
    }
}

int privateQuality(const QString& sourcePath,const QString& outputPath)
{
    QVulkanInstance instance;
    const bool native=QGuiApplication::platformName()=="wayland"||QGuiApplication::platformName()=="xcb";
    if(native){instance.setApiVersion(QVersionNumber(1,2));instance.setLayers({"VK_LAYER_KHRONOS_validation"});CHECK(instance.create());}
    Fixture f(native?&instance:nullptr);CHECK(f.valid());
    CHECK(f.window.openImageFromPath(sourcePath));f.action("ToolAction_smartselect");f.quick();f.mode("Replace");
    f.find<ui::CompactValueControl>("SmartSize")->setValue(24);
    auto* timer=f.find<QTimer>("SmartSelectionTimer");
    const auto idle=[&]{return waitFor([&]{return !timer->isActive();},30000);};
    const auto save=[&](const QString& name) {
        const auto extent=f.document().canvas().extent;QImage mask(int(extent.width),int(extent.height),QImage::Format_Grayscale8);
        for(int y=0;y<mask.height();++y)for(int x=0;x<mask.width();++x)mask.scanLine(y)[x]=std::uint8_t(std::max(0,pixel(f.selection(),x,y)));
        CHECK(QDir().mkpath(outputPath));CHECK(mask.save(QDir(outputPath).filePath(name+"-mask.png")));
        CHECK(f.window.grab().save(QDir(outputPath).filePath(name+"-window.png")));
        if(native&&qEnvironmentVariableIsSet("IMAGEEDITOR_SMART_SELECTION_REVIEW")) {
            f.window.activateWindow();QTest::qWait(200);
            QProcess capture;capture.start(QStringLiteral("spectacle"),{QStringLiteral("--background"),QStringLiteral("--nonotify"),
                QStringLiteral("--activewindow"),QStringLiteral("--output"),QDir(outputPath).filePath(name+"-native.png")});
            CHECK(capture.waitForFinished(5000)&&capture.exitCode()==0);
        }
    };
    const auto document=f.window.activeDocumentId(),revision=f.document().revision(),pixels=f.surface().revision();
    const auto depth=f.session().history().undoDepth();QElapsedTimer elapsed;elapsed.start();
    f.press({693,1756});f.move({735,1685});QTest::qWait(30);f.move({782,1610});
    CHECK(idle());CHECK(!f.selection());CHECK(f.session().history().undoDepth()==depth);
    const auto displayed=*f.canvas->scene().selectionEdges;
    f.release({782,1610});CHECK(idle());
    std::cout<<"private_quick_gesture_ms="<<elapsed.elapsed()<<'\n';save("quick-floor");
    CHECK(f.selection()&&f.selection()->bounds().width>200);
    CHECK(displayed==f.selection()->boundaryEdges()); // Actual displayed proposal is the publication.
    CHECK(f.session().history().undoDepth()==depth+1);
    f.shortcut("Ctrl+Z");CHECK(!f.selection());f.shortcut("Ctrl+Shift+Z");
    f.mode("Subtract");f.press({748,1690});CHECK(idle());const auto subtract=*f.canvas->scene().selectionEdges;
    f.release({748,1690});CHECK(idle());CHECK(subtract==f.selection()->boundaryEdges());
    f.shortcut("Ctrl+Z");f.action("DeselectAction");f.button("SmartModeObject");f.mode("Replace");
    CHECK(!f.window.findChild<QToolButton*>("ObjectModelSetup"));
    elapsed.restart();f.press({80,400});f.move({770,1847});f.release({770,1847});CHECK(idle());
    std::cout<<"private_object_gesture_ms="<<elapsed.elapsed()<<'\n';save("object-box");
    CHECK(pixel(f.selection(),450,650)==255&&pixel(f.selection(),800,300)==0);
    const auto box=f.selection();const auto objectDepth=f.session().history().undoDepth();
    f.click({787,1270},Qt::AltModifier);CHECK(idle());save("object-corrected");
    const auto corrected=f.selection();CHECK(pixel(corrected,787,1270)==0);
    CHECK(f.session().history().undoDepth()==objectDepth+1);
    f.shortcut("Ctrl+Z");CHECK(same(f.selection(),box));
    f.shortcut("Ctrl+Shift+Z");CHECK(same(f.selection(),corrected));
    f.press({500,1500});f.release({500,1500});f.escape();CHECK(idle());CHECK(same(f.selection(),corrected));
    // Leaving a tab during work cannot publish into a new document, including
    // another import whose local layer IDs begin at the same value.
    f.press({80,400});f.move({770,1847});f.release({770,1847});
    CHECK(f.window.openImageFromPath(sourcePath));const auto second=f.window.activeDocumentId();CHECK(second!=document);
    CHECK(idle());CHECK(!f.selection());
    CHECK(f.window.activateDocument(document));CHECK(same(f.selection(),corrected));
    CHECK(f.document().revision()==revision&&f.surface().revision()==pixels);
    f.window.close();settle();
    return failures?1:0;
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland") && QGuiApplication::platformName()!=QStringLiteral("xcb")) return 77;
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
        std::cerr << "Vulkan Smart Select: " << (data ? data->pMessage : "unknown") << '\n';
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
        f.quick(); f.press({3.5, 10.5}); f.move({12.5, 30.5}); f.release({12.5, 30.5}); CHECK(f.idle());
        CHECK(pixel(f.selection(), 3, 10) == 255 && pixel(f.selection(), 12, 30) == 255);
        const auto artifacts = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../../test-artifacts"));
        CHECK(QDir().mkpath(artifacts));
        CHECK(f.window.grab().save(QDir(artifacts).filePath(QStringLiteral("smart-select-quick.png"))));
        const auto nativeCapture=[&](const QString& suffix) {
            const auto prefix=qEnvironmentVariable("IMAGEEDITOR_SMART_SELECTION_REVIEW");
            if(prefix.isEmpty())return;
            f.window.activateWindow();QTest::qWait(200);
            QProcess capture;capture.start(QStringLiteral("spectacle"),{QStringLiteral("--background"),QStringLiteral("--nonotify"),
                QStringLiteral("--activewindow"),QStringLiteral("--output"),prefix+suffix+QStringLiteral(".png")});
            CHECK(capture.waitForFinished(5000)&&capture.exitCode()==0);
        };
        nativeCapture(QStringLiteral("-quick"));
        f.mode("Replace"); f.press({27.5, 10.5}); CHECK(f.idle());
        CHECK(!f.canvas->scene().selectionRetainedEdges);
        for (int i = 0; i < 4; ++i) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        }
        f.escape(); CHECK(f.idle()); f.release({27.5, 10.5});
        f.wand(); f.mode("Replace"); f.click({3.5, 10.5});
        f.refine(40);
        CHECK(f.window.grab().save(QDir(artifacts).filePath(QStringLiteral("smart-select-wand.png"))));
        nativeCapture(QStringLiteral("-wand"));
        f.refine(16); f.shortcut("Ctrl+Z"); f.shortcut("Ctrl+Shift+Z");
        QTest::qWait(150);
        const auto final = f.canvas->rendererStats();
        CHECK(final.fullUploads == baseline.fullUploads && final.regionalUploads == baseline.regionalUploads
            && final.uploadedBytes == baseline.uploadedBytes);
        CHECK(final.resourceGeneration == baseline.resourceGeneration && final.swapchainGeneration == baseline.swapchainGeneration);
        CHECK(f.surface().revision() == pixels && f.document().revision() == revision);
    }
    instance.destroy(); settle(); CHECK(warnings == 0 && errors == 0);
    std::cout << "Native Smart Select Vulkan validation: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SmartSelectionInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings; CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if(const auto i=application.arguments().indexOf("--private-quality");i>=0&&application.arguments().size()>i+2)
        return privateQuality(application.arguments()[i+1],application.arguments()[i+2]);
    if(application.arguments().contains(QStringLiteral("--large-reference"))){highResolutionResponsiveness();return failures?EXIT_FAILURE:EXIT_SUCCESS;}
    if (application.arguments().contains(QStringLiteral("--wayland-validation"))) return nativeValidation();
    controlsAndTypedShortcutOwnership();
    normalizedQuickDragCommitsOnlyOnReleaseAndKeepsArtworkClean();
    wandIsContiguousAndModifiersLatchWhileOriginalAntsRemain();
    toleranceRefinementsUseOriginalClickBaseline();
    cancellationAndNoopsPreserveHistoryAndRedo();
    undoAndRedoRestoreQuickHintsBeforeFurtherRefinement();
    referenceChangesInvalidateRetainedSamplesAndPendingResults();
    rapidCompletedGesturesCommitInOrderFromAColdReference();
    queuedHeldStrokeStartsLiveRefinementAfterPrecedingCommit();
    crossPanelReleaseAndSharedSelectionTransform();
    if (failures) std::cerr << failures << " Smart Select assertion(s) failed\n";
    else std::cout << "Smart Select interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
