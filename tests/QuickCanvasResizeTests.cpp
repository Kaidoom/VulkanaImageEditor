#include "imageeditor/core/CanvasResize.hpp"
#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "NativeGraphicsStartup.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures { };
void check(bool ok, const char* message, int line)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << line << ": " << message << '\n';
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
void settle()
{
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

void geometry()
{
    // Every edge/corner uses the same handle order as the canvas transform.
    const std::array<c::Vec2d, 8> points { { { 0, 0 }, { 50, 0 }, { 100, 0 }, { 100, 40 }, { 100, 80 },
        { 50, 80 }, { 0, 80 }, { 0, 40 } } };
    const std::array<c::Vec2d, 8> deltas { { { -10, -20 }, { 0, -20 }, { 10, -20 }, { 10, 0 }, { 10, 20 },
        { 0, 20 }, { -10, 20 }, { -10, 0 } } };
    const std::array<c::RectI, 8> expected { { { -10, -20, 110, 100 }, { 0, -20, 100, 100 },
        { 0, -20, 110, 100 }, { 0, 0, 110, 80 }, { 0, 0, 110, 100 }, { 0, 0, 100, 100 }, { -10, 0, 110, 100 },
        { -10, 0, 110, 80 } } };
    for (std::size_t i = 0; i < 8; ++i) {
        c::CanvasResizeFrame frame({ 100, 80 }, 16384);
        CHECK(frame.begin(c::TransformHandle(i), points[i]));
        CHECK(frame.drag(points[i] + deltas[i], { }, false));
        CHECK(frame.frame() == expected[i]);
        frame.end();
        CHECK(frame.canUndo());
        frame.step(false);
        CHECK(frame.frame() == c::RectI(0, 0, 100, 80));
        frame.step(true);
        CHECK(frame.frame() == expected[i]);
        frame.reset();
        CHECK(frame.frame() == c::RectI(0, 0, 100, 80));
        frame.step(false);
        CHECK(frame.frame() == expected[i]);
    }
    c::CanvasResizeFrame frame({ 100, 80 }, 16384, 40 * 1024 * 1024);
    CHECK(frame.begin(c::TransformHandle::TopLeft, { 0, 0 }));
    CHECK(frame.drag({ -10, -20 }, { .alt = true }, false));
    CHECK(frame.frame() == c::RectI(-10, -20, 120, 120));
    frame.cancelDrag();
    CHECK(!frame.canUndo());
    CHECK(frame.begin(c::TransformHandle::BottomRight, { 100, 80 }));
    CHECK(frame.drag({ 150, 120 }, { }, true));
    CHECK(frame.frame() == c::RectI(0, 0, 150, 120));
    CHECK(frame.drag({ 150, 100 }, { .shift = true }, true));
    CHECK(frame.frame() == c::RectI(0, 0, 150, 100));
    frame.cancelDrag();
    CHECK(frame.begin(c::TransformHandle::Left, { 0, 40 }));
    CHECK(frame.drag({ 120, 40 }, { }, false));
    CHECK(frame.frame() == c::RectI(100, 0, 20, 80));
    frame.cancelDrag();
    CHECK(!frame.set({ 0, 0, 16385, 1 }));
    CHECK(!frame.set({ 0, 0, 8000, 8000 }));
    CHECK(!frame.set({ NAN, 0, 1, 1 }));
    CHECK(!frame.set({ 0, 0, -1, 1 }));
    CHECK(!frame.begin(c::TransformHandle::Rotate, { }));
    CHECK(frame.set({ -.4, -.6, 100.8, 80.2 }));
    CHECK(frame.frame() == c::RectI(0, -1, 100, 81));
}

void commandAndSelections()
{
    const c::CanvasSpec original { { 64, 48 }, 300, c::CompositingSpace::EncodedSRgb };
    c::Document doc(original);
    c::History history;
    auto surface
        = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 60, 40 }, c::Rgba8 { 30, 60, 100, 170 });
    auto a = c::Layer::raster("Visible", surface), b = c::Layer::raster("Hidden", surface);
    a.localToDocument = { 1.2, .1, 7, .3, .9, -6 };
    b.localToDocument = { 1, 0, 4, 0, 1, 5, .001, .002, 1 };
    b.visible = false;
    a.mask = std::make_shared<c::LayerMask>(
        c::LayerMask { c::SelectionMask::rectangle({ 60, 40 }, { 5, 6, 10, 12 }), { } });
    a.crop = c::LayerCrop { 1, 2, 40, 30 };
    struct Original {
        c::LayerId id;
        c::AffineTransform localToDocument;
        c::LayerMaskState mask;
        std::optional<c::LayerCrop> crop;
        bool visible;
    };
    const Original originalA { a.id, a.localToDocument, a.mask, a.crop, a.visible };
    const Original originalB { b.id, b.localToDocument, b.mask, b.crop, b.visible };
    const auto aid = a.id, bid = b.id;
    CHECK(doc.insertLayer(0, std::move(a)));
    CHECK(doc.insertLayer(1, std::move(b)));
    auto selection = c::SelectionMask::rectangle(original.extent, { 2, 3, 20, 25 }, 173);
    auto remembered = c::SelectionMask::rectangle(original.extent, { 50, 30, 14, 18 }, 91);
    CHECK(doc.setSelection(selection));
    doc.setLastSelection(remembered);
    doc.markSaved();
    const auto saved = doc.contentState();
    CHECK(history.execute(doc, std::make_unique<c::ReframeCanvasCommand>(c::RectI { -90, -70, 180, 140 })));
    CHECK(doc.canvas().extent == c::Extent2u(180, 140));
    CHECK(doc.canvas().dotsPerInch == 300);
    CHECK(doc.canvas().compositingSpace == original.compositingSpace);
    CHECK(doc.isModified() && history.undoDepth() == 1);
    CHECK(history.canvasOriginHint()->x == -90 && history.canvasOriginHint()->y == -70);
    CHECK(doc.selection()->bounds() == c::RectI(92, 73, 20, 25));
    CHECK(doc.selection()->coverageAtDocumentPixel(92, 73) == 173);
    CHECK(doc.lastSelection()->bounds() == c::RectI(140, 100, 14, 18));
    for (const auto& before : std::array { std::cref(originalA), std::cref(originalB) }) {
        const auto& originalLayer = before.get();
        const auto& after = *doc.layer(originalLayer.id);
        CHECK(after.localToDocument
            == c::composeTransform({ 1, 0, 90, 0, 1, 70 }, originalLayer.localToDocument));
        CHECK(after.mask == originalLayer.mask && after.crop == originalLayer.crop
            && after.visible == originalLayer.visible);
        CHECK(std::get<c::RasterLayer>(after.payload).surface == surface);
        const auto old = originalLayer.localToDocument.map({ 12, 13 }),
                   now = after.localToDocument.map({ 12, 13 });
        CHECK(near(now.x - 90, old.x) && near(now.y - 70, old.y));
    }
    CHECK(history.undo(doc));
    CHECK(doc.canvas() == original && doc.contentState() == saved && !doc.isModified());
    CHECK(doc.layer(aid)->localToDocument == originalA.localToDocument);
    CHECK(doc.layer(bid)->localToDocument == originalB.localToDocument);
    CHECK(doc.selection() == selection && doc.lastSelection() == remembered);
    CHECK(history.canvasOriginHint()->x == 90);
    CHECK(!history.execute(doc, std::make_unique<c::ReframeCanvasCommand>(c::RectI { 0, 0, 64, 48 })));
    CHECK(history.redoDepth() == 1);
    CHECK(history.redo(doc));
    CHECK(history.undo(doc));
    CHECK(history.execute(doc, std::make_unique<c::ReframeCanvasCommand>(c::RectI { 8, 10, 30, 25 })));
    CHECK(doc.selection()->bounds() == c::RectI(0, 0, 14, 18));
    CHECK(!doc.lastSelection());
    CHECK(history.undo(doc));
    CHECK(doc.selection() == selection && doc.lastSelection() == remembered);
    // Stale geometry is rejected before publishing any of the batch.
    c::ReframeCanvasCommand guarded({ -2, -4, 70, 55 });
    CHECK(guarded.apply(doc));
    CHECK(doc.setLayerTransform(bid, { }));
    const auto before = doc.snapshot();
    CHECK(!guarded.undo(doc));
    CHECK(doc.canvas() == before.canvas
        && doc.layer(aid)->localToDocument == before.layersBottomToTop[0].localToDocument);
    // Selection translation across tile boundaries keeps every 8-bit sample.
    std::vector<std::uint8_t> pixels(259 * 133);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = std::uint8_t(i % 256);
    auto mask = c::SelectionMask::fromR8({ 259, 133 }, pixels, 259);
    auto shifted = mask->reframed({ 600, 400 }, 271, 151);
    for (int y = 0; y < 400; ++y)
        for (int x = 0; x < 600; ++x)
            CHECK(shifted->coverageAtDocumentPixel(x, y) == mask->coverageAtDocumentPixel(x - 271, y - 151));
    CHECK(mask->translated(INT_MIN, INT_MAX)->bounds().empty());
    c::Document empty(original);
    c::History emptyHistory;
    CHECK(emptyHistory.execute(empty, std::make_unique<c::ReframeCanvasCommand>(c::RectI { 3, 4, 64, 48 })));
    CHECK(emptyHistory.canvasOriginHint()->x == 3 && empty.canvas() == original);
    CHECK(emptyHistory.undo(empty) && emptyHistory.canvasOriginHint()->x == -3);
}

struct Fixture {
    QTemporaryDir files;
    u::MainWindow window;
    r::CanvasWindow* canvas { };
    u::CropOptionsPage* page { };
    c::LayerId id { };
    explicit Fixture(QVulkanInstance* instance = nullptr)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1500, 950);
        window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window));
        settle();
        for (auto* w : QGuiApplication::allWindows())
            if (w->objectName() == "VulkanCanvasWindow")
                canvas = dynamic_cast<r::CanvasWindow*>(w);
        page = dynamic_cast<u::CropOptionsPage*>(window.findChild<QWidget*>("CanvasResizeOptionsPage"));
        CHECK(canvas && page);
        QImage image(160, 120, QImage::Format_RGBA8888);
        image.fill(QColor(80, 170, 200));
        CHECK(image.save(files.filePath("resize.png")));
        CHECK(window.openImageFromPath(files.filePath("resize.png")));
        id = session().activeLayer().value();
        settle();
        if (instance) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 30000
                && (!window.startupGraphicsReady() || canvas->rendererStats().fullUploads == 0
                    || canvas->presentationSuppressedForResize()))
                QTest::qWait(10);
            CHECK(window.startupGraphicsReady() && canvas->rendererStats().fullUploads > 0);
        }
    }
    ~Fixture()
    {
        window.close();
        settle();
    }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& doc() { return *session().document(); }
    QAction* action(const char* name)
    {
        auto* result = window.findChild<QAction*>(name);
        CHECK(result);
        return result;
    }
    void history(bool redo)
    {
        const auto key = redo ? QKeySequence("Ctrl+Shift+Z") : QKeySequence(QKeySequence::Undo);
        for (auto* a : window.findChildren<QAction*>())
            if (a->shortcuts().contains(key)) {
                a->trigger();
                settle();
                return;
            }
        CHECK(false);
    }
    bool active()
    {
        return session().activeTool() == c::ToolId::CanvasResize && page->isVisible()
            && canvas->scene().transformOverlay.has_value();
    }
    void begin(bool shortcut = false)
    {
        if (auto* focus = QApplication::focusWidget())
            focus->clearFocus();
        canvas->requestActivate();
        settle();
        if (shortcut)
            QTest::keyClick(canvas, Qt::Key_R, Qt::ControlModifier | Qt::AltModifier);
        else
            action("QuickCanvasResizeAction")->trigger();
        settle();
        CHECK(active());
    }
    void value(const char* axis, double value)
    {
        auto* n = window.findChild<QDoubleSpinBox*>(QString("CanvasResize%1Control").arg(axis));
        CHECK(n);
        n->setValue(value);
        settle();
    }
    void click(const char* name)
    {
        auto* b = window.findChild<QAbstractButton*>(name);
        CHECK(b);
        b->click();
        settle();
    }
    c::RectI frame()
    {
        const auto& overlay = canvas->scene().transformOverlay;
        return overlay ? c::RectI { int(overlay->localToDocument.m02), int(overlay->localToDocument.m12),
            int(overlay->extent.width), int(overlay->extent.height) }
                       : c::RectI { };
    }
    c::Vec2d screenPoint()
    {
        const auto& scene = canvas->scene();
        return scene.viewport.documentToViewport(doc().layer(id)->localToDocument.map({ 35, 42 }),
            { double(doc().canvas().extent.width), double(doc().canvas().extent.height) },
            { double(canvas->width()), double(canvas->height()) });
    }
    void mouse(QEvent::Type type, QPointF p, Qt::MouseButton button, Qt::MouseButtons buttons)
    {
        QMouseEvent e(type, p, p, canvas->mapToGlobal(p.toPoint()), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &e);
    }
    void dragTopLeft()
    {
        const auto handle = canvas->logicalTransformHandles()[0];
        const QPointF from(handle.x, handle.y), to = from - QPointF(10, 20) * canvas->zoom();
        mouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        CHECK(canvas->transformDragging());
        mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        settle();
    }
};

void workflow(QVulkanInstance* instance = nullptr)
{
    Fixture f(instance);
    const auto base = f.doc().canvas();
    const auto matrix = f.doc().layer(f.id)->localToDocument;
    const auto revision = f.doc().revision(), state = f.doc().contentState();
    const auto depth = f.session().history().undoDepth();
    CHECK(f.action("QuickCanvasResizeAction")->shortcut() == QKeySequence("Ctrl+Alt+R"));
    bool ordered = false;
    for (auto* menu : f.window.findChildren<QMenu*>()) {
        const auto actions = menu->actions();
        const auto index = actions.indexOf(f.action("ChangeCanvasSizeAction"));
        if (index >= 0 && index + 1 < actions.size())
            ordered = actions[index + 1] == f.action("QuickCanvasResizeAction");
    }
    CHECK(ordered);
    f.begin(true);
    if (!f.active()) return;
    CHECK(!f.canvas->scene().transformOverlay->rotationEnabled);
    f.dragTopLeft();
    CHECK(f.frame() == c::RectI(-10, -20, 170, 140));
    CHECK(f.doc().canvas() == base && f.doc().revision() == revision && f.doc().contentState() == state);
    CHECK(f.session().history().undoDepth() == depth && f.doc().layer(f.id)->localToDocument == matrix);
    f.history(false);
    CHECK(f.frame() == c::RectI(0, 0, 160, 120));
    f.history(true);
    CHECK(f.frame() == c::RectI(-10, -20, 170, 140));
    QTest::keyClick(f.canvas, Qt::Key_Right);
    settle();
    CHECK(f.frame().x == -9);
    CHECK(f.doc().layer(f.id)->localToDocument == matrix);
    f.history(false);
    CHECK(f.frame().x == -10);
    const auto point = f.screenPoint();
    f.click("CanvasResizeApply");
    CHECK(!f.active());
    CHECK(f.doc().canvas().extent == c::Extent2u(170, 140));
    CHECK(f.session().history().undoDepth() == depth + 1);
    CHECK(near(f.screenPoint().x, point.x) && near(f.screenPoint().y, point.y));
    // Saved projects and flattened exports use the same rebased coordinates.
    auto exported = u::flattenDocument(f.doc());
    CHECK(exported);
    CHECK(exported.image.size() == QSize(170, 140));
    CHECK(exported.image.pixelColor(0, 0).alpha() == 0);
    CHECK(exported.image.pixelColor(10, 20) == QColor(80, 170, 200));
    CHECK(u::saveProject(f.files.filePath("resize.vulkana"), f.doc()));
    auto reloaded = u::loadProject(f.files.filePath("resize.vulkana"));
    CHECK(reloaded);
    if (reloaded)
        CHECK(u::flattenDocument(*reloaded.document).image == exported.image);
    f.history(false);
    CHECK(f.doc().canvas() == base && f.doc().contentState() == state);
    CHECK(f.doc().layer(f.id)->localToDocument == matrix);
    CHECK(near(f.screenPoint().x, point.x) && near(f.screenPoint().y, point.y));
    f.history(true);
    CHECK(f.doc().canvas().extent == c::Extent2u(170, 140));
    CHECK(near(f.screenPoint().x, point.x) && near(f.screenPoint().y, point.y));
    f.history(false);
    f.begin();
    f.value("W", 200);
    f.value("X", -30);
    f.click("CanvasResizeCancel");
    CHECK(f.doc().canvas() == base && f.doc().contentState() == state && f.session().history().canRedo());
    f.begin();
    f.click("CanvasResizeApply");
    CHECK(f.session().history().canRedo());
    f.begin();
    f.value("W", 210);
    QTest::keyClick(f.canvas, Qt::Key_Escape);
    settle();
    CHECK(!f.active() && f.doc().canvas() == base && f.session().history().canRedo());
    f.begin();
    f.value("H", 180);
    f.click("CanvasResizeReset");
    CHECK(f.frame() == c::RectI(0, 0, 160, 120));
    f.value("W", 220);
    QTest::keyClick(f.canvas, Qt::Key_Return);
    settle();
    CHECK(!f.active() && f.doc().canvas().extent == c::Extent2u(220, 120));
    f.history(false);
    // Switching documents commits the frame, without leaking it into the new tab.
    const auto first = f.window.activeDocumentId();
    f.begin();
    f.value("X", -5);
    f.value("W", 170);
    CHECK(f.window.openImageFromPath(f.files.filePath("resize.png")));
    settle();
    CHECK(!f.active() && f.doc().canvas() == base);
    CHECK(f.window.activateDocument(first));
    settle();
    CHECK(f.doc().canvas().extent == c::Extent2u(170, 120));
    f.history(false);
    CHECK(f.doc().canvas() == base);
    f.begin();
    f.value("W", 200);
    if (const auto path = qEnvironmentVariable("VULKANA_CANVAS_RESIZE_SCREENSHOT"); !path.isEmpty()) {
        QTest::qWait(250);
        const auto shot = instance ? f.window.screen()->grabWindow(f.window.winId()) : f.window.grab();
        CHECK(shot.save(path));
    }
    f.click("CanvasResizeCancel");
    // Save settles an active resize; its result survives a normal reopen.
    f.begin(); f.value("H",130);
    const auto savePath=f.files.filePath("quick-save.vulkana");
    u::MainWindow::FileInteractions hooks;
    hooks.chooseSavePath=[&]{return savePath;};
    hooks.reportError=[](const QString& error){std::cerr<<error.toStdString()<<'\n'; CHECK(false);};
    f.window.setFileInteractions(std::move(hooks));
    CHECK(f.window.saveDocument(true)); settle();
    CHECK(!f.active() && !f.doc().isModified());
    auto saved=u::loadProject(savePath); CHECK(saved);
    if(saved) CHECK(saved.document->canvas().extent==c::Extent2u(160,130));
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("QuickCanvasResizeTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    geometry();
    commandAndSelections();
    if (app.arguments().contains("--native")) {
        QVulkanInstance instance;
        instance.setApiVersion(QVersionNumber(1, 1));
        CHECK(instance.create());
        if (instance.isValid())
            workflow(&instance);
    } else
        workflow();
    std::cout << (failures ? "Quick canvas resize FAILED\n" : "Quick canvas resize passed\n");
    return failures ? 1 : 0;
}
