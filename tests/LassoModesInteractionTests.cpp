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
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
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
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
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
bool near(core::Vec2d a, core::Vec2d b) { return std::hypot(a.x - b.x, a.y - b.y) < 1e-7; }
bool sameEdges(const std::vector<core::SelectionEdge>& a, const std::vector<core::SelectionEdge>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
        return near(x.from, y.from) && near(x.to, y.to);
    });
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::ToolOptionsBar* options {};
    ui::OverlayDockWorkspace* workspace {};
    ui::CrossWindowPointerRouter* router {};
    explicit Fixture(QVulkanInstance* instance = nullptr) : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1640, 900);
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        options = dynamic_cast<ui::ToolOptionsBar*>(window.findChild<QToolBar*>(QStringLiteral("ToolOptionsBar")));
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        open();
    }
    ~Fixture() { window.close(); settle(); }
    bool valid() const { return canvas && options && workspace && router && window.editorSession().document(); }
    const core::Document& document() const { return *window.editorSession().document(); }
    const core::History& history() const { return window.editorSession().history(); }
    core::SelectionState selection() const { return document().selection(); }
    const core::RasterSurface& surface() const
    {
        return *std::get<core::RasterLayer>(document().layer(*window.editorSession().activeLayer())->payload).surface;
    }
    std::vector<std::byte> pixels() const
    {
        const auto extent = surface().extent();
        std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4);
        surface().copyRgba8({0,0,int(extent.width),int(extent.height)}, result, extent.width * 4);
        return result;
    }
    int alpha(int x, int y) const
    {
        std::array<std::byte,4> data {};
        surface().copyRgba8({x,y,1,1},data,4);
        return std::to_integer<int>(data[3]);
    }
    void open(int width = 96, int height = 72, bool edge = false)
    {
        QImage image(width,height,QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        if (edge) {
            image.fill(QColor(22,28,35,255));
            for (int y=18; y<height-18; ++y)
                for (int x=24; x<width-24; ++x)
                    image.setPixelColor(x,y,QColor(235,222,200,255));
        }
        const auto path=assets.filePath(QStringLiteral("lasso-modes-source.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        settle();
    }
    void action(const char* name)
    {
        auto* target=window.findChild<QAction*>(QString::fromLatin1(name));
        CHECK(target);
        if (target) target->trigger();
        settle();
    }
    void keyAction(const char* sequence)
    {
        QAction* target=nullptr;
        for (auto* candidate : window.findChildren<QAction*>())
            if (candidate->shortcuts().contains(QKeySequence(QString::fromLatin1(sequence)))) { target=candidate; break; }
        CHECK(target);
        if (target) target->trigger();
        settle();
    }
    QToolButton* control(const char* name) const { return window.findChild<QToolButton*>(QString::fromLatin1(name)); }
    void button(const char* name) { auto* target=control(name); CHECK(target); if (target) target->click(); settle(); }
    void mode(const char* name) { action("ToolAction_lasso"); button(name); }
    QPointF logical(core::Vec2d p) const
    {
        const auto extent=document().canvas().extent;
        const auto& scene=canvas->scene();
        const auto result=scene.viewport.documentToViewport(p,{double(extent.width),double(extent.height)},scene.logicalViewport);
        return {result.x,result.y};
    }
    void hover(core::Vec2d p, Qt::KeyboardModifiers mods=Qt::NoModifier)
    { mouse(*canvas,QEvent::MouseMove,logical(p),Qt::NoButton,Qt::NoButton,mods); }
    void press(core::Vec2d p, Qt::KeyboardModifiers mods=Qt::NoModifier)
    { hover(p,mods); mouse(*canvas,QEvent::MouseButtonPress,logical(p),Qt::LeftButton,Qt::LeftButton,mods); }
    void drag(core::Vec2d p, Qt::KeyboardModifiers mods=Qt::NoModifier)
    { mouse(*canvas,QEvent::MouseMove,logical(p),Qt::NoButton,Qt::LeftButton,mods); }
    void release(core::Vec2d p, Qt::KeyboardModifiers mods=Qt::NoModifier)
    { mouse(*canvas,QEvent::MouseButtonRelease,logical(p),Qt::LeftButton,Qt::NoButton,mods); }
    void click(core::Vec2d p, Qt::KeyboardModifiers mods=Qt::NoModifier)
    {
        press(p,mods); release(p,mods);
        // Human clicks are separate event-loop turns. Settle the cooperative
        // magnetic live-wire request before assertions about a committed anchor.
        if (auto* magnetic=control("LassoModeMagnetic"); magnetic && magnetic->isChecked())
            CHECK(waitFor([&] {
                const auto* timer=window.findChild<QTimer*>(QStringLiteral("MagneticLassoTimer"));
                return !timer || !timer->isActive();
            }));
    }
    void doubleClick(core::Vec2d p)
    {
        click(p);
        mouse(*canvas,QEvent::MouseButtonDblClick,logical(p),Qt::LeftButton,Qt::LeftButton);
        release(p);
    }
    void key(Qt::Key k) { QTest::keyClick(canvas,k); settle(); }
    void waitIdle()
    {
        CHECK(waitFor([&] {
            const auto* timer=window.findChild<QTimer*>(QStringLiteral("SelectionRasterizationTimer"));
            return !canvas->selectionConstructionActive() && !canvas->selectionDragging()
                && !canvas->scene().selectionPathPreview && (!timer || !timer->isActive());
        }));
    }
    void cancel() { key(Qt::Key_Escape); waitIdle(); }
    void undo() { keyAction("Ctrl+Z"); }
    void redo() { keyAction("Ctrl+Shift+Z"); }
    void rectangle(core::Vec2d from,core::Vec2d to)
    { keyAction("M"); press(from); drag(to); release(to); waitIdle(); }
    void freehand(const std::vector<core::Vec2d>& points,Qt::KeyboardModifiers mods=Qt::NoModifier)
    {
        mode("LassoModeFreehand");
        CHECK(!points.empty());
        if (points.empty()) return;
        press(points.front(),mods);
        for (std::size_t i=1;i<points.size();++i) drag(points[i]);
        release(points.back()); waitIdle();
    }
    void polygon(const std::vector<core::Vec2d>& points,Qt::KeyboardModifiers mods=Qt::NoModifier)
    {
        mode("LassoModePolygonal");
        CHECK(!points.empty());
        if (points.empty()) return;
        click(points.front(),mods);
        for (std::size_t i=1;i<points.size();++i) click(points[i]);
        key(Qt::Key_Return); waitIdle();
    }
};
int coverage(const core::SelectionState& selection,int x,int y)
{ CHECK(selection); return selection ? selection->coverageAtDocumentPixel(x,y) : -1; }
const std::vector<core::Vec2d> concave {{8.25,8.25},{69.25,8.25},{69.25,24.25},{35.25,24.25},{35.25,57.25},{8.25,57.25}};

void threeModesShareCachedControlsAndKeepShortcutOwnership()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const auto geometry=f.workspace->canvasContainer()->geometry();
    auto* page=f.options->pageForTool(core::ToolId::Lasso);
    CHECK(page && page==f.options->pageForTool(core::ToolId::Marquee));
    for (const auto* name : {"LassoModeFreehand","LassoModePolygonal","LassoModeMagnetic"}) {
        f.mode(name);
        auto* button=f.control(name);
        CHECK(button && button->isVisible() && button->isChecked());
        CHECK(button && !button->icon().isNull() && !button->toolTip().isEmpty());
        CHECK(f.options->pageForTool(core::ToolId::Lasso)==page);
        CHECK(f.workspace->canvasContainer()->geometry()==geometry);
    }
    auto* radius=f.window.findChild<QDoubleSpinBox*>(QStringLiteral("MagneticRadiusControl"));
    CHECK(radius && radius->isVisible() && radius->value()==12);
    CHECK(f.control("MagneticSourceMergedVisible") && f.control("MagneticSourceMergedVisible")->isChecked());
    f.keyAction("M");
    for (const auto* name : {"LassoModeFreehand","LassoModePolygonal","LassoModeMagnetic"})
        CHECK(f.control(name) && !f.control(name)->isVisible());
    f.canvas->requestActivate(); settle(); f.key(Qt::Key_L);
    CHECK(f.window.editorSession().activeTool()==core::ToolId::Lasso);
    CHECK(f.control("LassoModeMagnetic")->isChecked());
    QLineEdit editor(&f.window); editor.setGeometry(80,80,220,35); editor.show(); editor.setFocus(); settle();
    QTest::keyClicks(&editor,"l");
    QTest::keyClick(&editor,Qt::Key_Backspace);
    CHECK(editor.text().isEmpty());
    CHECK(f.history().undoDepth()==0);
}

void additivePreviewRetainsCommittedContoursForEverySelectionTool()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.rectangle({5,5},{25,30});
    const auto before=f.selection();
    const auto boundary=before->boundaryEdges();
    const auto revision=f.document().selectionRevision();
    const auto depth=f.history().undoDepth();
    for (const auto* mode : {"marquee","LassoModeFreehand","LassoModePolygonal","LassoModeMagnetic"}) {
        if (QString::fromLatin1(mode)==QStringLiteral("marquee")) f.keyAction("M"); else f.mode(mode);
        for (const auto* operation : {"SelectionModeAdd","SelectionModeSubtract","SelectionModeIntersect"}) {
            f.button(operation);
            const bool persistent=QString::fromLatin1(mode)==QStringLiteral("LassoModePolygonal")
                || QString::fromLatin1(mode)==QStringLiteral("LassoModeMagnetic");
            if (persistent) { f.click({40,12}); f.hover({70,39}); }
            else { f.press({40,12}); f.drag({70,39}); }
            const auto& scene=f.canvas->scene();
            CHECK(f.selection()==before && f.document().selectionRevision()==revision);
            CHECK(f.history().undoDepth()==depth);
            CHECK(scene.selectionRetainedEdges && sameEdges(*scene.selectionRetainedEdges,boundary));
            CHECK(scene.selectionPathPreview && scene.selectionEdges && !scene.selectionEdges->empty());
            CHECK(QString::fromLatin1(mode)==QStringLiteral("marquee")
                ? scene.selectionEdges->size()==4 : scene.selectionClosingEdges>0);
            f.cancel();
            CHECK(f.selection()==before && f.document().selectionRevision()==revision);
            CHECK(!f.canvas->scene().selectionRetainedEdges);
        }
    }
    // Replace intentionally shows only the incoming path until commit.
    f.mode("LassoModeFreehand"); f.button("SelectionModeReplace");
    f.press({40,12}); f.drag({70,39});
    CHECK(!f.canvas->scene().selectionRetainedEdges);
    CHECK(f.canvas->scene().selectionPathPreview);
    CHECK(f.canvas->scene().selectionClosingEdges==1);
    f.cancel();
}

void polygonClosureRemovalAndWindingUseTheApprovedMaskRasterizer()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.freehand(concave); const auto expected=f.selection(); f.undo();
    f.polygon(concave);
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    CHECK(f.history().undoDepth()==1);
    f.undo();
    f.mode("LassoModePolygonal");
    for (std::size_t i=0;i+1<concave.size();++i) f.click(concave[i]);
    f.hover(concave.back());
    CHECK(f.canvas->selectionConstructionActive() && !f.canvas->selectionDragging());
    CHECK(!f.selection() && f.history().undoDepth()==0 && f.history().redoDepth()==1);
    f.key(Qt::Key_Return); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.undo();
    auto reversed=concave; std::reverse(reversed.begin(),reversed.end());
    f.polygon(reversed);
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.undo();
    f.mode("LassoModePolygonal");
    for (const auto point : concave) f.click(point);
    f.click({80,65}); // Remove a deliberately incorrect final anchor, without history.
    const auto anchors=f.canvas->scene().selectionAnchorCount;
    f.key(Qt::Key_Backspace);
    CHECK(f.canvas->selectionConstructionActive());
    CHECK(f.canvas->scene().selectionAnchorCount<anchors);
    CHECK(f.history().undoDepth()==0 && f.history().redoDepth()==1);
    f.hover(concave.back());
    f.key(Qt::Key_Return); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.undo();
    f.mode("LassoModePolygonal");
    for (const auto point : concave) f.click(point);
    f.click(concave.front()); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    f.undo();
    f.mode("LassoModePolygonal");
    for (std::size_t i=0;i+1<concave.size();++i) f.click(concave[i]);
    f.doubleClick(concave.back()); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    CHECK(f.history().undoDepth()==1);
}

void polygonDegeneraciesSelfCrossingsAndOffCanvasVerticesStayDeterministic()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const std::vector<core::Vec2d> eight {{8,8},{74,58},{8,58},{74,8},{74,8}};
    f.polygon(eight);
    CHECK(coverage(f.selection(),40,12)==255 && coverage(f.selection(),40,54)==255);
    CHECK(coverage(f.selection(),10,32)==0);
    const auto crossing=f.selection(); f.undo();
    auto reversed=eight; std::reverse(reversed.begin(),reversed.end());
    f.polygon(reversed);
    CHECK(f.selection() && f.selection()->equivalent(*crossing));
    f.undo();
    const std::vector<core::Vec2d> clipped {{-10.25,12.25},{12.25,-10.25},{34.75,12.25},{12.25,34.75}};
    f.polygon(clipped);
    CHECK(coverage(f.selection(),3,3)==255 && coverage(f.selection(),0,0)==0);
    CHECK(coverage(f.selection(),12,0)==255 && coverage(f.selection(),0,12)==255);
    const auto outside=f.selection(); f.undo();
    f.canvas->resetTo100Percent();
    f.polygon(clipped);
    CHECK(f.selection() && f.selection()->equivalent(*outside));
    f.undo();
    f.polygon({{8.2,9.4},{8.2,9.4},{30.2,9.4},{60.2,9.4}});
    CHECK(f.selection() && f.selection()->bounds().empty());
    const auto revision=f.document().selectionRevision();
    f.action("DeselectAction"); f.undo();
    f.polygon({{8.2,9.4},{8.2,9.4},{30.2,9.4},{60.2,9.4}});
    CHECK(f.history().undoDepth()==1 && f.history().redoDepth()==1);
    CHECK(f.document().selectionRevision()==revision+2);
}

void zoomChangesAndModifierLatchingDoNotChangeConstructionGeometry()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.rectangle({3,3},{22,28});
    const auto before=f.selection();
    f.mode("LassoModePolygonal");
    f.click({35.25,7.25},Qt::ShiftModifier);
    f.click({70.25,7.25});
    const auto revision=f.document().selectionRevision();
    f.canvas->resetTo100Percent(); settle();
    CHECK(f.canvas->selectionConstructionActive());
    f.click({70.25,46.25},Qt::AltModifier);
    f.hover({35.25,46.25});
    CHECK(f.selection()==before && f.document().selectionRevision()==revision);
    f.key(Qt::Key_Return); f.waitIdle();
    CHECK(coverage(f.selection(),10,10)==255 && coverage(f.selection(),50,25)==255);
    CHECK(coverage(f.selection(),28,15)==0);
    f.undo();
    f.mode("LassoModePolygonal");
    // This checks raw pointer routing, not alignment. Selection movement now
    // snaps: at 100% the proposed y=5 is within the canvas-edge capture radius.
    // Dedicated selection-snapping tests cover that intentional correction.
    f.window.findChild<QAction*>(QStringLiteral("SnappingAction"))->setChecked(false);
    f.press({7,7}); f.drag({11,9});
    CHECK(!f.canvas->scene().selectionPathPreview); // Existing coverage takes shared Move precedence.
    f.release({11,9}); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*before->translated(4,2)));
    f.undo();
    f.mode("LassoModeMagnetic");
    f.press({7,7}); f.drag({11,9}); f.release({11,9}); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*before->translated(4,2)));
}

void unfinishedPathsCancelWithoutHistoryAndReferenceChangesInvalidateMagneticPaths()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.rectangle({2,2},{22,22});
    f.action("InvertSelectionAction"); f.undo();
    const auto before=f.selection();
    const auto revision=f.document().selectionRevision();
    const auto depth=f.history().undoDepth(),redo=f.history().redoDepth();
    for (const auto* mode : {"LassoModePolygonal","LassoModeMagnetic"}) {
        for (int reason=0;reason<4;++reason) {
            f.mode(mode); f.click({35,8}); f.click({75,8}); f.hover({60,55});
            CHECK(f.canvas->selectionConstructionActive());
            CHECK(f.selection()==before && f.history().undoDepth()==depth && f.history().redoDepth()==redo);
            if (reason==0) f.key(Qt::Key_Escape);
            if (reason==1) { QFocusEvent event(QEvent::FocusOut,Qt::OtherFocusReason); QCoreApplication::sendEvent(f.canvas,&event); }
            if (reason==2) f.keyAction("B");
            if (reason==3) f.button(QString::fromLatin1(mode)==QStringLiteral("LassoModeMagnetic")
                ? "LassoModePolygonal" : "LassoModeMagnetic");
            f.waitIdle();
            CHECK(f.selection()==before && f.document().selectionRevision()==revision);
            CHECK(f.history().undoDepth()==depth && f.history().redoDepth()==redo);
            CHECK(f.router->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
            CHECK(QWidget::mouseGrabber()==nullptr);
        }
    }
    f.mode("LassoModeMagnetic"); f.click({35,8}); f.hover({75,8});
    f.button("MagneticSourceActiveLayer"); f.waitIdle();
    CHECK(f.selection()==before && f.history().undoDepth()==depth && f.history().redoDepth()==redo);
}

void magneticWeakEdgesManualFallbackAndAnchorsRemainStable()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.mode("LassoModeMagnetic");
    f.click({15.25,12.25}); f.hover({70.25,12.25});
    CHECK(f.canvas->selectionConstructionActive() && !f.selection());
    f.click({70.25,12.25},Qt::ControlModifier);
    const auto prefixCount=f.canvas->scene().selectionStablePrefix;
    const auto anchored=*f.canvas->scene().selectionEdges;
    f.hover({70.25,54.25});
    const auto& updated=*f.canvas->scene().selectionEdges;
    CHECK(prefixCount>0 && updated.size()>=prefixCount);
    for (std::size_t i=0;i<std::min(prefixCount,updated.size());++i) {
        CHECK(near(anchored[i].from,updated[i].from));
        CHECK(near(anchored[i].to,updated[i].to));
    }
    f.click({70.25,54.25},Qt::ControlModifier);
    const auto count=f.canvas->scene().selectionAnchorCount;
    f.key(Qt::Key_Delete);
    CHECK(f.canvas->scene().selectionAnchorCount==count); // Delete no longer removes anchors.
    f.key(Qt::Key_Backspace);
    CHECK(f.canvas->scene().selectionAnchorCount<count);
    f.click({70.25,54.25},Qt::ControlModifier);
    f.click({15.25,54.25},Qt::ControlModifier);
    f.key(Qt::Key_Return); f.waitIdle();
    CHECK(coverage(f.selection(),40,30)==255);
    CHECK(coverage(f.selection(),5,30)==0);
    CHECK(f.history().undoDepth()==1);
    const auto result=f.selection(); f.undo(); CHECK(!f.selection()); f.redo();
    CHECK(f.selection() && f.selection()->equivalent(*result));
}

void constructionReleasesCrossPanelCaptureAndPendingCompletionCanBeCancelled()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    QWidget receiver(f.workspace->panelOverlay());
    receiver.setGeometry(20,70,120,50); receiver.show();
    for (const auto* mode : {"LassoModePolygonal","LassoModeMagnetic"}) {
        f.mode(mode); f.press({12,12});
        const auto routed=f.router->routedEventCount();
        const auto global=f.canvas->mapToGlobal(f.logical({40,20}).toPoint());
        const auto local=QPointF(receiver.mapFromGlobal(global));
        mouse(receiver,QEvent::MouseMove,local,Qt::NoButton,Qt::LeftButton);
        mouse(receiver,QEvent::MouseButtonRelease,local,Qt::LeftButton,Qt::NoButton);
        CHECK(f.router->routedEventCount()>=routed+2);
        CHECK(f.router->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(f.canvas->selectionConstructionActive() && !f.canvas->selectionDragging());
        CHECK(!f.selection() && f.history().undoDepth()==0);
        f.cancel();
    }
    f.open(2048,1536); f.mode("LassoModePolygonal");
    f.click({20,20}); f.click({2000,20}); f.click({2000,1500}); f.click({20,1500});
    // Deliver Enter without pumping the timer. Completion is cooperative and
    // Escape must cancel after the multi-click interaction has already ended.
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
    QCoreApplication::sendEvent(f.canvas,&enter);
    auto* timer=f.window.findChild<QTimer*>(QStringLiteral("SelectionRasterizationTimer"));
    CHECK(timer && timer->isActive());
    CHECK(!f.selection() && f.history().undoDepth()==0);
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QCoreApplication::sendEvent(f.canvas,&escape);
    f.waitIdle(); QTest::qWait(10);
    CHECK(!f.selection() && f.history().undoDepth()==0 && f.history().redoDepth()==0);
    CHECK(!timer || !timer->isActive());
}

void constructionCannotBecomeAMaskMoveAndManualAnchorsClearTheGuideLimit()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.rectangle({30,20},{60,50});
    const auto before=f.selection();
    for (const auto* mode : {"LassoModePolygonal","LassoModeMagnetic"}) {
        f.mode(mode); f.button("SelectionModeReplace");
        f.click({10,10});
        // An unfinished path already owns its interaction. Clicking old
        // coverage must add an anchor, not advertise or start a mask move.
        f.press({40,30},Qt::ControlModifier);
        CHECK(f.canvas->cursor().shape()==Qt::CrossCursor);
        f.release({40,30},Qt::ControlModifier);
        CHECK(f.selection()==before && f.canvas->selectionConstructionActive());
        f.cancel();
    }
    f.action("DeselectAction"); f.mode("LassoModeMagnetic"); f.click({10,20});
    // Do not pump the event loop: approach the 510-point guide safety cap,
    // then let Ctrl+press itself add the point that requests auto-anchoring.
    for (int i=1;i<=508;++i) f.hover({10+i*0.1,20+(i%2?2.0:-2.0)});
    mouse(*f.canvas,QEvent::MouseButtonPress,f.logical({75,35}),Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier);
    f.release({75,35},Qt::ControlModifier);
    CHECK(f.canvas->scene().selectionAnchorCount==2);
    f.hover({80,40});
    const auto& scene=f.canvas->scene();
    CHECK(scene.selectionEdges && scene.selectionClosingEdges>0);
    if (scene.selectionEdges && scene.selectionClosingEdges>0) {
        const auto closing=scene.selectionEdges->size()-scene.selectionAnchorCount-scene.selectionClosingEdges;
        CHECK(near((*scene.selectionEdges)[closing].from,{80,40}));
    }
    f.cancel();
}

void rapidMagneticAnchorRequestsRetainExplicitClicksAndClosure()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.mode("LassoModeMagnetic");
    // No event-loop turns: explicit anchors and the final Enter must not be
    // silently dropped while earlier cooperative searches are still pending.
    for (const auto p : std::vector<core::Vec2d>{{15,12},{70,12},{70,54},{15,54}}) {
        f.press(p); f.release(p);
    }
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
    QCoreApplication::sendEvent(f.canvas,&enter);
    f.waitIdle();
    CHECK(coverage(f.selection(),40,30)==255);
    CHECK(coverage(f.selection(),5,30)==0 && coverage(f.selection(),85,30)==0);
    CHECK(f.history().undoDepth()==1);
    const auto expected=f.selection(); f.undo();
    f.mode("LassoModeMagnetic");
    for (const auto p : std::vector<core::Vec2d>{{15,12},{70,12},{70,54},{15,54}})
        f.click(p);
    f.key(Qt::Key_Return); f.waitIdle();
    CHECK(f.selection() && f.selection()->equivalent(*expected));
}

void rightClickClosesAnchoredLassosLikeEnterWithoutAddingAnEndpoint()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const std::array<core::Vec2d, 4> points {{{15.25,12.25},{70.25,12.25},{70.25,54.25},{15.25,54.25}}};
    const core::Vec2d unrelatedClick {90,65};
    for (const auto* mode : {"LassoModePolygonal", "LassoModeMagnetic"}) {
        const auto construct = [&] {
            f.mode(mode);
            for (std::size_t i=0; i+1<points.size(); ++i) f.click(points[i]);
            f.hover(points.back());
        };
        construct();
        f.key(Qt::Key_Return); f.waitIdle();
        const auto expected=f.selection();
        CHECK(expected && !expected->bounds().empty());
        f.undo();
        const auto depth=f.history().undoDepth();
        construct();
        CHECK(f.canvas->selectionConstructionActive());
        // Completion uses the existing live endpoint, just like Enter. A right
        // press is not another anchor and must not feed its position to the path.
        mouse(*f.canvas,QEvent::MouseButtonPress,f.logical(unrelatedClick),Qt::RightButton,Qt::RightButton);
        mouse(*f.canvas,QEvent::MouseButtonRelease,f.logical(unrelatedClick),Qt::RightButton,Qt::NoButton);
        QContextMenuEvent menu(QContextMenuEvent::Mouse,f.logical(unrelatedClick).toPoint(),
            f.canvas->mapToGlobal(f.logical(unrelatedClick).toPoint()));
        menu.setAccepted(false);
        QCoreApplication::sendEvent(f.canvas,&menu);
        CHECK(menu.isAccepted());
        f.waitIdle();
        CHECK(expected && f.selection() && f.selection()->equivalent(*expected));
        CHECK(f.history().undoDepth()==depth+1 && f.history().redoDepth()==0);
        const auto selection=f.selection();
        const auto revision=f.document().selectionRevision();
        // A delayed release from the preceding anchor must not complete twice,
        // update the endpoint, or start a selection move.
        f.release(unrelatedClick);
        settle();
        CHECK(f.selection()==selection && f.document().selectionRevision()==revision);
        CHECK(f.history().undoDepth()==depth+1);
        CHECK(f.router->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
        f.undo();
        f.redo();
        CHECK(expected && f.selection() && f.selection()->equivalent(*expected));
        f.undo();
    }
}

void rightClickQueuesMagneticClosureWithoutLosingPendingAnchors()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    const std::array<core::Vec2d,4> points {{{15,12},{70,12},{70,54},{15,54}}};
    const auto constructWithoutPumpingTimers = [&] {
        f.mode("LassoModeMagnetic");
        for (const auto point : points) { f.press(point); f.release(point); }
    };
    constructWithoutPumpingTimers();
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
    QCoreApplication::sendEvent(f.canvas,&enter);
    f.waitIdle();
    const auto expected=f.selection();
    CHECK(expected && !expected->bounds().empty());
    f.undo();
    constructWithoutPumpingTimers();
    CHECK(f.canvas->selectionConstructionActive());
    CHECK(f.history().undoDepth()==0 && f.history().redoDepth()==1);
    // The earlier anchors still have cooperative edge searches queued here.
    // Right completion must use exactly Enter's queued-close path.
    mouse(*f.canvas,QEvent::MouseButtonPress,f.logical(points.back()),Qt::RightButton,Qt::RightButton);
    mouse(*f.canvas,QEvent::MouseButtonRelease,f.logical(points.back()),Qt::RightButton,Qt::NoButton);
    f.waitIdle();
    CHECK(expected && f.selection() && f.selection()->equivalent(*expected));
    CHECK(f.history().undoDepth()==1 && f.history().redoDepth()==0);
    CHECK(f.router->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
}

void rightClickFinishesHeldFreehandOnceAndDoesNotDisableLaterContextMenus()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.freehand(concave);
    const auto expected=f.selection();
    f.undo();
    f.mode("LassoModeFreehand");
    f.press(concave.front());
    for (std::size_t i=1; i<concave.size(); ++i) f.drag(concave[i]);
    CHECK(f.canvas->selectionDragging());
    const core::Vec2d unrelatedClick {90,65};
    mouse(*f.canvas,QEvent::MouseButtonPress,f.logical(unrelatedClick),Qt::RightButton,
        Qt::LeftButton|Qt::RightButton);
    CHECK(!f.canvas->selectionDragging());
    // Platforms may send their context-menu event at press rather than release.
    QContextMenuEvent menu(QContextMenuEvent::Mouse,f.logical(unrelatedClick).toPoint(),
        f.canvas->mapToGlobal(f.logical(unrelatedClick).toPoint()));
    menu.setAccepted(false);
    QCoreApplication::sendEvent(f.canvas,&menu);
    CHECK(menu.isAccepted());
    mouse(*f.canvas,QEvent::MouseButtonRelease,f.logical(unrelatedClick),Qt::RightButton,Qt::LeftButton);
    f.drag(unrelatedClick);
    f.release(unrelatedClick);
    f.waitIdle();
    CHECK(expected && f.selection() && f.selection()->equivalent(*expected));
    CHECK(f.history().undoDepth()==1 && f.history().redoDepth()==0);
    CHECK(f.router->captureDomain()==ui::CrossWindowPointerRouter::CaptureDomain::None);
    const auto revision=f.document().selectionRevision();
    f.release(unrelatedClick);
    settle();
    CHECK(f.document().selectionRevision()==revision && f.history().undoDepth()==1);
    f.undo();
    f.redo();
    CHECK(expected && f.selection() && f.selection()->equivalent(*expected));

    // A new, ordinary right click is not swallowed by stale finish suppression.
    mouse(*f.canvas,QEvent::MouseButtonPress,f.logical(unrelatedClick),Qt::RightButton,Qt::RightButton);
    mouse(*f.canvas,QEvent::MouseButtonRelease,f.logical(unrelatedClick),Qt::RightButton,Qt::NoButton);
    QContextMenuEvent ordinaryMenu(QContextMenuEvent::Mouse,f.logical(unrelatedClick).toPoint(),
        f.canvas->mapToGlobal(f.logical(unrelatedClick).toPoint()));
    ordinaryMenu.setAccepted(false);
    QCoreApplication::sendEvent(f.canvas,&ordinaryMenu);
    CHECK(!ordinaryMenu.isAccepted());
    CHECK(f.document().selectionRevision()==revision+2 && f.history().undoDepth()==1);
}

void sustainedMagneticTracingResolvesAndAutoAnchorsDuring180HzInput()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.open(3840,2160,true);
    f.mode("LassoModeMagnetic");
    const auto rasterRevision=f.surface().revision();
    const auto selectionRevision=f.document().selectionRevision();
    // A two-thousand-document-pixel trace along a real high-contrast image
    // edge. Slight hand-like variation avoids an artificially perfect guide.
    // There are no manual anchors or pauses after the initial click.
    const core::Vec2d first{128,18};
    f.click(first);
    constexpr int samples=180;
    constexpr double travel=2000;
    constexpr qint64 intervalNs=1'000'000'000LL/180;
    int delivered=0, prefixChanges=0, changesBeforeLastQuarter=0;
    std::size_t previousPrefix=0, maximumPendingSegments=0;
    double maximumPendingDistanceAfterHalf=0, maximumAnchoredLength=0;
    qint64 maximumHandlerNs=0,maximumDeliveryLagNs=0;
    QElapsedTimer clock; clock.start();
    QTimer input;
    input.setSingleShot(true);
    input.setTimerType(Qt::PreciseTimer);
    QObject::connect(&input,&QTimer::timeout,&f.window,[&] {
        const int sample=++delivered;
        maximumDeliveryLagNs=std::max(maximumDeliveryLagNs,
            std::max<qint64>(0,clock.nsecsElapsed()-intervalNs*sample));
        const core::Vec2d point{first.x+travel*sample/samples,
            first.y+.6*std::sin(sample*.23)};
        QElapsedTimer handler; handler.start();
        f.hover(point);
        maximumHandlerNs=std::max(maximumHandlerNs,handler.nsecsElapsed());
        const auto& scene=f.canvas->scene();
        CHECK(f.canvas->selectionConstructionActive());
        CHECK(!f.selection() && f.history().undoDepth()==0);
        CHECK(f.document().selectionRevision()==selectionRevision && f.surface().revision()==rasterRevision);
        if (scene.selectionEdges) {
            const auto& edges=*scene.selectionEdges;
            CHECK(scene.selectionStablePrefix<=edges.size());
            const auto prefix=std::min(scene.selectionStablePrefix,edges.size());
            if (prefix>previousPrefix) {
                ++prefixChanges;
                if (sample<samples*3/4) ++changesBeforeLastQuarter;
            }
            previousPrefix=prefix;
            double anchoredLength=0;
            for (std::size_t i=0;i<prefix;++i)
                anchoredLength+=std::hypot(edges[i].to.x-edges[i].from.x,edges[i].to.y-edges[i].from.y);
            maximumAnchoredLength=std::max(maximumAnchoredLength,anchoredLength);
            const auto nonPath=std::min(edges.size()-prefix,scene.selectionClosingEdges+scene.selectionAnchorCount);
            maximumPendingSegments=std::max(maximumPendingSegments,edges.size()-prefix-nonPath);
            if (sample>samples/2) {
                const auto anchor=prefix?edges[prefix-1].to:first;
                maximumPendingDistanceAfterHalf=std::max(maximumPendingDistanceAfterHalf,
                    std::hypot(point.x-anchor.x,point.y-anchor.y));
            }
        }
        if (sample<samples) {
            // Schedule separate native Qt event-loop turns at alternating
            // 5/6 ms deadlines rather than blocking/sleeping inside callbacks.
            // Late delivery never creates an unbounded queued pointer burst.
            const auto remainingNs=intervalNs*(sample+1)-clock.nsecsElapsed();
            input.start(int(std::max<qint64>(1,(remainingNs+999'999)/1'000'000)));
        }
    });
    input.start(6);
    CHECK(waitFor([&]{return delivered==samples;},15000));
    input.stop();
    // These are geometric progress bounds, not machine-speed assertions.
    // Checking before any settling pause catches "restart every event" search
    // starvation that a final wait-for-completion would otherwise conceal.
    CHECK(changesBeforeLastQuarter>=3);
    CHECK(maximumAnchoredLength>=travel*.5);
    CHECK(maximumPendingDistanceAfterHalf<512);
    CHECK(maximumPendingSegments<2048);
    std::cout<<"Magnetic sustained trace: requested_hz=180 samples="<<delivered
        <<" elapsed_ms="<<clock.elapsed()<<" prefix_changes="<<prefixChanges
        <<" anchored_document_px="<<maximumAnchoredLength
        <<" maximum_pending_document_px_after_half="<<maximumPendingDistanceAfterHalf
        <<" maximum_pending_segments="<<maximumPendingSegments
        <<" maximum_input_handler_us="<<double(maximumHandlerNs)/1000
        <<" maximum_delivery_lag_ms="<<double(maximumDeliveryLagNs)/1'000'000<<'\n';
    f.cancel();
    CHECK(!f.selection() && f.history().undoDepth()==0 && f.history().redoDepth()==0);
    CHECK(f.document().selectionRevision()==selectionRevision && f.surface().revision()==rasterRevision);
}

void magneticStrongEdgesCompleteThroughTheSharedEditingPipeline()
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    f.open(96,72,true); f.mode("LassoModeMagnetic");
    const auto raster=f.pixels(); const auto revision=f.surface().revision();
    f.click({24,18});
    for (const auto p : std::vector<core::Vec2d>{{48,20},{72,18},{70,36},{72,54},{48,52},{24,54},{26,36}})
        f.click(p);
    f.click({24,18}); f.waitIdle();
    CHECK(coverage(f.selection(),48,36)==255 && coverage(f.selection(),5,5)==0);
    CHECK(f.pixels()==raster && f.surface().revision()==revision);
    CHECK(f.history().undoDepth()==1);
    const auto selected=f.selection();
    auto* colors=dynamic_cast<ui::ColorSelector*>(f.window.findChild<QWidget*>(QStringLiteral("ColorPanelColors")));
    CHECK(colors && colors->onColorsChanged);
    if (colors && colors->onColorsChanged)
        colors->onColorsChanged({{220,40,70,255},{0,0,0,255},core::ColorSlot::Primary});
    f.action("FillForegroundAction");
    CHECK(waitFor([&]{return f.history().undoDepth()==2;}));
    CHECK(f.selection()==selected && f.pixels()!=raster);
    f.undo(); CHECK(f.pixels()==raster);
    f.action("LayerViaCopyAction");
    CHECK(f.document().layers().size()==2 && f.selection()==selected);
    f.undo(); CHECK(f.document().layers().size()==1);
    f.keyAction("Ctrl+T"); CHECK(f.canvas->scene().transformOverlay);
    f.button("TransformCancel");
    CHECK(f.selection()==selected && f.pixels()==raster);
}

void reviewSheet(const QString& output)
{
    Fixture f; CHECK(f.valid()); if (!f.valid()) return;
    QImage sheet(1640,1570,QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor("#14171f"));
    QPainter painter(&sheet);
    auto titleFont=painter.font(); titleFont.setPixelSize(24); titleFont.setBold(true);
    painter.setFont(titleFont); painter.setPen(QColor("#ecedf3"));
    painter.drawText(QRect(20,8,1600,40),QStringLiteral("Lasso modes · actual controls and selection geometry"));
    auto font=titleFont; font.setPixelSize(16); font.setBold(false);
    for (int mode=0;mode<3;++mode) {
        f.open(96,72,mode==2);
        f.rectangle({5,5},{18,28});
        const auto before=f.selection();
        const auto* name=mode==0?"LassoModeFreehand":mode==1?"LassoModePolygonal":"LassoModeMagnetic";
        f.mode(name); f.button("SelectionModeAdd");
        if (mode==0) {
            f.press(concave.front());
            for (std::size_t i=1;i<concave.size();++i) f.drag(concave[i]);
        } else if (mode==1) {
            for (const auto p:concave) f.click(p);
        } else {
            f.click({24,18}); f.click({72,18}); f.click({72,54}); f.hover({24,54});
            CHECK(waitFor([&]{const auto* t=f.window.findChild<QTimer*>(QStringLiteral("MagneticLassoTimer"));
                return !t || !t->isActive();}));
        }
        const auto pending=*f.canvas->scene().selectionEdges;
        const auto closing=f.canvas->scene().selectionClosingEdges;
        const auto anchors=f.canvas->scene().selectionAnchorCount;
        CHECK(f.selection()==before);
        const auto bar=f.options->grab();
        if (mode==0) { f.release(concave.back()); f.waitIdle(); }
        else { f.key(Qt::Key_Return); f.waitIdle(); }
        const auto after=f.selection();
        const int y=62+mode*493;
        painter.setFont(titleFont); painter.setPen(QColor("#ecedf3"));
        painter.drawText(QRect(20,y,1600,32),mode==0?QStringLiteral("Freehand · open trace and closing chord")
            :mode==1?QStringLiteral("Polygonal · explicit vertices and rubber-band closure")
                    :QStringLiteral("Magnetic · bounded edge following and stable anchors"));
        painter.drawPixmap(QRect(20,y+42,1600,int(bar.height()*1600.0/bar.width())),bar);
        const auto pixels=f.pixels();
        const QImage source(reinterpret_cast<const uchar*>(pixels.data()),96,72,96*4,QImage::Format_RGBA8888);
        const auto draw=[&](QRect target,const core::SelectionState& mask,bool preview) {
            painter.save(); painter.setClipRect(target);
            for (int dy=0;dy<target.height();dy+=16)
                for (int dx=0;dx<target.width();dx+=16)
                    painter.fillRect(target.x()+dx,target.y()+dy,16,16,
                        ((dx/16+dy/16)%2)?QColor("#373c46"):QColor("#454b56"));
            painter.drawImage(target,source);
            QImage coverageImage(96,72,QImage::Format_ARGB32_Premultiplied); coverageImage.fill(Qt::transparent);
            if (mask)
                for (int py=0;py<72;++py) for (int px=0;px<96;++px)
                    coverageImage.setPixelColor(px,py,QColor(110,133,248,mask->coverageAtDocumentPixel(px,py)*170/255));
            painter.drawImage(target,coverageImage);
            painter.translate(target.topLeft()); painter.scale(target.width()/96.0,target.height()/72.0);
            painter.setRenderHint(QPainter::Antialiasing);
            if (mask) {
                QPen pen(QColor("#ffffff"),.35,Qt::DashLine); painter.setPen(pen);
                for (const auto& e:mask->boundaryEdges()) painter.drawLine(QPointF(e.from.x,e.from.y),QPointF(e.to.x,e.to.y));
            }
            if (preview) {
                const auto lineCount=pending.size()-anchors;
                for (std::size_t i=0;i<lineCount;++i) {
                    const bool close=i>=lineCount-closing;
                    painter.setPen(QPen(close?QColor("#f1c36b"):QColor("#94a5ff"),.5,
                        close?Qt::DashLine:Qt::SolidLine));
                    const auto& e=pending[i]; painter.drawLine(QPointF(e.from.x,e.from.y),QPointF(e.to.x,e.to.y));
                }
                painter.setPen(QPen(QColor("#eaf0ff"),.3)); painter.setBrush(QColor("#8298ff"));
                for (std::size_t i=lineCount;i<pending.size();++i)
                    painter.drawEllipse(QPointF(pending[i].from.x,pending[i].from.y),.7,.7);
            }
            painter.restore();
        };
        painter.setFont(font); painter.setPen(QColor("#dfe2ee"));
        painter.drawText(QRect(24,y+95,470,25),QStringLiteral("Construction · committed selection remains intact"));
        painter.drawText(QRect(526,y+95,470,25),QStringLiteral("Completed · one Add selection command"));
        draw(QRect(24,y+127,460,345),before,true);
        draw(QRect(526,y+127,460,345),after,false);
        painter.setPen(QColor("#a9b2c8"));
        painter.drawText(QRect(1030,y+132,565,285),Qt::TextWordWrap,
            QStringLiteral("Actual cached toolbar above. Coverage and path geometry below come from the real UI interaction. "
                           "This diagnostic rendering magnifies R8 coverage; it is not a Vulkan framebuffer screenshot.\n\n"
                           "Blue area: selected mask. White boundary: existing selection. "
                           "Blue trace: pending path. Gold dashed line: closing segment. "
                           "Anchor dots: polygonal or magnetic checkpoints."));
    }
    painter.end(); CHECK(sheet.save(output));
}

int nativeValidation()
{
    if (QGuiApplication::platformName()!=QStringLiteral("wayland")) return 77;
    std::atomic_uint64_t warnings{0},errors{0};
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1,2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) return EXIT_FAILURE;
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type,const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data=static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr<<"Vulkan lasso modes validation: "<<(data?data->pMessage:"unknown")<<'\n';
        return true;
    });
    CHECK(instance.create());
    if (!instance.isValid()) return EXIT_FAILURE;
    {
        Fixture f(&instance); CHECK(f.valid()); if (!f.valid()) return EXIT_FAILURE;
        CHECK(waitFor([&]{return f.canvas->rendererStats().fullUploads>=1;}));
        QTest::qWait(180);
        CHECK(waitFor([&]{return !f.canvas->presentationSuppressedForResize();}));
        f.rectangle({4,4},{22,30});
        const auto before=f.selection(); const auto selectionRevision=f.document().selectionRevision();
        const auto pixels=f.pixels(); const auto rasterRevision=f.surface().revision();
        const auto geometry=f.workspace->canvasContainer()->geometry();
        const auto initial=f.canvas->rendererStats();
        const auto* page=f.options->pageForTool(core::ToolId::Lasso);
        for (const auto* mode : {"LassoModeFreehand","LassoModePolygonal","LassoModeMagnetic"}) {
            f.mode(mode); f.button("SelectionModeAdd");
            const bool freehand=QString::fromLatin1(mode)==QStringLiteral("LassoModeFreehand");
            if (freehand) f.press({35,12}); else f.click({35,12});
            for (int i=1;i<=24;++i) {
                const auto frame=f.canvas->rendererStats().framesSubmitted;
                const core::Vec2d p{35+i*1.6,12+std::sin(i*.15)*12};
                if (freehand) f.drag(p); else f.hover(p);
                CHECK(waitFor([&]{return f.canvas->rendererStats().framesSubmitted>frame;}));
                CHECK(f.selection()==before && f.document().selectionRevision()==selectionRevision);
                CHECK(f.canvas->scene().selectionRetainedEdges && f.canvas->scene().selectionClosingEdges>0);
                const bool animating=waitFor([&]{return f.canvas->selectionAnimationActive();},250);
                if (!animating)
                    std::cerr<<"Retained animation inactive: mode="<<mode<<" sample="<<i
                        <<" retained="<<(f.canvas->scene().selectionRetainedEdges
                            ? f.canvas->scene().selectionRetainedEdges->size():0)
                        <<" exposed="<<f.canvas->isExposed()<<'\n';
                CHECK(animating);
            }
            CHECK(f.options->pageForTool(core::ToolId::Lasso)==page);
            f.cancel();
        }
        CHECK(f.canvas->rendererStats().uploadedBytes==initial.uploadedBytes);
        CHECK(f.canvas->rendererStats().fullUploads==initial.fullUploads);
        CHECK(f.canvas->rendererStats().regionalUploadBatches==initial.regionalUploadBatches);
        CHECK(f.canvas->rendererStats().swapchainGeneration==initial.swapchainGeneration);
        CHECK(f.workspace->canvasContainer()->geometry()==geometry);
        CHECK(f.pixels()==pixels && f.surface().revision()==rasterRevision);
        f.action("DeselectAction");
        QTest::qWait(60);
        f.mode("LassoModePolygonal"); f.button("SelectionModeReplace");
        f.click({35,12}); f.hover({70,35});
        CHECK(waitFor([&]{return !f.canvas->selectionAnimationActive();}));
        const auto quiet=f.canvas->rendererStats(); QTest::qWait(100);
        CHECK(f.canvas->rendererStats().selectionUploadedBytes==quiet.selectionUploadedBytes);
        f.cancel();
        f.window.logRendererDiagnostics();
    }
    CHECK(warnings==0 && errors==0);
    std::cout<<"Native lasso modes validation: warnings="<<warnings<<" errors="<<errors<<'\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
} // namespace

int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LassoModesInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings; CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    ui::applyEditorTheme(app);
    if (qEnvironmentVariableIsSet("IMAGEEDITOR_TEST_LASSO_MODES_NATIVE")) return nativeValidation();
    if (const auto output=qEnvironmentVariable("IMAGEEDITOR_TEST_LASSO_MODES_PREVIEW"); !output.isEmpty()) {
        reviewSheet(output);
        return failures?EXIT_FAILURE:EXIT_SUCCESS;
    }
    threeModesShareCachedControlsAndKeepShortcutOwnership();
    additivePreviewRetainsCommittedContoursForEverySelectionTool();
    polygonClosureRemovalAndWindingUseTheApprovedMaskRasterizer();
    polygonDegeneraciesSelfCrossingsAndOffCanvasVerticesStayDeterministic();
    zoomChangesAndModifierLatchingDoNotChangeConstructionGeometry();
    unfinishedPathsCancelWithoutHistoryAndReferenceChangesInvalidateMagneticPaths();
    magneticWeakEdgesManualFallbackAndAnchorsRemainStable();
    constructionReleasesCrossPanelCaptureAndPendingCompletionCanBeCancelled();
    constructionCannotBecomeAMaskMoveAndManualAnchorsClearTheGuideLimit();
    rapidMagneticAnchorRequestsRetainExplicitClicksAndClosure();
    rightClickClosesAnchoredLassosLikeEnterWithoutAddingAnEndpoint();
    rightClickQueuesMagneticClosureWithoutLosingPendingAnchors();
    rightClickFinishesHeldFreehandOnceAndDoesNotDisableLaterContextMenus();
    sustainedMagneticTracingResolvesAndAutoAnchorsDuring180HzInput();
    magneticStrongEdgesCompleteThroughTheSharedEditingPipeline();
    if (failures) std::cerr<<failures<<" lasso modes interaction assertion(s) failed\n";
    else std::cout<<"All lasso modes interaction tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
