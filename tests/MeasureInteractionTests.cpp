#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/RulerStrip.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabletEvent>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVulkanInstance>

#include <array>
#include <atomic>
#include <cmath>
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
bool near(double a, double b, double tolerance = 1e-6)
{ return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance; }
bool nearPoint(core::Vec2d a, core::Vec2d b, double tolerance = 1e-6)
{ return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance); }
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
void key(QObject* target, QEvent::Type type, int code, QString text = {}, bool repeat = false,
    Qt::KeyboardModifiers modifiers = {})
{
    QKeyEvent event(type, code, modifiers, text, repeat);
    QCoreApplication::sendEvent(target, &event);
    settle();
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::OverlayDockWorkspace* workspace {};
    ui::CrossWindowPointerRouter* router {};
    explicit Fixture(QVulkanInstance* instance = nullptr, bool persist = false) : window(instance, persist, false)
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
        QImage fixture(128, 96, QImage::Format_RGBA8888);
        fixture.fill(QColor(72, 140, 218, 255));
        const auto path = assets.filePath(QStringLiteral("measure-source.png"));
        CHECK(fixture.save(path)); CHECK(window.openImageFromPath(path));
        action("ToolAction_measure"); settle();
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return canvas && workspace && router && window.editorSession().document(); }
    core::EditorSession& session() { return const_cast<core::EditorSession&>(window.editorSession()); }
    core::Document& document() { return *session().document(); }
    const core::RasterSurface& surface()
    { return *std::get<core::RasterLayer>(document().layer(*session().activeLayer())->payload).surface; }
    template<class T> T* find(const char* name)
    {
        auto* result = window.findChild<T*>(QString::fromLatin1(name));
        CHECK(result); return result;
    }
    void action(const char* name)
    { if (auto* target = find<QAction>(name)) target->trigger(); settle(); }
    QPointF logical(core::Vec2d point) const
    {
        const auto extent = window.editorSession().document()->canvas().extent;
        const auto& scene = canvas->scene();
        const auto mapped = scene.viewport.documentToViewport(point,
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return {mapped.x, mapped.y};
    }
    void press(QPointF point)
    {
        mouse(*canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        mouse(*canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    }
    void move(QPointF point) { mouse(*canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton); }
    void release(QPointF point) { mouse(*canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton); settle(); }
    void drag(core::Vec2d a, core::Vec2d b) { press(logical(a)); move(logical(b)); release(logical(b)); }
    void escape() { key(canvas, QEvent::KeyPress, Qt::Key_Escape); }
    void r(bool held, bool repeat = false)
    { key(canvas, held ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_R, QStringLiteral("r"), repeat); }
};

void measurementsAreDocumentSpaceViewState()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    auto* measure = f.find<QAction>("ToolAction_measure");
    CHECK(measure && measure->shortcut() == QKeySequence(QStringLiteral("Shift+R")));
    CHECK(f.canvas->measureActive() && !f.canvas->temporaryMeasureActive());
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(*f.session().activeLayer(), .75F)));
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(*f.session().activeLayer(), .5F)));
    CHECK(f.session().undo());
    f.canvas->setDocument(f.document().snapshot(), false);
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto history = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    f.document().markSaved();
    const core::Vec2d a {-10.25, 8.125}, b {19.75, 48.125};
    for (const bool oneToOne : {false, true}) {
        if (oneToOne) f.canvas->resetTo100Percent();
        f.canvas->clearMeasurement();
        f.press(f.logical(a)); f.move(f.logical(b));
        CHECK(f.canvas->measureDragging());
        CHECK(f.canvas->scene().measurement);
        if (const auto line = f.canvas->scene().measurement) {
            CHECK(nearPoint(line->a, a) && nearPoint(line->b, b));
            const auto values = core::measurementValues(*line);
            CHECK(values && near(values->distance, 50) && values->angleDegrees
                && near(*values->angleDegrees, 53.13010235415598));
        }
        CHECK(f.canvas->scene().pointerTooltip.find("50.0 px") != std::string::npos);
        CHECK(f.canvas->scene().pointerTooltip.find("53.1") != std::string::npos);
        f.release(f.logical(b));
        CHECK(!f.canvas->measureDragging() && f.canvas->scene().measurement);
        const auto line = f.canvas->scene().measurement;
        const QPointF panStart(600, 450), panEnd = panStart + QPointF(37.5, -21.25);
        mouse(*f.canvas, QEvent::MouseButtonPress, panStart, Qt::MiddleButton, Qt::MiddleButton);
        mouse(*f.canvas, QEvent::MouseMove, panEnd, Qt::NoButton, Qt::MiddleButton);
        mouse(*f.canvas, QEvent::MouseButtonRelease, panEnd, Qt::MiddleButton, Qt::NoButton);
        CHECK(f.canvas->scene().measurement == line);
        f.action("ToolAction_brush"); CHECK(!f.canvas->scene().measurement);
        f.action("ToolAction_measure"); CHECK(f.canvas->scene().measurement == line);
    }
    f.canvas->clearMeasurement();
    f.press(f.logical({10.5, 12.75})); f.release(f.logical({10.5, 12.75}));
    CHECK(f.canvas->scene().measurement);
    if (const auto line = f.canvas->scene().measurement) {
        const auto value = core::measurementValues(*line);
        CHECK(value && value->distance == 0 && !value->angleDegrees);
    }
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels && !f.document().isModified());
    CHECK(f.session().history().undoDepth() == history && f.session().history().redoDepth() == redo);
}

void endpointAdjustmentCancellationAndCrossPanelCapture()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.drag({15, 20}, {75, 60});
    f.press(f.logical({15, 20})); f.move(f.logical({5.5, 8.5})); f.release(f.logical({5.5, 8.5}));
    const auto retained = f.canvas->scene().measurement;
    CHECK(retained);
    if (retained) CHECK(nearPoint(retained->a, {5.5, 8.5}) && nearPoint(retained->b, {75, 60}));
    f.press(f.logical({75, 60})); f.move(f.logical({93, 76})); f.escape();
    CHECK(!f.canvas->measureDragging() && f.canvas->scene().measurement == retained);
    f.release(f.logical({93, 76})); CHECK(f.canvas->scene().measurement == retained);
    QWidget recipient(f.workspace->panelOverlay());
    recipient.setGeometry(15, 80, 160, 100); recipient.show();
    f.press(f.logical({75, 60}));
    const auto end = f.logical({102, 84});
    const auto global = f.canvas->mapToGlobal(end.toPoint());
    const auto routed = f.router->routedEventCount();
    mouse(recipient, QEvent::MouseMove, QPointF(recipient.mapFromGlobal(global)), Qt::NoButton, Qt::LeftButton);
    CHECK(f.router->routedEventCount() > routed && f.canvas->measureDragging());
    mouse(recipient, QEvent::MouseButtonRelease, QPointF(recipient.mapFromGlobal(global)), Qt::LeftButton, Qt::NoButton);
    CHECK(!f.canvas->measureDragging());
    if (const auto line = f.canvas->scene().measurement) CHECK(nearPoint(line->b, {102, 84}, 2 / f.canvas->zoom()));
    else CHECK(false);
    const auto committed = f.canvas->scene().measurement;
    f.press(f.logical({102, 84})); f.move(f.logical({115, 91}));
    QFocusEvent focus(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(f.canvas, &focus); settle();
    CHECK(!f.canvas->measureDragging() && f.canvas->scene().measurement == committed);
    f.release(f.logical({115, 91}));
}

void measureShiftSnapsWithoutPromotingTemporaryAccess()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    const auto history = f.session().history().undoDepth();
    const core::Vec2d a {10.25, 20.5}, b {50.25, 40.5};
    const auto shift = [&](bool held) {
        key(f.canvas, held ? QEvent::KeyPress : QEvent::KeyRelease,
            Qt::Key_Shift, {}, false, held ? Qt::ShiftModifier : Qt::NoModifier);
    };
    for (const bool oneToOne : {false, true}) {
        if (oneToOne) f.canvas->resetTo100Percent();
        f.action("ToolAction_move"); f.r(true);
        f.press(f.logical(a)); f.move(f.logical(b));
        shift(true);
        CHECK(f.canvas->temporaryMeasureActive() && f.canvas->measureDragging());
        CHECK(f.session().activeTool() == core::ToolId::Move);
        const auto snapped = core::constrainLineEndpoint(a, b);
        CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->b, snapped));
        CHECK(f.canvas->scene().pointerTooltip.find("45.0°") != std::string::npos);
        // Qt probes Shift+R on repeated R events after Shift goes down. Both
        // the shortcut probe and repeat must remain owned by the initial R.
        QKeyEvent probe(QEvent::ShortcutOverride, Qt::Key_R, Qt::ShiftModifier, "R", true);
        probe.setAccepted(false); QCoreApplication::sendEvent(f.canvas, &probe);
        CHECK(probe.isAccepted());
        key(f.canvas, QEvent::KeyPress, Qt::Key_R, "R", true, Qt::ShiftModifier);
        CHECK(f.canvas->measureDragging() && f.canvas->temporaryMeasureActive());
        CHECK(f.session().activeTool() == core::ToolId::Move);
        shift(false);
        CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->b, b));
        shift(true);
        CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->b, snapped));
        key(f.canvas, QEvent::KeyRelease, Qt::Key_R, "R", false, Qt::ShiftModifier);
        CHECK(!f.canvas->measureActive() && !f.canvas->measureDragging());
        f.release(f.logical(b)); shift(false);
        CHECK(f.session().activeTool() == core::ToolId::Move);
    }
    // Permanent Measure, already-held Shift, final mouse-up, and adjustment
    // of A all use the same fixed opposite endpoint and document-space math.
    f.action("ToolAction_measure");
    mouse(*f.canvas, QEvent::MouseButtonPress, f.logical(a), Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    mouse(*f.canvas, QEvent::MouseMove, f.logical(b), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical(b), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    const auto fixedB = core::constrainLineEndpoint(a, b);
    CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->b, fixedB));
    f.press(f.logical(a));
    const core::Vec2d newA {-10.5, 24.25};
    mouse(*f.canvas, QEvent::MouseMove, f.logical(newA), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->a, core::constrainLineEndpoint(fixedB, newA))
        && nearPoint(f.canvas->scene().measurement->b, fixedB));
    f.escape(); f.release(f.logical(newA));
    CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->a, a));
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels);
    CHECK(f.session().history().undoDepth() == history);
}

void advancedReadoutPreferenceAppliesAndLoadsOnce()
{
    QSettings settings;
    const auto settingKey = QStringLiteral("preferences/measurement/advancedReadout");
    settings.remove(settingKey); settings.sync();
    {
        Fixture f; CHECK(f.valid()); if (!f.valid()) return;
        f.drag({10,20}, {40,60});
        CHECK(!f.canvas->advancedMeasurementReadout());
        CHECK(f.canvas->scene().pointerTooltip.find("50.0 px") != std::string::npos);
        CHECK(f.canvas->scene().pointerTooltip.find("ΔX") == std::string::npos);
        const auto revision = f.document().revision(), pixels = f.surface().revision();
        const auto history = f.session().history().undoDepth();
        const auto original = f.canvas->scene().measurement;
        QTimer::singleShot(0, &f.window, [&] {
            auto* dialog = dynamic_cast<ui::PreferencesDialog*>(f.window.findChild<QDialog*>("PreferencesDialog"));
            CHECK(dialog); if (!dialog) return;
            auto* checkbox = dialog->findChild<QCheckBox*>("AdvancedMeasurementReadout");
            CHECK(checkbox); if (!checkbox) { dialog->reject(); return; }
            checkbox->setChecked(true);
            CHECK(f.canvas->advancedMeasurementReadout());
            CHECK(f.canvas->scene().pointerTooltip.find("ΔX: +30.0 px") != std::string::npos);
            CHECK(f.canvas->scene().pointerTooltip.find("ΔY: +40.0 px") != std::string::npos);
            auto* apply = dialog->findChild<QPushButton*>("PreferencesApply");
            CHECK(apply); if (apply) apply->click();
            checkbox->setChecked(false);
            dialog->reject(); // Restore the last applied true value, not entry false.
        });
        f.action("PreferencesAction");
        CHECK(f.canvas->advancedMeasurementReadout() && settings.value(settingKey).toBool());
        CHECK(f.canvas->scene().measurement == original && f.document().revision() == revision);
        CHECK(f.surface().revision() == pixels && f.session().history().undoDepth() == history);
    }
    {
        Fixture f(nullptr, true); CHECK(f.valid()); if (!f.valid()) return;
        CHECK(f.canvas->advancedMeasurementReadout());
        settings.setValue(settingKey, false); settings.sync();
        f.drag({10,20}, {40,60}); settle();
        CHECK(f.canvas->advancedMeasurementReadout()); // No live preference-file polling.
    }
    Fixture reopened(nullptr, true); CHECK(reopened.valid());
    CHECK(!reopened.canvas->advancedMeasurementReadout());
}

void rightClickClearsMeasureWithoutLeakingPendingReleases()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    CHECK(!f.window.findChild<QToolButton*>(QStringLiteral("ClearMeasurement")));
    CHECK(f.session().execute(std::make_unique<core::SetLayerOpacityCommand>(*f.session().activeLayer(), .75F)));
    CHECK(f.session().undo());
    f.canvas->setDocument(f.document().snapshot(), false);
    const auto pixels = f.surface().revision(), revision = f.document().revision();
    const auto history = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    const auto rightClick = [&](QPointF point, Qt::MouseButtons buttons = Qt::RightButton) {
        mouse(*f.canvas, QEvent::MouseButtonPress, point, Qt::RightButton, buttons);
        CHECK(!f.canvas->scene().measurement && !f.canvas->measureDragging());
        CHECK(f.canvas->scene().pointerTooltip.empty());
        QContextMenuEvent context(QContextMenuEvent::Mouse, point.toPoint(),
            f.canvas->mapToGlobal(point.toPoint()));
        context.setAccepted(false);
        QCoreApplication::sendEvent(f.canvas, &context);
        CHECK(context.isAccepted());
        mouse(*f.canvas, QEvent::MouseButtonRelease, point, Qt::RightButton,
            buttons & ~Qt::MouseButtons(Qt::RightButton));
    };
    f.drag({15, 20}, {75, 60});
    rightClick(f.logical({75, 60}));
    CHECK(!f.canvas->pointerGestureActive() && f.canvas->measureActive());
    // Clearing an empty measurement is still harmless; unlike a document
    // command it cannot disturb the current content or the redo branch.
    rightClick(f.logical({75, 60}));
    f.drag({25, 30}, {85, 65});
    f.press(f.logical({85, 65})); f.move(f.logical({100, 80}));
    rightClick(f.logical({100, 80}), Qt::LeftButton | Qt::RightButton);
    f.move(f.logical({110, 85})); f.release(f.logical({110, 85}));
    CHECK(!f.canvas->scene().measurement && !f.canvas->pointerGestureActive());

    f.action("ToolAction_brush");
    f.r(true); f.press(f.logical({30, 25})); f.move(f.logical({65, 55}));
    rightClick(f.logical({65, 55}), Qt::LeftButton | Qt::RightButton);
    CHECK(f.canvas->temporaryMeasureActive());
    f.r(false);
    f.move(f.logical({90, 75})); f.release(f.logical({90, 75}));
    CHECK(!f.canvas->temporaryMeasureActive() && !f.canvas->pointerGestureActive());
    CHECK(f.session().activeTool() == core::ToolId::Brush);
    CHECK(f.surface().revision() == pixels && f.document().revision() == revision);
    CHECK(f.session().history().undoDepth() == history && f.session().history().redoDepth() == redo);
    f.action("ToolAction_measure");
    f.drag({20, 25}, {60, 45});
    CHECK(f.canvas->scene().measurement && !f.canvas->pointerGestureActive());
}

void temporaryMeasureOwnsItsCompleteGestureAndPreservesTool()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.action("ToolAction_brush");
    const auto pixels = f.surface().revision(), revision = f.document().revision();
    const auto history = f.session().history().undoDepth();
    // Panel/ruler/splitter gestures own capture too: holding R must not hide
    // their current tool page or take over an in-progress widget drag.
    QWidget panelControl(f.workspace->panelOverlay());
    panelControl.setGeometry(15, 80, 160, 100); panelControl.show();
    mouse(panelControl, QEvent::MouseButtonPress, {20,20}, Qt::LeftButton, Qt::LeftButton);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::Widget);
    f.r(true); CHECK(!f.canvas->temporaryMeasureActive());
    mouse(panelControl, QEvent::MouseButtonRelease, {20,20}, Qt::LeftButton, Qt::NoButton);
    f.r(true, true); CHECK(!f.canvas->temporaryMeasureActive());
    f.r(false);
    f.r(true); CHECK(f.canvas->temporaryMeasureActive() && f.canvas->measureActive());
    CHECK(f.session().activeTool() == core::ToolId::Brush);
    f.r(true, true); f.r(false, true); CHECK(f.canvas->temporaryMeasureActive());
    f.press(f.logical({30, 25})); f.move(f.logical({65, 55}));
    CHECK(f.canvas->measureDragging());
    f.r(false);
    CHECK(!f.canvas->temporaryMeasureActive() && !f.canvas->measureActive() && !f.canvas->measureDragging());
    CHECK(!f.canvas->scene().measurement && f.canvas->scene().pointerTooltip.empty());
    CHECK(f.session().activeTool() == core::ToolId::Brush);
    f.move(f.logical({90, 75})); f.release(f.logical({90, 75}));
    CHECK(f.surface().revision() == pixels && f.document().revision() == revision);
    CHECK(f.session().history().undoDepth() == history);

    // Deactivation cancels held access even if no R release returns to the app.
    f.r(true); f.press(f.logical({35, 30})); f.move(f.logical({70, 60}));
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate); settle();
    CHECK(!f.canvas->temporaryMeasureActive() && !f.canvas->measureDragging());
    CHECK(f.session().activeTool() == core::ToolId::Brush && !f.canvas->scene().measurement);
    f.move(f.logical({90, 75})); f.release(f.logical({90, 75})); f.r(false);
    CHECK(f.surface().revision() == pixels);

    // An in-progress paint stroke cannot be commandeered by temporary measure.
    f.press(f.logical({35, 30})); f.move(f.logical({55, 35}));
    CHECK(f.canvas->pointerGestureActive());
    f.r(true); CHECK(!f.canvas->temporaryMeasureActive());
    f.release(f.logical({55, 35}));
    f.r(true, true); CHECK(!f.canvas->temporaryMeasureActive()); f.r(false);
    CHECK(f.surface().revision() > pixels);
}

void rapidSecondPressStaysOwnedByMeasure()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.drag({15, 20}, {75, 60});
    // Qt delivers a rapid second press as MouseButtonDblClick, not another
    // ordinary press. It must still begin an endpoint-adjustment gesture.
    mouse(*f.canvas, QEvent::MouseButtonDblClick, f.logical({75, 60}), Qt::LeftButton, Qt::LeftButton);
    CHECK(f.canvas->measureDragging());
    f.move(f.logical({90, 72})); f.release(f.logical({90, 72}));
    if (const auto line = f.canvas->scene().measurement)
        CHECK(nearPoint(line->a, {15, 20}) && nearPoint(line->b, {90, 72}));
    else CHECK(false);

    // A temporary measure keeps Move as the underlying session tool. Put a
    // real editable text layer under the double click to catch routing leaks
    // through Move's normal double-click-to-edit behavior.
    f.action("ToolAction_text");
    f.press(f.logical({20, 25})); f.release(f.logical({20, 25}));
    key(f.canvas, QEvent::KeyPress, Qt::Key_O, QStringLiteral("O"));
    key(f.canvas, QEvent::KeyRelease, Qt::Key_O, QStringLiteral("O"));
    f.action("ToolAction_move");
    CHECK(f.session().activeTool() == core::ToolId::Move);
    const auto revision = f.document().revision();
    const auto history = f.session().history().undoDepth();
    const auto selected = f.session().activeLayer();
    CHECK(selected && std::holds_alternative<core::TextLayer>(f.document().layer(*selected)->payload));
    f.r(true); CHECK(f.canvas->temporaryMeasureActive());
    const auto start = f.logical({20.5, 25.5});
    f.press(start); f.release(start);
    mouse(*f.canvas, QEvent::MouseButtonDblClick, start, Qt::LeftButton, Qt::LeftButton);
    CHECK(f.canvas->measureDragging() && f.canvas->temporaryMeasureActive());
    CHECK(f.session().activeTool() == core::ToolId::Move);
    f.move(f.logical({60.5, 55.5})); f.release(f.logical({60.5, 55.5})); f.r(false);
    CHECK(f.session().activeTool() == core::ToolId::Move && f.session().activeLayer() == selected);
    CHECK(f.document().revision() == revision && f.session().history().undoDepth() == history);
}

void focusCancellationRecoversWithoutAnExternalRelease()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto revision = f.document().revision(), pixels = f.surface().revision();
    for (const bool deactivate : {false, true}) {
        f.canvas->clearMeasurement(); f.drag({15, 20}, {75, 60});
        const auto retained = f.canvas->scene().measurement;
        f.press(f.logical({75, 60})); f.move(f.logical({90, 70}));
        if (deactivate) {
            QEvent event(QEvent::WindowDeactivate); QCoreApplication::sendEvent(f.canvas, &event);
        } else {
            QFocusEvent event(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        settle(); CHECK(!f.canvas->measureDragging() && f.canvas->scene().measurement == retained);
        // The original release occurs in another application: deliberately do
        // not deliver it here. A fresh press after focus returns must work on
        // the first attempt, even without an intervening canvas hover event.
        QFocusEvent activate(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(f.canvas, &activate);
        mouse(*f.canvas, QEvent::MouseButtonPress, f.logical({100, 65}), Qt::LeftButton, Qt::LeftButton);
        CHECK(f.canvas->measureDragging());
        f.move(f.logical({110, 85})); f.release(f.logical({110, 85}));
        if (const auto line = f.canvas->scene().measurement)
            CHECK(nearPoint(line->a, {100, 65}) && nearPoint(line->b, {110, 85}));
        else CHECK(false);
    }
    CHECK(f.document().revision() == revision && f.surface().revision() == pixels);
}

void tabletTemporaryMeasureDoesNotLeakIntoBrush()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.action("ToolAction_brush");
    const auto pixels = f.surface().revision(), revision = f.document().revision();
    const auto history = f.session().history().undoDepth();
    const QPointingDevice pen(QStringLiteral("Measure test pen"), 19773,
        QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2);
    const auto tablet = [&](QEvent::Type type, core::Vec2d point) {
        const bool released = type == QEvent::TabletRelease;
        const auto local = f.logical(point);
        QTabletEvent event(type, &pen, local, QPointF(f.canvas->mapToGlobal(local.toPoint())),
            released ? 0.0 : .7, 10, -15, 0, 30, 0, Qt::NoModifier,
            type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
            released ? Qt::NoButton : Qt::LeftButton);
        QCoreApplication::sendEvent(f.canvas, &event);
    };
    f.r(true);
    tablet(QEvent::TabletPress, {30.25, 25.5});
    tablet(QEvent::TabletMove, {60.25, 65.5});
    CHECK(f.canvas->measureDragging() && f.canvas->scene().measurement);
    if (const auto line = f.canvas->scene().measurement) {
        const auto value = core::measurementValues(*line);
        CHECK(value && near(value->distance, 50));
    }
    f.r(false);
    CHECK(!f.canvas->temporaryMeasureActive() && !f.canvas->measureDragging());
    tablet(QEvent::TabletMove, {85, 80}); tablet(QEvent::TabletRelease, {85, 80});
    CHECK(f.surface().revision() == pixels && f.document().revision() == revision);
    CHECK(f.session().history().undoDepth() == history && !f.canvas->pointerGestureActive());
    // The next pen stroke is independent and must not inherit release guards.
    tablet(QEvent::TabletPress, {35, 30}); tablet(QEvent::TabletMove, {55, 35});
    tablet(QEvent::TabletRelease, {55, 35});
    CHECK(f.surface().revision() > pixels && f.session().history().undoDepth() == history + 1);
}

void editableFieldsAndCanvasTextOwnR()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.action("ToolAction_brush");
    QLineEdit field(f.workspace->panelOverlay());
    field.setGeometry(80, 70, 180, 32); field.show(); field.setFocus(); settle();
    key(&field, QEvent::KeyPress, Qt::Key_R, QStringLiteral("r"));
    key(&field, QEvent::KeyRelease, Qt::Key_R, QStringLiteral("r"));
    CHECK(field.text() == QStringLiteral("r") && !f.canvas->temporaryMeasureActive());
    field.clearFocus(); field.hide();
    f.action("ToolAction_text");
    f.press(f.logical({20, 25})); f.release(f.logical({20, 25}));
    f.r(true); f.r(false);
    CHECK(!f.canvas->temporaryMeasureActive());
    CHECK(std::ranges::any_of(f.document().layers(), [](const core::Layer& layer) {
        const auto* text = std::get_if<core::TextLayer>(&layer.payload);
        return text && text->utf8 == "r";
    }));
}

void selectedBoundsAndRulerWorkspaceGeometry()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    auto* readout = f.find<QLabel>("MeasureBounds");
    auto* selectionStatus = f.find<QLabel>("SelectionStatus");
    CHECK(readout && readout->text().contains(QStringLiteral("128"))
        && readout->text().contains(QStringLiteral("96")));
    CHECK(selectionStatus && selectionStatus->text().contains(QStringLiteral("measure-source"))
        && selectionStatus->text().contains(QStringLiteral("(Raster)"))
        && selectionStatus->text().contains(QStringLiteral("128.0 × 96.0 px")));
    const auto first = *f.session().activeLayer();
    core::ShapeLayer shape; shape.size = {40.5, 20.25};
    auto second = core::Layer::shape("Measured shape", shape);
    second.localToDocument = {-2, 0, 230, 0, 1, -30};
    const auto secondId = second.id;
    CHECK(f.document().insertLayer(1, std::move(second)));
    f.session().setLayerSelection(std::array {first, secondId}, secondId);
    f.canvas->setDocument(f.document().snapshot(), false);
    f.action("ToolAction_brush"); f.action("ToolAction_measure");
    CHECK(readout && readout->text().contains(QStringLiteral("230"))
        && readout->text().contains(QStringLiteral("126")));
    CHECK(selectionStatus && selectionStatus->text() == QStringLiteral("2 selected layers"));
    const auto group = core::makeLayerId();
    core::LayerTree tree {{group}, {{group, "Measured group", core::ContainerKind::Group,
        core::ColorLabel::None, {first, secondId}, false}}};
    CHECK(f.document().replaceStructure(f.document().tree(), tree));
    f.session().setActiveLayer(group);
    f.canvas->setDocument(f.document().snapshot(), false);
    f.action("ToolAction_brush"); f.action("ToolAction_measure");
    CHECK(readout && readout->text().contains(QStringLiteral("230"))
        && readout->text().contains(QStringLiteral("126")));
    CHECK(selectionStatus && selectionStatus->text().contains(QStringLiteral("Measured group (Group)"))
        && selectionStatus->text().contains(QStringLiteral("230.0 × 126.0 px")));
    const auto size = f.canvas->size();
    const auto canvasRect = f.workspace->canvasContainer()->geometry();
    const auto zoom = f.canvas->zoom();
    const auto pan = f.canvas->scene().viewport.pan();
    const auto unchanged = [&] {
        CHECK(f.canvas->size() == size && f.workspace->canvasContainer()->geometry() == canvasRect);
        CHECK(f.canvas->zoom() == zoom && f.canvas->scene().viewport.pan() == pan);
    };
    CHECK(f.workspace->rulerVisible(Qt::Horizontal) && f.workspace->rulerVisible(Qt::Vertical));
    for (const bool bottom : {false, true}) for (const bool right : {false, true}) {
        f.workspace->setRulerFarEdge(Qt::Horizontal, bottom);
        f.workspace->setRulerFarEdge(Qt::Vertical, right); settle(); unchanged();
        const auto area = f.workspace->rulerContentRect();
        const auto* horizontal = f.workspace->rulerStrip(Qt::Horizontal);
        const auto* vertical = f.workspace->rulerStrip(Qt::Vertical);
        CHECK(horizontal && vertical && horizontal->isVisible() && vertical->isVisible());
        if (!horizontal || !vertical) continue;
        CHECK(horizontal->height() == ui::RulerStrip::thickness && vertical->width() == ui::RulerStrip::thickness);
        CHECK(bottom ? horizontal->geometry().bottom() == area.bottom() : horizontal->y() == area.top());
        CHECK(right ? vertical->geometry().right() == area.right() : vertical->x() == area.left());
    }
    // Reparenting rebuilds splitter handles; collect stable panel targets
    // before mutating the widget tree rather than revisiting stale children.
    std::vector<ui::WorkspacePanel*> panels;
    for (auto* widget : f.window.findChildren<QWidget*>())
        if (auto* panel = dynamic_cast<ui::WorkspacePanel*>(widget)) panels.push_back(panel);
    for (auto* panel : panels) {
        f.workspace->floatPanel(panel, QRect(350, 180, 300, 260)); settle(); unchanged();
    }
    auto* rail = f.find<QToolBar>("ToolRail");
    if (rail) for (const auto edge : {ui::OverlayDockWorkspace::ToolRailPlacement::Left,
             ui::OverlayDockWorkspace::ToolRailPlacement::Right, ui::OverlayDockWorkspace::ToolRailPlacement::Top,
             ui::OverlayDockWorkspace::ToolRailPlacement::Bottom}) {
        f.workspace->setToolRail(rail, edge); settle(); unchanged();
        CHECK(f.workspace->rulerContentRect().isValid());
    }
    f.action("ViewHorizontalRuler"); f.action("ViewVerticalRuler"); settle(); unchanged();
    CHECK(!f.workspace->rulerVisible(Qt::Horizontal) && !f.workspace->rulerVisible(Qt::Vertical));
    f.session().setActiveLayer({});
    f.action("ToolAction_brush"); f.action("ToolAction_measure");
    CHECK(selectionStatus && selectionStatus->text().isEmpty());
}

void defaultWorkspaceArrangement()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto placement = [&](const char* name) {
        auto* panel = dynamic_cast<ui::WorkspacePanel*>(f.find<QWidget>(name));
        CHECK(panel);
        return panel ? f.workspace->panelPlacement(panel) : ui::OverlayDockWorkspace::PanelPlacement::Floating;
    };
    CHECK(placement("LayersPanel") == ui::OverlayDockWorkspace::PanelPlacement::DockedLeft);
    CHECK(placement("ColorPanelShell") == ui::OverlayDockWorkspace::PanelPlacement::DockedRight);
    CHECK(placement("PropertiesPanelShell") == ui::OverlayDockWorkspace::PanelPlacement::DockedRight);
    for (const auto& [name, index] : std::array {std::pair {"ColorPanelShell", 0},
             std::pair {"PropertiesPanelShell", 1}}) {
        auto* panel = dynamic_cast<ui::WorkspacePanel*>(f.find<QWidget>(name));
        CHECK(panel && f.workspace->dockedPanelIndex(panel) == index);
    }
    CHECK(f.workspace->toolRailPlacement() == ui::OverlayDockWorkspace::ToolRailPlacement::Left);
    CHECK(f.workspace->rulerVisible(Qt::Horizontal) && f.workspace->rulerVisible(Qt::Vertical));
    if (auto* horizontal = f.find<QAction>("ViewHorizontalRuler")) CHECK(horizontal->isChecked());
    if (auto* vertical = f.find<QAction>("ViewVerticalRuler")) CHECK(vertical->isChecked());
}

void rulerPreferencesSurviveRestart()
{
    QSettings settings;
    settings.clear();
    {
        Fixture first(nullptr, true); CHECK(first.valid()); if (!first.valid()) return;
        CHECK(first.workspace->rulerVisible(Qt::Horizontal) && first.workspace->rulerVisible(Qt::Vertical));
        auto* layers = dynamic_cast<ui::WorkspacePanel*>(first.find<QWidget>("LayersPanel"));
        CHECK(layers && first.workspace->panelPlacement(layers)
            == ui::OverlayDockWorkspace::PanelPlacement::DockedLeft);
        first.workspace->setRulerVisible(Qt::Horizontal, true);
        first.workspace->setRulerVisible(Qt::Vertical, false);
        first.workspace->setRulerFarEdge(Qt::Horizontal, true);
        first.workspace->setRulerFarEdge(Qt::Vertical, true);
        settle();
    }
    {
        Fixture second(nullptr, true); CHECK(second.valid()); if (!second.valid()) return;
        CHECK(second.workspace->rulerVisible(Qt::Horizontal));
        CHECK(!second.workspace->rulerVisible(Qt::Vertical));
        CHECK(second.workspace->rulerFarEdge(Qt::Horizontal) && second.workspace->rulerFarEdge(Qt::Vertical));
        if (auto* horizontal = second.find<QAction>("ViewHorizontalRuler")) CHECK(horizontal->isChecked());
        if (auto* vertical = second.find<QAction>("ViewVerticalRuler")) CHECK(!vertical->isChecked());
    }
    settings.clear();
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 77;
    std::atomic_uint64_t warnings {0}, errors {0};
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) return EXIT_FAILURE;
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << "Vulkan measure/ruler: " << (data ? data->pMessage : "unknown") << '\n';
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
        const auto zoom = f.canvas->zoom();
        const auto pan = f.canvas->scene().viewport.pan();
        const auto size = f.canvas->size();
        CHECK(f.workspace->rulerVisible(Qt::Horizontal) && f.workspace->rulerVisible(Qt::Vertical));
        f.drag({-12.25, 10.5}, {82.75, 67.5});
        CHECK(f.canvas->scene().pointerTooltip.find("ΔX") == std::string::npos);
        f.canvas->setAdvancedMeasurementReadout(true);
        CHECK(f.canvas->scene().pointerTooltip.find("ΔX") != std::string::npos);
        f.canvas->setAdvancedMeasurementReadout(false);
        f.press(f.logical({82.75, 67.5}));
        f.move(f.logical({95.5, 83.25}));
        key(f.canvas, QEvent::KeyPress, Qt::Key_Shift, {}, false, Qt::ShiftModifier);
        CHECK(f.canvas->scene().measurement && nearPoint(f.canvas->scene().measurement->b,
            core::constrainLineEndpoint({-12.25, 10.5}, {95.5, 83.25})));
        mouse(*f.canvas, QEvent::MouseButtonRelease, f.logical({95.5, 83.25}),
            Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        key(f.canvas, QEvent::KeyRelease, Qt::Key_Shift);
        for (const bool bottom : {false, true}) for (const bool right : {false, true}) {
            f.workspace->setRulerFarEdge(Qt::Horizontal, bottom);
            f.workspace->setRulerFarEdge(Qt::Vertical, right); settle();
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        }
        for (int i = 0; i < 8; ++i) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        }
        QTest::qWait(150);
        const auto cached = f.canvas->rendererStats();
        QTest::qWait(180);
        const auto idle = f.canvas->rendererStats();
        CHECK(idle.framesSubmitted == cached.framesSubmitted);
        CHECK(idle.pointerTooltipRasterizations == cached.pointerTooltipRasterizations
            && idle.pointerTooltipUploads == cached.pointerTooltipUploads);
        CHECK(idle.fullUploads == baseline.fullUploads && idle.regionalUploads == baseline.regionalUploads
            && idle.uploadedBytes == baseline.uploadedBytes);
        CHECK(idle.resourceGeneration == baseline.resourceGeneration && idle.swapchainGeneration == baseline.swapchainGeneration);
        CHECK(f.surface().revision() == pixels && f.document().revision() == revision);
        CHECK(f.canvas->size() == size && f.canvas->zoom() == zoom && f.canvas->scene().viewport.pan() == pan);
        const auto review = qEnvironmentVariable("IMAGEEDITOR_MEASURE_REVIEW");
        if (!review.isEmpty()) {
            f.workspace->setRulerFarEdge(Qt::Horizontal, false);
            f.workspace->setRulerFarEdge(Qt::Vertical, false);
            f.window.activateWindow();
            QTest::qWait(200);
            QProcess capture;
            capture.start(QStringLiteral("spectacle"), {QStringLiteral("--background"), QStringLiteral("--nonotify"),
                QStringLiteral("--activewindow"), QStringLiteral("--output"), review});
            CHECK(capture.waitForFinished(5000) && capture.exitCode() == 0);
        }
    }
    instance.destroy(); settle(); CHECK(warnings == 0 && errors == 0);
    std::cout << "Native measure/ruler Vulkan validation: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("MeasureInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings; CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (application.arguments().contains(QStringLiteral("--wayland-validation"))) return nativeValidation();
    measurementsAreDocumentSpaceViewState();
    endpointAdjustmentCancellationAndCrossPanelCapture();
    measureShiftSnapsWithoutPromotingTemporaryAccess();
    advancedReadoutPreferenceAppliesAndLoadsOnce();
    rightClickClearsMeasureWithoutLeakingPendingReleases();
    temporaryMeasureOwnsItsCompleteGestureAndPreservesTool();
    rapidSecondPressStaysOwnedByMeasure();
    focusCancellationRecoversWithoutAnExternalRelease();
    tabletTemporaryMeasureDoesNotLeakIntoBrush();
    editableFieldsAndCanvasTextOwnR();
    selectedBoundsAndRulerWorkspaceGeometry();
    defaultWorkspaceArrangement();
    rulerPreferencesSurviveRestart();
    if (failures) std::cerr << failures << " measure/ruler assertion(s) failed\n";
    else std::cout << "Measure/ruler interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
