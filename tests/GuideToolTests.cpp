#include "NativeGraphicsStartup.hpp"
#include "imageeditor/core/BoundsSnapping.hpp"
#include "imageeditor/core/DocumentGuides.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/GuideCodec.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QAction>
#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QtTest>
#include <limits>
namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
class GuideToolTests : public QObject {
    Q_OBJECT
    QTemporaryDir files_, settings_;
    QVulkanInstance vulkan_;
    std::unique_ptr<u::MainWindow> window_;
    r::CanvasWindow* canvas_ { };
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window_->editorSession()); }
    QPointF logical(c::Vec2d point) const
    {
        const auto& scene = canvas_->scene();
        const auto e = scene.document.canvas.extent;
        const auto p = scene.viewport.documentToViewport(
            point, { double(e.width), double(e.height) }, scene.logicalViewport);
        return { p.x, p.y };
    }
    void mouse(QEvent::Type type, c::Vec2d point, Qt::MouseButton button, Qt::MouseButtons buttons,
        Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const auto p = logical(point);
        QMouseEvent event(
            type, p, p, QPointF(canvas_->mapToGlobal(p.toPoint())), button, buttons, modifiers);
        QCoreApplication::sendEvent(canvas_, &event);
        QCoreApplication::processEvents();
    }
    void hover(c::Vec2d point) { mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton); }
    void click(c::Vec2d point, Qt::MouseButton button = Qt::LeftButton)
    {
        hover(point);
        mouse(QEvent::MouseButtonPress, point, button, button);
        mouse(QEvent::MouseButtonRelease, point, button, Qt::NoButton);
    }
    void wheel(int delta)
    {
        const auto p = logical({ 100, 60 });
        QWheelEvent event(p, canvas_->mapToGlobal(p.toPoint()), { }, QPoint(0, delta), Qt::NoButton,
            Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(canvas_, &event);
        QCoreApplication::processEvents();
    }
    template <class T> T* find(const char* name)
    {
        return dynamic_cast<T*>(window_->findChild<QObject*>(name));
    }
    void action(const char* name)
    {
        auto* a = find<QAction>(name);
        QVERIFY(a);
        a->trigger();
        QCoreApplication::processEvents();
    }
    void publish() { canvas_->setDocument(session().document()->snapshot(), false); }
private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName("VulkanaGuideTests");
        QCoreApplication::setApplicationName("Guides");
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        u::applyEditorTheme(*qApp);
        if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
            vulkan_.setApiVersion(QVersionNumber(1, 2));
            QVERIFY(vulkan_.create());
        }
    }
    void init()
    {
        QSettings().clear();
        window_ = std::make_unique<u::MainWindow>(vulkan_.isValid() ? &vulkan_ : nullptr, false, false);
        window_->setUnsavedPromptEnabled(false);
        window_->resize(1640, 940);
        window_->show();
        QCoreApplication::processEvents();
        QVERIFY(imageeditor::test::waitForGraphicsStartup(*window_));
        for (auto* w : QGuiApplication::allWindows())
            if (w->objectName() == "VulkanCanvasWindow")
                canvas_ = dynamic_cast<r::CanvasWindow*>(w);
        QVERIFY(canvas_);
        QImage image(200, 120, QImage::Format_RGBA8888);
        image.fill(QColor(40, 80, 120));
        const auto path = files_.filePath("guide-source.png");
        QVERIFY(image.save(path));
        QVERIFY(window_->openImageFromPath(path));
        action("ToolAction_guides");
    }
    void cleanup()
    {
        window_->close();
        window_.reset();
        canvas_ = nullptr;
        QCoreApplication::processEvents();
    }
    void placementCountOrientationAndNativeNumericInput()
    {
        auto* count = find<u::ToolOptionsNumber>("GuideCount");
        QVERIFY(count);
        QCOMPARE(count->value(), 1.);
        QVERIFY(!find<QToolButton>("ClearGuidesButton")->isEnabled());
        hover({ 33.25, 25.5 });
        QCOMPARE(canvas_->scene().guidePreview.size(), std::size_t(1));
        QCOMPARE(canvas_->scene().guidePreview.front().position, 60.);
        hover({ 170, 93 });
        QCOMPARE(canvas_->scene().guidePreview.front().position, 60.);
        canvas_->setSpacePanHeld(true);
        QVERIFY(canvas_->scene().guidePreview.empty());
        canvas_->setSpacePanHeld(false);
        QCOMPARE(canvas_->scene().guidePreview.size(), std::size_t(1));
        QVERIFY(canvas_->setTemporaryMeasure(true));
        QVERIFY(canvas_->scene().guidePreview.empty());
        QVERIFY(canvas_->setTemporaryMeasure(false));
        QCOMPARE(canvas_->scene().guidePreview.size(), std::size_t(1));
        const auto zoom = canvas_->zoom();
        wheel(240);
        QCOMPARE(count->value(), 3.);
        QCOMPARE(canvas_->guideCount(), 3U);
        QCOMPARE(canvas_->zoom(), zoom);
        auto* vertical = find<QToolButton>("GuideVertical");
        QVERIFY(vertical);
        vertical->click();
        hover({ 110, 60 });
        QCOMPARE(canvas_->guideCount(), 3U);
        QCOMPARE(canvas_->scene().guidePreview.size(), std::size_t(3));
        QCOMPARE(canvas_->scene().guidePreview[0].position, 50.);
        QCOMPARE(canvas_->scene().guidePreview[1].position, 100.);
        QCOMPARE(canvas_->scene().guidePreview[2].position, 150.);
        const auto revision = session().document()->revision();
        QVERIFY(session().document()->guides().items.empty());
        click({ 110, 60 });
        QCOMPARE(session().document()->guides().items.size(), std::size_t(3));
        QCOMPARE(session().history().undoDepth(), std::size_t(1));
        QVERIFY(session().document()->revision() > revision);
        // Enter the count through the native host-window keyboard route.
        auto* editor = count->findChild<QLineEdit*>();
        QVERIFY(editor);
        QTest::mouseClick(editor, Qt::LeftButton);
        editor->selectAll();
        QInputMethodEvent input;
        input.setCommitString("5");
        QCoreApplication::sendEvent(window_->windowHandle(), &input);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(), &enter);
        QCOMPARE(count->value(), 5.);
        QCOMPARE(canvas_->guideCount(), 5U);
        find<QToolButton>("GuideHorizontal")->click();
        QCOMPARE(canvas_->guideCount(), 5U);
    }
    void priorityDragCancelRemoveAndToolIsolation()
    {
        hover({ 60, 60 });
        click({ 60, 60 });
        QCOMPARE(session().document()->guides().items.size(), std::size_t(1));
        const auto id = session().document()->guides().items[0].id;
        hover({ 70, 60 + 2 / canvas_->zoom() });
        QVERIFY(canvas_->scene().highlightedGuide == id);
        QVERIFY(canvas_->scene().guidePreview.empty());
        click({ 70, 60 + 2 / canvas_->zoom() });
        QCOMPARE(session().document()->guides().items[0].position, 60.);
        QCOMPARE(session().history().undoDepth(), std::size_t(1));
        mouse(QEvent::MouseButtonPress, { 70, 60 }, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, { 70, 50 }, Qt::NoButton, Qt::LeftButton);
        QCOMPARE(session().document()->guides().items[0].position, 60.);
        QCOMPARE(canvas_->scene().guidePreview[0].position, 50.);
        mouse(QEvent::MouseButtonPress, { 70, 50 }, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        mouse(QEvent::MouseButtonRelease, { 70, 50 }, Qt::RightButton, Qt::LeftButton);
        QVERIFY(canvas_->guideDragging());
        mouse(QEvent::MouseButtonRelease, { 70, 50 }, Qt::LeftButton, Qt::NoButton);
        QVERIFY(!canvas_->guideDragging());
        QCOMPARE(session().document()->guides().items[0].position, 50.);
        QCOMPARE(session().history().undoDepth(), std::size_t(2));
        QVERIFY(session().undo());
        publish();
        QCOMPARE(session().document()->guides().items[0].position, 60.);
        QVERIFY(session().redo());
        publish();
        QCOMPARE(session().document()->guides().items[0].position, 50.);
        mouse(QEvent::MouseButtonPress, { 70, 50 }, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, { 70, 80 }, Qt::NoButton, Qt::LeftButton);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas_, &escape);
        mouse(QEvent::MouseButtonRelease, { 70, 80 }, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(session().document()->guides().items[0].position, 50.);
        QVERIFY(canvas_->scene().guidePreview.empty());
        click({ 70, 90 });
        QCOMPARE(session().document()->guides().items.size(), std::size_t(1));
        action("ToolAction_guides");
        hover({ 70, 90 });
        QVERIFY(!canvas_->scene().guidePreview.empty());
        click({ 70, 50 }, Qt::RightButton);
        QVERIFY(session().document()->guides().items.empty());
        QVERIFY(canvas_->scene().guidePreview.empty());
        QVERIFY(session().undo());
        publish();
        action("ToolAction_move");
        const auto guides = session().document()->guides();
        hover({ 70, 50 });
        QVERIFY(!canvas_->scene().highlightedGuide);
        click({ 70, 50 });
        QCOMPARE(session().document()->guides(), guides);
        QVERIFY(canvas_->scene().guidePreview.empty());
    }
    void zoomPanVisibilityPersistenceAndExport()
    {
        click({ 70, 45.125 });
        mouse(QEvent::MouseButtonPress, { 70, 60 }, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, { 70, 45.125 }, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, { 70, 45.125 }, Qt::LeftButton, Qt::NoButton);
        const auto guides = session().document()->guides();
        QCOMPARE(guides.items.size(), std::size_t(1));
        const auto before = u::flattenDocument(*session().document());
        QVERIFY(before);
        canvas_->setZoomAt(2.25, { 200, 150 });
        hover({ 70, 45.125 });
        QVERIFY(canvas_->scene().highlightedGuide == guides.items[0].id);
        mouse(QEvent::MouseButtonPress, { 30, 20 }, Qt::MiddleButton, Qt::MiddleButton);
        mouse(QEvent::MouseMove, { 55, 35 }, Qt::NoButton, Qt::MiddleButton);
        mouse(QEvent::MouseButtonRelease, { 55, 35 }, Qt::MiddleButton, Qt::NoButton);
        hover({ 70, 45.125 });
        QVERIFY(canvas_->scene().highlightedGuide == guides.items[0].id);
        QCOMPARE(session().document()->guides(), guides);
        action("ShowGuidesAction");
        QVERIFY(!canvas_->scene().documentGuidesVisible);
        QVERIFY(canvas_->scene().guidePreview.empty());
        click({ 70, 45.125 }, Qt::RightButton);
        QCOMPARE(session().document()->guides(), guides);
        action("ShowGuidesAction");
        auto* clear = find<QToolButton>("ClearGuidesButton");
        QVERIFY(clear && clear->isVisible() && clear->isEnabled());
        QVERIFY(!clear->icon().isNull());
        clear->click();
        QVERIFY(session().document()->guides().items.empty());
        QVERIFY(!clear->isEnabled());
        QAction* undo = nullptr;
        for (auto* candidate : window_->findChildren<QAction*>())
            if (candidate->shortcut() == QKeySequence(QKeySequence::Undo)) {
                undo = candidate;
                break;
            }
        QVERIFY(undo && undo->isEnabled());
        undo->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(session().document()->guides(), guides);
        const auto after = u::flattenDocument(*session().document());
        QVERIFY(after);
        QCOMPARE(after.image, before.image);
        const auto path = files_.filePath("guides.vulkana");
        QVERIFY(u::saveProject(path, *session().document()));
        auto loaded = u::loadProject(path);
        QVERIFY2(loaded, qPrintable(loaded.error));
        QCOMPARE(loaded.document->guides(), guides);
        QCOMPARE(loaded.document->snapshot().guides, guides);
        action("ClearGuidesAction");
        const auto empty = session().document()->guides();
        QVERIFY(empty.items.empty());
        QVERIFY(u::saveProject(path, *session().document()));
        loaded = u::loadProject(path);
        QVERIFY2(loaded, qPrintable(loaded.error));
        QCOMPARE(loaded.document->guides(), empty);
    }
    void dragSnapsToLayerAndCanvasAnchorsWithControlBypass()
    {
        auto layer = c::Layer::raster("Snap target", std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{20,30}, c::Rgba8{150,70,90,255}));
        layer.localToDocument.m02 = 43; layer.localToDocument.m12 = 29;
        const auto layerId = layer.id;
        QVERIFY(session().document()->insertLayer(session().document()->layers().size(),std::move(layer)));
        publish();
        canvas_->setZoomAt(2, {200,150});
        click({90,80}); // Placement is centered regardless of the click position.
        QCOMPARE(session().document()->guides().items[0].position, 60.);
        mouse(QEvent::MouseButtonPress, {90,60}, Qt::LeftButton, Qt::LeftButton);
        const auto drag = [&](double position) {
            mouse(QEvent::MouseMove, {90,position}, Qt::NoButton, Qt::LeftButton);
            QVERIFY(canvas_->guideDragging());
        };
        drag(28);
        QCOMPARE(canvas_->scene().guidePreview[0].position,29.);
        QVERIFY(canvas_->scene().draggedGuide && canvas_->scene().snapGuides[1]);
        QCOMPARE(canvas_->scene().snapGuides[1]->target,layerId);
        QCOMPARE(session().document()->guides().items[0].position,60.);
        // Modifier-only updates use the same native host route as editor shortcuts.
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        QCoreApplication::sendEvent(window_->windowHandle(), &press);
        QCOMPARE(canvas_->scene().guidePreview[0].position,28.);
        QVERIFY(!canvas_->scene().snapGuides[1]);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(), &release);
        QCOMPARE(canvas_->scene().guidePreview[0].position,29.);
        drag(32.5); // Retain snapping inside the release radius.
        QCOMPARE(canvas_->scene().guidePreview[0].position,29.);
        drag(34);
        QCOMPARE(canvas_->scene().guidePreview[0].position,34.);
        QVERIFY(!canvas_->scene().snapGuides[1]);
        drag(43);
        QCOMPARE(canvas_->scene().guidePreview[0].position,44.);
        drag(58);
        QCOMPARE(canvas_->scene().guidePreview[0].position,59.);
        drag(72); drag(61);
        QCOMPARE(canvas_->scene().guidePreview[0].position,60.);
        QCOMPARE(canvas_->scene().snapGuides[1]->target,c::LayerId(0));
        // Canvas and layer snapping toggles remain independent.
        action("SnapCanvasAction"); action("SnapLayersAction");
        QCOMPARE(canvas_->scene().guidePreview[0].position,61.);
        QVERIFY(!canvas_->scene().snapGuides[1]);
        action("SnapCanvasAction");
        QCOMPARE(canvas_->scene().guidePreview[0].position,60.);
        drag(1);
        QCOMPARE(canvas_->scene().guidePreview[0].position,0.);
        drag(119);
        QCOMPARE(canvas_->scene().guidePreview[0].position,120.);
        mouse(QEvent::MouseButtonRelease, {90,119}, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(session().document()->guides().items[0].position,120.);
        QVERIFY(!canvas_->scene().snapGuides[1]);
        QVERIFY(session().undo()); publish();
        QCOMPARE(session().document()->guides().items[0].position,60.);
        action("SnapLayersAction");
        find<QToolButton>("GuideVertical")->click();
        click({20,80});
        QCOMPARE(session().document()->guides().items[1].position,100.);
        mouse(QEvent::MouseButtonPress, {100,80}, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, {44,80}, Qt::NoButton, Qt::LeftButton);
        QCOMPARE(canvas_->scene().guidePreview[0].position,43.);
        QVERIFY(canvas_->scene().snapGuides[0] && !canvas_->scene().snapGuides[1]);
        mouse(QEvent::MouseMove, {52,80}, Qt::NoButton, Qt::LeftButton);
        QCOMPARE(canvas_->scene().guidePreview[0].position,53.);
        mouse(QEvent::MouseMove, {64,80}, Qt::NoButton, Qt::LeftButton);
        QCOMPARE(canvas_->scene().guidePreview[0].position,63.);
        mouse(QEvent::MouseMove, {101,80}, Qt::NoButton, Qt::LeftButton);
        QCOMPARE(canvas_->scene().guidePreview[0].position,100.);
        mouse(QEvent::MouseMove, {102,80}, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
        QCOMPARE(canvas_->scene().guidePreview[0].position,102.);
        QVERIFY(!canvas_->scene().snapGuides[0]);
        mouse(QEvent::MouseButtonRelease, {102,80}, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
        QCOMPARE(session().document()->guides().items[1].position,102.);
        QVERIFY(!canvas_->scene().draggedGuide && !canvas_->scene().snapGuides[0]);
    }
    void snappingValidationAndStaleCommands()
    {
        c::Document doc({ { 200, 120 } });
        c::ShapeLayer shape;
        shape.size = { 20, 20 };
        auto layer = c::Layer::shape("Mover", shape);
        layer.localToDocument.m02 = 20;
        layer.localToDocument.m12 = 20;
        const auto id = layer.id;
        QVERIFY(doc.insertLayer(0, std::move(layer)));
        c::DocumentGuides guides {
            { { 1, c::GuideOrientation::Vertical, 75 }, { 2, c::GuideOrientation::Horizontal, 60 } }, 3
        };
        QVERIFY(doc.setGuides({ }, guides));
        c::BoundsSnapping snap;
        QVERIFY(snap.begin(doc, std::array { id }));
        c::SnapOptions options;
        options.canvas = false;
        options.layers = false;
        const auto result = snap.resolve(doc, { 34, 19 }, options);
        QCOMPARE(result.translation.x, 35.);
        QCOMPARE(result.translation.y, 20.);
        QVERIFY(result.guides[0] && result.guides[1]);
        options.guides = false;
        const auto unsnapped = snap.resolve(doc, { 34, 19 }, options);
        QCOMPARE(unsnapped.translation.x, 34.);
        QCOMPARE(unsnapped.translation.y, 19.);
        auto invalid = guides;
        invalid.items[0].position = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!doc.setGuides(guides, invalid));
        invalid = guides;
        invalid.items[1].id = 1;
        QVERIFY(!c::validDocumentGuides(invalid));
        c::ChangeDocumentGuidesCommand stale({ }, guides, "Guides");
        QVERIFY(!stale.apply(doc));
        QCOMPARE(doc.guides(), guides);
        QCOMPARE(u::decodeDocumentGuides(u::encodeDocumentGuides(guides)), guides);
        auto json = u::encodeDocumentGuides(guides);
        json["nextId"] = "1";
        QVERIFY_EXCEPTION_THROWN(u::decodeDocumentGuides(json), std::invalid_argument);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    GuideToolTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "GuideToolTests.moc"
