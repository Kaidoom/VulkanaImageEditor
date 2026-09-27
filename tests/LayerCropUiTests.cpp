#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLineF>
#include <QMouseEvent>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVulkanInstance>
#include <atomic>
#include <cmath>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;

namespace {
int failures = 0;
void check(bool passed, const char* expression, int line)
{
    if (!passed) {
        ++failures;
        std::cerr << "FAIL " << line << ": " << expression << '\n';
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
bool sameRect(c::RectD a, c::RectD b)
{
    return near(a.x, b.x) && near(a.y, b.y)
        && near(a.width, b.width) && near(a.height, b.height);
}

template<class Receiver>
void mouse(Receiver& receiver, QEvent::Type type, QPointF point,
    Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF global(receiver.mapToGlobal(point.toPoint()));
    QMouseEvent event(type, point, point, global, button, buttons, modifiers);
    QCoreApplication::sendEvent(&receiver, &event);
}

QPointF handle(r::CanvasWindow& canvas, std::size_t index)
{
    const auto point = canvas.logicalTransformHandles()[index];
    return {point.x, point.y};
}

QPointF center(r::CanvasWindow& canvas)
{
    return (handle(canvas, 0) + handle(canvas, 4)) / 2;
}

void drag(r::CanvasWindow& canvas, QPointF from, QPointF to,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    mouse(canvas, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton, modifiers);
    mouse(canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, modifiers);
    mouse(canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, modifiers);
    mouse(canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, modifiers);
    settle();
}

struct Fixture {
    QTemporaryDir files;
    u::MainWindow window;
    r::CanvasWindow* canvas {};
    u::CrossWindowPointerRouter* router {};
    u::CropOptionsPage* page {};
    u::LayerListView* view {};
    u::LayerListModel* model {};
    c::LayerId id {};

    explicit Fixture(QVulkanInstance* instance = nullptr)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        // Fit the actual primary monitor for native validation; do not rely on
        // a 1550-pixel test window fitting a deliberately broad options page.
        window.resize(instance && QGuiApplication::primaryScreen()
                ? QGuiApplication::primaryScreen()->availableGeometry().size() - QSize(100, 100)
                : QSize(2000, 1080));
        window.show();
        settle();
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == "VulkanCanvasWindow")
                canvas = dynamic_cast<r::CanvasWindow*>(candidate);
        router = dynamic_cast<u::CrossWindowPointerRouter*>(
            window.findChild<QObject*>("CrossWindowPointerRouter"));
        page = dynamic_cast<u::CropOptionsPage*>(window.findChild<QWidget*>("CropOptionsPage"));
        view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
        model = view ? dynamic_cast<u::LayerListModel*>(view->model()) : nullptr;
        QImage image(160, 120, QImage::Format_RGBA8888);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor((x * 5) % 256, (y * 7) % 256,
                    (x + 2 * y) % 256, (x + y) % 3 == 0 ? 140 : 255));
        const auto path = files.filePath("crop-source.png");
        CHECK(image.save(path));
        CHECK(window.openImageFromPath(path));
        id = session().activeLayer().value_or(0);
        CHECK(canvas && router && page && view && model && id);
        settle();
    }
    ~Fixture() { window.close(); settle(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& document() { return *session().document(); }
    const c::Layer& layer() { return *document().layer(id); }
    std::optional<c::RectD> crop() { return layer().crop; }
    c::RectD frame() { return crop().value_or(c::layerSourceBounds(layer())); }
    bool active() { return session().activeTool() == c::ToolId::Crop && canvas->scene().transformOverlay && page->isVisible(); }
    std::shared_ptr<c::RasterSurface> source() { return std::get<c::RasterLayer>(layer().payload).surface; }
    std::vector<std::byte> pixels()
    {
        const auto extent = source()->extent();
        std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
        source()->copyRgba8({0, 0, int(extent.width), int(extent.height)}, bytes, std::size_t(extent.width) * 4);
        return bytes;
    }
    template<class T> T* widget(const char* name)
    {
        auto* result = window.findChild<T*>(QString::fromLatin1(name));
        CHECK(result);
        return result;
    }
    u::ToolOptionsNumber* number(const char* name)
    {
        auto* result = dynamic_cast<u::ToolOptionsNumber*>(widget<QDoubleSpinBox>(name));
        CHECK(result);
        return result;
    }
    QAction* action(const QKeySequence& shortcut)
    {
        for (auto* result : window.findChildren<QAction*>())
            if (result->shortcuts().contains(shortcut)) return result;
        CHECK(false);
        return nullptr;
    }
    void begin(bool shortcut = false)
    {
        if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
        canvas->requestActivate();
        settle();
        if (shortcut) QTest::keyClick(canvas, Qt::Key_C);
        else if (auto* command = action(QKeySequence("C"))) command->trigger();
        settle();
        CHECK(active());
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::keyClick(canvas, key, modifiers);
        settle();
    }
    void history(bool redo)
    {
        if (auto* command = action(redo ? QKeySequence("Ctrl+Shift+Z") : QKeySequence(QKeySequence::Undo)))
            command->trigger();
        settle();
    }
    void select(c::LayerId target, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        model->refresh();
        const auto index = model->index(model->rowForLayer(target));
        CHECK(index.isValid());
        view->scrollTo(index);
        settle();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, modifiers, view->visualRect(index).center());
        settle();
    }
};

void compactToolbarAndChamferHandles()
{
    Fixture f;
    f.begin();
    for (const auto* name : { "CropAspectLock", "CropChamfer", "CropShowSource", "CropRemove",
             "TransformAspectLock", "TransformFlipHorizontal", "TransformFlipVertical",
             "MoveFlipHorizontal", "MoveFlipVertical" }) {
        auto* button = f.widget<QToolButton>(name);
        CHECK(button->toolButtonStyle() == Qt::ToolButtonIconOnly);
        CHECK(button->property("toolOptionsIcon").toBool());
        CHECK(button->size() == QSize(28, 28) && button->iconSize() == QSize(18, 18));
        CHECK(!button->icon().isNull() && !button->toolTip().isEmpty());
        CHECK(!button->accessibleName().isEmpty() && button->focusPolicy() == Qt::NoFocus);
    }
    for (const auto* name : { "CropApply", "CropCancel", "TransformApply", "TransformCancel" })
        CHECK(f.widget<QToolButton>(name)->toolButtonStyle() == Qt::ToolButtonTextOnly);
    CHECK(f.widget<QToolButton>("CropRemove")->accessibleName() == "Reset crop");
    for (const auto* name : { "CropAspectLock", "CropChamfer", "CropShowSource", "TransformAspectLock" }) {
        auto* button = f.widget<QToolButton>(name);
        CHECK(button->isCheckable());
        CHECK(button->icon().pixmap(QSize(18, 18), QIcon::Normal, QIcon::On).toImage()
            != button->icon().pixmap(QSize(18, 18), QIcon::Normal, QIcon::Off).toImage());
    }
    CHECK(f.widget<QToolButton>("MoveFlipHorizontal")->icon().pixmap(QSize(18, 18)).toImage()
        != f.widget<QToolButton>("MoveFlipVertical")->icon().pixmap(QSize(18, 18)).toImage());
    const std::array names { "CropXControl", "CropYControl", "CropWControl", "CropHControl",
        "CropAspectLock", "CropChamfer", "CropShowSource", "CropRemove", "CropApply", "CropCancel" };
    for (int width : { 2000, 2400 }) {
        f.window.resize(width, 1080);
        settle();
        for (std::size_t i = 1; i < names.size(); ++i) {
            auto* a = f.widget<QWidget>(names[i - 1]);
            auto* b = f.widget<QWidget>(names[i]);
            CHECK(b->mapTo(f.page, QPoint { }).x() - a->mapTo(f.page, QPoint { }).x() - a->width() == 8);
        }
    }
    const auto pixels = f.pixels();
    const auto source = f.source();
    const auto matrix = f.layer().localToDocument;
    const auto token = f.document().contentState();
    auto* mode = f.widget<QAbstractButton>("CropChamfer");
    mode->click();
    settle();
    CHECK(f.document().contentState() == token && !f.crop()); // Interaction preference only.
    const auto a = handle(*f.canvas, 0), b = handle(*f.canvas, 2), d = handle(*f.canvas, 6);
    const auto u = (b - a) / 160.0, v = (d - a) / 120.0;
    drag(*f.canvas, a, a + (u + v) * 10);
    CHECK(f.layer().crop && near(f.layer().crop->corners[0], 20));
    CHECK(f.canvas->logicalTransformHandles().size() == 8);
    CHECK(QLineF(handle(*f.canvas, 0), a + (u + v) * 10).length() < 1e-5);
    const auto cut = *f.layer().crop;
    mode->click();
    settle();
    CHECK(f.layer().crop == cut); // Off restores frame resizing, not destructive reset.
    CHECK(QLineF(handle(*f.canvas, 0), a).length() < 1e-5);
    f.widget<QAbstractButton>("CropApply")->click();
    settle();
    f.history(false);
    CHECK(!f.crop());
    f.history(true);
    CHECK(f.layer().crop == cut);
    CHECK(f.pixels() == pixels && f.source() == source && f.layer().localToDocument == matrix);
    f.begin();
    CHECK(mode->isChecked());
    if (!qEnvironmentVariable("IMAGEEDITOR_CROP_TOOLBAR_REVIEW").isEmpty())
        CHECK(f.page->grab().save(qEnvironmentVariable("IMAGEEDITOR_CROP_TOOLBAR_REVIEW")));
    f.widget<QAbstractButton>("CropRemove")->click();
    CHECK(!f.crop());
    f.widget<QAbstractButton>("CropCancel")->click();
    CHECK(f.layer().crop == cut);
}

void entryPrimaryTargetAndCachedControls()
{
    Fixture f;
    const auto bytes = f.pixels();
    const auto source = f.source();
    const auto revision = source->revision();
    const auto matrix = f.layer().localToDocument;
    const auto initialContent = f.document().contentState();
    const auto depth = f.session().history().undoDepth();
    auto* page = f.page;
    auto* width = f.number("CropWControl");
    auto* height = f.number("CropHControl");
    f.begin(true);
    CHECK(!f.crop());
    CHECK(f.document().contentState() == initialContent);
    CHECK(f.session().history().undoDepth() == depth);
    CHECK(near(width->value(), 160) && near(height->value(), 120));
    CHECK(!f.canvas->scene().transformOverlay->rotationEnabled);
    auto* show = f.widget<QAbstractButton>("CropShowSource");
    CHECK(show->isChecked() && f.canvas->scene().cropPreviewLayer == f.id);
    show->click();
    CHECK(!f.canvas->scene().cropPreviewLayer);
    show->click();
    CHECK(f.document().contentState() == initialContent);
    f.widget<QAbstractButton>("CropApply")->click();
    settle();
    CHECK(!f.active() && !f.crop());
    CHECK(f.session().history().undoDepth() == depth);
    f.begin();
    CHECK(f.page == page && f.number("CropWControl") == width);
    f.widget<QAbstractButton>("CropAspectLock")->click();
    width->setValue(80);
    CHECK(sameRect(f.frame(), {0, 0, 80, 60}));
    f.widget<QAbstractButton>("CropApply")->click();
    CHECK(f.session().history().undoDepth() == depth + 1);
    CHECK(f.layer().localToDocument == matrix && f.source() == source);
    CHECK(f.source()->revision() == revision && f.pixels() == bytes);
    f.history(false);
    CHECK(!f.crop());
    f.history(true);
    CHECK(sameRect(f.frame(), {0, 0, 80, 60}));

    auto other = c::Layer::raster("Other selected", std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{60, 40}));
    const auto otherId = other.id;
    CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(other), f.document().layers().size())));
    f.select(otherId);
    f.select(f.id, Qt::ControlModifier);
    CHECK(f.session().selectedLayers().size() == 2 && f.session().activeLayer() == f.id);
    f.begin();
    width->setValue(50);
    f.widget<QAbstractButton>("CropApply")->click();
    CHECK(!f.document().layer(otherId)->crop);
    CHECK(f.session().selectedLayers().size() == 2);
}

void perActionUndoRedoCancelAndNoop()
{
    Fixture f;
    const auto depth = f.session().history().undoDepth();
    f.begin(true);
    f.number("CropXControl")->setValue(5);
    const auto first = f.frame();
    f.number("CropWControl")->setValue(90);
    const auto second = f.frame();
    CHECK(f.session().history().undoDepth() == depth);
    f.key(Qt::Key_Z, Qt::ControlModifier);
    CHECK(f.active() && sameRect(f.frame(), first));
    CHECK(near(f.number("CropWControl")->value(), first.width));
    f.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(f.active() && sameRect(f.frame(), second));
    f.key(Qt::Key_Return);
    CHECK(!f.active() && f.session().history().undoDepth() == depth + 2);
    f.history(false);
    CHECK(sameRect(f.frame(), first));
    const auto redoDepth = f.session().history().redoDepth();
    const auto token = f.document().contentState();
    f.begin();
    f.number("CropYControl")->setValue(11);
    f.number("CropHControl")->setValue(60);
    f.key(Qt::Key_Escape);
    CHECK(!f.active() && sameRect(f.frame(), first));
    CHECK(f.document().contentState() == token);
    CHECK(f.session().history().redoDepth() == redoDepth);
    f.begin();
    // A click-only frame move, and a session without mutations, are no-ops.
    drag(*f.canvas, center(*f.canvas), center(*f.canvas));
    f.widget<QAbstractButton>("CropApply")->click();
    CHECK(f.session().history().redoDepth() == redoDepth);
    f.history(true);
    CHECK(sameRect(f.frame(), second));
    f.begin();
    f.number("CropYControl")->setValue(9);
    f.number("CropHControl")->setValue(66);
    f.history(false);
    f.number("CropHControl")->setValue(77);
    CHECK(!f.action(QKeySequence("Ctrl+Shift+Z"))->isEnabled());
    f.widget<QAbstractButton>("CropApply")->click();
    CHECK(sameRect(f.frame(), {5, 9, 90, 77}));
    f.history(false);
    CHECK(sameRect(f.frame(), {5, 9, 90, 120}));
    f.history(false);
    CHECK(sameRect(f.frame(), second));
    f.begin();
    f.widget<QAbstractButton>("CropRemove")->click();
    CHECK(!f.crop());
    f.history(false);
    CHECK(sameRect(f.frame(), second));
    f.history(true);
    f.widget<QAbstractButton>("CropApply")->click();
    CHECK(!f.crop());
    f.history(false);
    CHECK(sameRect(f.frame(), second));
}

void numericOwnershipAndHeldStepperCapture()
{
    Fixture f;
    f.begin();
    auto* x = f.number("CropXControl");
    auto* edit = x->findChild<QLineEdit*>();
    CHECK(edit);
    x->setFocus();
    x->selectAll();
    settle();
    for (const auto key : {Qt::Key_B, Qt::Key_E, Qt::Key_V, Qt::Key_C, Qt::Key_I}) {
        QTest::keyClick(x, key);
        CHECK(f.active());
    }
    QTest::keyClick(x, Qt::Key_T, Qt::ControlModifier);
    CHECK(f.active());
    x->selectAll();
    QTest::keyClicks(x, "12.5");
    CHECK(!f.crop());
    QTest::keyClick(x, Qt::Key_Return);
    CHECK(f.active() && near(f.frame().x, 12.5));
    f.history(false);
    CHECK(!f.crop());
    x->setFocus();
    x->selectAll();
    QTest::keyClicks(x, "7.25");
    QTest::keyClick(x, Qt::Key_Escape);
    CHECK(f.active() && near(f.frame().x, 7.25));
    f.history(false);
    CHECK(!f.crop());

    x->setFocus();
    x->selectAll();
    settle();
    auto* inputWindow = x->window()->windowHandle();
    const auto up = x->mapTo(x->window(), QPoint(x->width() - 8, 7));
    QTest::mousePress(inputWindow, Qt::LeftButton, Qt::NoModifier, up);
    QTest::qWait(650);
    CHECK(x->interactionActive() && x->value() >= 2);
    CHECK(f.router->captureOwner() == x);
    CHECK(!x->hasFocus() && (!edit || !edit->hasSelectedText()));
    const auto routed = f.router->routedEventCount();
    const auto elsewhere = center(*f.canvas);
    mouse(*f.canvas, QEvent::MouseMove, elsewhere, Qt::NoButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseButtonRelease, elsewhere, Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(!x->interactionActive());
    CHECK(f.router->captureDomain() == u::CrossWindowPointerRouter::CaptureDomain::None);
    CHECK(f.router->routedEventCount() >= routed + 2 && QWidget::mouseGrabber() == nullptr);
    // The stepper itself, not this test, must have removed typing focus.
    auto* keyReceiver = QApplication::focusWidget();
    QTest::keyClick(keyReceiver ? keyReceiver : &f.window, Qt::Key_Z, Qt::ControlModifier);
    settle();
    CHECK(f.active() && !f.crop()); // The entire hold is one pending action.
    f.key(Qt::Key_Escape);
    CHECK(!f.active() && !f.crop());
}

void dragAcrossPanelsAndPersistentFocusLoss()
{
    Fixture f;
    const auto matrix = f.layer().localToDocument;
    const auto bytes = f.pixels();
    const auto revision = f.source()->revision();
    const auto depth = f.session().history().undoDepth();
    f.begin();
    const auto original = f.frame();
    const auto corner = handle(*f.canvas, 4);
    drag(*f.canvas, corner, corner - QPointF(35, 22));
    const auto resized = f.frame();
    CHECK(resized.width < original.width && resized.height < original.height);
    CHECK(near(resized.x, original.x) && near(resized.y, original.y));
    auto* panel = f.widget<QWidget>("LayersPanel");
    const auto start = center(*f.canvas);
    mouse(*f.canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    CHECK(f.canvas->transformDragging());
    CHECK(f.router->captureDomain() == u::CrossWindowPointerRouter::CaptureDomain::NativeWindow);
    const auto routed = f.router->routedEventCount();
    mouse(*panel, QEvent::MouseMove, panel->rect().center(), Qt::NoButton, Qt::LeftButton);
    mouse(*panel, QEvent::MouseButtonRelease, panel->rect().center(), Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(f.active() && !f.canvas->transformDragging());
    CHECK(!sameRect(f.frame(), resized));
    CHECK(near(f.frame().width, resized.width) && near(f.frame().height, resized.height));
    CHECK(f.router->routedEventCount() >= routed + 2);
    CHECK(f.router->captureDomain() == u::CrossWindowPointerRouter::CaptureDomain::None);
    const auto moved = f.frame();
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(f.canvas, &focusOut);
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    settle();
    CHECK(f.active() && sameRect(f.frame(), moved));
    // A focus loss during a drag cancels only that uncompleted gesture.
    const auto newStart = center(*f.canvas);
    mouse(*f.canvas, QEvent::MouseButtonPress, newStart, Qt::LeftButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseMove, newStart + QPointF(15, 10), Qt::NoButton, Qt::LeftButton);
    CHECK(f.canvas->transformDragging());
    QFocusEvent midDragLoss(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(f.canvas, &midDragLoss);
    CHECK(f.active() && !f.canvas->transformDragging() && sameRect(f.frame(), moved));
    CHECK(f.router->captureDomain() == u::CrossWindowPointerRouter::CaptureDomain::None);
    f.history(false);
    CHECK(sameRect(f.frame(), resized));
    f.history(true);
    CHECK(sameRect(f.frame(), moved));
    CHECK(f.layer().localToDocument == matrix && f.source()->revision() == revision && f.pixels() == bytes);
    f.widget<QAbstractButton>("CropApply")->click();
    CHECK(f.session().history().undoDepth() == depth + 2);
    f.history(false);
    f.history(false);
    CHECK(!f.crop());
}

void rightClickCompletionAndToolSwitch()
{
    Fixture f;
    const auto depth = f.session().history().undoDepth();
    f.begin();
    f.number("CropWControl")->setValue(110);
    const auto first = f.frame();
    const auto idlePoint = center(*f.canvas);
    mouse(*f.canvas, QEvent::MouseButtonPress, idlePoint, Qt::RightButton, Qt::RightButton);
    mouse(*f.canvas, QEvent::MouseButtonRelease, idlePoint, Qt::RightButton, Qt::NoButton);
    CHECK(!f.active() && sameRect(f.frame(), first));
    CHECK(f.session().history().undoDepth() == depth + 1);
    f.begin();
    const auto press = center(*f.canvas);
    const auto end = press + QPointF(18, 13);
    mouse(*f.canvas, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    CHECK(f.canvas->transformDragging());
    const auto moved = f.frame();
    mouse(*f.canvas, QEvent::MouseButtonPress, end, Qt::RightButton, Qt::RightButton | Qt::LeftButton);
    CHECK(!f.active() && !f.canvas->transformDragging());
    QContextMenuEvent context(QContextMenuEvent::Mouse, end.toPoint(), f.canvas->mapToGlobal(end.toPoint()));
    context.setAccepted(false);
    QCoreApplication::sendEvent(f.canvas, &context);
    CHECK(context.isAccepted());
    mouse(*f.canvas, QEvent::MouseButtonRelease, end, Qt::RightButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseMove, end + QPointF(30, 40), Qt::NoButton, Qt::LeftButton);
    mouse(*f.canvas, QEvent::MouseButtonRelease, end + QPointF(30, 40), Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(sameRect(f.frame(), moved) && f.session().history().undoDepth() == depth + 2);
    CHECK(f.router->captureDomain() == u::CrossWindowPointerRouter::CaptureDomain::None);
    f.begin();
    f.number("CropHControl")->setValue(45);
    // Same cancellation convention as existing explicit transform sessions.
    f.action(QKeySequence("B"))->trigger();
    settle();
    CHECK(f.session().activeTool() == c::ToolId::Brush);
    CHECK(sameRect(f.frame(), moved));
    CHECK(!f.canvas->scene().cropPreviewLayer && !f.canvas->scene().transformOverlay);
    CHECK(f.session().history().undoDepth() == depth + 2);
    f.begin();
    f.number("CropXControl")->setValue(80);
    f.widget<QAbstractButton>("CropCancel")->click();
    CHECK(f.session().activeTool() == c::ToolId::Brush && sameRect(f.frame(), moved));
}

void transformedFramesKeepLocalNumbersAndOriginalContent()
{
    Fixture f;
    const auto bytes = f.pixels();
    const auto revision = f.source()->revision();
    // Existing whole-layer scale, rotation/shear, reflection, and translation
    // remain untouched while crop coordinates describe the original local grid.
    const c::AffineTransform matrix {-.85, -.32, 155, -.2, 1.25, 25};
    CHECK(f.document().setLayerTransform(f.id, matrix));
    f.begin();
    const auto original = f.frame();
    const auto& scene = f.canvas->scene();
    const auto extent = f.document().canvas().extent;
    const auto logical = [&](c::Vec2d local) {
        const auto result = scene.viewport.documentToViewport(matrix.map(local),
            {double(extent.width), double(extent.height)}, scene.logicalViewport);
        return QPointF(result.x, result.y);
    };
    drag(*f.canvas, handle(*f.canvas, 4), logical({100, 85}));
    CHECK(sameRect(f.frame(), {0, 0, 100, 85}));
    CHECK(near(f.number("CropWControl")->value(), 100));
    CHECK(near(f.number("CropHControl")->value(), 85));
    const auto dragStart = center(*f.canvas);
    const auto logicalDelta = logical({13, -6}) - logical({0, 0});
    drag(*f.canvas, dragStart, dragStart + logicalDelta);
    CHECK(sameRect(f.frame(), {13, -6, 100, 85}));
    CHECK(f.layer().localToDocument == matrix);
    CHECK(f.source()->revision() == revision && f.pixels() == bytes);
    f.history(false);
    CHECK(sameRect(f.frame(), {0, 0, 100, 85}));
    f.history(false);
    CHECK(!f.crop() && sameRect(f.frame(), original));
    f.key(Qt::Key_Escape);
    CHECK(f.layer().localToDocument == matrix);
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != "wayland") return 77;
    std::atomic_uint64_t warnings {0}, errors {0};
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) return 1;
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags type, const void* message) {
        if (!type.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << "Layer Crop Vulkan: " << (data ? data->pMessage : "unknown") << '\n';
        return true;
    });
    if (!instance.create()) return 1;
    const auto waitFor = [](const auto& predicate) {
        QElapsedTimer timer;
        timer.start();
        do {
            settle();
            if (predicate()) return true;
            QTest::qWait(5);
        } while (timer.elapsed() < 10000);
        return predicate();
    };
    {
        Fixture f(&instance);
        CHECK(waitFor([&] { return f.canvas->rendererStats().fullUploads > 0 && !f.canvas->presentationSuppressedForResize(); }));
        QTest::qWait(150);
        settle();
        const auto baseline = f.canvas->rendererStats();
        const auto canvasSize = f.canvas->size();
        const auto viewport = f.canvas->scene().viewport;
        const auto bytes = f.pixels();
        const auto source = f.source();
        const auto revision = source->revision();
        const auto step = [&](const auto& operation) {
            const auto frame = f.canvas->rendererStats().framesSubmitted;
            operation();
            CHECK(waitFor([&] { return f.canvas->rendererStats().framesSubmitted > frame; }));
        };
        step([&] { f.begin(); });
        step([&] { f.number("CropXControl")->setValue(12.5); });
        step([&] { f.number("CropYControl")->setValue(8.25); });
        step([&] { f.number("CropWControl")->setValue(80.5); });
        step([&] { f.number("CropHControl")->setValue(70.25); });
        step([&] { f.widget<QAbstractButton>("CropChamfer")->click(); });
        const auto corner = handle(*f.canvas, 0);
        const auto inward = (handle(*f.canvas, 2) - corner) / 80.5
            + (handle(*f.canvas, 6) - corner) / 70.25;
        step([&] { drag(*f.canvas, corner, corner + inward * 7.5, Qt::AltModifier); });
        CHECK(f.layer().crop && near(f.layer().crop->corners[0], 15)
            && near(f.layer().crop->corners[2], 15));
        CHECK(f.canvas->scene().transformOverlay->chamferHandles);
        const auto token = f.document().contentState();
        const auto depth = f.session().history().undoDepth();
        for (int i = 0; i < 6; ++i) {
            step([&] { f.widget<QAbstractButton>("CropShowSource")->click(); });
            CHECK(bool(f.canvas->scene().cropPreviewLayer) == f.widget<QAbstractButton>("CropShowSource")->isChecked());
        }
        CHECK(f.document().contentState() == token && f.session().history().undoDepth() == depth);
        const auto cached = f.canvas->rendererStats();
        CHECK(cached.fullUploads == baseline.fullUploads);
        CHECK(cached.regionalUploads == baseline.regionalUploads && cached.uploadedBytes == baseline.uploadedBytes);
        CHECK(cached.resourceGeneration == baseline.resourceGeneration && cached.swapchainGeneration == baseline.swapchainGeneration);
        CHECK(f.canvas->size() == canvasSize && near(f.canvas->zoom(), viewport.zoom()));
        step([&] { f.history(false); });
        step([&] { f.history(true); });
        step([&] { f.widget<QAbstractButton>("CropApply")->click(); });
        CHECK(!f.canvas->scene().cropPreviewLayer);
        step([&] { f.history(false); });
        step([&] { f.history(true); });
        step([&] { f.begin(); });
        step([&] { f.widget<QAbstractButton>("CropRemove")->click(); });
        step([&] { f.widget<QAbstractButton>("CropCancel")->click(); });
        const auto crop = f.layer().crop;
        CHECK(crop);
        const auto generation = f.canvas->rendererStats().swapchainGeneration;
        f.window.resize(f.window.size() - QSize(100, 60));
        CHECK(waitFor([&] { return f.canvas->rendererStats().swapchainGeneration > generation
            && !f.canvas->presentationSuppressedForResize(); }));
        CHECK(f.layer().crop == crop && f.source() == source && source->revision() == revision && f.pixels() == bytes);
        CHECK(f.canvas->rendererStats().fullUploads == baseline.fullUploads);
        CHECK(f.canvas->rendererStats().regionalUploads == baseline.regionalUploads);
        // Give resize/presentation events time to drain before measuring idle.
        QTest::qWait(150);
        settle();
        const auto idle = f.canvas->rendererStats().framesSubmitted;
        QTest::qWait(200);
        settle();
        CHECK(f.canvas->rendererStats().framesSubmitted == idle);
    }
    instance.destroy();
    settle();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Native Layer Crop Vulkan: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("LayerCropUiTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    if (app.arguments().contains("--wayland-validation")) return nativeValidation();
    entryPrimaryTargetAndCachedControls();
    compactToolbarAndChamferHandles();
    perActionUndoRedoCancelAndNoop();
    numericOwnershipAndHeldStepperCapture();
    dragAcrossPanelsAndPersistentFocusLoss();
    rightClickCompletionAndToolSwitch();
    transformedFramesKeepLocalNumbersAndOriginalContent();
    std::cout << (failures ? "Layer Crop UI tests FAILED\n" : "Layer Crop UI tests passed\n");
    return failures ? 1 : 0;
}
