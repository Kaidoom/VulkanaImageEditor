#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/ProjectFile.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QVulkanInstance>

#include <cmath>
#include <iostream>
#include <numbers>

namespace {
namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;
int failures = 0;
QVulkanInstance* nativeVulkan=nullptr;
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
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
bool near(core::Vec2d a, core::Vec2d b) { return near(a.x, b.x) && near(a.y, b.y); }

template <class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF local, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(
        type, local, local, QPointF(receiver.mapToGlobal(local.toPoint())), button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window { nativeVulkan, false, false };
    render::CanvasWindow* canvas { nullptr };
    ui::OverlayDockWorkspace* workspace { nullptr };
    ui::CrossWindowPointerRouter* router { nullptr };

    Fixture()
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1500, 900);
        window.show();
        if(nativeVulkan)QTest::qWait(120);
        settle();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
                canvas = dynamic_cast<render::CanvasWindow*>(candidate);
        workspace = dynamic_cast<ui::OverlayDockWorkspace*>(
            window.findChild<QWidget*>(QStringLiteral("CanvasWorkspace")));
        router = dynamic_cast<ui::CrossWindowPointerRouter*>(
            window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
        QImage image(128, 96, QImage::Format_RGBA8888);
        image.fill(QColor(73, 149, 203, 191));
        const auto path = assets.filePath(QStringLiteral("selection-gizmo.png"));
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        shortcut("M");
        settle();
    }
    ~Fixture()
    {
        window.close();
        settle();
    }
    bool valid() const { return canvas && workspace && router && window.editorSession().document(); }
    const core::Document& document() const { return *window.editorSession().document(); }
    const core::History& history() const { return window.editorSession().history(); }
    core::SelectionState selection() const { return document().selection(); }
    const core::Layer& layer() const { return document().layers().front(); }
    std::shared_ptr<core::RasterSurface> surface() const
    {
        return std::get<core::RasterLayer>(layer().payload).surface;
    }
    QAction* action(const char* binding)
    {
        for (auto* item : window.findChildren<QAction*>())
            if (item->shortcuts().contains(QKeySequence(QString::fromLatin1(binding))))
                return item;
        CHECK(false);
        return nullptr;
    }
    void shortcut(const char* binding)
    {
        if (auto* item = action(binding))
            item->trigger();
        settle();
    }
    void button(const char* name)
    {
        auto* item = window.findChild<QToolButton*>(QString::fromLatin1(name));
        CHECK(item);
        if (item)
            item->click();
        settle();
    }
    QDoubleSpinBox* number(const char* suffix)
    {
        auto* item = window.findChild<QDoubleSpinBox*>(
            QStringLiteral("Transform") + QString::fromLatin1(suffix) + QStringLiteral("Control"));
        CHECK(item);
        return item;
    }
    QPointF logical(core::Vec2d p) const
    {
        const auto e = document().canvas().extent;
        const auto& scene = canvas->scene();
        const auto mapped = scene.viewport.documentToViewport(
            p, { double(e.width), double(e.height) }, scene.logicalViewport);
        return { mapped.x, mapped.y };
    }
    void press(QPointF p, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, modifiers);
        mouse(*canvas, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, modifiers);
    }
    void move(QPointF p, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, modifiers);
    }
    void release(QPointF p, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        mouse(*canvas, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, modifiers);
        settle();
    }
    void rectangle(core::Vec2d a, core::Vec2d b, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        press(logical(a), modifiers);
        move(logical(b), modifiers);
        release(logical(b), modifiers);
    }
    void selectWithHole()
    {
        rectangle({ 24, 20 }, { 94, 72 });
        rectangle({ 46, 35 }, { 70, 54 }, Qt::AltModifier);
        CHECK(selection() && selection()->coverageAtDocumentPixel(50, 40) == 0);
    }
    core::AffineTransform matrix() const
    {
        CHECK(canvas->scene().transformOverlay.has_value());
        return canvas->scene().transformOverlay ? canvas->scene().transformOverlay->localToDocument
                                                : core::AffineTransform { };
    }
    core::AffineTransform documentMapping(core::RectI originalBounds) const
    {
        auto result = matrix();
        result.m02 -= result.m00 * originalBounds.x + result.m01 * originalBounds.y;
        result.m12 -= result.m10 * originalBounds.x + result.m11 * originalBounds.y;
        return result;
    }
    void drag(core::Vec2d a, core::Vec2d b)
    {
        press(logical(a));
        move(logical(b));
        release(logical(b));
    }
};

void selectionToolsTargetMasksOnlyAndRequireActualCoverage()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    auto* transform = f.action("Ctrl+T");
    CHECK(transform && !transform->isEnabled());
    f.selectWithHole();
    CHECK(transform && transform->isEnabled());
    auto* page = f.window.findChild<QWidget*>(QStringLiteral("TransformOptionsPage"));
    const auto base = f.workspace->canvasContainer()->geometry();
    const auto before = f.selection();
    const auto layerMatrix = f.layer().localToDocument;
    const auto surfaceRevision = f.surface()->revision();
    for (const char* tool : { "M", "L" }) {
        f.shortcut(tool);
        f.canvas->requestActivate();
        settle();
        QTest::keyClick(f.canvas, Qt::Key_T, Qt::ControlModifier);
        settle();
        CHECK(f.window.editorSession().activeTool() == core::ToolId::Transform);
        CHECK(f.action(tool)->isChecked());
        CHECK(f.canvas->scene().transformOverlay.has_value());
        if (f.canvas->scene().transformOverlay) {
            CHECK(f.canvas->scene().transformOverlay->extent == core::Extent2u({ 70, 52 }));
            CHECK(near(f.matrix().map({ 0, 0 }), { 24, 20 }));
        }
        CHECK(page && page->isVisible());
        CHECK(f.workspace->canvasContainer()->geometry() == base);
        f.button("TransformCancel");
        CHECK(f.window.editorSession().activeTool()
            == (tool[0] == 'M' ? core::ToolId::Marquee : core::ToolId::Lasso));
        CHECK(!f.canvas->scene().transformOverlay);
        CHECK(f.selection() == before);
        CHECK(f.layer().localToDocument == layerMatrix);
        CHECK(f.surface()->revision() == surfaceRevision);
    }
    f.shortcut("M");
    f.rectangle({ 0, 0 }, { 128, 96 }, Qt::AltModifier);
    CHECK(f.selection() && f.selection()->bounds().empty());
    CHECK(transform && !transform->isEnabled());
}

void compositeMouseAndNumericGesturesPublishOneMaskCommandAndReuseLayerPixels()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.selectWithHole();
    const auto before = f.selection();
    const auto depth = f.history().undoDepth();
    const auto documentRevision = f.document().revision();
    const auto selectionRevision = f.document().selectionRevision();
    const auto surface = f.surface();
    const auto surfaceRevision = surface->revision();
    const auto layerMatrix = f.layer().localToDocument;
    const auto active = f.window.editorSession().activeLayer();
    f.shortcut("Ctrl+T");
    f.press(f.logical({ 32, 28 }));
    f.move(f.logical({ 180, 28 })); // leave the document, then return before release
    CHECK(f.selection() == before);
    f.move(f.logical({ 39, 33 }));
    f.release(f.logical({ 39, 33 }));
    const auto moved = f.matrix();
    CHECK(near(moved.map({ 0, 0 }), { 31, 25 }));
    CHECK(f.document().revision() == documentRevision);
    CHECK(f.document().selectionRevision() == selectionRevision);
    CHECK(f.history().undoDepth() == depth);

    const auto handle = f.matrix().map({ 70, 52 });
    f.press(f.logical(handle));
    f.move(f.logical(handle + core::Vec2d { 14, 13 }));
    CHECK(f.canvas->transformDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    QWidget receiver(f.workspace->panelOverlay());
    receiver.setGeometry(15, 60, 150, 80);
    receiver.show();
    const auto destination = f.logical(handle + core::Vec2d { 14, 13 });
    const auto global = f.canvas->mapToGlobal(destination.toPoint());
    const auto routed = f.router->routedEventCount();
    mouse(receiver, QEvent::MouseButtonRelease, QPointF(receiver.mapFromGlobal(global)), Qt::LeftButton,
        Qt::NoButton);
    settle();
    CHECK(!f.canvas->transformDragging());
    CHECK(f.router->routedEventCount() > routed);
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    const auto scaled = f.matrix();
    CHECK(near(scaled.map({ 0, 0 }), moved.map({ 0, 0 })));
    CHECK(scaled != moved);
    f.shortcut("Ctrl+Z");
    CHECK(f.matrix() == moved);
    f.canvas->requestActivate();
    settle();
    QTest::keyClick(f.canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.matrix() == scaled);

    f.button("TransformFlipHorizontal");
    auto* angle = f.number("Angle");
    if (angle)
        angle->setValue(23);
    settle();
    const auto finalMatrix = f.matrix();
    const auto expected = before->transformed(f.documentMapping(before->bounds()));
    CHECK(f.selection() == before);
    CHECK(f.surface() == surface && surface->revision() == surfaceRevision);
    CHECK(f.layer().localToDocument == layerMatrix);
    CHECK(f.document().revision() == documentRevision);
    CHECK(f.history().undoDepth() == depth);
    f.shortcut("Ctrl+Z");
    CHECK(f.matrix() != finalMatrix);
    f.shortcut("Ctrl+Shift+Z");
    CHECK(f.matrix() == finalMatrix);
    f.button("TransformApply");
    CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.selection() && f.selection()->equivalent(*expected));
    CHECK(f.history().undoDepth() == depth + 1);
    CHECK(f.window.editorSession().activeLayer() == active);
    CHECK(f.layer().localToDocument == layerMatrix);
    CHECK(f.surface() == surface && surface->revision() == surfaceRevision);
    f.shortcut("Ctrl+Z");
    CHECK(f.selection()->equivalent(*before));
    f.shortcut("Ctrl+Shift+Z");
    CHECK(f.selection()->equivalent(*expected));
}

void cancelAndNoopKeepPreexistingHistoryAndRedoIncludingLocalDivergence()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.selectWithHole();
    f.shortcut("Ctrl+Shift+I");
    f.shortcut("Ctrl+Z");
    const auto before = f.selection();
    const auto undoDepth = f.history().undoDepth(), redoDepth = f.history().redoDepth();
    CHECK(redoDepth == 1);
    for (const bool cancel : { true, false }) {
        f.shortcut("Ctrl+T");
        const auto start = f.matrix();
        f.drag({ 32, 28 }, { 39, 34 });
        f.button("TransformFlipVertical");
        f.shortcut("Ctrl+Z");
        f.shortcut("Ctrl+Z");
        CHECK(f.matrix() == start);
        CHECK(f.history().undoDepth() == undoDepth);
        CHECK(f.history().redoDepth() == redoDepth);
        if (cancel) {
            f.drag({ 32, 28 }, { 36, 26 }); // local divergent branch, still not global history
            CHECK(f.action("Ctrl+Shift+Z") && !f.action("Ctrl+Shift+Z")->isEnabled());
            f.canvas->requestActivate();
            QTest::keyClick(f.canvas, Qt::Key_Escape);
            settle();
        } else
            f.button("TransformApply"); // returned exactly to entry geometry
        CHECK(f.selection() == before);
        CHECK(f.history().undoDepth() == undoDepth);
        CHECK(f.history().redoDepth() == redoDepth);
        CHECK(!f.canvas->scene().transformOverlay);
    }
    f.shortcut("Ctrl+T");
    f.press(f.logical({ 32, 28 }));
    f.move(f.logical({ 80, 35 }));
    CHECK(f.canvas->transformDragging());
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    settle();
    CHECK(!f.canvas->transformDragging());
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(f.selection() == before);
    CHECK(f.history().redoDepth() == redoDepth);
}

void rotationMouseAndHeldNumericInputUseSharedControlsAndGroupedLocalHistory()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.selectWithHole();
    const auto before = f.selection();
    const auto depth = f.history().undoDepth();
    f.shortcut("Ctrl+T");
    const auto initial = f.matrix();
    const QPointF center = f.logical({ 59, 46 }), corner = f.logical({ 94, 72 });
    auto outward = corner - center;
    outward *= 17 / std::hypot(outward.x(), outward.y());
    const auto press = corner + outward;
    const auto offset = press - center;
    const auto radians = 31.0 * std::numbers::pi / 180.0;
    const auto end = center
        + QPointF(std::cos(radians) * offset.x() - std::sin(radians) * offset.y(),
            std::sin(radians) * offset.x() + std::cos(radians) * offset.y());
    f.press(press);
    CHECK(f.canvas->transformDragging());
    f.move(end, Qt::ShiftModifier);
    f.release(end, Qt::ShiftModifier);
    auto* angle = f.number("Angle");
    CHECK(angle && near(angle->value(), 30));
    CHECK(f.selection() == before);
    f.shortcut("Ctrl+Z");
    CHECK(f.matrix() == initial);
    if (!angle)
        return;
    CHECK(angle->singleStep() == 1);
    angle->setFocus();
    angle->selectAll();
    settle();
    const QPoint up(angle->width() - 8, 7);
    QTest::mousePress(
        angle->window()->windowHandle(), Qt::LeftButton, Qt::NoModifier, angle->mapTo(angle->window(), up));
    QTest::qWait(650);
    CHECK(angle->value() >= 2);
    CHECK(f.router->captureOwner() == angle);
    CHECK(!angle->hasFocus());
    CHECK(f.selection() == before);
    const auto held = f.matrix();
    mouse(*f.canvas, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(f.history().undoDepth() == depth);
    f.shortcut("Ctrl+Z");
    CHECK(f.matrix() == initial); // one local action, not one per repeated step
    f.shortcut("Ctrl+Shift+Z");
    CHECK(f.matrix() == held);
    angle->setFocus();
    angle->selectAll();
    QTest::keyClicks(angle, QStringLiteral("17.5"));
    QTest::keyClick(angle, Qt::Key_Return);
    settle();
    CHECK(near(angle->value(), 17.5));
    f.shortcut("Ctrl+Z");
    CHECK(f.matrix() == held);
    f.button("TransformCancel");
    CHECK(f.selection() == before);
    CHECK(f.history().undoDepth() == depth);
}

void textFieldsKeepCtrlTAndRedoOwnership()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid())
        return;
    f.selectWithHole();
    const auto before = f.selection();
    QLineEdit input(&f.window);
    input.setGeometry(60, 60, 250, 35);
    input.show();
    input.window()->activateWindow();
    input.setFocus();
    settle();
    QTest::keyClicks(&input, QStringLiteral("abc"));
    QTest::keyClicks(&input, QStringLiteral("T"), Qt::ShiftModifier);
    CHECK(input.text()==QStringLiteral("abcT"));
    QTest::keyClick(&input, Qt::Key_T, Qt::ControlModifier);
    settle();
    CHECK(input.hasFocus());
    CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Marquee);
    QTest::keyClick(&input, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&input, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    settle();
    CHECK(f.selection() == before);
    CHECK(f.history().undoDepth() == 2);
}

void applicationSwitchAndMinimizePreserveSelectionSessionAndCancelOnlyLiveDrag()
{
    for (const int lifecycle : { 0, 1, 2 }) {
        Fixture f;
        CHECK(f.valid());
        if (!f.valid()) return;
        f.selectWithHole();
        f.shortcut("Ctrl+Shift+I");
        const auto oldFuture = f.selection();
        f.shortcut("Ctrl+Z");
        const auto before = f.selection();
        const auto undoDepth = f.history().undoDepth(), redoDepth = f.history().redoDepth();
        const auto surfaceRevision = f.surface()->revision();
        CHECK(redoDepth == 1);
        f.shortcut("Ctrl+T");
        f.drag({ 32, 28 }, { 39, 34 });
        const auto moved = f.matrix();
        f.button("TransformFlipVertical");
        const auto flipped = f.matrix();
        f.shortcut("Ctrl+Z");
        CHECK(f.matrix() == moved);
        CHECK(f.action("Ctrl+Shift+Z")->isEnabled());

        const auto deactivateAndRestore = [&] {
            if (lifecycle == 0) {
                QEvent loss(QEvent::ApplicationDeactivate);
                QCoreApplication::sendEvent(qApp, &loss);
            } else if (lifecycle == 1) {
                QEvent loss(QEvent::WindowDeactivate);
                QCoreApplication::sendEvent(&f.window, &loss);
            } else {
                f.window.showMinimized();
            }
            settle();
            CHECK(f.window.editorSession().activeTool() == core::ToolId::Transform);
            CHECK(f.canvas->scene().transformOverlay.has_value());
            CHECK(!f.canvas->transformDragging());
            CHECK(f.router->captureDomain() == ui::CrossWindowPointerRouter::CaptureDomain::None);
            CHECK(f.selection() == before);
            if (lifecycle == 2) f.window.showNormal();
            QEvent appActive(QEvent::ApplicationActivate);
            QCoreApplication::sendEvent(qApp, &appActive);
            f.window.activateWindow();
            f.canvas->requestActivate();
            settle();
            auto* page = f.window.findChild<QWidget*>(QStringLiteral("TransformOptionsPage"));
            CHECK(page && page->isVisible());
        };
        deactivateAndRestore();
        CHECK(f.matrix() == moved);
        CHECK(f.action("Ctrl+Shift+Z")->isEnabled());
        f.shortcut("Ctrl+Shift+Z");
        CHECK(f.matrix() == flipped);
        f.shortcut("Ctrl+Z");
        CHECK(f.matrix() == moved);

        const auto center = moved.map({ 35, 26 });
        f.press(f.logical(center));
        f.move(f.logical(center + core::Vec2d { 42, 23 }));
        CHECK(f.canvas->transformDragging());
        CHECK(f.matrix() != moved);
        deactivateAndRestore();
        CHECK(f.matrix() == moved);
        CHECK(f.action("Ctrl+Shift+Z")->isEnabled());
        f.shortcut("Ctrl+Shift+Z");
        CHECK(f.matrix() == flipped);
        f.shortcut("Ctrl+Z");
        CHECK(f.matrix() == moved);

        f.drag(center, center + core::Vec2d { -8, 11 });
        const auto resumed = f.matrix();
        CHECK(resumed != moved);
        f.shortcut("Ctrl+Z");
        CHECK(f.matrix() == moved);
        f.shortcut("Ctrl+Shift+Z");
        CHECK(f.matrix() == resumed);
        CHECK(f.selection() == before);
        CHECK(f.history().undoDepth() == undoDepth);
        CHECK(f.history().redoDepth() == redoDepth);
        CHECK(f.surface()->revision() == surfaceRevision);
        f.button("TransformCancel");
        CHECK(!f.canvas->scene().transformOverlay);
        CHECK(f.selection() == before);
        CHECK(f.history().undoDepth() == undoDepth);
        CHECK(f.history().redoDepth() == redoDepth);
        f.shortcut("Ctrl+Shift+Z");
        CHECK(f.selection() && f.selection()->equivalent(*oldFuture));
    }
}

void applicationDeactivateFinishesSelectionNumericTextWithoutApplyingTheMask()
{
    Fixture f;
    CHECK(f.valid());
    if (!f.valid()) return;
    f.selectWithHole();
    const auto before = f.selection();
    const auto depth = f.history().undoDepth();
    f.shortcut("Ctrl+T");
    const auto initial = f.matrix();
    auto* angle = f.number("Angle");
    auto* editor = angle ? angle->findChild<QLineEdit*>() : nullptr;
    CHECK(angle && editor);
    if (!angle || !editor) return;
    angle->setFocus(Qt::MouseFocusReason);
    settle();
    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("17.5"));
    CHECK(f.matrix() == initial);
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    settle();
    CHECK(f.window.editorSession().activeTool() == core::ToolId::Transform);
    CHECK(f.canvas->scene().transformOverlay.has_value());
    CHECK(near(angle->value(), 17.5));
    const auto edited = f.matrix();
    CHECK(edited != initial);
    CHECK(f.selection() == before);
    CHECK(f.history().undoDepth() == depth);
    f.shortcut("Ctrl+Z");
    CHECK(f.matrix() == initial);
    f.shortcut("Ctrl+Shift+Z");
    CHECK(f.matrix() == edited);
    f.button("TransformCancel");
    CHECK(f.selection() == before);
    CHECK(f.history().undoDepth() == depth);
}
std::vector<std::byte> rasterBytes(const core::RasterSurface& surface)
{
    const auto e=surface.extent();std::vector<std::byte> b(std::size_t(e.width)*e.height*4);
    surface.copyRgba8({0,0,int(e.width),int(e.height)},b,std::size_t(e.width)*4);return b;
}
void selectionMovementSnapsWithAndWithoutTransform()
{
    for (const bool transform : {false, true}) for (const bool actualPixels : {false, true}) {
        Fixture f; CHECK(f.valid()); if(!f.valid())return;
        if(actualPixels)f.canvas->resetTo100Percent();
        else f.canvas->fitDocumentToView();
        settle();
        f.window.findChild<QAction*>("SnapLayersAction")->setChecked(false);
        f.rectangle({24,20},{55,43});
        f.rectangle({30,26},{35,31},Qt::AltModifier);
        const auto before=f.selection();
        const auto source=f.surface();
        const auto original=rasterBytes(*source);
        const auto revision=f.document().revision(), depth=f.history().undoDepth();
        if(transform)f.shortcut("Ctrl+T");
        const core::Vec2d grab{43,34}, snapped{24.5,16.5};
        const auto raw=snapped-core::Vec2d{3/f.canvas->zoom(),2/f.canvas->zoom()};
        const auto position=grab+raw;
        const auto previewAt=[&](core::Vec2d offset) {
            if(transform) CHECK(near(f.matrix().map({0,0}),core::Vec2d{24,20}+offset));
            else CHECK(f.canvas->scene().selectionEdges && *f.canvas->scene().selectionEdges
                == core::translatedSelectionPreviewEdges(before,offset.x,offset.y));
        };
        const auto freeOffset=transform?raw:core::Vec2d{std::round(raw.x),std::round(raw.y)};
        f.press(f.logical(grab));
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        f.move(f.logical(position));
        previewAt(snapped);
        CHECK(f.canvas->scene().snapGuides[0] && f.canvas->scene().snapGuides[1]);
        if(f.canvas->scene().snapGuides[0])CHECK(near(f.canvas->scene().snapGuides[0]->a.x,64));
        if(f.canvas->scene().snapGuides[1])CHECK(near(f.canvas->scene().snapGuides[1]->a.y,48));
        CHECK(f.selection()==before && f.history().undoDepth()==depth);
        // Ctrl and preferences reevaluate the original raw displacement with
        // no extra pointer event, including when the key arrives via a panel.
        auto* panel=f.window.findChild<QWidget*>("LayersPanel"); CHECK(panel);
        QTest::keyPress(panel,Qt::Key_Control); settle();
        previewAt(freeOffset);
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        QTest::keyRelease(panel,Qt::Key_Control); settle(); previewAt(snapped);
        auto* master=f.window.findChild<QAction*>("SnappingAction");
        master->setChecked(false); previewAt(freeOffset);
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        master->setChecked(true); previewAt(snapped);
        // Retain the alignment outside capture but inside release tolerance.
        f.move(f.logical(grab+snapped+core::Vec2d{8/f.canvas->zoom(),8/f.canvas->zoom()}));
        previewAt(snapped);
        f.move(f.logical(grab)); previewAt({});
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        f.move(f.logical(position)); previewAt(snapped);
        f.release(f.logical(position));
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        if(transform) {
            f.shortcut("Ctrl+Z"); previewAt({});
            f.shortcut("Ctrl+Shift+Z"); previewAt(snapped);
            f.button("TransformApply");
        }
        core::AffineTransform translation;translation.m02=snapped.x;translation.m12=snapped.y;
        const auto expected=before->transformed(translation);
        CHECK(f.selection()->equivalent(*expected));
        CHECK(f.history().undoDepth()==depth+1);
        CHECK(f.surface()==source && rasterBytes(*source)==original && f.document().revision()==revision);
        f.shortcut("Ctrl+Z");CHECK(f.selection()->equivalent(*before));
        if(transform)f.shortcut("Ctrl+T");
        f.press(f.logical(grab));f.move(f.logical(position));
        QTest::keyClick(f.canvas,Qt::Key_Escape);settle();
        if(transform)f.button("TransformCancel");
        CHECK(f.selection()->equivalent(*before));CHECK(f.history().canRedo());
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        f.shortcut("Ctrl+Shift+Z");CHECK(f.selection()->equivalent(*expected));
    }
    // Selection-only movement must not exclude the active layer as a target.
    for(const bool transform:{false,true}) {
        Fixture f;CHECK(f.valid());if(!f.valid())return;
        QImage image(40,30,QImage::Format_RGBA8888);image.fill(Qt::red);
        const auto path=f.assets.filePath("snap-target.png");CHECK(image.save(path));
        CHECK(f.window.importImageAsLayerFromPath(path));f.shortcut("M");
        f.canvas->fitDocumentToView();settle();
        const auto id=*f.window.editorSession().activeLayer();
        const auto bounds=core::layerDocumentBounds(*f.document().layer(id));CHECK(bounds);if(!bounds)return;
        f.window.findChild<QAction*>("SnapCanvasAction")->setChecked(false);
        f.rectangle({8,8},{26,22});const auto before=f.selection();
        if(transform)f.shortcut("Ctrl+T");
        const core::Vec2d grab{17,15}, delta=bounds->maximum-core::Vec2d{8,8};
        const auto position=grab+delta-core::Vec2d{2/f.canvas->zoom(),3/f.canvas->zoom()};
        f.press(f.logical(grab));f.move(f.logical(position));
        for(const auto& guide:f.canvas->scene().snapGuides){CHECK(guide);if(guide)CHECK(guide->target==id);}
        f.release(f.logical(position));if(transform)f.button("TransformApply");
        core::AffineTransform translation;translation.m02=delta.x;translation.m12=delta.y;
        CHECK(f.selection()->equivalent(*before->transformed(translation)));
    }
}

void edgeSnapsAndCtrlChangesMidCornerDrag()
{
    for (const auto* shortcut : {"Ctrl+T", "Shift+T"}) {
        Fixture f; CHECK(f.valid()); if(!f.valid()) return;
        f.selectWithHole(); f.shortcut(shortcut);
        auto* lock=f.window.findChild<QToolButton*>("TransformAspectLock"); CHECK(lock); if(!lock)return;
        lock->setChecked(false);
        auto h=f.canvas->scene().transformOverlay->handles();
        const auto unsnapped=128-3.0/f.canvas->zoom(); // Capture tolerance is logical screen pixels.
        f.press(f.logical(h[3])); f.move(f.logical({unsnapped,h[3].y}));
        QTest::qWait(25); settle();
        CHECK(near(f.canvas->scene().transformOverlay->handles()[3].x,128));
        CHECK(f.canvas->scene().snapGuides[0]);
        QTest::keyPress(f.canvas,Qt::Key_Control); QTest::qWait(25); settle();
        CHECK(near(f.canvas->scene().transformOverlay->handles()[3].x,unsnapped));
        CHECK(!f.canvas->scene().snapGuides[0]);
        QTest::keyRelease(f.canvas,Qt::Key_Control); QTest::qWait(25); settle();
        CHECK(near(f.canvas->scene().transformOverlay->handles()[3].x,128));
        f.release(f.logical({unsnapped,h[3].y}));
        CHECK(!f.canvas->scene().snapGuides[0]);
        h=f.canvas->scene().transformOverlay->handles();
        const auto corner=h[0], point=corner+core::Vec2d{3,4};
        f.press(f.logical(corner)); f.move(f.logical(point)); QTest::qWait(25); settle();
        const auto scaled=f.matrix(); CHECK(scaled.isAffine());
        CHECK(!f.canvas->scene().snapGuides[0] && !f.canvas->scene().snapGuides[1]);
        QTest::keyPress(f.canvas,Qt::Key_Control); QTest::qWait(25); settle();
        CHECK(!f.matrix().isAffine());
        CHECK(f.canvas->cursor().shape()==Qt::SizeAllCursor);
        const auto distorted=f.canvas->scene().transformOverlay->handles();
        for (std::size_t other:{2u,4u,6u}) CHECK(near(distorted[other],h[other]));
        QTest::keyRelease(f.canvas,Qt::Key_Control); QTest::qWait(25); settle();
        CHECK(f.matrix()==scaled);
        CHECK(f.canvas->cursor().shape()==Qt::SizeFDiagCursor || f.canvas->cursor().shape()==Qt::SizeBDiagCursor);
        f.release(f.logical(point));
        f.button("TransformCancel");
    }
}

void selectedPixelsDistortAndRoundTrip()
{
    Fixture f;CHECK(f.valid());if(!f.valid())return;
    f.selectWithHole();const auto source=f.surface();const auto original=rasterBytes(*source);
    const auto selection=f.selection();const auto depth=f.history().undoDepth();
    const auto id=f.layer().id;const auto external=f.layer().localToDocument;
    f.shortcut("Shift+T");CHECK(f.canvas->scene().transformOverlay);
    f.drag({30,25},{40,28});
    CHECK(f.surface()!=source);CHECK(f.selection()->coverageAtDocumentPixel(60,43)==0);
    const auto first=rasterBytes(*f.surface());
    const auto matrix=f.matrix();const auto corner=matrix.map({0,0});
    f.press(f.logical(corner),Qt::ControlModifier);
    f.move(f.logical(corner+core::Vec2d{3,4}),Qt::ControlModifier);
    f.release(f.logical(corner+core::Vec2d{3,4}),Qt::ControlModifier);
    CHECK(!f.matrix().isAffine());const auto distorted=rasterBytes(*f.surface());
    CHECK(first!=distorted);CHECK(f.layer().localToDocument==external);
    f.shortcut("Ctrl+Z");CHECK(rasterBytes(*f.surface())==first);
    f.shortcut("Ctrl+Shift+Z");CHECK(rasterBytes(*f.surface())==distorted);
    f.button("TransformApply");CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.history().undoDepth()==depth+2);CHECK(f.layer().id==id);CHECK(rasterBytes(*source)==original);
    const auto path=f.assets.filePath("committed-pixels.vulkana");const auto saved=ui::saveProject(path,f.document());if(!saved)std::cerr<<saved.error.toStdString()<<'\n';CHECK(saved);
    auto loaded=ui::loadProject(path);CHECK(loaded);if(loaded){const auto* layer=loaded.document->layer(id);CHECK(layer);
        if(layer){CHECK(layer->localToDocument==external);CHECK(layer->rasterOrigin==f.layer().rasterOrigin);
            CHECK(layer->rasterEffectFrame==f.layer().rasterEffectFrame);
            CHECK(rasterBytes(*std::get<core::RasterLayer>(layer->payload).surface)==distorted);}}
    f.shortcut("Ctrl+Z");CHECK(rasterBytes(*f.surface())==first);
    f.shortcut("Ctrl+Z");CHECK(f.surface()==source);CHECK(f.selection()->equivalent(*selection));
    CHECK(rasterBytes(*f.surface())==original);
    // A no-op/cancel never consumes the existing redo branch.
    f.shortcut("Shift+T");f.drag({30,25},{20,28});f.button("TransformCancel");
    CHECK(f.history().canRedo());CHECK(f.surface()==source);CHECK(f.selection()->equivalent(*selection));
}
void pixelTransformSettlesBeforeFileAndTabChanges()
{
    Fixture f;CHECK(f.valid());if(!f.valid())return;f.selectWithHole();f.shortcut("Shift+T");
    f.drag({30,25},{40,30});const auto firstId=f.window.activeDocumentId();
    const auto moved=rasterBytes(*f.surface());
    QImage image(19,23,QImage::Format_RGBA8888);image.fill(Qt::red);
    const auto path=f.assets.filePath("second.png");CHECK(image.save(path));CHECK(f.window.openImageFromPath(path));
    CHECK(f.window.activeDocumentId()!=firstId);CHECK(!f.canvas->scene().transformOverlay);
    const auto* first=f.window.documentContext(firstId);CHECK(first);
    if(first){CHECK(first->session.history().canUndo());
        CHECK(rasterBytes(*std::get<core::RasterLayer>(first->session.document()->layers().front().payload).surface)==moved);}
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SelectionTransformInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(application);
    const bool native=application.arguments().contains(QStringLiteral("--native"));
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1,2));
    int validationMessages=0;
    if(native){
        if(!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation")))return 77;
        instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        instance.installDebugOutputFilter([&](auto severity,auto type,const void* raw){
            if(type.testFlag(QVulkanInstance::ValidationMessage)&&(severity.testFlag(QVulkanInstance::ErrorSeverity)||severity.testFlag(QVulkanInstance::WarningSeverity))){
                ++validationMessages;const auto* data=static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(raw);std::cerr<<data->pMessage<<'\n';}return false;});
        if(!instance.create())return 1;
        nativeVulkan=&instance;
        selectionMovementSnapsWithAndWithoutTransform();
        edgeSnapsAndCtrlChangesMidCornerDrag();selectedPixelsDistortAndRoundTrip();pixelTransformSettlesBeforeFileAndTabChanges();
        nativeVulkan=nullptr;instance.destroy();
        std::cout<<"Native selected-pixel/projective interaction: platform="<<QGuiApplication::platformName().toStdString()<<" validation=enabled messages="<<validationMessages<<" failures="<<failures<<'\n';
        return failures||validationMessages?1:0;
    }
    selectionToolsTargetMasksOnlyAndRequireActualCoverage();
    compositeMouseAndNumericGesturesPublishOneMaskCommandAndReuseLayerPixels();
    cancelAndNoopKeepPreexistingHistoryAndRedoIncludingLocalDivergence();
    rotationMouseAndHeldNumericInputUseSharedControlsAndGroupedLocalHistory();
    textFieldsKeepCtrlTAndRedoOwnership();
    applicationSwitchAndMinimizePreserveSelectionSessionAndCancelOnlyLiveDrag();
    applicationDeactivateFinishesSelectionNumericTextWithoutApplyingTheMask();
    edgeSnapsAndCtrlChangesMidCornerDrag();selectedPixelsDistortAndRoundTrip();
    selectionMovementSnapsWithAndWithoutTransform();
    pixelTransformSettlesBeforeFileAndTabChanges();
    if (failures)
        std::cerr << failures << " selection gizmo interaction assertions failed\n";
    else
        std::cout << "Selection gizmo interaction tests passed\n";
    return failures ? 1 : 0;
}
