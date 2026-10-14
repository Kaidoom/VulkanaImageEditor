#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerEffects.hpp"
#include "imageeditor/core/LayerMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QContextMenuEvent>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; } } while (false)
void icons(QMenu* menu)
{
    for (auto* action : menu->actions()) {
        if (action->isSeparator()) continue;
        // The empty-recents placeholder is informational, not an operation.
        if (!action->isEnabled() && action->text() == "No recent files") continue;
        if (action->icon().isNull()) std::cerr << "Missing icon: " << action->text().toStdString() << '\n';
        CHECK(!action->icon().isNull());
        for (qreal dpr : {1., 1.5, 2.}) {
            const auto image = action->icon().pixmap(QSize(18,18), dpr, QIcon::Normal, QIcon::Off).toImage();
            bool ink = false;
            for (int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) ink |= qAlpha(image.pixel(x,y)) != 0;
            CHECK(ink);
        }
        if (action->menu()) icons(action->menu());
    }
}
}
int main(int argc, char** argv)
{
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir files;
    app.setOrganizationName("VulkanaTests"); app.setApplicationName("MenuClipboard");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,files.path());
    QIcon::setThemeName("missing-theme-for-menu-test");
    u::applyEditorTheme(app);
    u::MainWindow window(nullptr,false); window.setUnsavedPromptEnabled(false);
    window.resize(1100,850); window.show(); QTest::qWait(30);
    const auto artifact = app.arguments().size() > 1 ? app.arguments()[1] : QString();
    if (!artifact.isEmpty()) QDir().mkpath(artifact);
    for (auto* top : window.menuBar()->actions()) if (top->menu()) icons(top->menu());
    // Checkable menu icons must still communicate on/off, in both themes.
    for (const auto preset : {u::ThemePreset::Dark,u::ThemePreset::Light}) {
        u::ThemeSettings theme; theme.preset=preset; u::applyEditorTheme(app,theme);
        for (auto* top : window.menuBar()->actions()) if (top->menu()) icons(top->menu());
        QMenu toggle;
        auto* option=toggle.addAction(u::toolGlyph(u::ToolGlyph::Pixels),QStringLiteral("Preview"));
        option->setCheckable(true); option->setChecked(true);
        toggle.popup(window.mapToGlobal(QPoint(120,80))); QTest::qWait(10);
        const auto on=toggle.grab().toImage(); option->setChecked(false); QTest::qWait(10);
        const auto off=toggle.grab().toImage(); CHECK(on!=off);
        if(!artifact.isEmpty())CHECK(on.save(QDir(artifact).filePath(preset==u::ThemePreset::Dark?"checked-dark.png":"checked-light.png")));
        toggle.hide();
    }
    u::applyEditorTheme(app);
    if (!artifact.isEmpty()) {
        int n=0;
        for (auto* top : window.menuBar()->actions()) if (auto* menu=top->menu()) {
            menu->popup(window.mapToGlobal(QPoint(120,80))); QTest::qWait(10);
            CHECK(menu->grab().save(QDir(artifact).filePath(QStringLiteral("menu-%1.png").arg(++n))));
            menu->hide();
        }
        QImage sheet(600,240,QImage::Format_ARGB32_Premultiplied); sheet.fill(app.palette().color(QPalette::Window));
        QPainter painter(&sheet);
        for(int i=int(u::ToolGlyph::NewDocument);i<=int(u::ToolGlyph::Hidden);++i) {
            const int cell=i-int(u::ToolGlyph::NewDocument);
            u::toolGlyph(u::ToolGlyph(i)).paint(&painter,QRect(10+(cell%10)*60,10+(cell/10)*70,24,24));
            painter.setPen(app.palette().color(QPalette::Text)); painter.drawText(10+(cell%10)*60,55+(cell/10)*70,QString::number(cell));
        }
        painter.end(); CHECK(sheet.save(QDir(artifact).filePath("glyphs.png")));
    }
    // Current canvas, not the view or selection; include transformed typed
    // content, an effect and a partial mask, while retaining transparency.
    QImage source(48,36,QImage::Format_RGBA8888); source.fill(QColor(30,70,140,100));
    source.setPixelColor(47,35,QColor(0,0,0,0));
    CHECK(source.save(files.filePath("input.png")));
    CHECK(window.openImageFromPath(files.filePath("input.png")));
    const auto firstTab=window.activeDocumentId();
    CHECK(source.save(files.filePath("other.png")));
    CHECK(window.openImageFromPath(files.filePath("other.png")));
    const auto otherTab=window.activeDocumentId(); CHECK(otherTab!=firstTab);
    CHECK(window.activateDocument(firstTab));
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    auto& doc = *session.document();
    c::ShapeLayer geometry; geometry.size={16,12}; geometry.fillColor={230,90,40,255};
    auto shape=c::Layer::shape("Shape",geometry); shape.localToDocument={0,-1,30,1,0,5};
    auto effect=std::make_shared<c::LayerEffectStack>();
    effect->items[std::size_t(c::LayerEffectType::ColorOverlay)].enabled=true;
    shape.effects=effect;
    shape.mask=std::make_shared<c::LayerMask>(c::LayerMask{c::SelectionMask::filled({16,12},128),{},255,true});
    CHECK(doc.insertLayer(doc.layers().size(),shape));
    auto hidden=c::Layer::raster("Hidden",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{48,36},c::Rgba8{255,0,255,255}));
    hidden.visible=false; CHECK(doc.insertLayer(doc.layers().size(),hidden));
    doc.setSelection(c::SelectionMask::rectangle({48,36},{5,5,2,2}));
    CHECK(session.execute(std::make_unique<c::AddLayerCommand>(c::Layer::raster("Redo",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{1,1},c::Rgba8{})), doc.layers().size())));
    CHECK(session.undo()); doc.markSaved();
    const auto state=doc.contentState(), revision=doc.revision();
    const auto selection=doc.selection(); const auto active=session.layerSelectionState();
    const auto depth=session.history().undoDepth(); const auto path=window.projectPath();
    u::ExportSettings settings; settings.size=QSize(48,36);
    const auto expected=u::renderExport(doc,settings); CHECK(expected);
    QImage copied; int publications=0;
    u::MainWindow::FileInteractions hooks;
    hooks.publishClipboardImage=[&](const QImage& image){copied=image;++publications;};
    window.setFileInteractions(hooks);
    auto* copy=window.findChild<QAction*>("CopyCanvasToClipboardAction"); CHECK(copy && copy->isEnabled());
    if(copy)copy->trigger();
    CHECK(publications==1); CHECK(copied==expected.image); CHECK(copied.size()==QSize(48,36));
    CHECK(copied.colorSpace()==QColorSpace(QColorSpace::SRgb));
    CHECK(copied.pixelColor(0,0).alpha()==100);
    CHECK(copied.pixelColor(47,35).alpha()==0);
    CHECK(doc.contentState()==state && doc.revision()==revision && !doc.isModified());
    CHECK(doc.selection()==selection && session.layerSelectionState()==active);
    CHECK(session.history().undoDepth()==depth && session.history().canRedo()); CHECK(window.projectPath()==path);
    // Cancellation and failed rendering preserve the existing clipboard.
    const auto originalTab=window.activeDocumentId();
    hooks.progress=[&](auto,auto){
        CHECK(!window.activateDocument(otherTab));
        return false;
    }; window.setFileInteractions(hooks);
    window.copyCanvasToClipboard(); CHECK(publications==1 && copied==expected.image);
    CHECK(window.activeDocumentId()==originalTab);
    hooks.progress={}; window.setFileInteractions(hooks);
    const auto originalTransform=doc.layer(shape.id)->localToDocument;
    doc.layer(shape.id)->localToDocument={0,0,0,0,0,0};
    window.copyCanvasToClipboard(); CHECK(publications==1 && copied==expected.image);
    doc.layer(shape.id)->localToDocument=originalTransform;
    // Actual Qt image publication is safe in an isolated offscreen test only.
    if (QGuiApplication::platformName()=="offscreen") {
        window.setFileInteractions({}); window.copyCanvasToClipboard();
        CHECK(QApplication::clipboard()->image()==expected.image);
    }
    imageeditor::render::CanvasWindow* canvas=nullptr;
    for(auto* w:QGuiApplication::allWindows())if(auto* c=dynamic_cast<imageeditor::render::CanvasWindow*>(w))canvas=c;
    CHECK(canvas);
    if(canvas) {
        const auto zoom=canvas->zoom(); canvas->resetTo100Percent();
        window.setFileInteractions(hooks); window.copyCanvasToClipboard(); CHECK(copied==expected.image);
        Q_UNUSED(zoom);
        window.findChild<QAction*>("ToolAction_marquee")->trigger();
        bool seen=false;
        QTimer::singleShot(0,&window,[&]{
            if(auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                seen=true; CHECK(menu->objectName()=="CanvasSelectionContextMenu"); icons(menu);
                if(!artifact.isEmpty())CHECK(menu->grab().save(QDir(artifact).filePath("canvas-context.png")));
                menu->close();
            }
        });
        QContextMenuEvent event(QContextMenuEvent::Keyboard,{20,20},window.mapToGlobal(QPoint(200,200)));
        QApplication::sendEvent(canvas,&event); CHECK(seen);
    }
    const auto ids=window.documentIds();
    for(auto id:ids)CHECK(window.closeDocument(id));
    CHECK(!copy->isEnabled()); window.copyCanvasToClipboard(); CHECK(copied==expected.image);
    window.close();
    std::cout<<"Menu/clipboard checks: "<<failures<<" failures\n";
    return failures?1:0;
}
