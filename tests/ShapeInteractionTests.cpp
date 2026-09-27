#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ShapeCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <cmath>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures = 0;
void check(bool ok, const char* text, int line)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << line << ": " << text << '\n';
    }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
void events()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
struct Fixture {
    u::MainWindow window;
    r::CanvasWindow* canvas = nullptr;
    explicit Fixture(QVulkanInstance* instance)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1600, 960);
        window.show();
        events();
        for (auto* w : QGuiApplication::allWindows())
            if (w->objectName() == "VulkanCanvasWindow")
                canvas = dynamic_cast<r::CanvasWindow*>(w);
        CHECK(canvas);
        // Raw geometry regressions opt out; snapping has a dedicated exercise.
        window.findChild<QAction*>("SnappingAction")->setChecked(false);
        window.findChild<QAction*>("ToolAction_shape")->trigger();
        events();
        if (instance) {
            window.activateWindow();
            canvas->requestActivate();
            QTest::qWait(200);
        }
    }
    ~Fixture()
    {
        window.close();
        events();
    }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    const c::Layer& active() { return *session().document()->layer(*session().activeLayer()); }
    const c::ShapeLayer& shape() { return std::get<c::ShapeLayer>(active().payload); }
    QPointF logical(c::Vec2d p)
    {
        const auto& s = canvas->scene();
        auto e = s.document.canvas.extent;
        const auto q = s.viewport.documentToViewport(p, { double(e.width), double(e.height) }, { double(canvas->width()), double(canvas->height()) });
        return { q.x, q.y };
    }
    void mouse(QEvent::Type type, c::Vec2d p, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = { })
    {
        const auto point = logical(p);
        QMouseEvent e(type, point, point, QPointF(canvas->mapToGlobal(point.toPoint())), button, buttons, mods);
        QCoreApplication::sendEvent(canvas, &e);
    }
    void press(c::Vec2d p, Qt::KeyboardModifiers mods = { }) { mouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods); }
    void move(c::Vec2d p, Qt::KeyboardModifiers mods = { }) { mouse(QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, mods); }
    void release(c::Vec2d p, Qt::KeyboardModifiers mods = { })
    {
        mouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
        events();
    }
    void click(c::Vec2d p, Qt::KeyboardModifiers mods = { })
    {
        press(p, mods);
        release(p, mods);
    }
    void drag(c::Vec2d a, c::Vec2d b, Qt::KeyboardModifiers mods = { })
    {
        press(a, mods);
        move(b, mods);
        release(b, mods);
    }
    void key(int k, Qt::KeyboardModifiers mods = { })
    {
        QKeyEvent e(QEvent::KeyPress, k, mods);
        QCoreApplication::sendEvent(canvas, &e);
        events();
    }
    void mode(int i) { window.findChild<QAbstractButton*>(QStringLiteral("ShapeMode%1").arg(i))->click(); }
    void toggle(const char* name)
    {
        auto* b = window.findChild<QAbstractButton*>(QString::fromLatin1(name));
        CHECK(b);
        b->click();
        events();
    }
    void undo()
    {
        for (auto* a : window.actions())
            if (a->shortcut() == QKeySequence(QKeySequence::Undo)) {
                a->trigger();
                break;
            }
        events();
    }
    void redo()
    {
        for (auto* a : window.actions())
            if (a->shortcuts().contains(QKeySequence("Ctrl+Shift+Z"))) {
                a->trigger();
                break;
            }
        events();
    }
};
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

void creationAndHistory(QVulkanInstance* instance)
{
    Fixture f(instance);
    auto& session = f.session();
    const auto startCount = session.document()->layers().size();
    const auto originalId = session.activeLayer();
    const auto history = session.history().undoDepth();
    f.press({ 80.25, 80.5 });
    f.move({ 280.75, 180.75 });
    CHECK(session.document()->layers().size() == startCount);
    QElapsedTimer previewWait;
    previewWait.start();
    while (f.canvas->scene().document.layersBottomToTop.size() != startCount + 1 && previewWait.elapsed() < 1000)
        QTest::qWait(10);
    CHECK(f.canvas->scene().document.layersBottomToTop.size() == startCount + 1);
    f.release({ 280.75, 180.75 });
    CHECK(session.document()->layers().size() == startCount + 1);
    CHECK(session.history().undoDepth() == history + 1);
    CHECK(f.shape().kind == c::ShapeKind::Rectangle);
    CHECK(near(f.shape().size.width, 200.5));
    CHECK(near(f.shape().size.height, 100.25));
    CHECK(f.shape().fillColor == session.foregroundColor());
    CHECK(f.canvas->scene().transformOverlay.has_value());
    const auto id = f.active().id;
    const auto createdShape = f.shape();
    f.undo();
    CHECK(session.activeLayer() == originalId);
    CHECK(!session.document()->containsLayer(id));
    const auto redoDepth = session.history().redoDepth();
    // Degenerate after a real preview must not resurrect the old geometry.
    f.press({ 600, 80 });
    f.move({ 730, 170 });
    QTest::qWait(25);
    f.move({ 600, 80 });
    f.release({ 600, 80 });
    CHECK(session.document()->layers().size() == startCount);
    CHECK(session.history().redoDepth() == redoDepth);
    f.redo();
    CHECK(session.activeLayer() == id);
    CHECK(f.shape() == createdShape);
    const auto cache = f.active().renderCache; // Undoing creation releases disposable cache data.
    // A move preserves content. Viewport mapping may leave a tiny fractional
    // translation, which must invalidate a final document-grid image.
    const auto transform = f.active().localToDocument;
    const auto beforeMove = session.history().undoDepth();
    f.drag({ 170, 130 }, { 205, 150 });
    CHECK(session.history().undoDepth() == beforeMove + 1);
    CHECK(f.active().renderCache->contentRevision == cache->contentRevision);
    CHECK(!f.active().renderCache->rasterizedDocumentTransform
        || f.active().renderCache->rasterizedDocumentTransform == f.active().localToDocument);
    CHECK(near(f.active().localToDocument.m02, transform.m02 + 35));
    f.undo();
    CHECK(f.active().localToDocument == transform);
    f.redo();
    const auto moved = f.active().localToDocument;
    f.press({ 200, 150 });
    f.move({ 240, 190 });
    f.key(Qt::Key_Escape);
    f.release({ 240, 190 });
    CHECK(f.active().localToDocument == moved);
    // Force creation on a body and release Shift mid-drag: purpose is latched.
    const auto count = session.document()->layers().size();
    f.press({ 200, 150 }, Qt::ShiftModifier);
    f.move({ 240, 170 });
    f.release({ 240, 170 });
    CHECK(session.document()->layers().size() == count + 1);
    CHECK(near(f.shape().size.width, 40));
    CHECK(near(f.shape().size.height, 20));
    // Shift both forces creation at press and constrains while held.
    CHECK(!f.window.findChild<QAbstractButton*>("ShapeAspectLock"));
    f.mode(2);
    f.drag({ 600, 250 }, { 720, 320 }, Qt::ShiftModifier);
    CHECK(f.shape().kind == c::ShapeKind::Ellipse);
    CHECK(near(f.shape().size.width, 120) && near(f.shape().size.height, 120));
    // Shape mode moves through the whole box, including an empty ellipse corner.
    const auto ellipses = session.document()->layers().size();
    const auto ellipseTransform = f.active().localToDocument;
    f.drag({ 610, 260 }, { 630, 290 });
    CHECK(session.document()->layers().size() == ellipses);
    CHECK(near(f.active().localToDocument.m02, ellipseTransform.m02 + 20));
    CHECK(near(f.active().localToDocument.m12, ellipseTransform.m12 + 30));
    f.undo();
    CHECK(f.active().localToDocument == ellipseTransform);
    // Escape owns pending construction, even if Shift is still held.
    const auto beforeCancel = session.document()->layers().size();
    f.press({ 800, 80 }, Qt::ShiftModifier);
    f.move({ 900, 150 });
    f.key(Qt::Key_Escape, Qt::ShiftModifier);
    f.release({ 900, 150 });
    CHECK(session.document()->layers().size() == beforeCancel);
}

void stylesPolygonsAndTransforms(QVulkanInstance* instance)
{
    Fixture f(instance);
    auto& session = f.session();
    f.mode(1);
    f.drag({ 100, 100 }, { 400, 280 });
    const auto id = f.active().id;
    auto* radius = dynamic_cast<u::ToolOptionsNumber*>(f.window.findChild<QDoubleSpinBox*>("ShapeCornerRadius"));
    const auto depth = session.history().undoDepth();
    radius->setValue(36);
    CHECK(near(f.shape().cornerRadius, 36));
    CHECK(session.history().undoDepth() == depth + 1);
    f.undo();
    CHECK(near(f.shape().cornerRadius, 12));
    f.redo();
    // A collapsed intrinsic frame remains editable and can recover its handles.
    auto* localWidth = f.window.findChild<QDoubleSpinBox*>("ShapeGeometryWidth");
    const auto geometryBefore = f.shape();
    const auto geometryDepth = session.history().undoDepth();
    localWidth->setValue(0);
    CHECK(near(f.shape().size.width, 0));
    CHECK(session.history().undoDepth() == geometryDepth + 1);
    localWidth->setValue(geometryBefore.size.width);
    CHECK(f.shape() == geometryBefore);
    CHECK(f.canvas->scene().transformOverlay.has_value());
    f.undo();
    CHECK(near(f.shape().size.width, 0));
    f.undo();
    CHECK(f.shape() == geometryBefore);
    // Continuous numeric steps are one command, not one per valueChanged.
    auto* width = dynamic_cast<u::ToolOptionsNumber*>(f.window.findChild<QDoubleSpinBox*>("ShapeStrokeWidth"));
    f.toggle("ShapeStrokeEnabled");
    const auto beforeSteps = session.history().undoDepth();
    const double widthBefore = f.shape().strokeWidth;
    QStyleOptionSpinBox option;
    option.initFrom(width);
    option.rect = width->rect();
    option.subControls = QStyle::SC_All;
    option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    const auto step = width->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, width).center();
    QTest::mousePress(width, Qt::LeftButton, Qt::NoModifier, step);
    width->stepUp();
    width->stepUp();
    QTest::mouseRelease(width, Qt::LeftButton, Qt::NoModifier, step);
    events();
    CHECK(session.history().undoDepth() == beforeSteps + 1);
    CHECK(f.shape().strokeWidth > widthBefore);
    f.undo();
    CHECK(near(f.shape().strokeWidth, widthBefore));
    // Use an exactly representable mapping for the integer-translation reuse
    // invariant. Fit zoom can turn 20px into 19.99999999999997 on another compiler.
    f.canvas->resetTo100Percent();
    events();
    f.toggle("ShapeFillEnabled");
    CHECK(!f.shape().fillEnabled);
    const auto hollowCache = f.active().renderCache;
    const auto hollowTransform = f.active().localToDocument;
    f.drag({ 240, 190 }, { 260, 210 });
    CHECK(f.active().localToDocument.m02 - hollowTransform.m02 == 20);
    CHECK(f.active().localToDocument.m12 - hollowTransform.m12 == 20);
    CHECK(f.active().id == id);
    CHECK(f.active().renderCache->surface == hollowCache->surface);
    // Passive handle resize is immediately undoable and never enters Ctrl+T.
    const auto beforeScale = f.active().localToDocument;
    const auto beforeGeometry = f.shape();
    const auto cacheBeforeResize = f.active().renderCache;
    const auto handles = c::geometryTransformHandles(beforeScale, f.shape().size);
    f.drag(handles[4], handles[4] + c::Vec2d { 70, 40 });
    CHECK(session.activeTool() == c::ToolId::Shape);
    CHECK(f.active().localToDocument == beforeScale);
    CHECK(near(f.shape().size.width, beforeGeometry.size.width + 70));
    CHECK(near(f.shape().size.height, beforeGeometry.size.height + 40));
    CHECK(f.shape().strokeWidth == beforeGeometry.strokeWidth);
    CHECK(f.shape().cornerRadius == beforeGeometry.cornerRadius);
    CHECK(f.active().renderCache != cacheBeforeResize);
    CHECK(near(localWidth->value(), f.shape().size.width));
    f.undo();
    CHECK(f.active().localToDocument == beforeScale);
    CHECK(f.shape() == beforeGeometry);
    // Shift on an actual handle must scale, not create another shape.
    const auto n = session.document()->layers().size();
    f.drag(handles[4], handles[4] + c::Vec2d { 40, 30 }, Qt::ShiftModifier);
    CHECK(session.document()->layers().size() == n);
    f.undo();
    // Explicit transform is still a deliberately cancellable session.
    f.window.findChild<QAction*>("LayerTransformAction")->trigger();
    events();
    CHECK(session.activeTool() == c::ToolId::Transform);
    auto* angle = f.window.findChild<QDoubleSpinBox*>("TransformAngleControl");
    angle->setValue(32);
    f.key(Qt::Key_Escape);
    CHECK(session.activeTool() == c::ToolId::Shape);
    CHECK(f.active().localToDocument == beforeScale);
    CHECK(f.canvas->scene().transformOverlay.has_value());
    // Click vertices. Hover does not insert an extra endpoint on Enter.
    f.mode(5);
    f.click({ 650.125, 100 }, Qt::ShiftModifier);
    f.click({ 830.125, 120 }, Qt::ShiftModifier);
    f.click({ 750.125, 280 }, Qt::ShiftModifier);
    f.move({ 860, 260 });
    const auto polygonDepth = session.history().undoDepth();
    f.key(Qt::Key_Return, Qt::ShiftModifier);
    CHECK(f.shape().kind == c::ShapeKind::Polygon);
    CHECK(f.shape().points.size() == 3);
    CHECK(session.history().undoDepth() == polygonDepth + 1);
    CHECK(near(f.shape().size.width, 180));
    auto* x = f.window.findChild<QDoubleSpinBox*>("ShapeVertexX");
    const auto original = f.shape();
    x->setValue(30);
    CHECK(near(f.shape().points[0].x, 30));
    f.undo();
    CHECK(f.shape() == original);
    const auto originalTransform = f.active().localToDocument;
    localWidth->setValue(360);
    CHECK(near(f.shape().size.width, 360));
    CHECK(near(f.shape().points[1].x, original.points[1].x * 2));
    CHECK(f.active().localToDocument == originalTransform);
    x->setValue(300);
    CHECK(near(f.shape().points[0].x, 300));
    f.undo();
    f.undo();
    CHECK(f.shape() == original);
    // Backspace and cancellation retain the old layer/history/redo branch.
    const auto keep = session.history().redoDepth();
    f.click({ 950, 120 }, Qt::ShiftModifier);
    f.click({ 1080, 150 });
    f.key(Qt::Key_Backspace, Qt::ShiftModifier);
    f.key(Qt::Key_Escape, Qt::ShiftModifier);
    CHECK(session.history().redoDepth() == keep);
    // Each mode remains separate, with subpixel geometry and no raster layer mutation.
    for (int mode : { 0, 1, 2, 3, 4 }) {
        f.mode(mode);
        const auto count = session.document()->layers().size();
        f.drag({ 80 + mode * 160.0, 450.25 }, { 170 + mode * 160.0, 550.75 }, Qt::ShiftModifier);
        CHECK(session.document()->layers().size() == count + 1);
        CHECK(static_cast<int>(f.shape().kind) == mode);
    }
    // A degenerate press creates nothing, including a line with no length.
    const auto count = session.document()->layers().size();
    f.click({ 1000, 600 }, Qt::ShiftModifier);
    CHECK(session.document()->layers().size() == count);
    // Shape tool hides its handles on tool change and restores without history.
    const auto history = session.history().undoDepth();
    f.window.findChild<QAction*>("ToolAction_brush")->trigger();
    events();
    CHECK(!f.canvas->scene().transformOverlay);
    f.window.findChild<QAction*>("ToolAction_shape")->trigger();
    events();
    CHECK(f.canvas->scene().transformOverlay);
    CHECK(session.history().undoDepth() == history);
    if (instance) {
        QTest::qWait(300);
        const auto uploads = f.canvas->rendererStats().fullUploads;
        const auto content = f.active().renderCache;
        for (int i = 0; i < 10; ++i) {
            f.mouse(QEvent::MouseMove, { 1100.0 + i, 650 }, Qt::NoButton, Qt::NoButton);
            f.canvas->scheduleFrame();
            QTest::qWait(20);
        }
        CHECK(f.active().renderCache == content);
        CHECK(f.canvas->rendererStats().fullUploads == uploads);
        const auto path = qEnvironmentVariable("IMAGEEDITOR_SHAPE_SCREENSHOT");
        if (!path.isEmpty()) {
            QProcess capture;
            capture.start("spectacle", { "--background", "--nonotify", "--activewindow", "--output", path });
            CHECK(capture.waitForFinished(5000));
            CHECK(capture.exitCode() == 0);
        }
    }
}
void creationModifiersAndMode(QVulkanInstance* instance)
{
    Fixture f(instance);
    f.mode(1);
    f.drag({ 100, 100 }, { 260, 210 });
    f.mode(2);
    f.drag({ 500, 100 }, { 600, 170 });
    const auto ellipse = f.active().id;
    f.undo();
    CHECK(f.session().activeTool() == c::ToolId::Shape);
    CHECK(f.window.findChild<QAbstractButton*>("ShapeMode2")->isChecked());
    CHECK(f.shape().kind == c::ShapeKind::RoundedRectangle);
    f.redo();
    CHECK(f.active().id == ellipse);
    CHECK(f.window.findChild<QAbstractButton*>("ShapeMode2")->isChecked());
    // Force-create purpose remains latched even when constraint is released.
    const auto draftCount = f.session().document()->layers().size() + 1;
    f.press({ 150, 150 }, Qt::ShiftModifier);
    f.move({ 210, 170 }, Qt::ShiftModifier);
    QElapsedTimer preview;
    preview.start();
    while (f.canvas->scene().document.layersBottomToTop.size() != draftCount && preview.elapsed() < 1000)
        QTest::qWait(10);
    CHECK(f.canvas->scene().document.layersBottomToTop.size() == draftCount);
    auto draft = std::get<c::ShapeLayer>(f.canvas->scene().document.layersBottomToTop.back().payload);
    CHECK(near(draft.size.width, draft.size.height));
    f.move({ 210, 170 });
    f.release({ 210, 170 });
    CHECK(near(f.shape().size.width, 60) && near(f.shape().size.height, 20));
    // Cardinal and diagonal line directions all remain available under Shift.
    f.mode(4);
    for (auto delta : { c::Vec2d { 100, 4 }, { 4, 100 }, { -100, 4 }, { 4, -100 }, { 90, 86 }, { -90, 86 } }) {
        const c::Vec2d start { 800, 450 };
        f.drag(start, start + delta, Qt::ShiftModifier);
        CHECK(f.shape().kind == c::ShapeKind::Line);
        const auto d = f.shape().points[1] - f.shape().points[0];
        CHECK(near(d.x, 0) || near(d.y, 0) || near(std::abs(d.x), std::abs(d.y)));
        CHECK(d.x * delta.x + d.y * delta.y > 0);
        f.undo();
        CHECK(f.window.findChild<QAbstractButton*>("ShapeMode4")->isChecked());
    }
    f.drag({ 800, 600 }, { 900, 603 }, Qt::ShiftModifier);
    CHECK(near(f.shape().size.height, 0));
    const auto line = f.shape();
    const auto handles = f.canvas->scene().transformOverlay->handles();
    f.drag(handles[0], handles[0] + c::Vec2d { -25, -60 });
    CHECK(near(f.shape().size.height, 60));
    CHECK(near(std::abs(f.shape().points[1].y - f.shape().points[0].y), 60));
    CHECK(f.shape().strokeWidth == line.strokeWidth);
    f.undo();
    CHECK(f.shape() == line);
    f.window.findChild<QDoubleSpinBox*>("ShapeGeometryHeight")->setValue(42);
    CHECK(near(f.shape().points[1].y, 42));
    f.undo();
    CHECK(f.shape() == line);
}

void resizedGeometryInteraction(QVulkanInstance* instance)
{
    Fixture f(instance);
    f.mode(1);
    f.drag({ 260.25, 200.5 }, { 360.75, 300.75 });
    f.toggle("ShapeStrokeEnabled");
    f.window.findChild<QDoubleSpinBox*>("ShapeStrokeWidth")->setValue(4);
    const auto id = f.active().id;
    const auto original = f.shape();
    const auto originalMatrix = f.active().localToDocument;
    auto handles = f.canvas->scene().transformOverlay->handles();
    const auto history = f.session().history().undoDepth();
    f.press(handles[5]);
    f.move(handles[5] + c::Vec2d { 0, 200 });
    CHECK(near(f.shape().size.height, original.size.height + 200));
    // A second scene producer cannot drop the outline during the coalescing
    // interval, nor stretch its old pixels into the new local dimensions.
    f.canvas->setDocument(f.session().document()->snapshot(), false);
    const auto& pending = f.canvas->scene().document.layersBottomToTop.back();
    CHECK(pending.renderCache);
    CHECK(std::get<c::ShapeLayer>(pending.payload) == original);
    CHECK(pending.localToDocument == originalMatrix);
    CHECK(f.session().history().undoDepth() == history);
    CHECK(near(f.window.findChild<QDoubleSpinBox*>("ShapeGeometryHeight")->value(), f.shape().size.height));
    f.release(handles[5] + c::Vec2d { 0, 200 });
    CHECK(f.session().history().undoDepth() == history + 1);
    CHECK(f.active().localToDocument == originalMatrix);
    CHECK(f.shape().strokeWidth == 4);
    f.undo();
    CHECK(f.shape() == original);
    CHECK(f.active().localToDocument == originalMatrix);
    const auto redo = f.session().history().redoDepth();
    // Exact fractional frame, opposite anchor, no accumulated preview scaling.
    handles = f.canvas->scene().transformOverlay->handles();
    f.press(handles[0]);
    f.move(handles[0] - c::Vec2d { 50, 80 });
    auto grown = f.canvas->scene().transformOverlay->handles();
    CHECK(near(grown[4].x, handles[4].x) && near(grown[4].y, handles[4].y));
    f.move(handles[0]);
    f.release(handles[0]);
    CHECK(f.shape() == original);
    CHECK(f.active().localToDocument == originalMatrix);
    CHECK(f.session().history().redoDepth() == redo);
    f.press(handles[0]);
    f.move(handles[0] - c::Vec2d { 40, 20 });
    f.key(Qt::Key_Escape);
    f.release(handles[0] - c::Vec2d { 40, 20 });
    CHECK(f.shape() == original);
    CHECK(f.active().localToDocument == originalMatrix);
    CHECK(f.session().history().redoDepth() == redo);
    // Ctrl+T is intentionally still whole-layer scaling, including its stroke.
    f.window.findChild<QAction*>("LayerTransformAction")->trigger();
    events();
    f.window.findChild<QDoubleSpinBox*>("TransformScaleXControl")->setValue(-150);
    f.window.findChild<QDoubleSpinBox*>("TransformScaleYControl")->setValue(220);
    f.window.findChild<QDoubleSpinBox*>("TransformAngleControl")->setValue(27);
    f.key(Qt::Key_Return);
    CHECK(f.shape() == original);
    const auto baseline = f.active().localToDocument;
    CHECK(baseline != originalMatrix);
    handles = f.canvas->scene().transformOverlay->handles();
    const auto localDelta = c::Vec2d { 0, 60 };
    const auto delta = baseline.map(localDelta) - baseline.map({ });
    f.drag(handles[5], handles[5] + delta);
    CHECK(f.active().id == id);
    CHECK(near(f.shape().size.height, original.size.height + 60));
    CHECK(f.active().localToDocument == baseline);
    CHECK(f.shape().strokeWidth == 4 && f.shape().cornerRadius == original.cornerRadius);
    f.undo();
    CHECK(f.shape() == original);
    CHECK(f.active().localToDocument == baseline);
    f.redo();
    CHECK(near(f.shape().size.height, original.size.height + 60));
    // Collapse and cross through zero without a singular matrix/losing handles.
    const auto beforeFlip = f.shape();
    handles = f.canvas->scene().transformOverlay->handles();
    const auto collapse = baseline.map({ 0, 0 }) - baseline.map({ beforeFlip.size.width, 0 });
    f.press(handles[3]);
    f.move(handles[3] + collapse);
    CHECK(near(f.shape().size.width, 0));
    CHECK(f.active().localToDocument.inverted());
    CHECK(f.canvas->scene().transformOverlay);
    f.move(handles[3] + collapse * 1.5);
    f.release(handles[3] + collapse * 1.5);
    CHECK(near(f.shape().size.width, beforeFlip.size.width * .5));
    CHECK(near(f.active().localToDocument.m00, -baseline.m00));
    f.undo();
    CHECK(f.shape() == beforeFlip);
    CHECK(f.active().localToDocument == baseline);
}
void shapeModeBoundsTargeting(QVulkanInstance* instance)
{
    Fixture f(instance);
    f.mode(2);
    f.drag({ 300, 150 }, { 500, 350 });
    f.window.findChild<QAction*>("LayerTransformAction")->trigger();
    events();
    f.window.findChild<QDoubleSpinBox*>("TransformScaleXControl")->setValue(-150);
    f.window.findChild<QDoubleSpinBox*>("TransformScaleYControl")->setValue(150);
    f.window.findChild<QDoubleSpinBox*>("TransformAngleControl")->setValue(27);
    f.key(Qt::Key_Return);
    const auto id = f.active().id;
    const auto matrix = f.active().localToDocument;
    const auto geometry = f.shape();
    // Inside the rotated/flipped local frame, but well outside the ellipse.
    const auto corner = matrix.map({ 20, 20 });
    CHECK(f.canvas->onShapeHit(corner, true));
    CHECK(!f.canvas->onShapeHit(matrix.map({ -20, 100 }), true));
    f.mode(3);
    f.drag({ 750, 450 }, { 850, 550 }, Qt::ShiftModifier);
    const auto count = f.session().document()->layers().size();
    const auto history = f.session().history().undoDepth();
    f.click(corner);
    CHECK(f.active().id == id);
    CHECK(f.session().history().undoDepth() == history);
    const auto cache = f.active().renderCache;
    const auto destination = corner + c::Vec2d { 23, 15 };
    f.drag(corner, destination);
    CHECK(f.session().document()->layers().size() == count);
    CHECK(f.session().history().undoDepth() == history + 1);
    CHECK(f.shape() == geometry);
    CHECK(f.active().renderCache->contentRevision == cache->contentRevision);
    CHECK(!f.active().renderCache->rasterizedDocumentTransform
        || f.active().renderCache->rasterizedDocumentTransform == f.active().localToDocument);
    CHECK(near(f.active().localToDocument.m02, matrix.m02 + 23));
    CHECK(near(f.active().localToDocument.m12, matrix.m12 + 15));
    f.undo();
    CHECK(f.active().localToDocument == matrix);
    // Ordinary Move must still ignore the empty corner, not move this layer.
    f.window.findChild<QAction*>("ToolAction_move")->trigger();
    events();
    f.drag(corner, destination);
    CHECK(f.session().document()->layer(id)->localToDocument == matrix);
    CHECK(f.session().history().undoDepth() == history);
    f.window.findChild<QAction*>("ToolAction_shape")->trigger();
    events();
    f.drag(corner, destination, Qt::ShiftModifier);
    CHECK(f.session().document()->layers().size() == count + 1);
    CHECK(f.session().document()->layer(id)->localToDocument == matrix);
}

void shapeMovementSnappingAndVulkanGuides(QVulkanInstance* instance)
{
    Fixture f(instance);
    f.drag({20,30},{120,110});
    const auto id=*f.session().activeLayer();
    const auto initial=f.active().localToDocument;
    const auto geometry=f.shape();
    const auto depth=f.session().history().undoDepth();
    f.window.findChild<QAction*>("SnappingAction")->setChecked(true);
    f.window.findChild<QAction*>("SnapLayersAction")->setChecked(false);
    f.press({70,70}); f.move({51,43});
    CHECK(near(f.active().localToDocument.m02,0) && near(f.active().localToDocument.m12,0));
    CHECK(f.shape()==geometry && f.canvas->scene().snapGuides[0] && f.canvas->scene().snapGuides[1]);
    if(instance) {
        QTest::qWait(200);
        const auto before=f.canvas->rendererStats();
        const auto guides=f.canvas->scene().snapGuides;
        const auto cache=f.active().renderCache;
        f.canvas->setSnapGuides({}); QTest::qWait(70);
        f.canvas->setSnapGuides(guides); QTest::qWait(70);
        const auto after=f.canvas->rendererStats();
        CHECK(after.framesSubmitted>before.framesSubmitted);
        CHECK(after.uploadedBytes==before.uploadedBytes && after.fullUploads==before.fullUploads);
        CHECK(after.compositionPasses==before.compositionPasses && f.active().renderCache==cache);
        QTest::qWait(100);
        CHECK(f.canvas->rendererStats().framesSubmitted==after.framesSubmitted);
        std::cout<<"Snap guides: Vulkan frames, no image upload/recomposition, no idle frames\n";
    }
    f.release({51,43});
    CHECK(!f.canvas->scene().snapGuides[0] && f.session().activeLayer()==id);
    CHECK(f.session().history().undoDepth()==depth+1);
    const auto final=f.active().localToDocument;
    f.undo(); CHECK(f.active().localToDocument==initial);
    f.redo(); CHECK(f.active().localToDocument==final);
    // Re-entering an active shape still uses the same solver and Escape path.
    f.press({50,40}); f.move({54,44}); CHECK(f.canvas->scene().snapGuides[0]);
    f.key(Qt::Key_Escape); CHECK(!f.canvas->scene().snapGuides[0]);
    CHECK(f.active().localToDocument==final);
    f.release({54,44});
}

void defaultStyleModeSwitch(QVulkanInstance* instance)
{
    Fixture f(instance);
    // Mode-specific vertex placeholders must never leak into primitive defaults.
    for (int mode : { 4, 5 }) {
        f.mode(mode);
        f.window.findChild<QDoubleSpinBox*>("ShapeStrokeWidth")->setValue(7 + mode);
        f.mode(0);
        auto* fill = f.window.findChild<QAbstractButton*>("ShapeFillEnabled");
        fill->setChecked(true);
        f.toggle("ShapeFillEnabled");
        CHECK(!fill->isChecked());
        f.toggle("ShapeFillEnabled");
        CHECK(fill->isChecked());
    }
    const auto before = f.session().history().undoDepth();
    f.drag({ 100, 100 }, { 200, 170 });
    CHECK(f.shape().fillEnabled);
    CHECK(near(f.shape().strokeWidth, 12));
    CHECK(f.shape().points.empty());
    CHECK(f.session().history().undoDepth() == before + 1);
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("ImageEditorTests");
    app.setApplicationName("ShapeInteractions");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    QVulkanInstance instance;
    int warnings = 0, errors = 0;
    const bool native = QGuiApplication::platformName() == "wayland";
    if (native) {
        instance.setApiVersion(QVersionNumber(1, 2));
        instance.setLayers({ "VK_LAYER_KHRONOS_validation" });
        instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags, QVulkanInstance::DebugMessageTypeFlags types, const void* message) {
            if (!types.testFlag(QVulkanInstance::ValidationMessage))
                return false;
            if (flags.testFlag(QVulkanInstance::ErrorSeverity))
                ++errors;
            else if (flags.testFlag(QVulkanInstance::WarningSeverity))
                ++warnings;
            else
                return false;
            const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
            std::cerr << (data ? data->pMessage : "Validation") << '\n';
            return false;
        });
        if (!instance.create())
            return 1;
    }
    creationAndHistory(native ? &instance : nullptr);
    stylesPolygonsAndTransforms(native ? &instance : nullptr);
    defaultStyleModeSwitch(native ? &instance : nullptr);
    creationModifiersAndMode(native ? &instance : nullptr);
    resizedGeometryInteraction(native ? &instance : nullptr);
    shapeModeBoundsTargeting(native ? &instance : nullptr);
    shapeMovementSnappingAndVulkanGuides(native ? &instance : nullptr);
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Shape interaction failures: " << failures << "; Vulkan warnings=" << warnings << ", errors=" << errors << '\n';
    return failures ? 1 : 0;
}
