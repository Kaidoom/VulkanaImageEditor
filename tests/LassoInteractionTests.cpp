#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"

#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QSettings>
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
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <numbers>
#include <vector>

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
bool waitFor(const std::function<bool()>& predicate, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        settle();
        if (predicate())
            return true;
        QTest::qWait(2);
    } while (timer.elapsed() < timeout);
    return predicate();
}
QAction* shortcut(ui::MainWindow& window, const char* sequence)
{
    for (auto* action : window.findChildren<QAction*>())
        if (action->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence))))
            return action;
    return nullptr;
}
template <class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF point, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(
        type, point, point, QPointF(receiver.mapToGlobal(point.toPoint())), button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}
bool near(core::Vec2d a, core::Vec2d b) { return std::hypot(a.x - b.x, a.y - b.y) < 1e-8; }

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas { };
    ui::ToolOptionsBar* options { };
    ui::OverlayDockWorkspace* workspace { };
    ui::CrossWindowPointerRouter* router { };

    explicit Fixture(QVulkanInstance* instance = nullptr)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1480, 880);
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        options = dynamic_cast<ui::ToolOptionsBar*>(
            window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        open(64, 48);
    }
    ~Fixture()
    {
        window.close();
        settle();
    }
    bool valid() const
    {
        return canvas && options && workspace && router && window.editorSession().document();
    }
    const core::Document& document() const { return *window.editorSession().document(); }
    const core::History& history() const { return window.editorSession().history(); }
    core::SelectionState selection() const { return document().selection(); }
    const core::Layer& layer() const { return *document().layer(*window.editorSession().activeLayer()); }
    const core::RasterSurface& surface() const
    {
        return *std::get<core::RasterLayer>(layer().payload).surface;
    }
    std::vector<std::byte> pixels() const
    {
        const auto extent = surface().extent();
        std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
        surface().copyRgba8({ 0, 0, int(extent.width), int(extent.height) }, bytes, extent.width * 4);
        return bytes;
    }
    int alphaAt(int x, int y) const
    {
        std::array<std::byte, 4> bytes { };
        surface().copyRgba8({ x, y, 1, 1 }, bytes, 4);
        return std::to_integer<int>(bytes[3]);
    }
    void open(int width, int height)
    {
        QImage image(width, height, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        const auto path = assets.filePath(QStringLiteral("lasso-source.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        settle();
    }
    void action(const char* name)
    {
        auto* target = window.findChild<QAction*>(QString::fromLatin1(name));
        CHECK(target);
        if (target)
            target->trigger();
        settle();
    }
    void keyAction(const char* key)
    {
        auto* target = shortcut(window, key);
        CHECK(target);
        if (target)
            target->trigger();
        settle();
    }
    void button(const char* name)
    {
        auto* target = window.findChild<QToolButton*>(QString::fromLatin1(name));
        CHECK(target);
        if (target)
            target->click();
        settle();
    }
    void number(const char* name, double value)
    {
        auto* target = window.findChild<QDoubleSpinBox*>(QString::fromLatin1(name));
        CHECK(target);
        if (target)
            target->setValue(value);
        settle();
    }
    QPointF logical(core::Vec2d p) const
    {
        const auto extent = document().canvas().extent;
        const auto& scene = canvas->scene();
        const auto q = scene.viewport.documentToViewport(
            p, { double(extent.width), double(extent.height) }, scene.logicalViewport);
        return { q.x, q.y };
    }
    void press(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseMove, logical(p), Qt::NoButton, Qt::NoButton, mods);
        mouse(*canvas, QEvent::MouseButtonPress, logical(p), Qt::LeftButton, Qt::LeftButton, mods);
    }
    void move(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseMove, logical(p), Qt::NoButton, Qt::LeftButton, mods);
    }
    void release(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseButtonRelease, logical(p), Qt::LeftButton, Qt::NoButton, mods);
    }
    void waitIdle()
    {
        CHECK(waitFor([this] {
            const auto* timer = window.findChild<QTimer*>(QStringLiteral("SelectionRasterizationTimer"));
            return !canvas->selectionDragging() && !canvas->scene().selectionPathPreview
                && (!timer || !timer->isActive());
        }));
    }
    void trace(const std::vector<core::Vec2d>& path, Qt::KeyboardModifiers initial = Qt::NoModifier,
        Qt::KeyboardModifiers later = Qt::NoModifier)
    {
        CHECK(!path.empty());
        if (path.empty())
            return;
        press(path.front(), initial);
        for (std::size_t i = 1; i < path.size(); ++i)
            move(path[i], later);
        release(path.back(), later);
        waitIdle();
    }
    void lasso() { action("ToolAction_lasso"); }
    void undo() { keyAction("Ctrl+Z"); }
    void redo() { keyAction("Ctrl+Shift+Z"); }
};

int coverage(const core::SelectionState& mask, int x, int y)
{
    CHECK(mask);
    return mask ? mask->coverageAtDocumentPixel(x, y) : -1;
}

const std::vector<core::Vec2d> concave { { 5.25, 5.25 }, { 42.25, 5.25 }, { 42.25, 17.25 },
    { 22.25, 17.25 }, { 22.25, 36.25 }, { 5.25, 36.25 } };

void sharedControlsAndRealShortcutPreserveLayoutAndTyping()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    const auto geometry = f.workspace->canvasContainer()->geometry();
    auto* page = f.options->pageForTool(core::ToolId::Marquee);
    CHECK(page && f.options->pageForTool(core::ToolId::Lasso) == page);
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_L);
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Lasso);
    CHECK(page && page->isVisible());
    for (const auto* name : { "SelectionModeReplace", "SelectionModeAdd", "SelectionModeSubtract",
             "SelectionModeIntersect", "SelectionTransform" }) {
        auto* button = f.window.findChild<QToolButton*>(QString::fromLatin1(name));
        CHECK(button && !button->icon().isNull() && !button->toolTip().isEmpty());
    }
    for (int i = 0; i < 5; ++i) {
        f.keyAction("M");
        f.keyAction("B");
        f.lasso();
        CHECK(f.options->pageForTool(core::ToolId::Lasso) == page);
        CHECK(f.workspace->canvasContainer()->geometry() == geometry);
    }
    CHECK(f.history().undoDepth() == 0);
    f.keyAction("B");
    QLineEdit editor(&f.window);
    editor.setGeometry(80, 80, 220, 35);
    editor.show();
    editor.setFocus(Qt::OtherFocusReason);
    settle();
    QTest::keyClick(&editor, Qt::Key_L);
    CHECK(editor.text() == QStringLiteral("l"));
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Brush);
}

void previewUsesOriginalSubpixelsAndClosingSegmentWithoutPublishingMasks()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    const auto revision = f.document().selectionRevision();
    const auto rasterRevision = f.surface().revision();
    const auto geometry = f.workspace->canvasContainer()->geometry();
    f.press(concave.front());
    CHECK(f.canvas->selectionDragging());
    for (std::size_t i = 1; i < concave.size(); ++i) {
        f.move(concave[i]);
        CHECK(!f.selection());
        CHECK(f.history().undoDepth() == 0);
        CHECK(f.document().selectionRevision() == revision);
        CHECK(f.surface().revision() == rasterRevision);
        CHECK(f.canvas->scene().selectionPathPreview);
        const auto& edges = f.canvas->scene().selectionEdges;
        CHECK(edges && edges->size() >= 2);
        if (edges && !edges->empty()) {
            CHECK(near(edges->front().from, concave.front()));
            CHECK(near(edges->back().from, concave[i]));
            CHECK(near(edges->back().to, concave.front()));
        }
    }
    f.release(concave.back());
    f.waitIdle();
    CHECK(f.history().undoDepth() == 1);
    CHECK(coverage(f.selection(), 10, 25) == 255);
    CHECK(coverage(f.selection(), 30, 10) == 255);
    CHECK(coverage(f.selection(), 30, 25) == 0);
    CHECK(coverage(f.selection(), 5, 10) > 0 && coverage(f.selection(), 5, 10) < 255);
    CHECK(f.surface().revision() == rasterRevision);
    CHECK(f.workspace->canvasContainer()->geometry() == geometry);
    const auto selected = f.selection();
    f.undo();
    CHECK(!f.selection());
    f.redo();
    CHECK(f.selection() && f.selection()->equivalent(*selected));
}

void windingZoomAndOutsideVerticesRetainTheSameIntendedPolygon()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    f.trace(concave);
    const auto expected = f.selection();
    f.undo();
    const auto fit = f.canvas->zoom();
    f.canvas->resetTo100Percent();
    CHECK(std::abs(fit - f.canvas->zoom()) > .1);
    auto reversed = concave;
    std::reverse(reversed.begin(), reversed.end());
    f.trace(reversed);
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.undo();
    // Clamping outside vertices would incorrectly exclude (3,3).
    f.trace({ { -10.25, 12.25 }, { 12.25, -10.25 }, { 34.75, 12.25 }, { 12.25, 34.75 } });
    CHECK(coverage(f.selection(), 0, 0) == 0);
    CHECK(coverage(f.selection(), 3, 3) == 255);
    CHECK(coverage(f.selection(), 12, 0) == 255);
    CHECK(coverage(f.selection(), 0, 12) == 255);
    CHECK(coverage(f.selection(), 36, 12) == 0);
    const auto clipped = f.selection();
    f.undo();
    f.canvas->fitDocumentToView();
    f.trace({ { 12.25, 34.75 }, { 34.75, 12.25 }, { 12.25, -10.25 }, { -10.25, 12.25 } });
    CHECK(f.selection() && f.selection()->equivalent(*clipped));
}

void selfIntersectionsDuplicatesAndLongCurvesCompleteDeterministically()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    // Opposing lobes have cancelling signed areas; even-odd must retain both.
    const std::vector<core::Vec2d> eight { { 8, 5 }, { 40, 37 }, { 8, 37 }, { 40, 5 }, { 40, 5 } };
    f.trace(eight);
    CHECK(coverage(f.selection(), 23, 8) == 255);
    CHECK(coverage(f.selection(), 23, 33) == 255);
    CHECK(coverage(f.selection(), 9, 21) == 0);
    const auto crossed = f.selection();
    f.undo();
    auto reverse = eight;
    std::reverse(reverse.begin(), reverse.end());
    f.trace(reverse);
    CHECK(f.selection() && f.selection()->equivalent(*crossed));
    f.undo();
    std::vector<core::Vec2d> path;
    for (int i = 0; i < 12000; ++i) {
        const auto angle = double(i) * 2 * std::numbers::pi / 11999.0;
        path.push_back({ 31.25 + 20.25 * std::cos(angle), 23.25 + 17.25 * std::sin(angle) });
        if (i % 17 == 0)
            path.push_back(path.back());
    }
    f.trace(path);
    CHECK(coverage(f.selection(), 31, 23) == 255);
    CHECK(coverage(f.selection(), 8, 23) == 0);
    CHECK(coverage(f.selection(), 31, 43) == 0);
    CHECK(f.history().undoDepth() == 1);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void modifierOperationsLatchAndSelectedHolesDoNotBecomeMoveInteriors()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    f.trace({ { 3, 3 }, { 24, 3 }, { 24, 25 }, { 3, 25 } });
    f.trace({ { 35, 7 }, { 46, 7 }, { 46, 20 }, { 35, 20 } }, Qt::ShiftModifier);
    CHECK(coverage(f.selection(), 10, 10) == 255);
    CHECK(coverage(f.selection(), 40, 12) == 255);
    f.trace({ { 8, 8 }, { 17, 8 }, { 17, 17 }, { 8, 17 } }, Qt::AltModifier, Qt::ShiftModifier);
    CHECK(coverage(f.selection(), 12, 12) == 0);
    const auto holed = f.selection();
    f.press({ 12, 12 });
    f.move({ 15, 12 });
    f.move({ 15, 15 });
    CHECK(f.canvas->scene().selectionPathPreview); // A hole begins a new freehand path.
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*holed));
    f.press({ 5, 5 });
    f.move({ 9, 7 });
    CHECK(!f.canvas->scene().selectionPathPreview); // Selected coverage uses shared mask translation.
    f.release({ 9, 7 });
    f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*holed->translated(4, 2)));
    f.undo();
    f.trace({ { 0, 0 }, { 28, 0 }, { 28, 28 }, { 0, 28 } }, Qt::ShiftModifier | Qt::AltModifier);
    CHECK(coverage(f.selection(), 5, 5) == 255);
    CHECK(coverage(f.selection(), 12, 12) == 0);
    CHECK(coverage(f.selection(), 40, 12) == 0);
    f.undo();
    f.button("SelectionModeIntersect");
    f.trace({ { 30, 0 }, { 50, 0 }, { 50, 25 }, { 30, 25 } });
    CHECK(coverage(f.selection(), 5, 5) == 0);
    CHECK(coverage(f.selection(), 40, 12) == 255);
}

void tinyNoopsAndCancelledGesturesPreserveUndoRedoAndCapture()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    f.trace({ { 8.2, 9.4 } });
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(f.history().undoDepth() == 1);
    f.action("DeselectAction");
    f.undo();
    const auto revision = f.document().selectionRevision();
    const auto memory = f.history().memoryUsed();
    f.trace({ { 8.2, 9.4 }, { 8.2, 9.4 }, { 13.2, 9.4 }, { 19.2, 9.4 } });
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(f.history().undoDepth() == 1 && f.history().redoDepth() == 1);
    CHECK(f.history().memoryUsed() == memory);
    CHECK(f.document().selectionRevision() == revision);
    for (int reason = 0; reason < 4; ++reason) {
        f.lasso();
        f.press({ 4, 4 });
        f.move({ 30, 4 });
        f.move({ 20, 30 });
        if (reason == 0)
            QTest::keyClick(f.canvas, Qt::Key_Escape);
        if (reason == 1) {
            QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        if (reason == 2)
            f.keyAction("V");
        if (reason == 3) {
            QPlatformSurfaceEvent event(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        f.waitIdle();
        CHECK(f.selection() && f.selection()->bounds().empty());
        CHECK(f.history().undoDepth() == 1 && f.history().redoDepth() == 1);
        CHECK(f.document().selectionRevision() == revision);
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
    f.redo();
    CHECK(!f.selection());
}

void panelBoundaryReleaseRoutesToTheOriginalLassoGesture()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    QWidget receiver(f.workspace->panelOverlay());
    receiver.setGeometry(20, 70, 120, 50);
    receiver.show();
    f.press({ 7, 7 });
    f.move({ 38, 7 });
    f.move({ 38, 32 });
    const auto routes = f.router->routedEventCount();
    const auto global = f.canvas->mapToGlobal(f.logical({ 7, 32 }).toPoint());
    const auto local = QPointF(receiver.mapFromGlobal(global));
    mouse(receiver, QEvent::MouseMove, local, Qt::NoButton, Qt::LeftButton);
    mouse(receiver, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
    f.waitIdle();
    CHECK(coverage(f.selection(), 20, 20) == 255);
    CHECK(f.history().undoDepth() == 1);
    CHECK(f.router->routedEventCount() >= routes + 2);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void cancellationAfterReleaseStopsCooperativeRasterizationWithoutPublishingAnything()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.open(2048, 1536);
    f.lasso();
    f.trace(concave);
    f.action("InvertSelectionAction");
    f.undo();
    const auto before = f.selection();
    const auto revision = f.document().selectionRevision();
    const auto rasterRevision = f.surface().revision();
    const auto depth = f.history().undoDepth();
    const auto redo = f.history().redoDepth();
    for (int reason = 0; reason < 6; ++reason) {
        f.lasso();
        f.press({ 100.25, 100.25 });
        f.move({ 2000.25, 100.25 });
        f.move({ 1900.25, 1480.25 });
        f.move({ 100.25, 1400.25 });
        f.release({ 100.25, 1400.25 });
        auto* timer = f.window.findChild<QTimer*>(QStringLiteral("SelectionRasterizationTimer"));
        CHECK(timer && timer->isActive());
        CHECK(f.selection() == before);
        if (reason == 0)
            QTest::keyClick(f.canvas, Qt::Key_Escape);
        if (reason == 1) {
            QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        if (reason == 2)
            f.keyAction("B");
        if (reason == 3) {
            QPlatformSurfaceEvent event(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        if (reason == 4 || reason == 5) {
            QEvent event(reason == 4 ? QEvent::Hide : QEvent::TouchCancel);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        f.waitIdle();
        QTest::qWait(20);
        CHECK(f.selection() == before);
        CHECK(f.document().selectionRevision() == revision);
        CHECK(f.surface().revision() == rasterRevision);
        CHECK(f.history().undoDepth() == depth && f.history().redoDepth() == redo);
        CHECK(!timer || !timer->isActive());
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    }
}

void existingGrowTransformCopyFillBrushAndEraseShareTheLassoMask()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.lasso();
    f.trace(concave);
    const auto original = f.selection();
    const auto transform = f.layer().localToDocument;
    const auto revision = f.surface().revision();
    f.button("SelectionTransform");
    f.number("SelectionGrowHorizontal", 2);
    f.number("SelectionGrowVertical", -1);
    f.button("SelectionGrowApply");
    CHECK(f.selection() && f.selection()->equivalent(*original->adjusted(2, -1)));
    f.undo();
    f.keyAction("Ctrl+T");
    CHECK(f.canvas->scene().transformOverlay);
    f.number("TransformAngleControl", 17);
    CHECK(f.selection() == original);
    f.button("TransformApply");
    CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.selection() && !f.selection()->equivalent(*original));
    CHECK(f.layer().localToDocument == transform && f.surface().revision() == revision);
    f.undo();
    CHECK(f.selection() && f.selection()->equivalent(*original));
    auto* colors = dynamic_cast<ui::ColorSelector*>(
        f.window.findChild<QWidget*>(QStringLiteral("ColorPanelColors")));
    CHECK(colors && colors->onColorsChanged);
    if (colors && colors->onColorsChanged)
        colors->onColorsChanged({ { 225, 80, 45, 255 }, { 0, 0, 0, 255 }, core::ColorSlot::Primary });
    f.action("FillForegroundAction");
    CHECK(waitFor([&] { return f.history().undoDepth() == 2; }));
    const auto filled = f.pixels();
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x)
            CHECK(f.alphaAt(x, y) == coverage(original, x, y));
    const auto sourceId = f.window.editorSession().activeLayer();
    f.action("LayerViaCopyAction");
    CHECK(f.document().layers().size() == 2);
    CHECK(f.window.editorSession().activeLayer() != sourceId);
    CHECK(f.selection() && f.selection()->equivalent(*original));
    f.undo();
    CHECK(f.window.editorSession().activeLayer() == sourceId);
    f.undo();
    CHECK(f.alphaAt(10, 10) == 0);
    f.keyAction("B");
    f.number("BrushSizeControl", 100);
    f.number("BrushHardnessControl", 100);
    f.number("BrushOpacityControl", 100);
    f.number("BrushFlowControl", 100);
    f.press({ 20, 20 });
    f.release({ 20, 20 });
    settle();
    CHECK(f.alphaAt(10, 10) > 0);
    CHECK(f.alphaAt(30, 25) == 0);
    CHECK(f.alphaAt(2, 2) == 0);
    CHECK(f.selection() && f.selection()->equivalent(*original));
    f.undo();
    f.action("FillForegroundAction");
    CHECK(waitFor([&] { return f.pixels() == filled; }));
    f.keyAction("E");
    f.press({ 20, 20 });
    f.release({ 20, 20 });
    settle();
    CHECK(f.alphaAt(10, 10) == 0);
    CHECK(f.selection() && f.selection()->equivalent(*original));
    f.undo();
    CHECK(f.pixels() == filled);
    f.redo();
    CHECK(f.alphaAt(10, 10) == 0);
}

int nativeLassoValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland"))
        return 77;
    std::atomic_uint64_t warnings { 0 }, errors { 0 };
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation")))
        return EXIT_FAILURE;
    instance.setLayers({ QByteArrayLiteral("VK_LAYER_KHRONOS_validation") });
    instance.installDebugOutputFilter(
        [&](QVulkanInstance::DebugMessageSeverityFlags severity,
            QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
            if (!type.testFlag(QVulkanInstance::ValidationMessage))
                return false;
            if (severity.testFlag(QVulkanInstance::ErrorSeverity))
                ++errors;
            else if (severity.testFlag(QVulkanInstance::WarningSeverity))
                ++warnings;
            else
                return false;
            const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
            std::cerr << "Vulkan lasso validation: " << (data ? data->pMessage : "unknown") << '\n';
            return true;
        });
    if (!instance.create())
        return EXIT_FAILURE;
    {
        Fixture f(&instance);
        CHECK(f.valid());
        if (!f.valid())
            return EXIT_FAILURE;
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads >= 1; }));
        // Wayland assigns the initial output asynchronously, after a first
        // surface upload. Let that output/resize handshake finish before the
        // interaction-only swapchain baseline is sampled.
        QTest::qWait(180);
        CHECK(waitFor([&] { return !f.canvas->presentationSuppressedForResize(); }));
        f.lasso();
        const auto raster = f.pixels();
        const auto revision = f.surface().revision();
        const auto selectionRevision = f.document().selectionRevision();
        const auto geometry = f.workspace->canvasContainer()->geometry();
        const auto stats = f.canvas->rendererStats();
        constexpr int samples = 128;
        core::Vec2d last { 49, 24 };
        f.press(last);
        for (int i = 1; i <= samples; ++i) {
            const double a = i * 2 * std::numbers::pi / samples;
            last = { 32 + 17 * std::cos(a), 24 + 17 * std::sin(a) };
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.move(last);
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
            CHECK(!f.selection() && f.document().selectionRevision() == selectionRevision);
        }
        const auto preview = f.canvas->rendererStats();
        CHECK(preview.selectionGeometryUploads > stats.selectionGeometryUploads);
        // A complete per-sample path upload costs >128 KiB. Incremental suffix
        // uploads including closing edges/frame-slot catch-up stay linear.
        CHECK(preview.selectionUploadedBytes - stats.selectionUploadedBytes < samples * 16U * 12U);
        CHECK(f.canvas->scene().selectionPathPreview);
        CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
        f.release(last);
        f.waitIdle();
        CHECK(f.selection() && coverage(f.selection(), 32, 24) == 255);
        CHECK(f.history().undoDepth() == 1);
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > preview.framesSubmitted; }));
        CHECK(f.canvas->selectionAnimationActive());
        CHECK(f.pixels() == raster && f.surface().revision() == revision);
        CHECK(f.canvas->rendererStats().uploadedBytes == stats.uploadedBytes);
        CHECK(f.canvas->rendererStats().fullUploads == stats.fullUploads);
        CHECK(f.canvas->rendererStats().regionalUploadBatches == stats.regionalUploadBatches);
        CHECK(f.canvas->rendererStats().swapchainGeneration == stats.swapchainGeneration);
        CHECK(f.workspace->canvasContainer()->geometry() == geometry);
        f.undo();
        CHECK(!f.selection() && !f.canvas->selectionAnimationActive());
        f.redo();
        CHECK(f.selection() && f.canvas->selectionAnimationActive());
        f.action("DeselectAction");
        QTest::qWait(60);
        const auto quiet = f.canvas->rendererStats();
        QTest::qWait(100);
        CHECK(!f.canvas->selectionAnimationActive());
        CHECK(f.canvas->rendererStats().selectionUploadedBytes == quiet.selectionUploadedBytes);
        // Bounds overlap the viewport's top-left, but the slanted segment
        // itself misses it. Invisible preview edges must not animate ants.
        const auto outsideA = f.canvas->documentPositionForLogical({-100, 60});
        const auto outsideB = f.canvas->documentPositionForLogical({60, -100});
        auto outside = std::make_shared<std::vector<core::SelectionEdge>>(
            std::initializer_list<core::SelectionEdge>{{outsideA, outsideB}, {outsideB, outsideA}});
        f.canvas->setSelectionPathPreview(outside);
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > quiet.framesSubmitted; }));
        CHECK(!f.canvas->selectionAnimationActive());
        f.canvas->setSelectionPreview({});
        f.window.logRendererDiagnostics();
    }
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Native lasso validation: warnings=" << warnings << " errors=" << errors << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

void reviewSheet(const QString& output)
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.window.resize(1600, 1100);
    f.lasso();
    QTest::qWait(60);
    std::vector<std::pair<QString, core::SelectionState>> cases;
    const auto save = [&](const QString& title) {
        cases.emplace_back(title, f.selection());
        f.action("DeselectAction");
    };
    f.trace(concave);
    save(QStringLiteral("Concave / subpixel edge"));
    f.trace({ { 8, 5 }, { 40, 37 }, { 8, 37 }, { 40, 5 } });
    save(QStringLiteral("Figure eight / even-odd crossing"));
    f.trace({ { -10.25, 12.25 }, { 12.25, -10.25 }, { 34.75, 12.25 }, { 12.25, 34.75 } });
    save(QStringLiteral("Outside vertices / canvas clipping"));
    auto reversed = concave;
    std::reverse(reversed.begin(), reversed.end());
    f.trace(reversed);
    save(QStringLiteral("Reversed concave / same coverage"));
    std::vector<core::Vec2d> circle, hole;
    for (int i = 0; i < 360; ++i) {
        const auto a = i * 2 * std::numbers::pi / 360;
        circle.push_back({ 32.25 + 20.25 * std::cos(a), 24.25 + 17.25 * std::sin(a) });
        hole.push_back({ 32.25 + 10.25 * std::cos(a), 24.25 + 9.25 * std::sin(a) });
    }
    f.trace(circle);
    f.trace(hole, Qt::AltModifier);
    save(QStringLiteral("Curved traces / subtract a hole"));
    f.trace({ { 5.25, 6.25 }, { 24.25, 10.25 }, { 16.25, 35.25 } });
    f.trace({ { 34.25, 5.25 }, { 57.25, 5.25 }, { 52.25, 33.25 }, { 35.25, 37.25 } }, Qt::ShiftModifier);
    save(QStringLiteral("Add / disconnected regions"));

    // Generated diagnostic figure: CPU coverage over a checkerboard, plus
    // grabs of the real cached toolbar and Properties widgets. This is not a
    // framebuffer sampling path and is never part of editor rendering.
    QImage sheet(1600, 1400, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor("#14171f"));
    QPainter painter(&sheet);
    painter.setPen(QColor("#ecedf3"));
    auto titleFont = painter.font();
    titleFont.setPixelSize(22);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(QRect(20, 10, 1560, 35), QStringLiteral("Freehand Lasso · R8 coverage review"));
    painter.drawPixmap(0, 52, f.options->grab());
    auto font = titleFont;
    font.setPixelSize(14);
    font.setBold(false);
    painter.setFont(font);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const int x = 20 + int(i % 3) * 380;
        const int y = 124 + int(i / 3) * 330;
        painter.fillRect(QRect(x - 6, y - 6, 364, 310), QColor("#1b1f28"));
        painter.setPen(QColor("#dfe2ee"));
        painter.drawText(QRect(x, y, 355, 32), cases[i].first);
        const QRect destination(x, y + 40, 350, 263);
        painter.save();
        painter.setClipRect(destination);
        for (int cy = 0; cy < destination.height(); cy += 14)
            for (int cx = 0; cx < destination.width(); cx += 14)
                painter.fillRect(destination.x() + cx, destination.y() + cy, 14, 14,
                    ((cx / 14 + cy / 14) % 2) ? QColor("#373c46") : QColor("#454b56"));
        QImage coverageImage(64, 48, QImage::Format_ARGB32_Premultiplied);
        coverageImage.fill(Qt::transparent);
        if (const auto& mask = cases[i].second)
            for (int py = 0; py < 48; ++py)
                for (int px = 0; px < 64; ++px)
                    coverageImage.setPixelColor(
                        px, py, QColor(110, 133, 248, mask->coverageAtDocumentPixel(px, py)));
        painter.drawImage(destination, coverageImage);
        painter.restore();
    }
    painter.setPen(QColor("#e0e3ef"));
    painter.drawText(QRect(1180, 124, 400, 30), QStringLiteral("Actual Properties content"));
    for (auto* body : f.window.findChildren<QWidget*>(QStringLiteral("PropertiesPageBody"))) {
        if (!body->isVisible())
            continue;
        const auto pixmap = body->grab();
        const auto scaled = pixmap.scaled(QSize(390, 1190), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawPixmap(1180, 162, scaled);
        break;
    }
    painter.setPen(QColor("#9aa4ba"));
    painter.drawText(QRect(20, 820, 1120, 70), Qt::TextWordWrap,
        QStringLiteral("Each sample uses actual lasso pointer capture and the shared selection command. "
                       "Coverage is magnified to make partial pixels visible. No smoothing is applied to "
                       "this review image."));
    painter.end();
    CHECK(sheet.save(output));
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LassoInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (qEnvironmentVariableIsSet("IMAGEEDITOR_TEST_LASSO_NATIVE"))
        return nativeLassoValidation();
    if (const auto preview = qEnvironmentVariable("IMAGEEDITOR_TEST_LASSO_PREVIEW"); !preview.isEmpty()) {
        reviewSheet(preview);
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    sharedControlsAndRealShortcutPreserveLayoutAndTyping();
    previewUsesOriginalSubpixelsAndClosingSegmentWithoutPublishingMasks();
    windingZoomAndOutsideVerticesRetainTheSameIntendedPolygon();
    selfIntersectionsDuplicatesAndLongCurvesCompleteDeterministically();
    modifierOperationsLatchAndSelectedHolesDoNotBecomeMoveInteriors();
    tinyNoopsAndCancelledGesturesPreserveUndoRedoAndCapture();
    panelBoundaryReleaseRoutesToTheOriginalLassoGesture();
    cancellationAfterReleaseStopsCooperativeRasterizationWithoutPublishingAnything();
    existingGrowTransformCopyFillBrushAndEraseShareTheLassoMask();
    if (failures)
        std::cerr << failures << " lasso interaction assertion(s) failed\n";
    else
        std::cout << "All lasso interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
