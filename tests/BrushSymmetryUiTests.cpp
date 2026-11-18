#include "NativeGraphicsStartup.hpp"
#include "imageeditor/core/BrushSymmetry.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTimer>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QtTest>
#include <algorithm>
#include <array>
namespace c=imageeditor::core;
namespace r=imageeditor::render;
namespace u=imageeditor::ui;
class BrushSymmetryUiTests : public QObject {
    Q_OBJECT
    QTemporaryDir files_, settings_;
    QVulkanInstance vulkan_;
    std::unique_ptr<u::MainWindow> window_;
    r::CanvasWindow* canvas_{};
    c::EditorSession& session(){return const_cast<c::EditorSession&>(window_->editorSession());}
    template<class T> T* find(const char* name){return dynamic_cast<T*>(window_->findChild<QObject*>(name));}
    void action(const char* name){auto* value=find<QAction>(name);QVERIFY(value);value->trigger();QCoreApplication::processEvents();}
    QPointF logical(c::Vec2d point)const {
        const auto& scene=canvas_->scene();const auto extent=scene.document.canvas.extent;
        const auto value=scene.viewport.documentToViewport(point,{double(extent.width),double(extent.height)},scene.logicalViewport);
        return {value.x,value.y};
    }
    void mouse(QEvent::Type type,c::Vec2d point,Qt::MouseButton button,Qt::MouseButtons buttons,
        Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
        const auto p=logical(point);
        QMouseEvent event(type,p,p,QPointF(canvas_->mapToGlobal(p.toPoint())),button,buttons,modifiers);
        QCoreApplication::sendEvent(canvas_,&event);QCoreApplication::processEvents();
    }
    void click(c::Vec2d point) {
        mouse(QEvent::MouseMove,point,Qt::NoButton,Qt::NoButton);
        mouse(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);
    }
    std::vector<std::byte> pixels() {
        const auto* layer=session().document()->layer(*session().activeLayer());
        const auto surface=std::get<c::RasterLayer>(layer->payload).surface;
        const auto e=surface->extent();std::vector<std::byte> bytes(std::size_t(e.width)*e.height*4);
        surface->copyRgba8({0,0,int(e.width),int(e.height)},bytes,std::size_t(e.width)*4);return bytes;
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("VulkanaBrushSymmetryTests");
        QCoreApplication::setApplicationName("Symmetry");
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());
        u::applyEditorTheme(*qApp);
        if(QGuiApplication::platformName()!=QStringLiteral("offscreen")) {
            vulkan_.setApiVersion(QVersionNumber(1,2));QVERIFY(vulkan_.create());
        }
    }
    void init() {
        QSettings().clear();
        window_=std::make_unique<u::MainWindow>(vulkan_.isValid()?&vulkan_:nullptr,false,false);
        window_->setUnsavedPromptEnabled(false);window_->resize(1640,940);window_->show();
        QCoreApplication::processEvents();QVERIFY(imageeditor::test::waitForGraphicsStartup(*window_));
        for(auto* w:QGuiApplication::allWindows())if(w->objectName()=="VulkanCanvasWindow")canvas_=dynamic_cast<r::CanvasWindow*>(w);
        QVERIFY(canvas_);
        QImage image(200,120,QImage::Format_RGBA8888);image.fill(Qt::white);
        const auto path=files_.filePath("symmetry.png");QVERIFY(image.save(path));QVERIFY(window_->openImageFromPath(path));
        action("ToolAction_brush");
    }
    void cleanup(){window_->close();window_.reset();canvas_=nullptr;QCoreApplication::processEvents();}
    void placementAxesToggleAndUndo() {
        auto* button=find<QToolButton>("BrushSymmetryButton");QVERIFY(button && button->isVisible() && button->isEnabled());
        QVERIFY(!button->icon().isNull());QVERIFY(!button->isChecked());
        const auto before=pixels();const auto depth=session().history().undoDepth();
        button->click();QVERIFY(canvas_->brushSymmetryPlacementActive());QVERIFY(button->isChecked());
        QVERIFY(!session().document()->brushSymmetry().enabled);QCOMPARE(session().history().undoDepth(),depth);
        click({80,55});
        QVERIFY(!canvas_->brushSymmetryPlacementActive());
        const auto state=session().document()->brushSymmetry();
        QVERIFY(state.enabled && state.center);QCOMPARE(state.center->x,80.);QCOMPARE(state.center->y,55.);
        QCOMPARE(state.axis,c::BrushSymmetryAxis::Vertical);
        QCOMPARE(pixels(),before);QCOMPARE(session().history().undoDepth(),depth+1);
        action("BrushSymmetryHorizontalAction");
        QCOMPARE(session().document()->brushSymmetry().axis,c::BrushSymmetryAxis::Horizontal);
        button->click();QVERIFY(!session().document()->brushSymmetry().enabled);
        QCOMPARE(session().document()->brushSymmetry().center,state.center);
        button->click();QVERIFY(session().document()->brushSymmetry().enabled);QVERIFY(!canvas_->brushSymmetryPlacementActive());
        action("BrushSymmetryRepositionAction");QVERIFY(canvas_->brushSymmetryPlacementActive());
        mouse(QEvent::MouseButtonPress,{80,55},Qt::LeftButton,Qt::LeftButton);
        mouse(QEvent::MouseMove,{110,75},Qt::NoButton,Qt::LeftButton);
        QCOMPARE(session().document()->brushSymmetry().center,state.center);
        mouse(QEvent::MouseButtonRelease,{110,75},Qt::LeftButton,Qt::NoButton);
        QVERIFY(session().document()->brushSymmetry().center);
        QCOMPARE(session().document()->brushSymmetry().center->x,110.);
        QCOMPARE(session().document()->brushSymmetry().center->y,75.);
        QCOMPARE(pixels(),before);
        QVERIFY(session().undo());
        canvas_->setDocument(session().document()->snapshot(),false);action("ToolAction_brush");
        QCOMPARE(session().document()->brushSymmetry().center,state.center);QVERIFY(button->isChecked());
        QVERIFY(session().redo());canvas_->setDocument(session().document()->snapshot(),false);action("ToolAction_brush");
        QVERIFY(session().document()->brushSymmetry().center);
        QCOMPARE(session().document()->brushSymmetry().center->x,110.);
        QCOMPARE(session().document()->brushSymmetry().center->y,75.);
        action("ToolAction_eraser");QVERIFY(button->isVisible() && button->isEnabled() && button->isChecked());
        action("ToolAction_move");QVERIFY(!button->isVisible());
        action("ToolAction_brush");QVERIFY(button->isVisible() && button->isChecked());
    }
    void dropdownKeepsInitialPlacementUsable()
    {
        auto* button=find<QToolButton>("BrushSymmetryButton");
        auto* options=find<QToolButton>("BrushSymmetryOptionsButton");
        auto* horizontal=find<QAction>("BrushSymmetryHorizontalAction");
        QVERIFY(button && options && options->menu() && horizontal);
        auto* menu=options->menu();
        // Start from native canvas focus, without laying down a brush mark.
        mouse(QEvent::MouseButtonPress,{100,60},Qt::MiddleButton,Qt::MiddleButton);
        mouse(QEvent::MouseButtonRelease,{100,60},Qt::MiddleButton,Qt::NoButton);
        QTest::mouseClick(button,Qt::LeftButton);
        QVERIFY(canvas_->brushSymmetryPlacementActive());
        const auto original=pixels();
        bool sawMenu=false, placingWhileMenuOpen=false, selected=false;
        QTimer chooser, watchdog;
        chooser.setInterval(20);
        connect(&chooser,&QTimer::timeout,this,[&] {
            if(!menu->isVisible())return;
            sawMenu=true;placingWhileMenuOpen=canvas_->brushSymmetryPlacementActive();
            const auto row=menu->actionGeometry(horizontal);
            if(row.isEmpty())return;
            chooser.stop();QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,row.center());selected=true;
        });
        watchdog.setSingleShot(true);
        connect(&watchdog,&QTimer::timeout,menu,&QMenu::close);
        chooser.start();watchdog.start(2500);
        QTest::mouseClick(options,Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(selected,3000);
        watchdog.stop();chooser.stop();
        qInfo()<<"Symmetry dropdown:"<<sawMenu<<"placement while open:"<<placingWhileMenuOpen
               <<"after selection:"<<canvas_->brushSymmetryPlacementActive();
        QCOMPARE(session().document()->brushSymmetry().axis,c::BrushSymmetryAxis::Horizontal);
        QTRY_VERIFY_WITH_TIMEOUT(canvas_->brushSymmetryPlacementActive(),1000);
        click({80,55});
        QVERIFY(session().document()->brushSymmetry().enabled);
        QCOMPARE(session().document()->brushSymmetry().axis,c::BrushSymmetryAxis::Horizontal);
        QCOMPARE(pixels(),original);
    }
    void symmetryToolbarLayout()
    {
        auto* button=find<QToolButton>("BrushSymmetryButton");
        auto* options=find<QToolButton>("BrushSymmetryOptionsButton");
        auto* group=find<QWidget>("BrushSymmetryControls");
        QVERIFY(button && options && group);
        QCOMPARE(button->text(),QStringLiteral("Symmetry"));
        QCOMPARE(button->toolButtonStyle(),Qt::ToolButtonTextBesideIcon);
        QCOMPARE(button->iconSize(),QSize(18,18));
        QCOMPARE(button->height(),30);
        QVERIFY(button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 18 + 12);
        QVERIFY(!button->menu());
        QCOMPARE(options->size(),QSize(30,30));
        QCOMPARE(options->arrowType(),Qt::DownArrow);
        QCOMPARE(options->popupMode(),QToolButton::InstantPopup);
        QVERIFY(options->menu() && !options->isCheckable());
        QCOMPARE(options->geometry().left() - button->geometry().right(),3);
        QCOMPARE(button->geometry().top(),options->geometry().top());
        QCOMPARE(group->width(),button->width()+options->width()+2);
        const auto capture=qEnvironmentVariable("VULKANA_SYMMETRY_TOOLBAR_CAPTURE_DIR");
        if(!capture.isEmpty()) {
            QVERIFY(QDir().mkpath(capture));
            QVERIFY(group->grab().save(QDir(capture).filePath("symmetry-toolbar.png")));
        }
    }
    void cancellationAndDocumentIndependence() {
        auto* button=find<QToolButton>("BrushSymmetryButton");QVERIFY(button);
        const auto before=pixels();const auto depth=session().history().undoDepth();
        button->click();QVERIFY(canvas_->brushSymmetryPlacementActive());
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(),&escape);
        QVERIFY(!canvas_->brushSymmetryPlacementActive());QVERIFY(!button->isChecked());
        QCOMPARE(pixels(),before);QCOMPARE(session().history().undoDepth(),depth);
        QVERIFY(!session().document()->brushSymmetry().center);
        button->click();click({70,45});
        const auto saved=session().document()->brushSymmetry();
        auto* firstDocument=session().document();
        action("BrushSymmetryRepositionAction");QVERIFY(canvas_->brushSymmetryPlacementActive());
        action("ToolAction_move");QVERIFY(!canvas_->brushSymmetryPlacementActive());
        QCOMPARE(firstDocument->brushSymmetry(),saved);
        QImage image(80,60,QImage::Format_RGBA8888);image.fill(Qt::black);
        const auto path=files_.filePath("second.png");QVERIFY(image.save(path));QVERIFY(window_->openImageFromPath(path));
        action("ToolAction_brush");
        QVERIFY(session().document()!=firstDocument);QVERIFY(!session().document()->brushSymmetry().enabled);
        QVERIFY(!button->isChecked());QCOMPARE(firstDocument->brushSymmetry(),saved);
        auto* tabs=find<QTabBar>("DocumentTabs");QVERIFY(tabs && tabs->count()==2);
        button->click();QVERIFY(canvas_->brushSymmetryPlacementActive());
        tabs->setCurrentIndex(0);QCoreApplication::processEvents();
        QVERIFY(!canvas_->brushSymmetryPlacementActive());
        QCOMPARE(session().document(),firstDocument);action("ToolAction_brush");
        QVERIFY(button->isChecked());QCOMPARE(canvas_->scene().document.brushSymmetry,saved);
        action("BrushSymmetryRepositionAction");QVERIFY(canvas_->brushSymmetryPlacementActive());
        tabs->setCurrentIndex(1);QCoreApplication::processEvents();
        QVERIFY(!canvas_->brushSymmetryPlacementActive());
        QVERIFY(!session().document()->brushSymmetry().enabled && !session().document()->brushSymmetry().center);
        QCOMPARE(firstDocument->brushSymmetry(),saved);
    }
    void controlsRemainAvailableAfterOrdinaryAndMirroredStrokes()
    {
        auto* button=find<QToolButton>("BrushSymmetryButton");
        auto* options=find<QToolButton>("BrushSymmetryOptionsButton");
        QVERIFY(button && options);
        const auto available=[&] {
            QVERIFY(!canvas_->pointerGestureActive());
            QVERIFY(button->isEnabled());QVERIFY(options->isEnabled());
            QVERIFY(find<QAction>("BrushSymmetryHorizontalAction")->isEnabled());
            QVERIFY(find<QAction>("BrushSymmetryRepositionAction")->isEnabled());
        };
        available();
        click({30,20}); // No symmetry interaction at all before this stroke.
        available();QVERIFY(!session().document()->brushSymmetry().enabled);
        button->click();click({80,55});available();
        click({30,20});available();
        button->click();QVERIFY(!session().document()->brushSymmetry().enabled);
        available();
        button->click();QVERIFY(session().document()->brushSymmetry().enabled);
        action("ToolAction_eraser");click({30,20});available();
        click({30,20});available(); // Already-erased/no-change completion.
        action("ToolAction_brush");
        mouse(QEvent::MouseButtonPress,{35,20},Qt::LeftButton,Qt::LeftButton);
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(),&escape);
        mouse(QEvent::MouseButtonRelease,{35,20},Qt::LeftButton,Qt::NoButton);
        available();
        button->click();QVERIFY(!session().document()->brushSymmetry().enabled);
        available();
    }
    void targetSnapsAndControlBypassesWithoutCommitting()
    {
        auto layer=c::Layer::raster("Snap target",std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{20,30},c::Rgba8{90,130,180,255}));
        layer.localToDocument.m02=40;layer.localToDocument.m12=25;
        const auto id=layer.id;
        QVERIFY(session().document()->insertLayer(session().document()->layers().size(),std::move(layer)));
        canvas_->setDocument(session().document()->snapshot(),false);
        canvas_->setZoomAt(2,{200,150});
        const auto before=pixels();const auto depth=session().history().undoDepth();
        find<QToolButton>("BrushSymmetryButton")->click();
        const auto hover=[&](c::Vec2d p,Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
            mouse(QEvent::MouseMove,p,Qt::NoButton,Qt::NoButton,modifiers);
        };
        const auto target=[&] {return *canvas_->scene().brushSymmetryPlacement->center;};
        hover({101,59});QCOMPARE(target(),(c::Vec2d{100,60}));
        QVERIFY(canvas_->scene().snapGuides[0] && canvas_->scene().snapGuides[1]);
        QVERIFY(!session().document()->brushSymmetry().center);
        QCOMPARE(session().history().undoDepth(),depth);QCOMPARE(pixels(),before);
        QKeyEvent ctrl(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier);
        QCoreApplication::sendEvent(window_->windowHandle(),&ctrl);
        QCOMPARE(target(),(c::Vec2d{101,59}));
        QVERIFY(!canvas_->scene().snapGuides[0] && !canvas_->scene().snapGuides[1]);
        QKeyEvent release(QEvent::KeyRelease,Qt::Key_Control,Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(),&release);
        QCOMPARE(target(),(c::Vec2d{100,60}));
        hover({104,64});QCOMPARE(target(),(c::Vec2d{100,60})); // Release hysteresis.
        hover({106,66});QCOMPARE(target(),(c::Vec2d{106,66}));
        hover({49,39});QCOMPARE(target(),(c::Vec2d{50,40}));
        QCOMPARE(canvas_->scene().snapGuides[0]->target,id);
        hover({39,24});QCOMPARE(target(),(c::Vec2d{40,25}));
        action("SnapLayersAction");hover({49,39});QCOMPARE(target(),(c::Vec2d{49,39}));
        // Persisted guides are also available through the existing snap targets.
        const c::DocumentGuides guides{{{1,c::GuideOrientation::Vertical,73},{2,c::GuideOrientation::Horizontal,83}},3};
        QVERIFY(session().document()->setGuides({},guides));
        canvas_->setDocument(session().document()->snapshot(),false);
        hover({74,84});QCOMPARE(target(),(c::Vec2d{73,83}));
        action("SnappingAction");QCOMPARE(target(),(c::Vec2d{74,84}));
        QVERIFY(!canvas_->scene().snapGuides[0] && !canvas_->scene().snapGuides[1]);
        action("SnappingAction");QCOMPARE(target(),(c::Vec2d{73,83}));
        mouse(QEvent::MouseButtonPress,{101,59},Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier);
        mouse(QEvent::MouseButtonRelease,{101,59},Qt::LeftButton,Qt::NoButton,Qt::ControlModifier);
        QVERIFY(!canvas_->brushSymmetryPlacementActive());
        QCOMPARE(*session().document()->brushSymmetry().center,(c::Vec2d{101,59}));
        QVERIFY(!canvas_->scene().snapGuides[0] && !canvas_->scene().snapGuides[1]);
        QCOMPARE(pixels(),before);QCOMPARE(session().history().undoDepth(),depth+1);
        action("BrushSymmetryRepositionAction");hover({101,59});
        QCOMPARE(target(),(c::Vec2d{100,60}));
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(),&escape);
        QVERIFY(!canvas_->scene().snapGuides[0] && !canvas_->scene().snapGuides[1]);
        QCOMPARE(*session().document()->brushSymmetry().center,(c::Vec2d{101,59}));
    }
    void paintEraseAndCancelShareOneStroke()
    {
        auto* button=find<QToolButton>("BrushSymmetryButton");QVERIFY(button);
        button->click();click({80,55});
        const auto symmetry=session().document()->brushSymmetry();QVERIFY(symmetry.enabled);
        session().setForegroundColor({16,66,144,255});
        const auto before=pixels();const auto depth=session().history().undoDepth();
        const auto pixel=[](const std::vector<std::byte>& bytes,int x,int y) {
            std::array<std::byte,4> value{};
            std::copy_n(bytes.begin()+(std::ptrdiff_t(y)*200+x)*4,4,value.begin());return value;
        };
        click({30,20});
        const auto painted=pixels();
        QVERIFY(painted!=before);
        QCOMPARE(pixel(painted,30,20),pixel(painted,129,20));
        QVERIFY(pixel(painted,30,20)!=pixel(before,30,20));
        QCOMPARE(session().history().undoDepth(),depth+1);
        QVERIFY(session().undo());canvas_->setDocument(session().document()->snapshot(),false);action("ToolAction_brush");
        QCOMPARE(pixels(),before);QCOMPARE(session().document()->brushSymmetry(),symmetry);

        action("ToolAction_eraser");click({30,20});
        const auto erased=pixels();
        QCOMPARE(pixel(erased,30,20),pixel(erased,129,20));
        QVERIFY(std::to_integer<unsigned>(pixel(erased,30,20)[3])<255);
        QCOMPARE(session().history().undoDepth(),depth+1);
        QVERIFY(session().undo());canvas_->setDocument(session().document()->snapshot(),false);action("ToolAction_brush");
        QCOMPARE(pixels(),before);QCOMPARE(session().document()->brushSymmetry(),symmetry);

        mouse(QEvent::MouseButtonPress,{30,20},Qt::LeftButton,Qt::LeftButton);
        mouse(QEvent::MouseMove,{35,24},Qt::NoButton,Qt::LeftButton);
        const auto live=pixels();
        QVERIFY(pixel(live,30,20)!=pixel(before,30,20));
        QCOMPARE(pixel(live,30,20),pixel(live,129,20));
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QCoreApplication::sendEvent(window_->windowHandle(),&escape);
        mouse(QEvent::MouseButtonRelease,{35,24},Qt::LeftButton,Qt::NoButton);
        QCOMPARE(pixels(),before);QCOMPARE(session().history().undoDepth(),depth);
        QCOMPARE(session().document()->brushSymmetry(),symmetry);
    }
    void refinementTemporarilySuppressesSymmetry()
    {
        auto* button=find<QToolButton>("BrushSymmetryButton");QVERIFY(button);
        button->click();click({80,55});
        const auto symmetry=session().document()->brushSymmetry();
        QVERIFY(symmetry.enabled && canvas_->scene().brushSymmetryContextEnabled);
        action("SelectAllAction");
        action("RefineSelectionAction");
        QCOMPARE(session().activeTool(),c::ToolId::Brush);
        QVERIFY(!canvas_->scene().brushSymmetryContextEnabled);
        QVERIFY(!button->isEnabled());
        auto* cancel=find<QPushButton>("RefineCancel");QVERIFY(cancel);cancel->click();
        QCoreApplication::processEvents();
        QCOMPARE(session().document()->brushSymmetry(),symmetry);
        QVERIFY(canvas_->scene().brushSymmetryContextEnabled);
        QVERIFY(button->isEnabled() && button->isChecked());
    }
};
int main(int argc,char**argv){QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);QApplication app(argc,argv);BrushSymmetryUiTests tests;return QTest::qExec(&tests,argc,argv);}
#include "BrushSymmetryUiTests.moc"
