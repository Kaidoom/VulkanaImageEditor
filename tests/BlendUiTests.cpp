#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QPushButton>
#include <QSettings>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <iostream>

namespace c = imageeditor::core;
namespace r = imageeditor::render;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value,const char* expression,int line)
{ if (!value) { ++failures; std::cerr << "FAIL " << line << ": " << expression << '\n'; } }
#define CHECK(...) check(bool(__VA_ARGS__),#__VA_ARGS__,__LINE__)
void settle()
{
    for (int i=0;i<3;++i) { QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(); }
}
void checkDisplayOrder(const QComboBox& combo)
{
    const QStringList expected {
        "Normal", "Dissolve", "Darken", "Multiply", "Color Burn", "Linear Burn",
        "Darker Color", "Lighten", "Screen", "Color Dodge", "Linear Dodge (Add)",
        "Lighter Color", "Overlay", "Soft Light", "Hard Light", "Vivid Light",
        "Linear Light", "Pin Light", "Hard Mix", "Difference", "Exclusion",
        "Subtract", "Divide", "Hue", "Saturation", "Color", "Luminosity"};
    CHECK(combo.count()==expected.size());
    for(int row=0;row<expected.size();++row) {
        CHECK(combo.itemText(row)==expected[row]);
        CHECK(QString::fromUtf8(c::blendModeName(c::BlendMode(combo.itemData(row).toUInt())))==expected[row]);
    }
    for(auto mode:c::allBlendModes) CHECK(combo.findData(int(mode))>=0);
}
struct Fixture {
    QTemporaryDir assets;
    u::MainWindow window {nullptr,false,false};
    QComboBox* combo {};
    u::LayerListView* view {};
    u::LayerListModel* model {};
    r::CanvasWindow* canvas {};
    c::LayerId base {};
    Fixture()
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1540,900); window.show(); settle();
        combo=window.findChild<QComboBox*>(QStringLiteral("LayerBlendModeCombo"));
        view=dynamic_cast<u::LayerListView*>(window.findChild<QListView*>(QStringLiteral("LayerList")));
        model=view?dynamic_cast<u::LayerListModel*>(view->model()):nullptr;
        for (auto* candidate:QGuiApplication::allWindows())
            if (candidate->objectName()==QStringLiteral("VulkanCanvasWindow")) canvas=dynamic_cast<r::CanvasWindow*>(candidate);
        QImage source(64,48,QImage::Format_RGBA8888); source.fill(QColor(67,136,218,153));
        const auto file=assets.filePath("blend-ui.png");
        CHECK(source.save(file)); CHECK(window.openImageFromPath(file));
        base=session().activeLayer().value_or(0);
        CHECK(combo && view && model && canvas && base);
    }
    ~Fixture() { window.close(); settle(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    c::Document& document() { return *session().document(); }
    bool valid() const { return combo && view && model && canvas && base; }
    void select(c::LayerId id,Qt::KeyboardModifiers modifiers={})
    {
        model->refresh();
        const auto index=model->index(model->rowForLayer(id)); CHECK(index.isValid());
        if (!index.isValid()) return;
        view->scrollTo(index); settle();
        QTest::mouseClick(view->viewport(),Qt::LeftButton,modifiers,view->visualRect(index).center()); settle();
    }
    void choose(c::BlendMode mode)
    {
        const int index=combo->findData(int(mode)); CHECK(index>=0);
        combo->setCurrentIndex(index);
        CHECK(QMetaObject::invokeMethod(combo,"activated",Qt::DirectConnection,Q_ARG(int,index))); settle();
    }
    void history(bool redo)
    {
        for (auto* action:window.findChildren<QAction*>())
            if (action->shortcuts().contains(redo?QKeySequence(QKeySequence::Redo):QKeySequence(QKeySequence::Undo))) {
                action->trigger(); settle(); return;
            }
        CHECK(false);
    }
};

void comboAndPrimaryHistory()
{
    Fixture f; if (!f.valid()) return;
    CHECK(f.combo->isEnabled()); CHECK(f.combo->count()==int(c::allBlendModes.size()));
    CHECK(f.combo->maxVisibleItems()==f.combo->count());
    CHECK(f.combo->style()->styleHint(QStyle::SH_ComboBox_Popup,nullptr,f.combo)==0);
    checkDisplayOrder(*f.combo);
    f.combo->showPopup(); settle();
    auto* popup = f.combo->view();
    CHECK(popup->isVisible());
    CHECK(popup->verticalScrollBar()->maximum()==0);
    CHECK(popup->viewport()->rect().contains(popup->visualRect(
        popup->model()->index(f.combo->count()-1,0))));
    f.combo->hidePopup(); settle();
    auto second=c::Layer::raster("Second",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u {20,20},c::Rgba8 {200,80,40,117}));
    const auto secondId=second.id;
    CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(second),1)));
    f.select(f.base); f.select(secondId,Qt::ControlModifier);
    CHECK(f.session().selectedLayers().size()==2 && f.session().activeLayer()==secondId);
    const auto pixels=std::get<c::RasterLayer>(f.document().layer(secondId)->payload).surface;
    const auto revision=pixels->revision();
    const auto depth=f.session().history().undoDepth();
    f.choose(c::BlendMode::Multiply);
    CHECK(f.document().layer(secondId)->blendMode==c::BlendMode::Multiply);
    CHECK(f.document().layer(f.base)->blendMode==c::BlendMode::Normal);
    CHECK(f.session().history().undoDepth()==depth+1);
    CHECK(f.canvas->scene().document.layersBottomToTop.back().blendMode==c::BlendMode::Multiply);
    f.choose(c::BlendMode::Multiply);
    CHECK(f.session().history().undoDepth()==depth+1);
    f.history(false);
    CHECK(f.combo->currentData().toInt()==int(c::BlendMode::Normal));
    CHECK(f.document().layer(secondId)->blendMode==c::BlendMode::Normal);
    const auto redoDepth=f.session().history().redoDepth();
    f.choose(c::BlendMode::Normal);
    CHECK(f.session().history().redoDepth()==redoDepth);
    CHECK(f.session().history().undoDepth()==depth);
    f.history(true);
    CHECK(f.combo->currentData().toInt()==int(c::BlendMode::Multiply));
    CHECK(f.document().layer(secondId)->blendMode==c::BlendMode::Multiply);
    CHECK(std::get<c::RasterLayer>(f.document().layer(secondId)->payload).surface==pixels);
    CHECK(pixels->revision()==revision);
    const auto wheelDepth=f.session().history().undoDepth();
    for (const int delta:{120,-120}) {
        f.combo->setFocus();
        const QPointF pos(f.combo->rect().center());
        QWheelEvent wheel(pos,f.combo->mapToGlobal(pos.toPoint()),{},QPoint(0,delta),
            Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QCoreApplication::sendEvent(f.combo,&wheel); settle();
        CHECK(f.combo->currentData().toInt()==int(c::BlendMode::Multiply));
        CHECK(f.session().history().undoDepth()==wheelDepth);
    }
}

void containersStayPassThrough()
{
    Fixture f; if (!f.valid()) return;
    auto tree=f.document().tree();
    const auto groupId=c::makeLayerId(),folderId=c::makeLayerId();
    c::LayerContainer group {groupId,"Group",c::ContainerKind::Group}; group.children={f.base};
    c::LayerContainer folder {folderId,"Folder",c::ContainerKind::Folder}; folder.children={groupId};
    tree.roots={folderId}; tree.containers={folder,group};
    CHECK(f.document().replaceStructure(f.document().tree(),std::move(tree)));
    const auto depth=f.session().history().undoDepth();
    for (const auto id:{folderId,groupId}) {
        f.select(id);
        CHECK(!f.combo->isEnabled());
        CHECK(f.combo->currentText()==QStringLiteral("Pass Through"));
        CHECK(f.document().layer(f.base)->blendMode==c::BlendMode::Normal);
        CHECK(f.session().history().undoDepth()==depth);
    }
    // Dissolve the containers so selecting a leaf returns the same cached
    // control to its ordinary leaf-mode presentation, not an isolated group mode.
    tree=f.document().tree(); CHECK(tree.dissolve(groupId)); CHECK(tree.dissolve(folderId));
    CHECK(f.document().replaceStructure(f.document().tree(),std::move(tree)));
    f.select(f.base);
    CHECK(f.combo->isEnabled() && f.combo->count()==int(c::allBlendModes.size()));
    CHECK(f.combo->currentIndex()==0);
}

void blendSettlesTransformWithoutDiscardingItsActions()
{
    Fixture f; if (!f.valid()) return;
    f.select(f.base);
    auto* transform=f.window.findChild<QAction*>(QStringLiteral("LayerTransformAction"));
    auto* x=f.window.findChild<QDoubleSpinBox*>(QStringLiteral("TransformXControl"));
    CHECK(transform && x); if (!transform || !x) return;
    const auto pixels=std::get<c::RasterLayer>(f.document().layer(f.base)->payload).surface;
    const auto revision=pixels->revision();
    const auto initial=f.document().layer(f.base)->localToDocument;
    const auto depth=f.session().history().undoDepth();
    transform->trigger(); settle();
    CHECK(f.canvas->scene().transformOverlay);
    x->setValue(x->value()+9); settle();
    const auto moved=f.document().layer(f.base)->localToDocument;
    CHECK(moved.m02==initial.m02+9);
    CHECK(f.session().history().undoDepth()==depth); // Pending Ctrl+T branch.
    f.choose(c::BlendMode::Normal); // Selecting unchanged mode must not finish it.
    CHECK(f.canvas->scene().transformOverlay);
    CHECK(f.document().layer(f.base)->localToDocument.m02==moved.m02);
    f.choose(c::BlendMode::Screen);
    CHECK(!f.canvas->scene().transformOverlay);
    CHECK(f.document().layer(f.base)->localToDocument.m02==moved.m02);
    CHECK(f.document().layer(f.base)->blendMode==c::BlendMode::Screen);
    CHECK(f.session().history().undoDepth()==depth+2);
    f.history(false);
    CHECK(f.document().layer(f.base)->blendMode==c::BlendMode::Normal);
    CHECK(f.document().layer(f.base)->localToDocument.m02==moved.m02);
    f.history(false);
    CHECK(f.document().layer(f.base)->localToDocument.m02==initial.m02);
    f.history(true); f.history(true);
    CHECK(f.document().layer(f.base)->localToDocument.m02==moved.m02);
    CHECK(f.combo->currentData().toInt()==int(c::BlendMode::Screen));
    CHECK(std::get<c::RasterLayer>(f.document().layer(f.base)->payload).surface==pixels);
    CHECK(pixels->revision()==revision);
}

void adjustmentBlendControls()
{
    Fixture f; if (!f.valid()) return;
    auto adjustment=c::Layer::adjustment("Correction");
    const auto id=adjustment.id;
    const auto seed=adjustment.blendSeed;
    CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(adjustment),1)));
    f.select(id);
    CHECK(f.combo->isEnabled());
    for (const auto mode:c::allBlendModes) {
        f.choose(mode);
        CHECK(f.document().layer(id)->blendMode==mode);
        CHECK(f.combo->currentData().toInt()==int(mode));
        f.select(f.base); f.select(id);
        CHECK(f.combo->currentData().toInt()==int(mode));
        CHECK(f.document().layer(id)->blendSeed==seed);
        CHECK(f.document().layer(f.base)->blendMode==c::BlendMode::Normal);
    }
    f.history(false);
    CHECK(f.combo->currentData().toInt()==int(c::BlendMode::PinLight));
    const auto redo=f.session().history().redoDepth();
    f.choose(c::BlendMode::PinLight);
    CHECK(f.session().history().redoDepth()==redo);
    f.history(true);
    CHECK(f.combo->currentData().toInt()==int(c::BlendMode::HardMix));
}

void effectBlendBindings()
{
    u::EffectsPanel panel;
    auto layer=c::Layer::shape("Styled",c::ShapeLayer{});
    auto state=std::make_shared<c::LayerEffectStack>();
    for(std::size_t i=0;i<c::layerEffectCount;++i)
        state->items[i].blendMode=c::allBlendModes[i+7];
    state->items[7].bevel.shadowBlend=c::BlendMode::DarkerColor;
    layer.effects=state;
    int previews=0;
    panel.onPreview=[&](c::LayerEffectState value){layer.effects=value;++previews;};
    panel.setTarget(&layer);
    CHECK(previews==0);
    auto* nav=panel.findChild<QComboBox*>("EffectNavigation");
    CHECK(nav); if(!nav)return;
    for(std::size_t i=0;i<c::layerEffectCount;++i) {
        nav->setCurrentIndex(int(i));
        for(bool shadow:{false,true}) {
            if(shadow&&i!=7)continue;
            const auto label=i==7?(shadow?"Shadow blend":"Highlight blend"):"Blend";
            auto* box=panel.findChild<QComboBox*>(QString("Effect%1%2").arg(i).arg(label));
            CHECK(box);if(!box)continue;
            checkDisplayOrder(*box);
            const auto current=shadow?layer.effects->items[7].bevel.shadowBlend:layer.effects->items[i].blendMode;
            CHECK(box->currentData().toInt()==int(current));
            for(int row=0;row<box->count();++row) {
                auto expected=*layer.effects;
                const auto mode=c::BlendMode(box->itemData(row).toUInt());
                if(shadow)expected.items[7].bevel.shadowBlend=mode;
                else expected.items[i].blendMode=mode;
                box->setCurrentIndex(row);
                CHECK(*layer.effects==expected);
                const auto count=previews;
                panel.setTarget(nullptr);panel.setTarget(&layer);
                CHECK(previews==count);
                CHECK(box->currentData().toInt()==int(mode));
            }
        }
    }
    panel.findChild<QPushButton*>("EffectResetCurrent")->click();
    CHECK(layer.effects->items[7]==c::defaultLayerEffect(c::LayerEffectType::BevelEmboss));
    CHECK(panel.findChild<QComboBox*>("Effect7Highlight blend")->currentData().toInt()==int(c::BlendMode::Screen));
    CHECK(panel.findChild<QComboBox*>("Effect7Shadow blend")->currentData().toInt()==int(c::BlendMode::Multiply));
}
}

int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("BlendUiTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    u::applyEditorTheme(app);
    comboAndPrimaryHistory();
    containersStayPassThrough();
    blendSettlesTransformWithoutDiscardingItsActions();
    adjustmentBlendControls();
    effectBlendBindings();
    std::cout << (failures?"Blend UI tests FAILED\n":"Blend UI tests passed\n");
    return failures?1:0;
}
