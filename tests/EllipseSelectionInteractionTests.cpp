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
#include <QKeyEvent>
#include <QLabel>
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
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        settle();
        if (predicate()) return true;
        QTest::qWait(2);
    } while (timer.elapsed() < timeout);
    return predicate();
}
template <class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF point, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, point, point, QPointF(receiver.mapToGlobal(point.toPoint())),
        button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}
struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::OverlayDockWorkspace* workspace {};
    ui::ToolOptionsBar* options {};
    ui::CrossWindowPointerRouter* router {};
    explicit Fixture(QVulkanInstance* instance = nullptr) : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1800, 900);
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        options = dynamic_cast<ui::ToolOptionsBar*>(
            window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        open(64, 48);
        action("ToolAction_marquee");
        button("SelectModeEllipse");
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return canvas && workspace && options && router && window.editorSession().document(); }
    const core::Document& document() const { return *window.editorSession().document(); }
    const core::History& history() const { return window.editorSession().history(); }
    core::SelectionState selection() const { return document().selection(); }
    const core::Layer& layer() const { return *document().layer(*window.editorSession().activeLayer()); }
    const core::RasterSurface& surface() const { return *std::get<core::RasterLayer>(layer().payload).surface; }
    std::vector<std::byte> pixels() const
    {
        const auto extent = surface().extent();
        std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
        surface().copyRgba8({ 0, 0, int(extent.width), int(extent.height) }, bytes, extent.width * 4);
        return bytes;
    }
    int alphaAt(int x, int y) const
    {
        std::array<std::byte, 4> pixel {};
        surface().copyRgba8({ x, y, 1, 1 }, pixel, 4);
        return std::to_integer<int>(pixel[3]);
    }
    void open(int width, int height)
    {
        QImage image(width, height, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        const auto path = assets.filePath(QStringLiteral("ellipse-source.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        settle();
    }
    template<class T> T* find(const char* name) const
    {
        auto* target = window.findChild<T*>(QString::fromLatin1(name));
        CHECK(target);
        return target;
    }
    void action(const char* name) { if (auto* item = find<QAction>(name)) item->trigger(); settle(); }
    void button(const char* name) { if (auto* item = find<QToolButton>(name)) item->click(); settle(); }
    void number(const char* name, double value)
    {
        if (auto* item = find<QDoubleSpinBox>(name)) item->setValue(value);
        settle();
    }
    void shortcut(const char* sequence)
    {
        for (auto* item : window.findChildren<QAction*>()) {
            if (!item->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence)))) continue;
            item->trigger(); settle(); return;
        }
        CHECK(false && "Missing shortcut action");
    }
    void undo() { shortcut("Ctrl+Z"); }
    void redo() { shortcut("Ctrl+Shift+Z"); }
    QPointF logical(core::Vec2d point) const
    {
        const auto extent = document().canvas().extent;
        const auto& scene = canvas->scene();
        const auto p = scene.viewport.documentToViewport(point,
            { double(extent.width), double(extent.height) }, scene.logicalViewport);
        return { p.x, p.y };
    }
    void press(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseMove, logical(p), Qt::NoButton, Qt::NoButton, mods);
        mouse(*canvas, QEvent::MouseButtonPress, logical(p), Qt::LeftButton, Qt::LeftButton, mods);
    }
    void move(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    { mouse(*canvas, QEvent::MouseMove, logical(p), Qt::NoButton, Qt::LeftButton, mods); }
    void release(core::Vec2d p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    { mouse(*canvas, QEvent::MouseButtonRelease, logical(p), Qt::LeftButton, Qt::NoButton, mods); }
    void key(QEvent::Type type, int value, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QKeyEvent event(type, value, mods);
        QCoreApplication::sendEvent(canvas, &event);
        settle();
    }
    void waitIdle()
    {
        CHECK(waitFor([this] {
            const auto* timer = window.findChild<QTimer*>(QStringLiteral("SelectionRasterizationTimer"));
            return !canvas->selectionDragging() && (!timer || !timer->isActive());
        }));
    }
    void ellipse(core::Vec2d first, core::Vec2d last, Qt::KeyboardModifiers mods = Qt::NoModifier)
    { press(first, mods); move(last, mods); release(last, mods); waitIdle(); }
};
int coverage(const core::SelectionState& mask, int x, int y)
{
    CHECK(mask);
    return mask ? mask->coverageAtDocumentPixel(x, y) : -1;
}
void equivalent(const core::SelectionState& actual, const core::SelectionState& expected)
{
    CHECK(actual && expected);
    if (actual && expected) CHECK(actual->equivalent(*expected));
}

void leadingModesShareTheSelectionFamilyAndDoNotMoveCanvas()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    auto* page = f.options->pageForTool(core::ToolId::Marquee);
    auto* modes = f.options->leadingWidgetForTool(core::ToolId::Marquee);
    auto* label = f.find<QLabel>("ToolOptionsContext");
    auto* rectangle = f.find<QToolButton>("SelectModeRectangle");
    auto* ellipse = f.find<QToolButton>("SelectModeEllipse");
    CHECK(page && modes && !page->isAncestorOf(modes));
    CHECK(label && label->text() == QStringLiteral("SELECT")); // Shared toolbar uses uppercase tool titles.
    CHECK(rectangle && ellipse && ellipse->isChecked() && !rectangle->isChecked());
    CHECK(rectangle && !rectangle->icon().isNull() && !rectangle->toolTip().isEmpty());
    CHECK(ellipse && !ellipse->icon().isNull() && !ellipse->toolTip().isEmpty());
    const auto geometry = f.workspace->canvasContainer()->geometry();
    for (int i = 0; i < 3; ++i) {
        f.button("SelectModeRectangle");
        CHECK(rectangle && rectangle->isChecked() && !ellipse->isChecked());
        f.button("SelectModeEllipse");
        f.shortcut("L");
        CHECK(modes && !modes->isVisible());
        CHECK(page == f.options->pageForTool(core::ToolId::Lasso));
        f.shortcut("M");
        CHECK(modes == f.options->leadingWidgetForTool(core::ToolId::Marquee));
        CHECK(modes && modes->isVisible());
        CHECK(ellipse && ellipse->isChecked());
        CHECK(f.workspace->canvasContainer()->geometry() == geometry);
    }
    CHECK(f.history().undoDepth() == 0);
    f.shortcut("B");
    QLineEdit editor(&f.window);
    editor.setGeometry(80, 80, 220, 35);
    editor.show(); editor.setFocus(); settle();
    QTest::keyClick(&editor, Qt::Key_M);
    CHECK(editor.text() == QStringLiteral("m"));
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Brush);
}

void subpixelEllipseIsSymmetricPreviewMatchedAndZoomIndependent()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    constexpr core::Vec2d a { 10.25, 8.25 }, b { 46.75, 36.75 };
    const auto revision = f.document().selectionRevision();
    const auto rasterRevision = f.surface().revision();
    f.press(a); f.move(b); settle();
    CHECK(!f.selection() && f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == 0 && f.surface().revision() == rasterRevision);
    const auto preview = f.canvas->scene().selectionEdges;
    CHECK(preview && !preview->empty());
    f.release(b); f.waitIdle();
    const auto expected = f.selection();
    CHECK(expected && f.history().undoDepth() == 1);
    if (!expected) return;
    CHECK(preview && *preview == expected->boundaryEdges());
    CHECK(coverage(expected, 28, 22) == 255);
    CHECK(coverage(expected, 10, 8) == 0);
    int partial = 0;
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
        const int c = coverage(expected, x, y);
        partial += c > 0 && c < 255;
        if (x <= 56) CHECK(c == coverage(expected, 56 - x, y));
        if (y <= 44) CHECK(c == coverage(expected, x, 44 - y));
    }
    CHECK(partial > 20);
    f.undo(); CHECK(!f.selection());
    const std::array<std::pair<core::Vec2d, core::Vec2d>, 4> directions {{
        { a, b }, { b, a }, { { a.x, b.y }, { b.x, a.y } }, { { b.x, a.y }, { a.x, b.y } }
    }};
    for (int zoom = 0; zoom < 2; ++zoom) {
        if (zoom) f.canvas->resetTo100Percent(); else f.canvas->fitDocumentToView();
        for (const auto& [first, last] : directions) {
            f.ellipse(first, last);
            equivalent(f.selection(), expected);
            f.undo(); CHECK(!f.selection());
        }
    }
    f.redo();
    equivalent(f.selection(), expected);
    CHECK(f.surface().revision() == rasterRevision);
}

void rectangleAndEllipseConstraintSeparateLatchedOperationFromRearmedShift()
{
    for (const char* mode : { "SelectModeRectangle", "SelectModeEllipse" }) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid())
            return;
        f.button(mode);
        // An inactive selection permits Shift from the initial press to constrain.
        f.ellipse({ 8, 8 }, { 28, 16 }, Qt::ShiftModifier);
        CHECK(f.selection() && f.selection()->bounds().width == f.selection()->bounds().height);
        f.undo();
        CHECK(!f.selection());
        // Starting without Shift also permits a later press to constrain, without
        // relatching Replace to Add. A release removes the temporary constraint.
        f.press({ 8, 8 });
        f.move({ 28, 16 });
        f.key(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        f.move({ 28, 16 }, Qt::ShiftModifier);
        f.key(QEvent::KeyRelease, Qt::Key_Shift);
        f.release({ 28, 16 });
        f.waitIdle();
        CHECK(f.selection() && f.selection()->bounds().width > f.selection()->bounds().height);
        const auto existing = f.selection();
        // With active coverage, initial Shift is Add, NOT a ratio constraint.
        f.ellipse({ 38, 8 }, { 58, 16 }, Qt::ShiftModifier);
        CHECK(coverage(f.selection(), 48, 12) == 255);
        CHECK(coverage(f.selection(), 48, 24) == 0);
        CHECK(coverage(f.selection(), 18, 12) == 255);
        f.undo();
        equivalent(f.selection(), existing);
        f.press({ 38, 8 }, Qt::ShiftModifier);
        f.move({ 58, 16 }, Qt::ShiftModifier);
        f.key(QEvent::KeyRelease, Qt::Key_Shift);
        f.move({ 58, 16 });
        f.key(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        f.move({ 58, 16 }, Qt::ShiftModifier);
        f.release({ 58, 16 }, Qt::ShiftModifier);
        f.waitIdle();
        f.key(QEvent::KeyRelease, Qt::Key_Shift);
        CHECK(coverage(f.selection(), 48, 24) == 255);
        CHECK(coverage(f.selection(), 18, 12) == 255); // Add remained latched.
        f.action("DeselectAction");
        f.ellipse({ 4, 4 }, { 4, 4 });
        CHECK(f.selection() && f.selection()->bounds().empty());
        // Active-empty is still an existing selection; initial Shift is Add only.
        f.ellipse({ 8, 8 }, { 28, 16 }, Qt::ShiftModifier);
        CHECK(f.selection() && f.selection()->bounds().width > f.selection()->bounds().height);
    }
}

void constrainedRectangleSnapsBeforeClippingInEveryDirectionAndAtEveryZoom()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.button("SelectModeRectangle");
    for (const auto start : { core::Vec2d { 28.25, 22.75 }, core::Vec2d { 28.75, 22.25 } }) {
        for (const auto delta : { core::Vec2d { 14.125, 5.125 }, core::Vec2d { -14.125, 5.125 },
                 core::Vec2d { 14.125, -5.125 }, core::Vec2d { -14.125, -5.125 },
                 core::Vec2d { 0, 14.125 }, core::Vec2d { 14.125, 0 } }) {
            core::SelectionState first;
            for (int zoom = 0; zoom < 2; ++zoom) {
                if (zoom)
                    f.canvas->resetTo100Percent();
                else
                    f.canvas->fitDocumentToView();
                f.ellipse(start, start + delta, Qt::ShiftModifier);
                CHECK(f.selection());
                if (!f.selection())
                    return;
                CHECK(f.selection()->bounds().width == f.selection()->bounds().height);
                CHECK(f.history().undoDepth() == 1);
                if (first)
                    equivalent(f.selection(), first);
                else
                    first = f.selection();
                f.undo();
                CHECK(!f.selection());
            }
        }
    }
    f.ellipse({ -6, 4 }, { 18, 11 }, Qt::ShiftModifier);
    CHECK(f.selection() && f.selection()->bounds() == core::RectI({ 0, 4, 18, 24 }));
    f.undo();
    CHECK(!f.selection());
    // Cancellation restores the existing mask and leaves its redo branch intact.
    f.ellipse({ 8, 8 }, { 20, 12 });
    const auto original = f.selection();
    f.ellipse({ 32, 8 }, { 44, 12 }, Qt::ShiftModifier);
    f.undo();
    equivalent(f.selection(), original);
    f.press({ 32, 8 }, Qt::ShiftModifier);
    f.move({ 44, 12 }, Qt::ShiftModifier);
    f.key(QEvent::KeyRelease, Qt::Key_Shift);
    f.key(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
    f.key(QEvent::KeyPress, Qt::Key_Escape, Qt::ShiftModifier);
    f.waitIdle();
    f.key(QEvent::KeyRelease, Qt::Key_Shift);
    equivalent(f.selection(), original);
    CHECK(f.history().redoDepth() == 1);
}

void combinationsRetainCommittedPreviewAndCombineOnlyOnce()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.ellipse({ 8, 8 }, { 34, 34 });
    const auto original = f.selection();
    f.undo();
    f.ellipse({ 20.25, 10.25 }, { 50.75, 30.75 });
    const auto incoming = f.selection();
    f.undo();
    f.ellipse({ 8, 8 }, { 34, 34 });
    const std::array modes { core::SelectionOperation::Add, core::SelectionOperation::Subtract,
        core::SelectionOperation::Intersect };
    const std::array names { "SelectionModeAdd", "SelectionModeSubtract", "SelectionModeIntersect" };
    for (std::size_t i = 0; i < modes.size(); ++i) {
        f.button(names[i]);
        const auto before = f.selection();
        const auto revision = f.document().selectionRevision();
        const auto depth = f.history().undoDepth();
        f.press({ 20.25, 10.25 }); f.move({ 50.75, 30.75 }); settle();
        CHECK(f.selection() == before && f.document().selectionRevision() == revision);
        const auto retained = f.canvas->scene().selectionRetainedEdges;
        CHECK(retained && *retained == before->boundaryEdges());
        CHECK(f.canvas->scene().selectionEdges && !f.canvas->scene().selectionEdges->empty());
        f.release({ 50.75, 30.75 }); f.waitIdle();
        equivalent(f.selection(), original->combined(*incoming, modes[i]));
        CHECK(f.history().undoDepth() == depth + 1);
        f.undo(); equivalent(f.selection(), original);
        f.press({ 20.25, 10.25 }); f.move({ 50.75, 30.75 });
        QTest::keyClick(f.canvas, Qt::Key_Escape); f.waitIdle();
        equivalent(f.selection(), original);
        CHECK(f.history().redoDepth() == 1);
    }
}

void unclippedGeometryEccentricAndTinyGesturesStaySafe()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.ellipse({ -10, 5 }, { 30, 45 });
    CHECK(coverage(f.selection(), 1, 12) == 255); // Vertex clamping would squeeze this out.
    CHECK(coverage(f.selection(), 0, 25) == 255);
    CHECK(coverage(f.selection(), 0, 5) == 0);
    const auto clipped = f.selection();
    f.undo(); f.ellipse({ 30, 45 }, { -10, 5 }); equivalent(f.selection(), clipped);
    f.undo();
    for (const auto& last : { core::Vec2d { 12.75, 40.75 }, core::Vec2d { 52.75, 10.75 } }) {
        f.ellipse({ 12.25, 10.25 }, last);
        CHECK(f.selection() && !f.selection()->bounds().empty());
        bool partial = false;
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
            const auto c = coverage(f.selection(), x, y);
            partial |= c > 0 && c < 255;
        }
        CHECK(partial);
        f.undo();
    }
    f.ellipse({ 12.25, 10.25 }, { 12.75, 10.75 });
    CHECK(coverage(f.selection(), 12, 10) > 0 && coverage(f.selection(), 12, 10) < 255);
    f.undo();
    f.ellipse({ 12.25, 10.25 }, { 12.25, 40.25 });
    CHECK(f.selection() && f.selection()->bounds().empty());
    f.action("DeselectAction"); f.undo();
    const auto revision = f.document().selectionRevision();
    const auto memory = f.history().memoryUsed();
    f.ellipse({ 12.25, 10.25 }, { 40.25, 10.25 });
    CHECK(f.history().undoDepth() == 1 && f.history().redoDepth() == 1);
    CHECK(f.document().selectionRevision() == revision && f.history().memoryUsed() == memory);
}

void selectedCoverageMovesButEllipseCornerDoesNotAndCancellationPreservesHistory()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.ellipse({ 8, 8 }, { 36, 36 });
    const auto original = f.selection();
    f.press({ 22, 22 }); f.move({ 27, 25 }); f.release({ 27, 25 }); f.waitIdle();
    equivalent(f.selection(), original->translated(5, 3));
    CHECK(f.history().undoDepth() == 2);
    f.undo();
    f.press({ 8.25, 8.25 }); f.move({ 28, 20 });
    CHECK(f.selection() == original); // Bounds corner is outside actual ellipse coverage.
    QTest::keyClick(f.canvas, Qt::Key_Escape); f.waitIdle();
    equivalent(f.selection(), original);
    CHECK(f.history().undoDepth() == 1 && f.history().redoDepth() == 1);
    for (int reason = 0; reason < 3; ++reason) {
        f.shortcut("M"); f.press({ 42, 5 }); f.move({ 60, 28 });
        if (reason == 0) {
            QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
            QCoreApplication::sendEvent(f.canvas, &event);
        } else if (reason == 1) f.shortcut("B");
        else {
            QPlatformSurfaceEvent event(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
            QCoreApplication::sendEvent(f.canvas, &event);
        }
        f.waitIdle(); equivalent(f.selection(), original);
        CHECK(f.history().undoDepth() == 1 && f.history().redoDepth() == 1);
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
}

void crossPanelCaptureAndCooperativeCancellation()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    QWidget receiver(f.workspace->panelOverlay());
    receiver.setGeometry(20, 70, 120, 50); receiver.show();
    f.press({ 8, 8 });
    const auto routes = f.router->routedEventCount();
    const auto global = f.canvas->mapToGlobal(f.logical({ 38, 34 }).toPoint());
    const auto local = QPointF(receiver.mapFromGlobal(global));
    mouse(receiver, QEvent::MouseMove, local, Qt::NoButton, Qt::LeftButton);
    mouse(receiver, QEvent::MouseButtonRelease, local, Qt::LeftButton, Qt::NoButton);
    f.waitIdle();
    CHECK(coverage(f.selection(), 22, 20) == 255);
    CHECK(f.history().undoDepth() == 1);
    CHECK(f.router->routedEventCount() >= routes + 2);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    receiver.hide();
    f.open(4096, 3072); f.shortcut("M"); f.button("SelectModeEllipse");
    f.ellipse({ 8, 8 }, { 36, 36 });
    const auto original = f.selection();
    const auto revision = f.document().selectionRevision();
    f.press({ 100, 100 }); f.move({ 4000, 3000 }); f.release({ 4000, 3000 });
    auto* timer = f.find<QTimer>("SelectionRasterizationTimer");
    CHECK(timer && timer->isActive());
    CHECK(f.selection() == original);
    QTest::keyClick(f.canvas, Qt::Key_Escape); f.waitIdle();
    CHECK(f.selection() == original && f.document().selectionRevision() == revision);
    CHECK(f.history().undoDepth() == 1);
}

void adjustmentsCopyFillBrushAndEraseReuseTheSameMask()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.ellipse({ 8.25, 8.25 }, { 48.75, 36.75 });
    const auto original = f.selection();
    const auto transform = f.layer().localToDocument;
    const auto revision = f.surface().revision();
    f.button("SelectionTransform");
    f.number("SelectionGrowHorizontal", 2); f.number("SelectionGrowVertical", -1);
    f.button("SelectionGrowApply"); equivalent(f.selection(), original->adjusted(2, -1));
    f.undo();
    f.shortcut("Ctrl+T");
    CHECK(f.canvas->scene().transformOverlay);
    f.number("TransformAngleControl", 17); f.button("TransformApply");
    CHECK(f.selection() && !f.selection()->equivalent(*original));
    CHECK(f.layer().localToDocument == transform && f.surface().revision() == revision);
    f.undo(); equivalent(f.selection(), original);
    auto* colors = dynamic_cast<ui::ColorSelector*>(f.find<QWidget>("ColorPanelColors"));
    CHECK(colors && colors->onColorsChanged);
    if (colors && colors->onColorsChanged)
        colors->onColorsChanged({ { 225, 80, 45, 255 }, { 0, 0, 0, 255 }, core::ColorSlot::Primary });
    f.action("FillForegroundAction");
    CHECK(waitFor([&] { return f.history().undoDepth() == 2; }));
    const auto filled = f.pixels();
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x)
        CHECK(f.alphaAt(x, y) == coverage(original, x, y));
    const auto sourceId = f.window.editorSession().activeLayer();
    f.action("LayerViaCopyAction");
    CHECK(f.document().layers().size() == 2 && f.window.editorSession().activeLayer() != sourceId);
    equivalent(f.selection(), original);
    f.undo(); CHECK(f.window.editorSession().activeLayer() == sourceId);
    f.undo(); CHECK(f.alphaAt(28, 22) == 0);
    f.shortcut("B");
    f.number("BrushSizeControl", 100); f.number("BrushHardnessControl", 100);
    f.number("BrushOpacityControl", 100); f.number("BrushFlowControl", 100);
    f.press({ 28, 22 }); f.release({ 28, 22 }); settle();
    CHECK(f.alphaAt(28, 22) == 255 && f.alphaAt(8, 8) == 0 && f.alphaAt(2, 2) == 0);
    equivalent(f.selection(), original);
    f.undo();
    f.action("FillForegroundAction"); CHECK(waitFor([&] { return f.pixels() == filled; }));
    f.shortcut("E"); f.press({ 28, 22 }); f.release({ 28, 22 }); settle();
    CHECK(f.alphaAt(28, 22) == 0);
    equivalent(f.selection(), original);
    f.undo(); CHECK(f.pixels() == filled);
    f.redo(); CHECK(f.alphaAt(28, 22) == 0);
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) return 77;
    std::atomic_uint64_t warnings { 0 }, errors { 0 };
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) return EXIT_FAILURE;
    instance.setLayers({ QByteArrayLiteral("VK_LAYER_KHRONOS_validation") });
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << "Vulkan ellipse validation: " << (data ? data->pMessage : "unknown") << '\n';
        return true;
    });
    if (!instance.create()) return EXIT_FAILURE;
    {
        Fixture f(&instance);
        CHECK(f.valid());
        if (!f.valid()) return EXIT_FAILURE;
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads >= 1; }));
        QTest::qWait(200);
        CHECK(waitFor([&] { return !f.canvas->presentationSuppressedForResize(); }));
        const auto geometry = f.workspace->canvasContainer()->geometry();
        const auto revision = f.surface().revision();
        const auto stats = f.canvas->rendererStats();
        f.press({ 8.25, 8.25 });
        for (int i = 1; i <= 12; ++i) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            f.move({ 8.25 + 3 * i, 8.25 + 2 * i });
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
            CHECK(!f.selection());
        }
        f.release({ 44.25, 32.25 }); f.waitIdle();
        const auto selected = f.selection();
        CHECK(selected && !selected->bounds().empty());
        f.button("SelectionModeAdd");
        f.press({ 32, 18 }); f.move({ 60, 44 });
        CHECK(f.canvas->scene().selectionRetainedEdges && !f.canvas->scene().selectionRetainedEdges->empty());
        const auto frame = f.canvas->rendererStats().framesSubmitted;
        CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        f.release({ 60, 44 }); f.waitIdle();
        f.undo(); equivalent(f.selection(), selected); f.redo();
        f.canvas->resetTo100Percent();
        f.button("SelectModeRectangle"); f.button("SelectModeEllipse");
        const auto after = f.canvas->rendererStats();
        CHECK(f.surface().revision() == revision);
        CHECK(after.fullUploads == stats.fullUploads && after.regionalUploads == stats.regionalUploads);
        CHECK(f.workspace->canvasContainer()->geometry() == geometry);
        CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
        if (const auto output = qEnvironmentVariable("IMAGEEDITOR_TEST_ELLIPSE_NATIVE_SCREENSHOT"); !output.isEmpty())
            CHECK(f.window.grab().save(output));
    }
    instance.destroy(); settle();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Native ellipse Vulkan validation: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

void reviewSheet(const QString& output)
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    QImage sheet(1160, 760, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor("#171a22"));
    QPainter painter(&sheet);
    painter.setPen(QColor("#dfe5f0"));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 16));
    painter.drawText(QRect(20, 14, 1120, 36), QStringLiteral("Ellipse Selection — actual pointer gestures / R8 coverage"));
    const std::array bounds {
        std::pair { core::Vec2d { 10, 4 }, core::Vec2d { 50, 44 } },
        std::pair { core::Vec2d { 4.25, 14.25 }, core::Vec2d { 59.75, 32.75 } },
        std::pair { core::Vec2d { -10, 5 }, core::Vec2d { 30, 45 } },
        std::pair { core::Vec2d { 26.25, 4.25 }, core::Vec2d { 29.75, 43.75 } },
        std::pair { core::Vec2d { 26.25, 18.25 }, core::Vec2d { 29.75, 21.75 } },
        std::pair { core::Vec2d { 50.75, 40.75 }, core::Vec2d { 6.25, 5.25 } }
    };
    const std::array labels { "Circle", "Subpixel ellipse", "Unclipped off-canvas bounds",
        "Narrow ellipse", "Small circle", "Reverse drag" };
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 11));
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        f.ellipse(bounds[i].first, bounds[i].second);
        const auto mask = f.selection();
        CHECK(mask);
        QImage coverageImage(64, 48, QImage::Format_RGB32);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
            const int c = coverage(mask, x, y);
            coverageImage.setPixelColor(x, y, QColor(c, c, c));
        }
        const int left = 20 + int(i % 3) * 380, top = 75 + int(i / 3) * 330;
        painter.drawText(QRect(left, top, 360, 30), QString::fromLatin1(labels[i]));
        painter.drawImage(QRect(left, top + 36, 352, 264), coverageImage);
        f.undo();
    }
    painter.end(); CHECK(sheet.save(output));
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("EllipseSelectionInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    if (qEnvironmentVariableIsSet("IMAGEEDITOR_TEST_ELLIPSE_NATIVE")) return nativeValidation();
    if (const auto output = qEnvironmentVariable("IMAGEEDITOR_TEST_ELLIPSE_PREVIEW"); !output.isEmpty()) {
        reviewSheet(output); return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    leadingModesShareTheSelectionFamilyAndDoNotMoveCanvas();
    subpixelEllipseIsSymmetricPreviewMatchedAndZoomIndependent();
    rectangleAndEllipseConstraintSeparateLatchedOperationFromRearmedShift();
    constrainedRectangleSnapsBeforeClippingInEveryDirectionAndAtEveryZoom();
    combinationsRetainCommittedPreviewAndCombineOnlyOnce();
    unclippedGeometryEccentricAndTinyGesturesStaySafe();
    selectedCoverageMovesButEllipseCornerDoesNotAndCancellationPreservesHistory();
    crossPanelCaptureAndCooperativeCancellation();
    adjustmentsCopyFillBrushAndEraseReuseTheSameMask();
    if (failures) std::cerr << failures << " ellipse interaction assertion(s) failed\n";
    else std::cout << "All ellipse selection interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
