#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/AdjustmentCurveEditor.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <atomic>
#include <iostream>
#include "AdjustmentMaskOutlineChecks.inc"

namespace c=imageeditor::core;
namespace u=imageeditor::ui;
namespace r=imageeditor::render;
namespace {
int failures=0;
void check(bool b,const char* text,int line){if(!b){++failures;std::cerr<<"FAIL "<<line<<": "<<text<<'\n';}}
#define CHECK(...) check(bool(__VA_ARGS__),#__VA_ARGS__,__LINE__)
void settle(){for(int i=0;i<3;++i){QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();}}
#include "AdjustmentCurvePaintChecks.inc"
struct Fixture {
    QTemporaryDir files;
    u::MainWindow window;
    u::AdjustmentsPanel* panel{};
    u::LayerListView* view{};
    u::LayerListModel* model{};
    r::CanvasWindow* canvas{};
    c::LayerId id{};
    explicit Fixture(QVulkanInstance* instance=nullptr):window(instance,false,false){
        window.setUnsavedPromptEnabled(false);window.resize(1550,1080);window.show();settle();
        panel=dynamic_cast<u::AdjustmentsPanel*>(window.findChild<QWidget*>("AdjustmentsPanelContent"));
        view=dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
        model=view?dynamic_cast<u::LayerListModel*>(view->model()):nullptr;
        for(auto* candidate:QGuiApplication::allWindows())if(candidate->objectName()=="VulkanCanvasWindow")canvas=dynamic_cast<r::CanvasWindow*>(candidate);
        QImage image(80,64,QImage::Format_RGBA8888);image.fill(QColor(100,140,180,160));
        const auto path=files.filePath("adjustments-ui.png");CHECK(image.save(path));CHECK(window.openImageFromPath(path));
        id=session().activeLayer().value_or(0);CHECK(panel&&model&&view&&canvas&&id);settle();
    }
    ~Fixture(){window.close();settle();}
    c::EditorSession& session(){return const_cast<c::EditorSession&>(window.editorSession());}
    c::Document& document(){return *session().document();}
    template<class T>T* widget(const char* name){auto* result=window.findChild<T*>(QString::fromLatin1(name));CHECK(result);return result;}
    u::CompactValueControl* number(const char* name){return dynamic_cast<u::CompactValueControl*>(widget<QDoubleSpinBox>(name));}
    c::AdjustmentStack state(c::LayerId target=0){const auto* layer=document().layer(target?target:id);return layer&&layer->adjustments?*layer->adjustments:c::AdjustmentStack{};}
    void history(bool redo){for(auto* a:window.findChildren<QAction*>())if(a->shortcuts().contains(redo?QKeySequence(QKeySequence::Redo):QKeySequence(QKeySequence::Undo))){a->trigger();settle();return;}CHECK(false);}
    void select(c::LayerId target,Qt::KeyboardModifiers modifiers={}){
        model->refresh();const auto row=model->index(model->rowForLayer(target));CHECK(row.isValid());
        view->scrollTo(row);settle();QTest::mouseClick(view->viewport(),Qt::LeftButton,modifiers,view->visualRect(row).center());settle();
    }
    void page(int group,int item){widget<QTabWidget>("AdjustmentTabs")->setCurrentIndex(group);widget<QComboBox>(qPrintable(QStringLiteral("AdjustmentNavigation%1").arg(group)))->setCurrentIndex(item);settle();}
};
#include "AdjustmentCapturedRegionUiChecks.inc"
void freshProfileUsesApprovedLayout()
{
    // main() redirects QSettings to a temporary directory, never user preferences.
    u::MainWindow window(nullptr,true,false);
    window.setUnsavedPromptEnabled(false);window.resize(1550,1080);window.show();settle();
    auto* workspace=dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
    auto* left=window.findChild<QSplitter*>("LeftPanelSplitter");
    auto* right=window.findChild<QSplitter*>("RightPanelSplitter");
    CHECK(workspace&&left&&right);if(!workspace||!left||!right)return;
    CHECK(workspace->leftPanelWidth()==305);CHECK(workspace->panelWidth()==556);
    CHECK(left->count()==1&&left->widget(0)->objectName()=="LayersPanel");
    CHECK(right->count()==3);if(right->count()!=3)return;
    CHECK(right->widget(0)->objectName()=="ColorPanelShell");
    CHECK(right->widget(1)->objectName()=="PropertiesPanelShell");
    CHECK(right->widget(2)->objectName()=="AdjustmentsPanelShell");
    const auto sizes=right->sizes();
    CHECK(sizes[0]<sizes[1]&&sizes[1]<sizes[2]);
    CHECK(std::abs(double(sizes[1])/sizes[2]-470.0/630.0)<.08);
    window.close();settle();
}
void activePageControlsScrolling()
{
    u::AdjustmentsPanel panel;
    panel.resize(556,400);panel.show();settle();
    auto* tabs=panel.findChild<QTabWidget*>("AdjustmentTabs");
    auto* navigation=panel.findChild<QComboBox*>("AdjustmentNavigation0");
    auto* scroll=tabs?tabs->widget(0)->findChild<QScrollArea*>():nullptr;
    CHECK(navigation&&scroll);if(!navigation||!scroll)return;
    CHECK(!scroll->verticalScrollBar()->isVisible());
    CHECK(scroll->verticalScrollBar()->maximum()==0);
    navigation->setCurrentIndex(3);settle(); // Curves genuinely needs more room.
    CHECK(scroll->verticalScrollBar()->isVisible());
    CHECK(scroll->verticalScrollBar()->maximum()>0);
    navigation->setCurrentIndex(0);settle();
    CHECK(!scroll->verticalScrollBar()->isVisible());
    CHECK(scroll->verticalScrollBar()->maximum()==0);
    panel.resize(305,400);settle(); // Wrapped descriptions still fit.
    CHECK(scroll->horizontalScrollBar()->maximum()==0);
    CHECK(!scroll->verticalScrollBar()->isVisible());
}
void navigationAndPrimary()
{
    Fixture f;if(!f.panel)return;
    const auto before=f.document().contentState();const auto depth=f.session().history().undoDepth();
    auto* exposure=f.number("AdjustmentExposure");
    auto* filters=f.panel->filtersPanel();
    CHECK(f.widget<QTabWidget>("AdjustmentTabs")->count()==5);CHECK(filters); // Effects is the fifth category.
    for(int group=0;group<3;++group){const int count=group==2?2:4;for(int item=0;item<count;++item)f.page(group,item);}
    for(const auto type:{c::SpatialFilterType::Gaussian,c::SpatialFilterType::Motion,c::SpatialFilterType::Lens}){
        f.panel->showFilter(type);settle();
        CHECK(f.panel->filtersCategoryActive());CHECK(f.panel->filtersPanel()==filters);
        CHECK(filters->currentType()==type);
    }
    CHECK(f.document().contentState()==before);CHECK(f.session().history().undoDepth()==depth);
    CHECK(f.number("AdjustmentExposure")==exposure); // Navigation never rebuilds widgets.
    f.page(0,0);exposure->setValue(1.25);settle();
    CHECK(f.state().items[0].enabled);CHECK(std::get<c::ExposureParameters>(f.state().items[0].parameters).stops==1.25);
    CHECK(f.session().history().undoDepth()==depth+1);
    auto second=c::Layer::raster("Other",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{20,20},c::Rgba8{240,90,30,255}));
    const auto secondId=second.id;CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(second),1)));
    f.select(f.id);f.select(secondId,Qt::ControlModifier);CHECK(f.session().selectedLayers().size()==2);
    CHECK(exposure->value()==0);exposure->setValue(-.5);settle();
    CHECK(std::get<c::ExposureParameters>(f.state(secondId).items[0].parameters).stops==-.5);
    CHECK(std::get<c::ExposureParameters>(f.state().items[0].parameters).stops==1.25);
    f.select(f.id);CHECK(exposure->value()==1.25);
}
void compactCategoryLayout()
{
    Fixture f;if(!f.panel)return;
    const auto rectangle=[&](QWidget* widget){return QRect(widget->mapTo(f.panel,QPoint{}),widget->size());};
    auto* tabs=f.widget<QTabWidget>("AdjustmentTabs");
    for(int group=0;group<3;++group){
        CHECK(tabs->widget(group)->layout()->contentsMargins()==QMargins(8,8,8,8));
        auto* navigation=f.widget<QComboBox>(qPrintable(QStringLiteral("AdjustmentNavigation%1").arg(group)));
        auto* enabledStack=f.widget<QStackedWidget>(qPrintable(QStringLiteral("AdjustmentEnabledStack%1").arg(group)));
        for(int row=0;row<navigation->count();++row){
            f.page(group,row);const int type=navigation->currentData().toInt();
            auto* enabled=f.widget<QCheckBox>(qPrintable(QStringLiteral("AdjustmentEnabled%1").arg(type)));
            CHECK(enabledStack->currentWidget()==enabled&&enabled->isVisible());
            const auto nav=rectangle(navigation),toggle=rectangle(enabled);
            CHECK(nav.right()<toggle.left()&&std::abs(nav.center().y()-toggle.center().y())<=1);
            auto* scope=f.widget<QComboBox>(qPrintable(QStringLiteral("AdjustmentScope%1").arg(type)));
            const auto s=rectangle(scope);
            const auto capture=rectangle(f.widget<QPushButton>(qPrintable(QStringLiteral("AdjustmentCapture%1").arg(type))));
            const auto reset=rectangle(f.widget<QPushButton>(qPrintable(QStringLiteral("AdjustmentReset%1").arg(type))));
            CHECK(s.right()<capture.left()&&capture.right()<reset.left());
            CHECK(std::abs(s.center().y()-capture.center().y())<=1&&std::abs(capture.center().y()-reset.center().y())<=1);
            CHECK(s.top()>nav.bottom()&&s.left()==nav.left());
            enabled->click();settle();
            const auto state=f.state();CHECK(state.items[std::size_t(type)].enabled);
            for(std::size_t i=0;i<c::adjustmentCount;++i)if(i!=std::size_t(type))CHECK(!state.items[i].enabled);
            enabled->click();settle();
        }
    }
}
void groupedNumericCancelAndNoop()
{
    Fixture f;auto* exposure=f.number("AdjustmentExposure");if(!exposure)return;
    const auto surface=std::get<c::RasterLayer>(f.document().layer(f.id)->payload).surface;
    const auto revision=surface->revision();const auto depth=f.session().history().undoDepth();
    QTest::mousePress(exposure,Qt::LeftButton,{},exposure->valueFieldRect().center());
    exposure->setValue(1);exposure->setValue(2);exposure->setValue(3);
    CHECK(f.session().history().undoDepth()==depth);
    f.panel->finishEditing(); // Commit current numeric state before an unrelated release position.
    QTest::mouseRelease(exposure,Qt::LeftButton,{},exposure->valueFieldRect().center());settle();
    CHECK(f.session().history().undoDepth()==depth+1);
    CHECK(std::get<c::ExposureParameters>(f.state().items[0].parameters).stops==3);
    CHECK(surface->revision()==revision);
    f.history(false);CHECK(!f.state().items[0].enabled);CHECK(!f.document().isModified());
    const auto redo=f.session().history().redoDepth();
    QTest::mousePress(exposure,Qt::LeftButton,{},exposure->valueFieldRect().center());
    exposure->setValue(2);exposure->setValue(0);
    f.panel->finishEditing();
    QTest::mouseRelease(exposure,Qt::LeftButton,{},exposure->valueFieldRect().center());settle();
    CHECK(f.session().history().undoDepth()==depth);CHECK(f.session().history().redoDepth()==redo);CHECK(!f.state().items[0].enabled);
    QTest::mousePress(exposure,Qt::LeftButton,{},exposure->valueFieldRect().center());exposure->setValue(-3);
    QTest::keyClick(exposure,Qt::Key_Escape);QTest::mouseRelease(exposure,Qt::LeftButton);settle();
    CHECK(f.session().history().undoDepth()==depth);CHECK(f.session().history().redoDepth()==redo);CHECK(!f.state().items[0].enabled);
    f.history(true);CHECK(std::get<c::ExposureParameters>(f.state().items[0].parameters).stops==3);
}
void resetMasksAndComparison()
{
    Fixture f;f.number("AdjustmentExposure")->setValue(1);settle();
    const auto selection=c::SelectionMask::rectangle(f.document().canvas().extent,{5,4,25,20},128);
    CHECK(f.document().setSelection(selection));f.select(f.id);
    auto* capture=f.widget<QPushButton>("AdjustmentCapture0");CHECK(capture->isEnabled());capture->click();settle();
    auto state=f.state();CHECK(state.items[0].mask);CHECK(state.items[0].mask->coverage==selection);
    CHECK(f.document().setSelection({}));f.select(f.id);
    CHECK(f.state().items[0].mask->coverage==selection);
    auto* compare=f.widget<QPushButton>("AdjustmentCompare");
    const auto token=f.document().contentState();const auto depth=f.session().history().undoDepth();
    QTest::mousePress(compare,Qt::LeftButton);CHECK(f.canvas->scene().adjustmentBypassLayer==f.id);
    CHECK(f.document().contentState()==token);CHECK(f.session().history().undoDepth()==depth);
    QTest::mouseRelease(compare,Qt::LeftButton);CHECK(!f.canvas->scene().adjustmentBypassLayer);
    QTest::mousePress(compare,Qt::LeftButton);CHECK(f.canvas->scene().adjustmentBypassLayer==f.id);
    QEvent deactivate(QEvent::ApplicationDeactivate);QCoreApplication::sendEvent(qApp,&deactivate);
    CHECK(!f.canvas->scene().adjustmentBypassLayer);QTest::mouseRelease(compare,Qt::LeftButton);
    CHECK(f.state().items[0].enabled);
    f.widget<QPushButton>("AdjustmentResetAll")->click();settle();
    CHECK(c::equivalentAdjustments(f.document().layer(f.id)->adjustments,{}));
    f.history(false);CHECK(f.state().items[0].mask);CHECK(f.state().items[0].enabled);
}
void curvesAndHistogram()
{
    Fixture f;f.page(0,3);
    auto* curve=dynamic_cast<u::AdjustmentCurveEditor*>(f.widget<QWidget>("AdjustmentCurveEditor"));CHECK(curve);if(!curve)return;
    const auto depth=f.session().history().undoDepth();
    QTest::mousePress(curve,Qt::LeftButton,{},QPoint(curve->width()/2,curve->height()/3));
    QTest::mouseMove(curve,QPoint(curve->width()/2+20,curve->height()/3+10));
    CHECK(f.session().history().undoDepth()==depth);
    QTest::mouseRelease(curve,Qt::LeftButton);settle();
    CHECK(f.session().history().undoDepth()==depth+1);
    CHECK(std::get<c::CurvesParameters>(f.state().items[3].parameters).channels[0].points.size()==3);
    CHECK(f.widget<QPushButton>("AdjustmentCurveRemove")->isEnabled());
    f.number("AdjustmentCurveOutput")->setValue(220);settle();
    CHECK(f.session().history().undoDepth()==depth+2);
    const auto channel=std::get<c::CurvesParameters>(f.state().items[3].parameters).channels[0];
    CHECK(std::abs(channel.points[1].output-220./255)<1e-9);
    QElapsedTimer timer;timer.start();while(f.panel->completedHistogramJobs()==0&&timer.elapsed()<5000){settle();QTest::qWait(2);}
    CHECK(f.panel->completedHistogramJobs()>0);
    CHECK(curve->toolTip().isEmpty());
    for(auto* label:f.panel->findChildren<QLabel*>())
        if(label->text().contains("input ·"))CHECK(label->toolTip().isEmpty());
    if(const auto review=qEnvironmentVariable("IMAGEEDITOR_ADJUSTMENT_PANEL_REVIEW");!review.isEmpty()){
        auto* workspace=dynamic_cast<u::OverlayDockWorkspace*>(f.window.findChild<QWidget*>("CanvasWorkspace"));
        auto* shell=dynamic_cast<u::WorkspacePanel*>(f.window.findChild<QWidget*>("AdjustmentsPanelShell"));
        CHECK(workspace&&shell);
        if(workspace&&shell){workspace->floatPanel(shell,QRect(300,80,430,810));settle();}
        CHECK(f.panel->grab().save(review));
    }
    const auto jobs=f.panel->completedHistogramJobs();
    f.number("AdjustmentCurveOutput")->setValue(180);settle();QTest::qWait(30);settle();
    CHECK(f.panel->completedHistogramJobs()==jobs); // Own output never feeds its input histogram.
    f.page(0,0);f.number("AdjustmentExposure")->setValue(.5);f.page(0,3);
    timer.restart();while(f.panel->completedHistogramJobs()==jobs&&timer.elapsed()<5000){settle();QTest::qWait(2);}
    CHECK(f.panel->completedHistogramJobs()>jobs);
    curve->setFocus();QTest::keyClick(curve,Qt::Key_Backspace);settle();
    CHECK(std::get<c::CurvesParameters>(f.state().items[3].parameters).channels[0].points.size()==2);
    CHECK(f.document().layer(f.id));
    const auto before=c::equivalentAdjustments(f.document().layer(f.id)->adjustments,{});
    const auto history=f.session().history().undoDepth();
    QTest::mousePress(curve,Qt::LeftButton,{},QPoint(curve->width()/3,curve->height()/3));
    QTest::keyClick(curve,Qt::Key_Escape);QTest::mouseRelease(curve,Qt::LeftButton);settle();
    CHECK(f.session().history().undoDepth()==history);
    CHECK(std::get<c::CurvesParameters>(f.state().items[3].parameters).channels[0].points.size()==2);
    CHECK(c::equivalentAdjustments(f.document().layer(f.id)->adjustments,{})==before);
}
void containerAndStaleTarget()
{
    Fixture f;auto* exposure=f.number("AdjustmentExposure");
    QTest::mousePress(exposure,Qt::LeftButton,{},exposure->valueFieldRect().center());exposure->setValue(2);
    const auto depth=f.session().history().undoDepth();
    auto second=c::Layer::raster("New target",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{10,10}));
    const auto other=second.id;
    // Normal structural commands settle previews through MainWindow. A direct
    // external model mutation also must not retarget a stale release callback.
    CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(second),1)));
    f.select(other);QTest::mouseRelease(exposure,Qt::LeftButton);settle();
    CHECK(!f.document().layer(other)->adjustments);CHECK(f.session().history().undoDepth()==depth+1);
    CHECK(!f.document().layer(f.id)->adjustments); // Foreign history cannot strand an unrecorded preview.
    auto tree=f.document().tree();const auto folder=c::makeLayerId();
    c::LayerContainer container{folder,"Folder",c::ContainerKind::Folder,c::ColorLabel::None,{},true};container.children=tree.roots;tree.roots={folder};tree.containers.push_back(container);
    CHECK(f.document().replaceStructure(f.document().tree(),std::move(tree)));f.select(folder);
    CHECK(!f.panel->target());CHECK(!f.widget<QTabWidget>("AdjustmentTabs")->isEnabled());
}
void largeHistogramBudget()
{
    for(const c::Extent2u extent: {c::Extent2u{3840,2160},c::Extent2u{5120,2880}}){
        u::AdjustmentsPanel panel;panel.resize(350,650);panel.show();
        auto layer=c::Layer::raster("Histogram benchmark",std::make_shared<c::ContiguousRasterSurface>(extent,c::Rgba8{180,90,45,128}));
        panel.setTarget(&layer,false);
        auto* navigation=panel.findChild<QComboBox*>("AdjustmentNavigation0");navigation->setCurrentIndex(2);
        panel.onHistogramRequested=[&]{panel.requestHistogram(&layer);};
        QElapsedTimer elapsed;elapsed.start();panel.requestHistogram(&layer);
        qint64 worstSlice=0;
        while(panel.completedHistogramJobs()==0&&elapsed.elapsed()<10000){
            QElapsedTimer slice;slice.start();QCoreApplication::processEvents();worstSlice=std::max(worstSlice,slice.nsecsElapsed());QTest::qWait(1);
        }
        CHECK(panel.completedHistogramJobs()==1);
        std::cout<<"Histogram "<<extent.width<<'x'<<extent.height<<": "<<elapsed.elapsed()<<" ms, GUI event slice max "<<double(worstSlice)/1e6<<" ms\n";
        const auto jobs=panel.completedHistogramJobs();panel.requestHistogram(&layer);settle();CHECK(panel.completedHistogramJobs()==jobs);
        panel.close();settle();
    }
}
int nativeValidation()
{
    if(QGuiApplication::platformName()!=QStringLiteral("wayland"))return 77;
    std::atomic_uint64_t warnings{0},errors{0};
    QVulkanInstance instance;instance.setApiVersion(QVersionNumber(1,2));
    if(!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation")))return 1;
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,QVulkanInstance::DebugMessageTypeFlags type,const void* message){
        if(!type.testFlag(QVulkanInstance::ValidationMessage))return false;
        if(severity.testFlag(QVulkanInstance::ErrorSeverity))++errors;
        else if(severity.testFlag(QVulkanInstance::WarningSeverity))++warnings;
        else return false;
        const auto* data=static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr<<"Adjustment Vulkan: "<<(data?data->pMessage:"unknown")<<'\n';return true;
    });
    if(!instance.create())return 1;
    const auto waitFor=[](const auto& predicate){QElapsedTimer timer;timer.start();do{settle();if(predicate())return true;QTest::qWait(5);}while(timer.elapsed()<10000);return predicate();};
    {
        Fixture f(&instance);if(!f.panel||!f.canvas)return 1;
        CHECK(waitFor([&]{return f.canvas->rendererStats().fullUploads>0&&!f.canvas->presentationSuppressedForResize();}));QTest::qWait(100);
        const auto baseline=f.canvas->rendererStats();const auto dimensions=f.canvas->size();const auto zoom=f.canvas->zoom();
        const auto surface=std::get<c::RasterLayer>(f.document().layer(f.id)->payload).surface;const auto revision=surface->revision();
        CHECK(f.document().setSelection(c::SelectionMask::rectangle(f.document().canvas().extent,{4,3,45,40},128)));
        f.select(f.id);f.widget<QPushButton>("AdjustmentCapture0")->click();settle();
        CHECK(f.state().items[0].mask);
        CHECK(f.document().setSelection({}));f.select(f.id);
        for(const auto [name,value]:std::array<std::pair<const char*,double>,8>{{
            {"AdjustmentExposure",.5},{"AdjustmentContrast",12},{"AdjustmentLevelsGamma",1.2},
            {"AdjustmentHue",18},{"AdjustmentVibrance",20},{"AdjustmentBalance0",5},
            {"AdjustmentWarmth",10},{"AdjustmentMonoTintStrength",8}}}){
            const auto frame=f.canvas->rendererStats().framesSubmitted;f.number(name)->setValue(value);
            CHECK(waitFor([&]{return f.canvas->rendererStats().framesSubmitted>frame;}));
        }
        CHECK(f.canvas->rendererStats().adjustmentMaskUploads>baseline.adjustmentMaskUploads);
        // Replace a resident mask and cycle actual in-flight slots; validation
        // checks the copy/read barriers and per-slot descriptor ownership.
        CHECK(f.document().setSelection(c::SelectionMask::rectangle(f.document().canvas().extent,{17,8,30,22},211)));
        f.select(f.id);f.widget<QPushButton>("AdjustmentCapture0")->click();settle();
        CHECK(f.document().setSelection({}));f.select(f.id);
        for(int i=0;i<4;++i){
            const auto frame=f.canvas->rendererStats().framesSubmitted;
            f.number("AdjustmentExposure")->setValue(.6+.1*i);
            CHECK(waitFor([&]{return f.canvas->rendererStats().framesSubmitted>frame;}));
        }
        auto* compare=f.widget<QPushButton>("AdjustmentCompare");
        const auto token=f.document().contentState();const auto depth=f.session().history().undoDepth();
        auto frame=f.canvas->rendererStats().framesSubmitted;
        QTest::mousePress(compare,Qt::LeftButton);CHECK(waitFor([&]{return f.canvas->rendererStats().framesSubmitted>frame;}));
        CHECK(f.canvas->scene().adjustmentBypassLayer==f.id);
        frame=f.canvas->rendererStats().framesSubmitted;QTest::mouseRelease(compare,Qt::LeftButton);
        CHECK(waitFor([&]{return f.canvas->rendererStats().framesSubmitted>frame;}));CHECK(!f.canvas->scene().adjustmentBypassLayer);
        CHECK(f.document().contentState()==token&&f.session().history().undoDepth()==depth);
        CHECK(f.canvas->scene().capturedRegionEdges);
        auto* region=f.widget<QCheckBox>("AdjustmentShowRegion0");
        const auto beforeRegion=f.canvas->rendererStats();
        for(int i=0;i<6;++i){
            frame=f.canvas->rendererStats().framesSubmitted;region->click();
            CHECK(waitFor([&]{return f.canvas->rendererStats().framesSubmitted>frame;}));
            CHECK(bool(f.canvas->scene().capturedRegionEdges)==region->isChecked());
        }
        CHECK(f.canvas->rendererStats().compositionPasses==beforeRegion.compositionPasses);
        CHECK(f.canvas->rendererStats().adjustmentMaskUploads==beforeRegion.adjustmentMaskUploads);
        CHECK(f.canvas->rendererStats().adjustmentParameterUploads==beforeRegion.adjustmentParameterUploads);
        CHECK(f.document().contentState()==token&&f.session().history().undoDepth()==depth);
        const auto cached=f.canvas->rendererStats();QTest::qWait(180);settle();
        CHECK(f.canvas->rendererStats().framesSubmitted==cached.framesSubmitted);
        CHECK(cached.fullUploads==baseline.fullUploads&&cached.regionalUploads==baseline.regionalUploads&&cached.uploadedBytes==baseline.uploadedBytes);
        CHECK(cached.swapchainGeneration==baseline.swapchainGeneration&&cached.resourceGeneration==baseline.resourceGeneration);
        CHECK(surface->revision()==revision&&f.canvas->size()==dimensions&&f.canvas->zoom()==zoom);
    }
    instance.destroy();settle();CHECK(warnings==0&&errors==0);
    std::cout<<"Native adjustments Vulkan: "<<warnings<<" warnings, "<<errors<<" errors\n";
    return failures?1:0;
}
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);QCoreApplication::setOrganizationName("ImageEditorTests");QCoreApplication::setApplicationName("AdjustmentUiTests");
    QStandardPaths::setTestModeEnabled(true);QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());u::applyEditorTheme(app);
    if(app.arguments().contains(QStringLiteral("--wayland-validation")))return nativeValidation();
    imageeditor::tests::adjustmentMaskOutlineChecks([](bool pass,std::string_view message){check(pass,message.data(),__LINE__);});
    curveStrokePreservesHistogramAndGrid();capturedRegionViewState();
    freshProfileUsesApprovedLayout();activePageControlsScrolling();navigationAndPrimary();compactCategoryLayout();groupedNumericCancelAndNoop();resetMasksAndComparison();curvesAndHistogram();containerAndStaleTarget();largeHistogramBudget();
    std::cout<<(failures?"Adjustment UI tests FAILED\n":"Adjustment UI tests passed\n");return failures?1:0;
}
