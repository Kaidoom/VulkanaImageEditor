#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QImage>
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
        combo->setCurrentIndex(int(mode));
        CHECK(QMetaObject::invokeMethod(combo,"activated",Qt::DirectConnection,Q_ARG(int,int(mode)))); settle();
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
    for (const auto mode:c::allBlendModes)
        CHECK(f.combo->itemText(int(mode))==QString::fromUtf8(c::blendModeName(mode)));
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
    CHECK(f.combo->currentIndex()==int(c::BlendMode::Normal));
    CHECK(f.document().layer(secondId)->blendMode==c::BlendMode::Normal);
    const auto redoDepth=f.session().history().redoDepth();
    f.choose(c::BlendMode::Normal);
    CHECK(f.session().history().redoDepth()==redoDepth);
    CHECK(f.session().history().undoDepth()==depth);
    f.history(true);
    CHECK(f.combo->currentIndex()==int(c::BlendMode::Multiply));
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
        CHECK(f.combo->currentIndex()==int(c::BlendMode::Multiply));
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
    CHECK(f.combo->currentIndex()==int(c::BlendMode::Screen));
    CHECK(std::get<c::RasterLayer>(f.document().layer(f.base)->payload).surface==pixels);
    CHECK(pixels->revision()==revision);
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
    std::cout << (failures?"Blend UI tests FAILED\n":"Blend UI tests passed\n");
    return failures?1:0;
}
