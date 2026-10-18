#include "imageeditor/ui/GradientStopEditor.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/SwatchesPanel.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QListWidget>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTabWidget>
#include <iostream>

namespace c=imageeditor::core;
namespace u=imageeditor::ui;
namespace {
int failures{};
#define CHECK(x) do { if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";} } while(false)
void settle(){QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();}
void move(u::GradientStopEditor& editor,QPointF point) {
    QMouseEvent event(QEvent::MouseMove,point,editor.mapToGlobal(point.toPoint()),
        Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&editor,&event);
}
void editorGestures() {
    u::GradientStopEditor editor;
    editor.resize(300,64);editor.show();settle();
    int starts{},finishes{},cancels{},previews{},colors{};
    editor.onInteractionStarted=[&]{++starts;return true;};
    editor.onInteractionFinished=[&](bool commit){++finishes;if(!commit)++cancels;};
    editor.onStopsChanged=[&](const auto&){++previews;};
    editor.onColorRequested=[&](std::size_t){++colors;};
    CHECK(editor.stops().size()==2);
    QTest::mouseClick(&editor,Qt::LeftButton,{},editor.rampRect().center().toPoint());
    CHECK(editor.stops().size()==3 && starts==1 && finishes==1 && colors==0);
    CHECK(editor.stops()[1].position==.5);
    // Sampled insertion preserves the visible linear ramp to RGBA8 precision.
    CHECK(editor.stops()[1].color.red>=187 && editor.stops()[1].color.red<=188);
    QTest::mouseClick(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    CHECK(colors==1 && starts==1);
    const auto before=editor.stops();
    QTest::mousePress(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    move(editor,editor.stopPosition(1)+QPointF(50,0));
    CHECK(editor.interactionActive() && starts==2 && editor.stops()[1].position>.6);
    move(editor,editor.stopPosition(1)+QPointF(20,0));
    QTest::mouseRelease(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    CHECK(!editor.interactionActive() && finishes==2 && colors==1 && previews==3);
    QTest::mousePress(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    move(editor,editor.stopPosition(1)-QPointF(70,0));
    const auto moved=editor.stops();
    QTest::keyClick(&editor,Qt::Key_Escape);
    QTest::mouseRelease(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    CHECK(cancels==1 && editor.stops()!=moved && colors==1);
    QTest::mouseClick(&editor,Qt::RightButton,{},editor.stopPosition(1).toPoint());
    CHECK(editor.stops().size()==2);
    const auto two=editor.stops();
    QTest::mouseClick(&editor,Qt::RightButton,{},editor.stopPosition(0).toPoint());
    CHECK(editor.stops()==two);
    editor.setStops(before);
    QTest::mouseClick(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    QTest::keyClick(&editor,Qt::Key_Right);
    CHECK(std::abs(editor.stops()[1].position-.51)<1e-9);
    // Duplicate positions imported from PSD stay independently editable by keyboard.
    editor.setStops({{0,{0,0,0,255}},{.5,{255,0,0,255}},{.5,{0,255,0,255}},{1,{255,255,255,255}}});
    QTest::keyClick(&editor,Qt::Key_Down);
    QTest::keyClick(&editor,Qt::Key_Right);
    CHECK(editor.stops()[2].color.green==255 && editor.stops()[2].position>.5);
    editor.setStops(before);
    QTest::mousePress(&editor,Qt::LeftButton,{},editor.stopPosition(1).toPoint());
    move(editor,editor.stopPosition(2)+QPointF(20,0));
    QTest::keyClick(&editor,Qt::Key_Escape);
    QTest::mouseRelease(&editor,Qt::LeftButton,{},editor.stopPosition(2).toPoint());
    QTest::keyClick(&editor,Qt::Key_Right);
    CHECK(std::abs(editor.stops()[1].position-.51)<1e-9);
}
void panelTransactions() {
    u::EffectsPanel panel;
    auto layer=c::Layer::raster("Gradient",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{4,4}));
    panel.setTarget(&layer);panel.selectType(c::LayerEffectType::GradientOverlay);
    panel.resize(330,600);panel.show();settle();
    auto* editor=dynamic_cast<u::GradientStopEditor*>(panel.findChild<QWidget*>("EffectGradientStops"));
    CHECK(editor);if(!editor)return;
    CHECK(!panel.findChild<QWidget*>("EffectColor60") && !panel.findChild<QWidget*>("EffectColor61"));
    int starts{},finishes{};bool accepted{};
    c::LayerEffectState latest;
    panel.onInteractionStarted=[&]{++starts;return true;};
    panel.onPreview=[&](auto state){latest=state;layer.effects=state;panel.setTarget(&layer);};
    panel.onInteractionFinished=[&](bool commit){++finishes;accepted=commit;};
    QTest::mouseClick(editor,Qt::LeftButton,{},editor->rampRect().center().toPoint());
    CHECK(latest && latest->items[6].gradientStops.size()==3);
    CHECK(starts==1 && finishes==1 && accepted);
    QTest::mousePress(editor,Qt::LeftButton,{},editor->stopPosition(1).toPoint());
    move(*editor,editor->stopPosition(1)+QPointF(30,0));
    move(*editor,editor->stopPosition(1)+QPointF(30,0));
    QTest::mouseRelease(editor,Qt::LeftButton,{},editor->stopPosition(1).toPoint());
    CHECK(starts==2 && finishes==2 && accepted);
    // A changed target cancels any pending gesture before applying another layer.
    QTest::mousePress(editor,Qt::LeftButton,{},editor->stopPosition(1).toPoint());
    auto other=c::Layer::raster("Other",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{4,4}));
    panel.setTarget(&other);
    move(*editor,editor->stopPosition(0)+QPointF(25,0));
    QTest::mouseRelease(editor,Qt::LeftButton,{},editor->stopPosition(0).toPoint());
    CHECK(starts==2 && finishes==2);
}
void presetWorkflow() {
    QTemporaryDir assets;
    u::MainWindow window(nullptr,false,false);window.setUnsavedPromptEnabled(false);
    window.resize(1280,850);window.show();settle();
    QImage image(48,48,QImage::Format_RGBA8888);image.fill(Qt::white);
    const auto path=assets.filePath("gradient.png");CHECK(image.save(path));
    CHECK(window.openImageFromPath(path));
    auto& session=const_cast<c::EditorSession&>(window.editorSession());
    auto* layer=session.document()->layer(*session.activeLayer());
    const auto layerId=layer->id;
    auto state=std::make_shared<c::LayerEffectStack>();
    auto& gradient=state->items[6];gradient.enabled=true;
    gradient.opacity=.4;gradient.blendMode=c::BlendMode::Multiply;gradient.angle=-28;
    gradient.scale=2.3;gradient.reverse=true;gradient.gradient=c::GradientType::Radial;
    state->items[0].enabled=true;state->items[0].size=2;
    CHECK(session.document()->setLayerEffects(layer->id,state));
    auto* swatches=dynamic_cast<u::SwatchesPanel*>(window.findChild<QWidget*>("SwatchesPanelContent"));
    auto* effects=dynamic_cast<u::EffectsPanel*>(window.findChild<QWidget*>("EffectsPanelContent"));
    CHECK(swatches && effects);if(!swatches || !effects)return;
    const auto colors=session.colors();
    const auto depth=session.history().undoDepth();
    const std::vector<c::GradientStop> preset{{0,{10,20,30,255}},{.3,{240,120,20,255}},{1,{90,50,220,255}}};
    swatches->onGradientSelected(preset);settle();
    layer=session.document()->layer(layerId);
    const auto after=layer->effects;
    CHECK(after && after->items[6].gradientStops==preset);
    CHECK(after->items[6].enabled && after->items[6].blendMode==gradient.blendMode);
    CHECK(after->items[6].opacity==gradient.opacity && after->items[6].angle==gradient.angle);
    CHECK(after->items[6].scale==gradient.scale && after->items[6].reverse==gradient.reverse);
    CHECK(after->items[6].gradient==gradient.gradient && after->items[0]==state->items[0]);
    CHECK(session.colors()==colors && session.history().undoDepth()==depth+1);
    swatches->onGradientSelected(preset);
    CHECK(session.history().undoDepth()==depth+1);
    auto* undo=window.findChild<QAction*>("UndoAction");
    CHECK(undo);if(undo)undo->trigger();settle();
    CHECK(c::equivalentLayerEffects(session.document()->layer(layerId)->effects,state));
    swatches->onGradientSelected(preset);settle();
    auto* showFilters=window.findChild<QAction*>("FilterAction_0");
    auto* tabs=window.findChild<QTabWidget*>("AdjustmentTabs");
    CHECK(showFilters && tabs);
    if(showFilters)showFilters->trigger();
    if(tabs)tabs->setCurrentIndex(4);
    settle();
    auto* editor=dynamic_cast<u::GradientStopEditor*>(effects->findChild<QWidget*>("EffectGradientStops"));
    CHECK(editor && editor->isVisible());if(!editor)return;
    // Accepted color edits replace only RGB on the selected stop and undo once.
    bool dialogSeen=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<u::ColorDialog*>("GradientStopColorDialog");
        CHECK(dialog);if(!dialog)return;dialogSeen=true;
        dialog->setCurrentColor(QColor(22,133,244));dialog->accept();
    });
    QTest::mouseClick(editor,Qt::LeftButton,{},editor->stopPosition(1).toPoint());settle();
    CHECK(dialogSeen);
    CHECK((session.document()->layer(layerId)->effects->items[6].gradientStops[1].color==c::Rgba8{22,133,244,255}));
    CHECK(session.history().undoDepth()==depth+2 && session.colors()==colors);
    if(undo)undo->trigger();
    settle();
    CHECK(session.document()->layer(layerId)->effects->items[6].gradientStops==preset);
    // Cancel color preview restores exact state without adding history.
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<u::ColorDialog*>("GradientStopColorDialog");
        CHECK(dialog);if(dialog){dialog->setCurrentColor(Qt::green);dialog->reject();}
    });
    effects->onGradientColorRequested(0);settle();
    CHECK(session.document()->layer(layerId)->effects->items[6].gradientStops==preset);
    CHECK(session.history().undoDepth()==depth+1);
    QTest::mousePress(editor,Qt::LeftButton,{},editor->stopPosition(1).toPoint());
    move(*editor,editor->stopPosition(1)+QPointF(30,0));
    move(*editor,editor->stopPosition(1)+QPointF(20,0));
    QTest::mouseRelease(editor,Qt::LeftButton,{},editor->stopPosition(1).toPoint());settle();
    CHECK(session.document()->layer(layerId)->effects->items[6].gradientStops!=preset);
    CHECK(session.history().undoDepth()==depth+2);
    if(undo)undo->trigger();
    settle();
    CHECK(session.document()->layer(layerId)->effects->items[6].gradientStops==preset);
    auto* folder=window.findChild<QAction*>("NewLayerFolderAction");
    CHECK(folder);if(folder)folder->trigger();settle();
    auto* grid=swatches->findChild<QListWidget*>("GradientSwatches");
    CHECK(grid && !grid->isEnabled());
    const auto folderDepth=session.history().undoDepth();
    swatches->onGradientSelected(preset);
    CHECK(session.history().undoDepth()==folderDepth);
    window.close();settle();
}
void reviewImage() {
    const auto path=qEnvironmentVariable("IMAGEEDITOR_GRADIENT_EDITOR_REVIEW");
    if(path.isEmpty())return;
    QWidget widget;auto* layout=new QHBoxLayout(&widget);
    auto* swatches=new u::SwatchesPanel;auto* effects=new u::EffectsPanel;
    layout->addWidget(swatches);layout->addWidget(effects);
    auto layer=c::Layer::raster("Gradient",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{4,4}));
    auto state=std::make_shared<c::LayerEffectStack>();state->items[6].enabled=true;
    state->items[6].gradientStops={{0,{38,58,148,255}},{.32,{136,100,205,255}},{.68,{236,165,203,255}},{1,{255,228,177,255}}};
    layer.effects=state;effects->setTarget(&layer);effects->selectType(c::LayerEffectType::GradientOverlay);
    swatches->setGradientEnabled(true);widget.resize(720,600);widget.show();settle();
    CHECK(widget.grab().save(path));
}
}
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName("ImageEditorTests");app.setApplicationName("GradientEditor");
    QStandardPaths::setTestModeEnabled(true);QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    u::applyEditorTheme(app);
    editorGestures();panelTransactions();presetWorkflow();reviewImage();
    return failures?1:0;
}
