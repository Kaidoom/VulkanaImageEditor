#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLayout>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <atomic>
#include <iostream>

namespace c=imageeditor::core;namespace u=imageeditor::ui;namespace r=imageeditor::render;
namespace {
int failures=0;
QVulkanInstance* nativeInstance=nullptr;
void check(bool ok,const char* message){if(!ok){++failures;std::cerr<<message<<'\n';}}
void settle(){QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();}
struct Fixture {
    QTemporaryDir files;
    u::MainWindow window{nativeInstance,false,false};
    u::AdjustmentsPanel* adjustments{};u::FiltersPanel* panel{};r::CanvasWindow* canvas{};
    c::LayerId id{};
    Fixture(){
        window.setUnsavedPromptEnabled(false);window.resize(1550,1050);window.show();settle();
        adjustments=dynamic_cast<u::AdjustmentsPanel*>(window.findChild<QWidget*>("AdjustmentsPanelContent"));
        panel=adjustments?adjustments->filtersPanel():nullptr;
        for(auto* candidate:QGuiApplication::allWindows())if(candidate->objectName()=="VulkanCanvasWindow")canvas=dynamic_cast<r::CanvasWindow*>(candidate);
        QImage image(40,32,QImage::Format_RGBA8888);image.fill(QColor(140,70,30,190));
        check(image.save(files.filePath("filter-ui.png")),"save fixture");
        check(window.openImageFromPath(files.filePath("filter-ui.png")),"open fixture");
        id=session().activeLayer().value_or(0);
        check(panel&&canvas&&id,"filter UI exists");
        if(auto* action=widget<QAction>("FilterAction_0"))action->trigger();
        settle();
    }
    ~Fixture(){
        if(nativeInstance&&canvas){QTest::qWait(80);check(canvas->rendererStats().framesSubmitted>0,"native filter canvas submitted Vulkan frames");}
        window.close();settle();
    }
    c::EditorSession& session(){return const_cast<c::EditorSession&>(window.editorSession());}
    c::Document& document(){return *session().document();}
    c::Layer& layer(){return *document().layer(id);}
    template<class T>T* widget(const char* name){auto* w=window.findChild<T*>(name);check(w!=nullptr,name);return w;}
    u::CompactValueControl* number(const char* name){return dynamic_cast<u::CompactValueControl*>(widget<QDoubleSpinBox>(name));}
    void waitCache(){
        QElapsedTimer timer;timer.start();
        while(!c::layerSpatialFilterCacheValid(layer())&&timer.elapsed()<10000)QTest::qWait(10);
        check(c::layerSpatialFilterCacheValid(layer()),"asynchronous filter publishes coherent cache");
    }
};
void embeddedEntryPoints()
{
    Fixture f;if(!f.panel||!f.adjustments)return;
    auto* tabs=f.widget<QTabWidget>("AdjustmentTabs");
    auto* visibility=f.widget<QAction>("AdjustmentsPanelVisibilityAction");
    auto* shell=f.widget<QWidget>("AdjustmentsPanelShell");
    if(!tabs||!visibility||!shell)return;
    check(tabs->count()==5&&tabs->tabText(3)=="Filters"&&tabs->tabText(4)=="Effects","Filters and Effects are fourth/fifth Adjustments categories");
    check(tabs->widget(3)==f.panel,"Filters reuses the cached child inside Adjustments");
    check(!f.window.findChild<QWidget*>("FiltersPanelShell")&&!f.window.findChild<QAction*>("FiltersPanelVisibilityAction"),
        "no separate Filters workspace or visibility action");
    check(!f.widget<QWidget>("FilterTarget")->isVisible()&&!f.widget<QPushButton>("FilterCompare")->isVisible(),
        "embedded filters hide their duplicate target and comparison header");
    check(f.widget<QWidget>("AdjustmentTarget")->isVisible()&&f.widget<QPushButton>("AdjustmentCompare")->isVisible(),
        "one shared target and comparison header remains visible");
    auto* radius=f.number("FilterGaussianRadiusX");
    const auto content=f.document().contentState(),depth=f.session().history().undoDepth();
    if(visibility->isChecked())visibility->trigger();
    settle();
    check(!shell->isVisible(),"parent workspace can be hidden");
    for(int i=0;i<3;++i){
        tabs->setCurrentIndex(i);
        const auto name=QStringLiteral("FilterAction_%1").arg(i);
        if(auto* action=f.widget<QAction>(qPrintable(name)))action->trigger();
        settle();
        check(shell->isVisible()&&visibility->isChecked(),"Filters menu reveals the existing Adjustments workspace");
        check(f.adjustments->filtersCategoryActive()&&int(f.panel->currentType())==i,
            "Filters menu selects its category and requested filter");
        check(f.adjustments->filtersPanel()==f.panel&&f.number("FilterGaussianRadiusX")==radius,
            "category navigation never rebuilds the filter controls");
    }
    check(f.document().contentState()==content&&f.session().history().undoDepth()==depth,
        "opening and navigating Filters never changes content or history");
    if(const auto review=qEnvironmentVariable("IMAGEEDITOR_FILTER_PANEL_REVIEW");!review.isEmpty()){
        f.adjustments->showFilter(c::SpatialFilterType::Gaussian);settle();
        check(f.adjustments->grab().save(review),"save consolidated Adjustments/Filters review image");
    }
}
void layerEffectsUi()
{
    Fixture f;auto* panel=f.adjustments->effectsPanel();auto* tabs=f.widget<QTabWidget>("AdjustmentTabs");tabs->setCurrentIndex(4);settle();
    auto* enabled=f.widget<QCheckBox>("EffectEnabled0");auto* size=f.number("Effect0Size");auto* nav=f.widget<QComboBox>("EffectNavigation");
    auto* reset=f.widget<QPushButton>("AdjustmentResetAll");auto* compare=f.widget<QPushButton>("AdjustmentCompare");
    const auto start=f.session().history().undoDepth();size->setValue(9);
    check(f.layer().effects&&!f.layer().effects->items[0].enabled,"parameter edits do not auto-enable styles");
    enabled->setChecked(true);QElapsedTimer timer;timer.start();while(!c::layerEffectCacheValid(f.layer())&&timer.elapsed()<10000)QTest::qWait(10);
    check(c::layerEffectCacheValid(f.layer()),"asynchronous style mask preparation");
    const auto cache=f.layer().effectCache;const auto source=c::intrinsicSurface(f.layer());
    nav->setCurrentIndex(5);f.widget<QCheckBox>("EffectEnabled5")->setChecked(true);settle();
    check(f.layer().effects->items[0].enabled&&f.layer().effects->items[5].enabled,"independent simultaneous effects");
    auto* opacity=f.number("Effect5Opacity");opacity->setValue(55);settle();
    check(f.layer().effectCache==cache&&c::intrinsicSurface(f.layer())==source,"material edits reuse silhouette and source");
    const auto state=f.layer().effects;const auto depth=f.session().history().undoDepth();
    for(int i=0;i<7;++i)nav->setCurrentIndex(i);
    check(f.layer().effects==state&&f.session().history().undoDepth()==depth,"navigation is view-only and controls persist");
    check(f.number("Effect0Size")==size,"persistent effect page");
    compare->pressed();check(f.canvas->scene().effectBypassLayer==f.id,"effects-only comparison enabled");compare->released();check(!f.canvas->scene().effectBypassLayer,"comparison release clears bypass");
    nav->setCurrentIndex(0);QTest::mousePress(size,Qt::LeftButton,{},size->valueFieldRect().center());size->setValue(12);QTest::keyClick(size,Qt::Key_Escape);QTest::mouseRelease(size,Qt::LeftButton);settle();
    check(f.layer().effects->items[0].size==9,"Escape-equivalent cancellation restores starting parameter");
    const auto adjustments=f.layer().adjustments;const auto filters=f.layer().filters;
    check(reset->text()=="Reset Effects","scoped reset label");reset->click();
    check(!c::hasActiveLayerEffects(f.layer().effects)&&f.layer().adjustments==adjustments&&f.layer().filters==filters,"Reset Effects does not reset adjustments/filters");
    check(f.session().history().undoDepth()>start,"effect interactions recorded");
    check(f.session().history().undo(f.document()),"undo reset");f.adjustments->setTarget(&f.layer(),false);check(c::hasActiveLayerEffects(f.layer().effects),"undo restores styles");
    if(const auto path=qEnvironmentVariable("IMAGEEDITOR_EFFECT_PANEL_REVIEW");!path.isEmpty())check(f.adjustments->grab().save(path),"save effects UI review");
}
void sharedHeaderScope()
{
    Fixture f;if(!f.panel||!f.canvas)return;
    auto* tabs=f.widget<QTabWidget>("AdjustmentTabs");
    auto* before=f.widget<QPushButton>("AdjustmentCompare");
    auto* reset=f.widget<QPushButton>("AdjustmentResetAll");
    tabs->setCurrentIndex(0);f.number("AdjustmentExposure")->setValue(1);settle();
    const auto adjustments=f.layer().adjustments;
    f.adjustments->showFilter(c::SpatialFilterType::Gaussian);
    f.number("FilterGaussianRadiusX")->setValue(3);settle();f.waitCache();
    const auto filters=f.layer().filters;
    const auto content=f.document().contentState(),depth=f.session().history().undoDepth();
    QTest::mousePress(before,Qt::LeftButton);
    check(f.canvas->scene().filterBypassLayer==f.id&&!f.canvas->scene().adjustmentBypassLayer,
        "shared Before in Filters bypasses only spatial filters");
    tabs->setCurrentIndex(0);settle();
    check(!f.canvas->scene().filterBypassLayer&&!f.canvas->scene().adjustmentBypassLayer,
        "leaving a category safely releases its held Before state");
    QTest::mouseRelease(before,Qt::LeftButton);
    QTest::mousePress(before,Qt::LeftButton);
    check(f.canvas->scene().adjustmentBypassLayer==f.id&&!f.canvas->scene().filterBypassLayer,
        "shared Before in Tone bypasses only color adjustments");
    QTest::mouseRelease(before,Qt::LeftButton);
    check(f.document().contentState()==content&&f.session().history().undoDepth()==depth,
        "shared comparison and category changes remain presentation state");
    reset->click();settle();
    check(c::equivalentAdjustments(f.layer().adjustments,{})&&c::equivalentSpatialFilters(f.layer().filters,filters),
        "Tone Reset All preserves the spatial-filter stack");
    f.number("AdjustmentExposure")->setValue(1);settle();
    f.adjustments->showFilter(c::SpatialFilterType::Gaussian);reset->click();settle();
    check(c::equivalentSpatialFilters(f.layer().filters,{})&&c::equivalentAdjustments(f.layer().adjustments,adjustments),
        "Filters Reset All preserves the color-adjustment stack");
}
void compactLayoutAndEnabledRouting()
{
    Fixture f;if(!f.panel||!f.adjustments)return;
    auto* tabs=f.widget<QTabWidget>("AdjustmentTabs");
    for(int i=0;i<tabs->count();++i)
        check(tabs->widget(i)->layout()->contentsMargins()==QMargins(8,8,8,8),"all categories use the same eight-pixel padding");
    auto rectangle=[&](QWidget* w){return QRect(w->mapTo(f.adjustments,QPoint{}),w->size());};
    auto* navigation=f.widget<QComboBox>("FilterNavigation");
    const auto source=c::intrinsicSurface(f.layer());const auto revision=source->revision();
    for(int i=0;i<3;++i){
        f.adjustments->showFilter(c::SpatialFilterType(i));settle();
        auto* enabled=f.widget<QCheckBox>(qPrintable(QStringLiteral("FilterEnabled%1").arg(i)));
        const auto nav=rectangle(navigation),toggle=rectangle(enabled);
        check(enabled->isVisible()&&nav.right()<toggle.left()&&std::abs(nav.center().y()-toggle.center().y())<=1,
            "active Enabled toggle sits to the right of the filter dropdown");
        for(int j=0;j<3;++j)check(f.widget<QCheckBox>(qPrintable(QStringLiteral("FilterEnabled%1").arg(j)))->isVisible()==(i==j),
            "only the current cached filter Enabled checkbox is shown");
        auto* scope=f.widget<QComboBox>(qPrintable(QStringLiteral("FilterScope%1").arg(i)));
        auto* capture=f.widget<QPushButton>(qPrintable(QStringLiteral("FilterCapture%1").arg(i)));
        auto* reset=f.widget<QPushButton>(qPrintable(QStringLiteral("FilterReset%1").arg(i)));
        const auto scopeRect=rectangle(scope),captureRect=rectangle(capture),resetRect=rectangle(reset);
        check(scopeRect.right()<captureRect.left()&&captureRect.right()<resetRect.left()
            &&std::abs(scopeRect.center().y()-captureRect.center().y())<=1
            &&std::abs(captureRect.center().y()-resetRect.center().y())<=1
            &&scopeRect.top()>nav.bottom()&&scopeRect.left()==nav.left(),
            "scope row orders Whole Layer, Capture and Reset directly below the header");
        check(!f.window.findChild<QPushButton*>(QStringLiteral("FilterRemove%1").arg(i)),"redundant Remove action is absent");
        const auto alpha=rectangle(f.widget<QCheckBox>(qPrintable(QStringLiteral("FilterPreserveAlpha%1").arg(i))));
        check(alpha.top()>scopeRect.bottom(),"Preserve Alpha follows the scope row");
        if(i==0){
            const auto linked=rectangle(f.widget<QCheckBox>("FilterGaussianLinked"));
            check(std::abs(alpha.center().y()-linked.center().y())<=1&&(alpha.right()<linked.left()||linked.right()<alpha.left()),
                "Gaussian Preserve Alpha and linked dimensions share one options row");
        }
        enabled->click();settle();
        check(f.layer().filters&&f.layer().filters->items[std::size_t(i)].enabled,"header Enabled modifies the selected filter");
        for(int j=0;j<3;++j)if(i!=j)check(!f.layer().filters->items[std::size_t(j)].enabled,"header Enabled does not modify another filter");
        enabled->click();settle();
    }
    check(source->revision()==revision,"layout and Enabled navigation preserve original pixels");
}
void completeResetAndRedo()
{
    Fixture f;if(!f.panel)return;
    const auto mask=c::SelectionMask::rectangle(f.document().canvas().extent,{5,4,11,7},128);
    f.document().setSelection(mask);f.adjustments->setTarget(&f.layer(),true);
    const std::array names{"FilterGaussianRadiusX","FilterMotionDistance","FilterLensRadius"};
    auto refresh=[&]{f.canvas->setDocument(f.document().snapshot(),false);f.adjustments->setTarget(&f.layer(),true);settle();};
    for(std::size_t i=0;i<names.size();++i){
        f.adjustments->showFilter(c::SpatialFilterType(i));settle();
        auto* value=f.number(names[i]);value->setValue(4);
        f.widget<QCheckBox>(qPrintable(QStringLiteral("FilterPreserveAlpha%1").arg(i)))->setChecked(true);
        f.widget<QPushButton>(qPrintable(QStringLiteral("FilterCapture%1").arg(i)))->click();settle();
        const auto before=f.layer().filters;
        check(before&&before->items[i].enabled&&before->items[i].mask&&before->items[i].preserveAlpha,"reset fixture has parameters, alpha option and captured mask");
        auto expected=std::make_shared<c::SpatialFilterStack>(*before);expected->items[i]=c::defaultSpatialFilter(c::SpatialFilterType(i));
        const auto depth=f.session().history().undoDepth();
        auto* reset=f.widget<QPushButton>(qPrintable(QStringLiteral("FilterReset%1").arg(i)));
        reset->click();settle();
        check(c::equivalentSpatialFilters(f.layer().filters,expected)&&f.session().history().undoDepth()==depth+1,
            "Reset restores the entire chosen default filter in one action without changing other filters");
        check(!f.widget<QCheckBox>(qPrintable(QStringLiteral("FilterEnabled%1").arg(i)))->isChecked()
            &&!f.widget<QCheckBox>(qPrintable(QStringLiteral("FilterPreserveAlpha%1").arg(i)))->isChecked()
            &&value->value()==0,"Reset immediately synchronizes header and parameter controls");
        check(f.session().history().undo(f.document()),"undo complete filter reset");refresh();
        check(c::equivalentSpatialFilters(f.layer().filters,before)&&f.layer().filters->items[i].mask->coverage==mask,
            "undo Reset restores parameters, enabled state, alpha option and exact capture");
        check(f.session().history().redo(f.document()),"redo complete filter reset");refresh();
        check(c::equivalentSpatialFilters(f.layer().filters,expected),"redo Reset restores the same default state");
        value->setValue(2);settle();check(f.session().history().undo(f.document()),"create redo branch above reset state");refresh();
        const auto redo=f.session().history().redoDepth(),undo=f.session().history().undoDepth(),content=f.document().contentState();
        reset->click();settle();
        check(f.session().history().redoDepth()==redo&&f.session().history().undoDepth()==undo&&f.document().contentState()==content,
            "resetting an already-default filter preserves existing redo and document state");
    }
}
void controlsAndAsync()
{
    Fixture f;if(!f.panel||!f.canvas)return;
    auto* horizontal=f.number("FilterGaussianRadiusX");auto* vertical=f.number("FilterGaussianRadiusY");
    auto* navigation=f.widget<QComboBox>("FilterNavigation");if(!horizontal||!vertical||!navigation)return;
    const auto state=f.document().contentState(),depth=f.session().history().undoDepth();
    for(int i=0;i<3;++i)navigation->setCurrentIndex(i);
    navigation->setCurrentIndex(0);
    check(f.document().contentState()==state&&f.session().history().undoDepth()==depth,"visiting pages is not history");
    check(f.number("FilterGaussianRadiusX")==horizontal,"pages and controls cached");
    horizontal->setValue(3);settle();
    check(f.layer().filters&&f.layer().filters->items[0].enabled,"numeric filter edit auto-enables");
    check(vertical->value()==3,"Gaussian linked axes update");
    check(f.session().history().undoDepth()==depth+1,"numeric edit is one action");
    f.waitCache();const auto cached=f.layer().filterCache;
    const auto original=c::intrinsicSurface(f.layer());const auto revision=original->revision();
    const auto sourceBytes=f.document().contentState();
    f.canvas->scheduleFrame();settle();QTest::qWait(100);
    check(f.layer().filterCache==cached&&original->revision()==revision,"idle frame reuses filter/source caches");
    auto* before=f.widget<QPushButton>("AdjustmentCompare");
    QTest::mousePress(before,Qt::LeftButton);check(f.canvas->scene().filterBypassLayer==f.id,"Before bypasses only presentation");
    QTest::mouseRelease(before,Qt::LeftButton);check(!f.canvas->scene().filterBypassLayer,"Before releases");
    check(f.document().contentState()==sourceBytes,"Before is not persistent content");
    f.widget<QCheckBox>("FilterGaussianLinked")->setChecked(false);
    horizontal->setValue(5);settle();
    check(vertical->value()==3,"unlinked axis retains other parameter");
    horizontal->setValue(7);horizontal->setValue(2);settle();f.waitCache();
    check(c::equivalentSpatialFilters(f.layer().filterCache->filters,f.layer().filters),"coalesced work never publishes obsolete parameters");
    f.widget<QPushButton>("FilterReset0")->click();settle();
    check(!c::hasActiveSpatialFilters(f.layer().filters),"Reset restores identity");
    check(f.session().history().undo(f.document()),"undo Reset");f.canvas->setDocument(f.document().snapshot(),false);f.panel->setTarget(&f.layer(),false);f.waitCache();
    check(c::hasActiveSpatialFilters(f.layer().filters),"undo retains editable filter parameters");
}
void transformedTypedFilterAdmission()
{
    for (const bool text : {false, true}) {
        Fixture f;
        if (!f.panel || !f.canvas) return;
        f.canvas->resetTo100Percent();
        const c::AffineTransform transform {.85,-.35,12.25,.2,1.1,6.5};
        c::ShapeLayer shape;
        shape.kind=c::ShapeKind::Ellipse;shape.size={18,14};shape.fillColor={90,170,210,190};
        c::TextLayer label;label.utf8="Aa";label.defaultStyle.sizePixels=15;
        auto typed=text?c::Layer::text("Transformed text",label):c::Layer::shape("Transformed shape",shape);
        typed.localToDocument=transform;
        auto native=std::make_shared<c::LayerRenderCache>(*(text
            ?u::QtTextLayout(label).rasterizeDocument(transform,65536)
            :u::QtShapeRenderService{}.renderDocument(shape,transform,65536)));
        native->contentRevision=text?typed.textRevision:typed.shapeRevision;
        f.id=typed.id;
        check(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(typed),1)),"insert native typed filter target");
        // History deliberately drops derived typed caches. Publish the fixture
        // presentation after insertion, as MainWindow's cache preparation does.
        f.layer().renderCache=std::move(native);
        f.session().setActiveLayer(f.id);
        f.adjustments->setTarget(&f.layer(),false);f.canvas->setDocument(f.document().snapshot(),false);
        f.adjustments->showFilter(c::SpatialFilterType::Gaussian);settle();
        check(f.layer().renderCache&&f.layer().renderCache->rasterizedDocumentTransform==transform,
            "filter target begins with transformed document raster");
        auto* value=f.number("FilterGaussianRadiusX");
        if (!value) return;
        QTest::mousePress(value,Qt::LeftButton,{},value->valueFieldRect().center());
        value->setValue(2);
        check(c::hasActiveSpatialFilters(f.layer().filters),"native typed filter preview passes admission");
        const auto source=f.layer().renderCache;
        check(source&&!source->rasterizedDocumentTransform&&source->density==1,
            "filter preview publishes canonical local typed source");
        value->setValue(3);
        check(f.layer().renderCache==source,"filter scrubbing reuses prepared typed source");
        QTest::mouseRelease(value,Qt::LeftButton);settle();f.waitCache();
        check(f.layer().localToDocument==transform,"filter preparation preserves authoritative transform");
        check(f.layer().renderCache==source,"committing native typed filter does not rerasterize source");
    }
}
void groupedEditsAndMasks()
{
    Fixture f;if(!f.panel)return;
    auto* value=f.number("FilterGaussianRadiusX");if(!value)return;
    const auto depth=f.session().history().undoDepth();
    QTest::mousePress(value,Qt::LeftButton,{},value->valueFieldRect().center());
    value->setValue(1);value->setValue(4);value->setValue(6);
    check(f.session().history().undoDepth()==depth,"continuous scrub does not add intermediate history");
    f.widget<QTabWidget>("AdjustmentTabs")->setCurrentIndex(0);
    QTest::mouseRelease(value,Qt::LeftButton);settle();
    check(f.session().history().undoDepth()==depth+1&&!f.panel->interactionActive(),
        "leaving Filters commits a continuous scrub as one action");
    f.adjustments->showFilter(c::SpatialFilterType::Gaussian);settle();
    check(f.number("FilterGaussianRadiusX")==value&&value->value()==6,"returning to Filters retains its control and completed value");
    check(f.session().history().undo(f.document()),"undo scrub");f.canvas->setDocument(f.document().snapshot(),false);f.panel->setTarget(&f.layer(),false);
    const auto redo=f.session().history().redoDepth();
    QTest::mousePress(value,Qt::LeftButton,{},value->valueFieldRect().center());value->setValue(3);
    f.panel->finishEditing(false);QTest::mouseRelease(value,Qt::LeftButton);settle();
    check(f.session().history().redoDepth()==redo&&!c::hasActiveSpatialFilters(f.layer().filters),"cancel restores state and redo");
    const auto mask=c::SelectionMask::rectangle(f.document().canvas().extent,{4,5,12,9},128);
    f.document().setSelection(mask);f.panel->setTarget(&f.layer(),true);
    auto* capture=f.widget<QPushButton>("FilterCapture0");if(capture)capture->click();settle();
    check(f.layer().filters&&f.layer().filters->items[0].mask&&f.layer().filters->items[0].mask->coverage==mask,"filter captures immutable selection");
    f.document().setSelection({});f.panel->setTarget(&f.layer(),false);
    check(f.layer().filters->items[0].mask->coverage==mask,"deselect does not change captured mask");
    f.canvas->setDocument(f.document().snapshot(),false);settle();
    check(f.canvas->scene().capturedRegionEdges&&!f.canvas->scene().capturedRegionEdges->empty(),"captured filter region displayed");
    f.widget<QCheckBox>("FilterShowRegion0")->setChecked(false);settle();
    check(!f.canvas->scene().capturedRegionEdges,"captured region can be hidden");
    f.widget<QCheckBox>("FilterShowRegion0")->setChecked(true);settle();
    check(bool(f.canvas->scene().capturedRegionEdges),"captured region can be restored");
    const auto toneMask=c::SelectionMask::rectangle(f.document().canvas().extent,{20,18,5,4},128);
    f.widget<QTabWidget>("AdjustmentTabs")->setCurrentIndex(0);settle();
    check(!f.canvas->scene().capturedRegionEdges,"leaving Filters hides its captured region");
    f.document().setSelection(toneMask);f.adjustments->setTarget(&f.layer(),true);
    f.widget<QPushButton>("AdjustmentCapture0")->click();settle();
    f.document().setSelection({});f.adjustments->setTarget(&f.layer(),false);f.canvas->setDocument(f.document().snapshot(),false);settle();
    const auto state=f.document().contentState(),history=f.session().history().undoDepth();
    auto visibleMask=[&](const c::SelectionState& expected){
        const auto& edges=f.canvas->scene().capturedRegionEdges;
        return edges&&edges->size()==expected->nonzeroBoundaryEdges().size();
    };
    check(visibleMask(toneMask),"Tone owns the displayed adjustment capture");
    f.adjustments->showFilter(c::SpatialFilterType::Gaussian);settle();
    check(visibleMask(mask),"Filters restores its own distinct captured region");
    f.widget<QTabWidget>("AdjustmentTabs")->setCurrentIndex(0);settle();
    check(visibleMask(toneMask),"returning to Tone restores its capture instead of the hidden filter's");
    check(f.document().contentState()==state&&f.session().history().undoDepth()==history,
        "captured-region ownership changes do not edit document content");
}
void pendingInvalidationAndCancellation()
{
    Fixture f;if(!f.panel)return;
    auto* value=f.number("FilterGaussianRadiusX");
    auto* timer=f.widget<QTimer>("FilterPreparationTimer");
    if(!value||!timer)return;
    value->setValue(24);
    QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
    // Edit the authoritative input after the worker has frozen it. The source
    // key, not just document metadata, must reject the now-stale completion.
    auto source=std::get<c::RasterLayer>(f.layer().payload).surface;
    const std::array patch{std::byte{20},std::byte{220},std::byte{60},std::byte{255}};
    source->replaceRgba8({15,15,1,1},patch,4);
    f.canvas->setDocument(f.document().snapshot(),false);
    f.waitCache();
    check(f.layer().filterCache->sourceRevision==source->revision(),"source edit rejects stale worker result");
    const auto revision=source->revision();
    value->setValue(48);
    QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
    const auto content=f.document().contentState();
    f.widget<QPushButton>("FilterCancelProcessing")->click();settle();QTest::qWait(100);
    check(source->revision()==revision&&f.document().contentState()==content,"cancel processing does not change completed content history");
    check(!c::layerSpatialFilterCacheValid(f.layer()),"cancelled worker does not publish");
    value->setValue(4);
    QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
    auto other=c::Layer::raster("Different target",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{12,12},c::Rgba8{100,90,80,255}));
    const auto otherId=other.id;
    check(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(other),1)),"create second target");
    f.session().setActiveLayer(otherId);f.panel->setTarget(f.document().layer(otherId),false);
    f.canvas->setDocument(f.document().snapshot(),false);f.waitCache();
    check(!f.document().layer(otherId)->filters,"target change does not transfer old filter edit");
    // Replacing the entire document while another preparation is pending must
    // not publish into a newly opened layer or retain the old source snapshot.
    f.session().setActiveLayer(f.id);f.panel->setTarget(&f.layer(),false);value->setValue(32);
    QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
    QImage replacement(18,15,QImage::Format_RGBA8888);replacement.fill(QColor(1,2,3,255));
    const auto path=f.files.filePath("replacement.png");check(replacement.save(path),"save replacement fixture");
    check(f.window.openImageFromPath(path),"replace document during processing");
    QTest::qWait(150);settle();
    check(f.session().document()&&f.session().document()->layers().size()==1,"replacement document remains independent");
    if(f.session().document()&&!f.session().document()->layers().empty())
        check(!f.session().document()->layers().front().filters&&!f.session().document()->layers().front().filterCache,
            "stale old-document work cannot publish into replacement");
}
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setOrganizationName("VulkanaTests");QCoreApplication::setApplicationName("FiltersUiTests");
    QApplication app(argc,argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    const bool native=app.arguments().contains("--native");
    if(native&&QGuiApplication::platformName()!="wayland"&&QGuiApplication::platformName()!="xcb")return 77;
    QVulkanInstance instance;std::atomic_int validationMessages{0};
    if(native){
        instance.setApiVersion(QVersionNumber(1,2));instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
        instance.installDebugOutputFilter([&](auto severity,auto type,const void* data){
            if(type.testFlag(QVulkanInstance::ValidationMessage)&&(severity.testFlag(QVulkanInstance::WarningSeverity)
                ||severity.testFlag(QVulkanInstance::ErrorSeverity))){
                ++validationMessages;std::cerr<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(data)->pMessage<<'\n';
            }
            return false;
        });
        check(instance.create(),"create native filter Vulkan validation instance");
        if(!instance.isValid())return 1;
        nativeInstance=&instance;
    }
    u::applyEditorTheme(app);
    try{embeddedEntryPoints();layerEffectsUi();sharedHeaderScope();compactLayoutAndEnabledRouting();completeResetAndRedo();controlsAndAsync();transformedTypedFilterAdmission();groupedEditsAndMasks();pendingInvalidationAndCancellation();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';++failures;}
    if(native){
        nativeInstance=nullptr;instance.destroy();settle();
        check(validationMessages==0,"native filter Vulkan validation remains clean through destruction");
        std::cout<<"Native "<<QGuiApplication::platformName().toStdString()<<" filters/effects: "<<validationMessages.load()<<" Vulkan warnings/errors\n";
    }
    if(!failures)std::cout<<"Filter UI: cached pages, grouped edits, masks, comparison and asynchronous stale-result rejection passed\n";
    return failures?1:0;
}
